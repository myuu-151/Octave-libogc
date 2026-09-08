// -----------------------------------------------------------------------------
// Minimal open-source GameCube apploader
//
// The GameCube IPL (BIOS) cannot run a game directly -- it reads this small
// "apploader" off the disc (at disc offset 0x2440) into memory at 0x81200000
// and hands control to it. The apploader is then responsible for:
//
//   1. Loading the disc FST into memory and recording its address in the OS
//      low-memory globals (0x80000038 / 0x8000003C), and lowering the arena top
//      (0x80000034) so nothing allocates over it.
//   2. Loading every section of main.dol to its target address.
//   3. Clearing the DOL's BSS and returning the DOL entry point.
//
// It talks to the IPL through three callbacks. The IPL performs the actual DVD
// reads we request; we never touch the drive directly. This is deliberately
// tiny, freestanding (no libc / libogc), and fully reviewable -- it is bundled
// as source and compiled by devkitPPC, not shipped as an opaque blob.
//
// Layout / protocol reference: YAGCD "Apploader" chapter (public domain).
//
// This file is released into the public domain (Unlicense). Do whatever.
// -----------------------------------------------------------------------------

typedef unsigned int   u32;
typedef unsigned char  u8;

typedef void  (*OSReport)(const char* fmt, ...);
typedef void  (*AppInit)(OSReport report);
typedef int   (*AppMain)(void** dst, int* size, int* offset);
typedef void* (*AppClose)(void);
typedef void  (*AppEntry)(AppInit* init, AppMain* main, AppClose* close);

// Disc header (boot.bin) is loaded by the IPL at 0x80000000, so these fields
// are directly readable. Everything is big-endian native on PPC.
#define DISC_DOL_OFFSET   (*(volatile u32*)0x80000420)
#define DISC_FST_OFFSET   (*(volatile u32*)0x80000424)
#define DISC_FST_SIZE     (*(volatile u32*)0x80000428)
#define DISC_FST_MAXSIZE  (*(volatile u32*)0x8000042C)

// OS low-memory globals the runtime and OS read later.
#define OS_ARENA_HI       (*(volatile u32*)0x80000034)
#define OS_FST_ADDR       (*(volatile u32*)0x80000038)
#define OS_FST_MAXSIZE    (*(volatile u32*)0x8000003C)

// Scratch addresses below the apploader load address (0x81200000), free during
// boot. We read the disc's boot header and the DOL header into these.
#define BOOTHDR_SCRATCH    0x81000000   // boot.bin (0x440 bytes)
#define DOL_HEADER_SCRATCH 0x81000800   // DOL header (0x100 bytes)

static u32   sState      = 0;
static int   sDolIndex   = 0;
static u32   sDolOffset  = 0;   // DOL data offset on disc
static u32   sFstOffset  = 0;   // FST offset on disc
static u32   sFstSize    = 0;   // FST size
static u8*   sBootHdr    = (u8*)BOOTHDR_SCRATCH;
static u8*   sDolHeader  = (u8*)DOL_HEADER_SCRATCH;

// Record loaded code/data regions so we can make them cache-coherent before the
// CPU executes/reads them (the IPL DMAs disc data into RAM).
#define MAX_REGIONS 20
static u32   sRegionAddr[MAX_REGIONS];
static u32   sRegionSize[MAX_REGIONS];
static int   sRegionCount = 0;

