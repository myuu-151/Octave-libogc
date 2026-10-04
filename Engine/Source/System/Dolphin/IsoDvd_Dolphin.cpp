// -----------------------------------------------------------------------------
// Minimal physical-disc reader for the on-disc ISO transport, isolated in its own
// translation unit ON PURPOSE.
//
// It reads raw sectors off the GameCube drive by driving the DI (Drive Interface)
// hardware registers at 0xCC006000 directly -- it links NOTHING from libogc's DVD
// driver (dvd.o), which must never be linked into the game DOL: doing so makes the
// DOL unbootable via the disc apploader on real hardware.
//
// Why a separate file: the volatile MMIO and inline asm below, when compiled in the
// same translation unit as System_Dolphin.cpp's SD/FST code, perturbed the compiler's
// codegen enough to break GX rendering on an ordinary SD boot -- even though none of
// this runs on an SD boot (the DVD path is only taken on a real-disc boot). Kept here,
// System_Dolphin.cpp compiles exactly like the proven SD-only build.
//
// Requirements enforced by the DI DMA engine: disc offset 4-byte aligned (we use 32),
// destination 32-byte aligned, length a multiple of 32. After an apploader boot the
// drive is already spun up and the disc recognized (the IPL/Swiss read the apploader +
// FST off it), so normally no bring-up is needed; OctDvdMount() re-asserts it for a
// drive that idled.
// -----------------------------------------------------------------------------
#if PLATFORM_DOLPHIN

#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

#define DI_REG(n)   (*(volatile uint32_t*)(0xCC006000 + (n)))
#define DI_SR       DI_REG(0x00)   // status / interrupt
#define DI_CMDBUF0  DI_REG(0x08)   // command
#define DI_CMDBUF1  DI_REG(0x0C)   // disc offset >> 2
#define DI_CMDBUF2  DI_REG(0x10)   // length in bytes
#define DI_MAR      DI_REG(0x14)   // DMA memory address (physical)
#define DI_LENGTH   DI_REG(0x18)   // DMA length
#define DI_CR       DI_REG(0x1C)   // control: bit0 TSTART, bit1 DMA, bit2 RW(0=read)
#define DI_IMMBUF   DI_REG(0x20)   // immediate data buffer (command result)

// Bounded wait on the DI transfer-start bit; false on timeout so a wedged drive can't
// hang the machine.
static bool DiWait()
{
    uint32_t g = 0;
    while (DI_CR & 0x1) { if (++g > 0x20000000u) return false; }
    return true;
}

// Read the drive's last error/status word (0 == OK). Immediate command 0xE0000000 ->
// DIIMMBUF. Also serves to clear a latched error before a retry.
static uint32_t DiGetError()
{
    DI_CMDBUF0 = 0xE0000000;
    DI_IMMBUF  = 0;
    DI_CR      = 0x1;            // TSTART, immediate (no DMA)
    DiWait();
    return DI_IMMBUF;
}

// One attempt at reading alignedLen (mult. of 32) bytes from 32-byte-aligned disc
// `alignedOff` into the 32-byte-aligned buffer `dst`. Returns false on timeout/error.
static bool DiReadOnce(uint32_t alignedOff, void* dst, uint32_t alignedLen)
{
    uint8_t* p = (uint8_t*)dst;
    for (uint32_t i = 0; i < alignedLen; i += 32)
        __asm__ volatile ("dcbi 0,%0" :: "r"(p + i) : "memory");
    __asm__ volatile ("sync" ::: "memory");

    DI_SR      = DI_SR;                      // clear any pending interrupt bits
    DI_CMDBUF0 = 0xA8000000;                 // DVD read (sector) command
    DI_CMDBUF1 = alignedOff >> 2;            // disc offset in 4-byte units
    DI_CMDBUF2 = alignedLen;                 // transfer length in bytes
    DI_MAR     = (uint32_t)((uintptr_t)dst) & 0x1FFFFFFF;   // physical address
    DI_LENGTH  = alignedLen;
    DI_CR      = 0x3;                         // TSTART | DMA, RW=read

    if (!DiWait()) return false;             // bounded poll -> timeout
    if (DI_SR & 0x4) return false;           // DEINT (error-interrupt) latched
    return true;
}

static void DiSpinUp();   // defined below: unlock + spin the drive (up)

// Read with retries. Burned media often has marginal sectors that read on a second try,
// so re-attempt a few times, clearing the drive error between attempts. If the whole
// batch fails, re-assert the drive once (DiSpinUp) and try again -- covers a drive that
// idled/spun down mid-session. That's rare on GameCube (the drive stays spinning for the
// whole session), but this makes it self-heal instead of failing the asset read.
static bool DiReadSectors(uint32_t alignedOff, void* dst, uint32_t alignedLen)
{
    if (((uintptr_t)dst & 31) || (alignedOff & 31) || (alignedLen & 31)) return false;

    for (int pass = 0; pass < 2; ++pass)
    {
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            if (DiReadOnce(alignedOff, dst, alignedLen)) return true;
            DiGetError();   // read/clear the latched error, then retry
        }
        if (pass == 0) DiSpinUp();   // reads failed -> drive may have napped; wake it and retry
    }
    return false;
}

// ---- Exported transport entry points (called from System_Dolphin.cpp) ------

// 32-aligned fast path: caller guarantees alignedOff and alignedLen are 32-byte
// aligned and dst is a 32-byte-aligned buffer of at least alignedLen bytes.
bool OctDvdReadAligned(uint32_t alignedOff, void* dst, uint32_t alignedLen)
{
    return DiReadSectors(alignedOff, dst, alignedLen);
}