static u32 Read32(const u8* p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static void ZeroMem(u32 addr, u32 size)
{
    volatile u8* p = (volatile u8*)addr;
    for (u32 i = 0; i < size; ++i) p[i] = 0;
}

static void DCFlushRange(u32 addr, u32 size)
{
    u32 a = addr & ~31u;
    u32 end = addr + size;
    for (; a < end; a += 32)
        __asm__ volatile ("dcbf 0,%0" :: "r"(a) : "memory");
    __asm__ volatile ("sync" ::: "memory");
}

static void ICInvalidateRange(u32 addr, u32 size)
{
    u32 a = addr & ~31u;
    u32 end = addr + size;
    for (; a < end; a += 32)
        __asm__ volatile ("icbi 0,%0" :: "r"(a) : "memory");
    __asm__ volatile ("sync; isync" ::: "memory");
}

static void app_init(OSReport report)
{
    (void)report;
    sState     = 0;
    sDolIndex  = 0;
    sRegionCount = 0;
}

// Called repeatedly by the IPL. Return 1 to request a DVD read of *size bytes
// from disc *offset into *dst; return 0 when finished.
static int app_main(void** dst, int* size, int* offset)
{
    switch (sState)
    {
    case 0:
    {
        // Read the disc boot header (boot.bin) from disc offset 0. We parse the
        // DOL/FST offsets from it ourselves rather than trusting the low-memory
        // globals at 0x80000420+, because some loaders (e.g. Dolphin's fake BS2)
        // do not populate the full boot.bin in RAM before running the apploader.
        *dst    = (void*)sBootHdr;
        *size   = 0x440;
        *offset = 0;
        sState  = 1;
        return 1;
    }

    case 1:
    {
        // Parse offsets from the boot header, then load the FST high in RAM.
        sDolOffset = Read32(sBootHdr + 0x420);
        sFstOffset = Read32(sBootHdr + 0x424);
        sFstSize   = Read32(sBootHdr + 0x428);
        u32 fstMax = Read32(sBootHdr + 0x42C);
        if (fstMax == 0) fstMax = sFstSize;

        u32 arenaHi = OS_ARENA_HI;
        if (arenaHi < 0x80004000 || arenaHi > 0x81800000)
            arenaHi = 0x81700000;   // sane fallback near top of MEM1

        u32 fstAddr = (arenaHi - fstMax) & ~31u;

        OS_FST_ADDR    = fstAddr;
        OS_FST_MAXSIZE = fstMax;
        OS_ARENA_HI    = fstAddr;   // keep allocations below the FST

        *dst    = (void*)fstAddr;
        *size   = (int)((sFstSize + 31) & ~31u);
        *offset = (int)sFstOffset;

        // FST is data the runtime reads via the CPU later.
        if (sRegionCount < MAX_REGIONS)
        {
            sRegionAddr[sRegionCount] = fstAddr;
            sRegionSize[sRegionCount] = (sFstSize + 31) & ~31u;
            sRegionCount++;
        }

        sState = 2;
        return 1;
    }

    case 2:
    {
        // Load the 0x100-byte DOL header into scratch.
        *dst    = (void*)sDolHeader;
        *size   = 0x100;
        *offset = (int)sDolOffset;
        sState  = 3;
        return 1;
    }

    case 3:
    {
        // One DOL section per call. 18 sections total: 7 text + 11 data,
        // stored as contiguous arrays of file-offset / mem-addr / size.
        while (sDolIndex < 18)
        {
            int i = sDolIndex++;
            u32 secOff  = Read32(sDolHeader + 0x00 + i * 4);
            u32 secAddr = Read32(sDolHeader + 0x48 + i * 4);
            u32 secSize = Read32(sDolHeader + 0x90 + i * 4);

            if (secSize == 0 || secAddr == 0) continue;

            *dst    = (void*)secAddr;
            *size   = (int)((secSize + 31) & ~31u);
            *offset = (int)(sDolOffset + secOff);

            if (sRegionCount < MAX_REGIONS)
            {
                sRegionAddr[sRegionCount] = secAddr;
                sRegionSize[sRegionCount] = (secSize + 31) & ~31u;
                sRegionCount++;
            }
            return 1;
        }

        // All sections scheduled. Clear BSS.
        u32 bssAddr = Read32(sDolHeader + 0xD8);
        u32 bssSize = Read32(sDolHeader + 0xDC);
        if (bssAddr != 0 && bssSize != 0)
            ZeroMem(bssAddr, bssSize);

        // Make every region we loaded coherent for CPU execute/read.
        for (int r = 0; r < sRegionCount; ++r)
        {
            DCFlushRange(sRegionAddr[r], sRegionSize[r]);
            ICInvalidateRange(sRegionAddr[r], sRegionSize[r]);
        }

        sState = 4;
        return 0;
    }

    default:
        return 0;
    }
}

static void* app_close(void)
{
    // DOL entry point.
    return (void*)Read32(sDolHeader + 0xE0);
}

// The IPL calls this first (it is placed at 0x81200000 by the linker). It just
// hands back our three callbacks; the IPL drives the rest.
__attribute__((section(".text.entry")))
void app_entry(AppInit* init, AppMain* main, AppClose* close)
{
    *init  = app_init;
    *main  = app_main;
    *close = app_close;
}