// Any-alignment read: bounce through an aligned scratch buffer (used for the small
// boot.bin/FST reads and misaligned asset offsets), a piece at a time. A scratch
// buffer the size of the whole read needed that much again in one piece: late in a
// long session a 499 KB file failed to load with 680 KB in one piece (the game's
// own 499 KB already taken), and the game retried it forever.
// The scratch buffer is a fixed one, not taken from the heap each read: a 64 KB block
// wanted for every read failed now and then with the heap full, and the read failed
// with it (a marathon's change of sky left frames half one sky). Every caller holds the
// disc lock (IsoReadRaw), so one buffer serves them all.
// Made at the first read from a disc (at boot, while memory is in one piece) and kept: a game that
// reads its image from the SD card never reads the disc, and doesn't keep these 64 KB.
static const uint32_t kBounceSize = 64 * 1024;
static uint8_t* sBounce = nullptr;

bool OctDvdRead(uint32_t offset, void* buf, uint32_t len)
{
    if (sBounce == nullptr)
    {
        sBounce = (uint8_t*)memalign(32, kBounceSize);
        if (sBounce == nullptr) return false;
    }
    const uint32_t kPiece = kBounceSize;
    uint32_t head    = offset & 31u;
    uint32_t scratch = (head + len + 31u) & ~31u;
    if (scratch > kPiece) scratch = kPiece;
    uint8_t* tmp = sBounce;
    uint8_t* out = (uint8_t*)buf;
    bool ok = true;
    while (ok && len > 0)
    {
        uint32_t alignedOff = offset & ~31u;
        head = offset - alignedOff;
        uint32_t n = scratch - head;         // this piece's bytes
        if (n > len) n = len;
        ok = DiReadSectors(alignedOff, tmp, (head + n + 31u) & ~31u);
        if (ok) memcpy(out, tmp + head, n);
        out += n;
        offset += n;
        len -= n;
    }
    return ok;
}

// Hand-rolled drive bring-up: unlock (retail debug backdoor) + enable the extended
// command set + spin the motor + read disc ID. Mirrors Swiss's dvd.c register sequence
// in our own MMIO. Bounded so a non-responsive drive fails fast. Does NOT hand-roll the
// hard reset (that pokes the system reset register 0xCC003024 -- a wrong value reboots
// the console, and Swiss already hard-resets at boot); the per-drive firmware patches
// that enable burned-media reads are likewise Swiss's job at boot and persist into here.
// Called at mount time (via OctDvdMount) and again by DiReadSectors if reads start
// failing mid-session (a napped drive). Safe to call repeatedly -- it just re-issues the
// unlock/spin commands; the unlock and firmware patch aren't lost by a spin-down.
static void DiSpinUp()
{
    // Unlock (MATSUSHITA / "DVD-GAME" magic).
    DI_SR |= 0x14; DI_REG(0x04) = 0;
    DI_CMDBUF0 = 0xFF014D41; DI_CMDBUF1 = 0x54534849; DI_CMDBUF2 = 0x54410200; DI_CR = 0x1; DiWait();
    DI_SR |= 0x14; DI_REG(0x04) = 0;
    DI_CMDBUF0 = 0xFF004456; DI_CMDBUF1 = 0x442D4741; DI_CMDBUF2 = 0x4D450300; DI_CR = 0x1; DiWait();

    // Enable the extended command set.
    DI_SR = 0x2E; DI_REG(0x04) = 0;
    DI_CMDBUF0 = 0x55010000; DI_CMDBUF1 = 0; DI_CMDBUF2 = 0; DI_CR = 0x1; DiWait();

    // Spin the motor (debug "start drive" + accept-copy: 0xFE110000 | 0x4100).
    DI_SR = 0x2E; DI_REG(0x04) = 1;
    DI_CMDBUF0 = 0xFE114100; DI_CMDBUF1 = 0; DI_CMDBUF2 = 0; DI_CR = 0x1; DiWait();

    // Set status.
    DI_SR = 0x2E; DI_REG(0x04) = 0;
    DI_CMDBUF0 = 0xEE060300; DI_CMDBUF1 = 0; DI_CMDBUF2 = 0; DI_CR = 0x1; DiWait();

    // Read the disc ID into our own aligned buffer (NOT 0x80000000 like Swiss -- that
    // would clobber low memory).
    static uint8_t idbuf[32] __attribute__((aligned(32)));
    for (uint32_t i = 0; i < 32; i += 32)
        __asm__ volatile ("dcbi 0,%0" :: "r"(idbuf + i) : "memory");
    __asm__ volatile ("sync" ::: "memory");
    DI_SR = 0x2E; DI_REG(0x04) = 0;
    DI_CMDBUF0 = 0xA8000040; DI_CMDBUF1 = 0; DI_CMDBUF2 = 0x20;
    DI_MAR = (uint32_t)((uintptr_t)idbuf) & 0x1FFFFFFF; DI_LENGTH = 0x20;
    DI_CR = 0x3; DiWait();
}

// Mount-time entry point: bring the drive up once. (The bring-up itself, DiSpinUp, is
// also re-invoked by DiReadSectors if the drive stops responding mid-session.)
void OctDvdMount()
{
    static bool mounted = false;
    if (mounted) return;
    mounted = true;
    DiSpinUp();
}

#endif // PLATFORM_DOLPHIN
