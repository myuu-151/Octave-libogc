#if PLATFORM_DOLPHIN

#include "System/System.h"

#include "Engine.h"
#include "Renderer.h"
#include "Log.h"
#include "Input/Input.h"
#include "Constants.h"
#include "InputDevices.h"

#include <gccore.h>
// NOTE: we deliberately do NOT #include <ogc/dvd.h>. Linking libogc's DVD driver
// (dvd.o) into the game DOL makes it unbootable via the disc apploader on real
// hardware -- the driver's startup/DI wiring faults under the IPL's bare boot
// environment (Swiss's direct-DOL loader and Dolphin mask it). So the DVD transport
// below reads the disc by poking the DI hardware registers directly (see DiRead),
// which links nothing from libogc and keeps the DOL apploader-bootable.
#include <ogc/semaphore.h>
#include <ogc/machine/processor.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <malloc.h>
#include <fat.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <ctype.h>
#include <stdarg.h>

#define ENABLE_LIBOGC_CONSOLE 0

#if PLATFORM_GAMECUBE
// SdGeckoDma.c: libogc's SD Gecko driver with DMA reads and a faster EXI clock.
extern "C" int OctSd_MountAll(void);
extern "C" const char* OctSd_GetModeName(int chan);
static int sSdChannel = -1;
#endif

static bool sFatInit = false;
// Every file read calls InitFAT(). With no card in (Dolphin, or a console booted from a disc)
// the mount fails -- and it used to be tried again on EVERY read. A game that streams assets
// does hundreds of reads: about 460 failed mounts in, the console froze. A few tries cover a
// card that is slow to come up; after that there is no card, and asking again will not make one.
static int32_t sFatAttempts = 0;
static const int32_t kMaxFatAttempts = 4;
static void InitFAT()
{
    if (!sFatInit && sFatAttempts < kMaxFatAttempts)
    {
        sFatAttempts++;
#if PLATFORM_GAMECUBE
        sSdChannel = OctSd_MountAll();
        if (sSdChannel >= 0)
        {
            LogDebug("FAT Initialized (EXI channel %d, mode %s).\n", sSdChannel, OctSd_GetModeName(sSdChannel));
            sFatInit = true;
            return;
        }
#endif
        if (fatInitDefault())
        {
            LogDebug("FAT Initialized Successfully.\n");
            sFatInit = true;
        }
        else
        {
            LogDebug("fatInitDefault() failure.\n");
        }
    }
}

// The video retrace count. This libogc has no VIDEO_GetRetraceCount; the post-retrace callback is
// handed the count. The engine's clock steps by it and the GX renderer waits on it for its flips.
static volatile uint32_t sRetraceCount = 0;
static void OnRetrace(u32 retraceCount) { sRetraceCount = retraceCount; }
uint32_t OctRetraceCount() { return sRetraceCount; }

void SYS_Initialize()
{
    EngineState& engine = *GetEngineState();
    SystemState& system = engine.mSystem;

    system.mFrameIndex = 0;

    VIDEO_Init();
    VIDEO_SetPostRetraceCallback(OnRetrace);
    GXRModeObj* rmode = VIDEO_GetPreferredMode(&system.mGxRmode);
    engine.mWindowWidth = rmode->fbWidth;
    engine.mWindowHeight = rmode->efbHeight;

#if PLATFORM_WII
    int32_t aspectRatio = CONF_GetAspectRatio();
    if (aspectRatio == CONF_ASPECT_16_9)
    {
        // On the Wii, if the TV is a 16:9 aspect ratio, then we still render
        // to the 640x480 framebuffer, but it will be stretched to fill the whole screen.
        // So save off an aspect ratio scale that we can use to adjust the camera's aspect ratio.
        engine.mAspectRatioScale = (16.0f / 9.0f) / (4.0f / 3.0f);
    }
#endif

    // allocate 2 framebuffers for double buffering
    system.mFrameBuffers[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    system.mFrameBuffers[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));

    VIDEO_Configure(&system.mGxRmode);
    VIDEO_SetNextFramebuffer(system.mFrameBuffers[system.mFrameIndex]);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    VIDEO_WaitVSync();
    engine.mAspectRatioScale = 1.0f;

#if ENABLE_LIBOGC_CONSOLE
    // Initialize the console, required for printf
    system.mConsoleBuffer = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));

    CON_Init(system.mConsoleBuffer, 0, 0, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);

    //printf("\x1b[2;0H");
#endif

    InitFAT();
}

void SYS_Shutdown()
{

}

void SYS_Update()
{
    GetEngineState()->mQuit = !SYS_MainLoop();
}

// Files
static bool IsoHasFile(const char* path);   // defined with the ISO reader below

bool SYS_DoesFileExist(const char* path, bool isAsset)
{
    // A bundled asset lives in the ISO's FST, not the SD's FAT -- so stat() alone
    // would report it missing and callers (e.g. ScriptUtils::RunScript) would bail
    // before ever reading it. Check the ISO first.
    if (isAsset && IsoHasFile(path))
    {
        return true;
    }

    struct stat info;
    bool exists = false;

    int32_t retStatus = stat(path, &info);

    if (retStatus == 0)
    {
        // If the file is actually a directory, than return false.
        exists = !(info.st_mode & S_IFDIR);
    }

    return exists;
}

// ---------------------------------------------------------------------------
// Version B on-disc streaming: read assets straight out of a single .iso file on
// the SD via fopen/fseek, parsing the GameCube disc FST in-engine. This sidesteps
// Swiss's disc emulation entirely (which libogc's low-memory clobber breaks) --
// it's ordinary SD file I/O, so it streams on demand (not RAM-limited), needs no
// USB Gecko, and is fully debuggable. Falls back to loose-file reads if no ISO is
// found, so existing SD-folder builds keep working.
// ---------------------------------------------------------------------------
struct IsoEntry { uint32_t offset; uint32_t size; };
static FILE* sIso = nullptr;
static int32_t sIsoAttempts = 0;
static std::unordered_map<std::string, IsoEntry> sIsoFiles;  // key: relpath, lowercase, '/'

static uint32_t IsoRead32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Diagnostic log of ISO asset streaming. The actual logger lives in a local,
// git-ignored header (IsoLog_local.h) so it never ships. If that file is present
// (a developer's machine), IsoLog writes /octiso.log; otherwise it's a no-op.
// A line for EVERY file read (H hit, L loose file, F failed). Off by default: a game that streams
// assets reads several files a second, and each line is a write to the same SD card.
#ifndef OCT_ISOLOG_FILES
#define OCT_ISOLOG_FILES 0
#endif

#if __has_include("IsoLog_local.h")
#include "IsoLog_local.h"
#else
static inline void IsoLog(const char*, ...) {}
#endif

// ---- Transport ------------------------------------------------------------
// The FST parser and everything above it are transport-agnostic; only IsoReadRaw
// differs -- it reads raw bytes at a disc offset from either the SD-hosted ISO file
// (fseek/fread) or the physical disc (DVD_ReadPrio). Delivery auto-selects: an SD
// ISO present => SD delivery; no SD ISO (booted from a real disc) => DVD delivery.
enum { ISO_NONE = 0, ISO_SD, ISO_DVD };
static int       sIsoMode = ISO_NONE;

// DVD-transport test flag (default 0 = normal SD-first auto-detect). Set to 1 to FORCE
// the physical-disc (DVD) transport, skipping SD detection, and enable mount tracing via
// the clobber-free IsoLog file logger (-> /octiso.log; no-op unless IsoLog_local.h is
// present). Used to validate the DI reader by booting the .iso as a disc in Dolphin (no
// SD card): the game reads its assets off the emulated drive via our DI reader, exercising
// the exact real-disc path. VERIFIED in Dolphin (mount OK, FST parsed, assets stream).
// NOTE: value of 1 will NOT run on an SD rig (no disc). Tracing goes to the SD file logger
// (no SYS_Report), so it no longer overwrites the RTC counter. Leave 0 for shipping builds.
#define OCT_FORCE_DVD 0

// Test switch: 1 = SD only. If no ISO is found on the SD, don't fall back to the disc
// drive -- for testing the SD path in Dolphin with an emulated SD adapter, where the
// hybrid build would otherwise pick the emulated disc. Leave 0 for shipping builds.
#define OCT_FORCE_SD 0

static bool IsoMounted() { return sIsoMode != ISO_NONE; }

// ---- DVD transport (physical disc) ----------------------------------------
// The raw DI-register disc reader lives in its OWN translation unit
// (IsoDvd_Dolphin.cpp). Keeping the volatile MMIO + inline asm (dcbi/sync, the DI
// register pokes) OUT of this file matters: when that code shared this translation
// unit, it perturbed the compiler's codegen enough to break GX rendering on an SD
// boot -- even though the DVD path is never taken there. With it in a separate TU,
// this file carries only plain SD/FST logic and compiles exactly like the proven
// SD-only build, while the DVD reader is still linked in for real-disc boots.
extern bool OctDvdRead(uint32_t offset, void* buf, uint32_t len);                   // any-alignment bounce read
extern bool OctDvdReadAligned(uint32_t alignedOff, void* dst, uint32_t alignedLen); // 32-aligned fast path
extern void OctDvdMount();                                                          // unlock + spin-up (no libogc)

// Read `len` bytes at disc `offset` into `buf` via the active transport: SD = plain
// fseek/fread on the ISO file; DVD = our DI reader in the separate TU.
// Asset loads (main thread + async load thread) and video streaming read at the same
// time, and the SD FILE* position and the DI reader are shared, so serialize reads.
static MutexObject* sIsoMutex = nullptr;

// Where the SD ISO file's read position is, so sequential reads can skip fseek.
static uint32_t sIsoFilePos = UINT32_MAX;

// stdio buffer for the SD ISO file: bigger chunks per FAT read for streaming.
static constexpr size_t kIsoFileBufferSize = 128 * 1024;
static char* sIsoFileBuffer = nullptr;

static MutexObject* GetIsoMutex()
{
    if (sIsoMutex == nullptr)
    {
        sIsoMutex = SYS_CreateMutex();
    }
    return sIsoMutex;
}

static bool IsoReadRaw(uint32_t offset, void* buf, uint32_t len)
{
    SCOPED_LOCK(GetIsoMutex());

    if (sIsoMode == ISO_SD)
    {
        // Sequential reads (e.g. video streaming) skip the seek: fseek discards stdio's
        // buffered data, and libfat may walk the file's cluster chain to find the offset.
        if (offset != sIsoFilePos && fseek(sIso, (long)offset, SEEK_SET) != 0)
        {
            sIsoFilePos = UINT32_MAX;
            return false;
        }

        const bool ok = fread(buf, 1, len, sIso) == len;
        sIsoFilePos = ok ? (offset + len) : UINT32_MAX;
        return ok;
    }
    if (sIsoMode == ISO_DVD)
    {
        return OctDvdRead(offset, buf, len);
    }
    return false;
}

// Read boot.bin -> FST -> path->{offset,size} map using the active transport.
static bool IsoParseFst()
{
    uint8_t hdr[0x440];
    if (!IsoReadRaw(0, hdr, sizeof(hdr))) {
#if OCT_FORCE_DVD
        IsoLog("ISO/DVD: boot.bin read FAILED (transport read returned false)\n");
#endif
        return false;
    }
#if OCT_FORCE_DVD
    IsoLog("ISO/DVD: boot.bin read OK, magic@0x1C=%08X (want C2339F3D), first8=%08X %08X\n",
               IsoRead32(hdr + 0x1C), IsoRead32(hdr + 0), IsoRead32(hdr + 4));
#endif
    if (IsoRead32(hdr + 0x1C) != 0xC2339F3D) return false;  // GC disc magic

    uint32_t fstOff  = IsoRead32(hdr + 0x424);
    uint32_t fstSize = IsoRead32(hdr + 0x428);
#if OCT_FORCE_DVD
    IsoLog("ISO/DVD: fstOff=%08X fstSize=%08X\n", fstOff, fstSize);
#endif
    if (fstOff == 0 || fstSize < 12) return false;

    std::vector<uint8_t> fst(fstSize);
    if (!IsoReadRaw(fstOff, fst.data(), fstSize)) {
#if OCT_FORCE_DVD
        IsoLog("ISO/DVD: FST read FAILED at off=%08X size=%08X\n", fstOff, fstSize);
#endif
        return false;
    }

    uint32_t numEntries = IsoRead32(&fst[8]);            // root entry length = entry count
#if OCT_FORCE_DVD
    IsoLog("ISO/DVD: numEntries=%u\n", numEntries);
#endif
    if ((uint64_t)numEntries * 12 > fstSize || numEntries == 0) return false;
    const char* strTable = (const char*)&fst[numEntries * 12];
    uint32_t strMax = fstSize - numEntries * 12;

    // Walk the FST, reconstructing full relative paths. Directory entries carry a
    // "next index" (one past their subtree); file entries carry disc offset + size.
    struct Frame { uint32_t end; size_t restoreLen; };
    std::vector<Frame> stack;
    std::string cur;                          // current dir prefix, lowercase, trailing '/'
    stack.push_back({ numEntries, 0 });       // root subtree

    sIsoFiles.clear();
    for (uint32_t i = 1; i < numEntries; ++i)
    {
        while (stack.size() > 1 && i >= stack.back().end)
        {
            cur.resize(stack.back().restoreLen);
            stack.pop_back();
        }

        const uint8_t* e = &fst[i * 12];
        uint8_t type = e[0];
        uint32_t nameOff = ((uint32_t)e[1] << 16) | ((uint32_t)e[2] << 8) | (uint32_t)e[3];

        std::string name;
        for (uint32_t k = nameOff; k < strMax && strTable[k] != '\0'; ++k)
            name += (char)tolower((unsigned char)strTable[k]);

        if (type == 1)   // directory
        {
            uint32_t next = IsoRead32(e + 8);
            stack.push_back({ next, cur.size() });
            cur += name;
            cur += '/';
        }
        else             // file
        {
            IsoEntry ent = { IsoRead32(e + 4), IsoRead32(e + 8) };
            sIsoFiles[cur + name] = ent;
        }
    }

    IsoLog("ISO MOUNTED (%s)  files=%u", sIsoMode == ISO_DVD ? "DVD" : "SD", (uint32_t)sIsoFiles.size());
#if PLATFORM_GAMECUBE
    // Which SD transfer mode the driver settled on: pio27, dma27, pio13.5, dma13.5, or "stock"
    // when libogc's own mount was used. LogDebug can't show it (SYS_Log is a no-op on console).
    IsoLog("SD: EXI channel %d, mode %s", sSdChannel, OctSd_GetModeName(sSdChannel));
#endif
    int32_t sample = 0;
    for (auto& kv : sIsoFiles) { IsoLog("  fst: %s (%u bytes)", kv.first.c_str(), kv.second.size); if (++sample >= 8) break; }
#if OCT_FORCE_DVD
    IsoLog("ISO/DVD: MOUNTED OK, %u files in FST\n", (uint32_t)sIsoFiles.size());
#endif
    return true;
}

// SD delivery: open the ISO file on the card and parse its FST.
static bool IsoOpenSD(const char* isoPath)
{
    FILE* f = fopen(isoPath, "rb");
    if (f == nullptr) return false;

    if (sIsoFileBuffer == nullptr)
    {
        sIsoFileBuffer = (char*)malloc(kIsoFileBufferSize);
    }
    if (sIsoFileBuffer != nullptr)
    {
        setvbuf(f, sIsoFileBuffer, _IOFBF, kIsoFileBufferSize);
    }

    sIso = f;
    sIsoFilePos = 0;
    sIsoMode = ISO_SD;
    if (IsoParseFst())
    {
        LogDebug("ISO mounted (SD): %s (%u files)", isoPath, (uint32_t)sIsoFiles.size());
        return true;
    }
    fclose(f); sIso = nullptr; sIsoMode = ISO_NONE;
    return false;
}

// DVD delivery: read the physical disc's FST directly via the DI registers. Only
// reached when no SD ISO was found -- i.e. a real-disc boot (disc present), not an
// empty-drive SD run. No DVD_Init/DVD_Mount: after an apploader boot the drive is
// already spun up and the disc recognized, and we link no libogc DVD driver (see the
// note at the top of the file). If the first read fails (empty drive / bad disc),
// we simply report not-mounted -- we never reset or poke a stopped drive.
static bool IsoOpenDVD()
{
    sIsoMode = ISO_DVD;
#if OCT_FORCE_DVD
    IsoLog("ISO/DVD: IsoOpenDVD() -- forcing physical-disc transport, parsing FST via DI\n");
#endif

    // Try reading straight away -- after a Swiss/IPL disc boot the drive is already
    // unlocked and spinning, so this usually succeeds with no bring-up at all.
    if (IsoParseFst()) { LogDebug("ISO mounted (DVD)"); return true; }

    // Otherwise bring the drive up ourselves (unlock + spin + read-ID) and retry. This
    // covers a drive that idled/spun down; it pokes only the DI command registers, never
    // libogc and never the system reset register.
    OctDvdMount();
    if (IsoParseFst()) { LogDebug("ISO mounted (DVD, after spin-up)"); return true; }

    sIsoMode = ISO_NONE;
    return false;
}

// Locate the disc image: SD first (an ISO on the card => SD delivery), else the
// physical disc (=> DVD delivery). The presence of an SD ISO IS the delivery signal.
static void IsoLocate()
{
    EngineState* es = GetEngineState();

#if OCT_FORCE_DVD
    // TEMP: skip SD entirely and read straight off the (emulated/physical) disc.
    if (!IsoMounted()) IsoOpenDVD();
    return;
#endif

    // 1) SD: known locations. (The argv[0] hint is DISABLED for now -- under Swiss's
    //    apploader/GCM boot argv is not reliably populated, and fopen()ing a stale/bogus
    //    argv[0] before video init was a candidate for the hard hang on real hardware.
    //    v1.3 booted reliably using only the candidate paths below, so match that.)
#if 0
    if (es->mArgC > 0 && es->mArgV != nullptr && es->mArgV[0] != nullptr && es->mArgV[0][0] != '\0')
    {
        IsoLog("ISO argv0: %s", es->mArgV[0]);
        if (IsoOpenSD(es->mArgV[0])) return;
    }
#endif

    const std::string& pn = es->mProjectName;
    const std::string& pd = es->mProjectDirectory;
    if (pn.empty()) return;   // too early (project name not set) -- retry on a later call

    std::string cands[] = {
        pd + pn + ".iso",
        pd + "../" + pn + ".iso",
        "/" + pn + ".iso",
        pn + ".iso",
    };
    for (const std::string& c : cands)
    {
        IsoLog("ISO try: %s", c.c_str());
        if (IsoOpenSD(c.c_str())) return;
    }

    // 1b) RENAMED: the image need not be called <project>.iso. Any .iso in the same places that
    //     holds THIS project -- its .octp at the root of the disc's file table -- is the one.
    //     (Named only, a renamed image was not found, the disc was tried instead, and on an SD rig
    //     with an empty drive the game sat on a green screen.)
    {
        std::string octp;
        for (char ch : pn + ".octp") octp += (char)tolower((unsigned char)ch);
        std::string dirs[] = { pd, pd + "../", "/", "./", "/games/", "sd:/", "sd:/games/" };
        for (const std::string& dir : dirs)
        {
            DIR* d = opendir(dir.empty() ? "." : dir.c_str());
            if (d == nullptr) continue;
            struct dirent* ent;
            while ((ent = readdir(d)) != nullptr)
            {
                std::string name = ent->d_name;
                if (name.size() < 5) continue;
                std::string ext;
                for (size_t k = name.size() - 4; k < name.size(); ++k) ext += (char)tolower((unsigned char)name[k]);
                if (ext != ".iso" && ext != ".gcm") continue;
                std::string path = dir + name;
                IsoLog("ISO scan: %s", path.c_str());
                if (!IsoOpenSD(path.c_str())) continue;
                if (sIsoFiles.count(octp) != 0)
                {
                    closedir(d);
                    return;
                }
                IsoLog("ISO scan: %s is not this game", path.c_str());     // another game's image
                fclose(sIso); sIso = nullptr; sIsoMode = ISO_NONE; sIsoFiles.clear();
            }
            closedir(d);
        }
    }

    // 2) No SD image found -> read the physical disc via the DI reader, NOW.
    //    We only reach here after the project name is set (checked above), which means
    //    LoadProject has run, which is after SYS_Initialize's fatInitDefault -- so the FAT
    //    is up and a failed SD open genuinely means there is no SD ISO, i.e. a real-disc
    //    (or Dolphin) boot. The DI reader links no libogc and is BOUNDED (it can't hang,
    //    even on an empty drive), unlike the old libogc DVD_Init/DVD_Mount that forced the
    //    old "wait 16 attempts" gate. Mount promptly: gating the disc mount behind many
    //    retries left the engine's default assets (meshes/fonts) unresolved when the
    //    renderer needed them -> null derefs / DSI crashes on a disc boot. On an SD rig the
    //    SD open above succeeds first, so we never touch the drive here.
#if OCT_FORCE_SD
    IsoLog("ISO: no SD image (OCT_FORCE_SD: not trying the disc)");
    return;
#endif
    IsoLog("ISO: no SD image -- reading the disc via the DI transport");
    IsoOpenDVD();
}

// Longest-suffix match: the FST stores paths relative to the packaged root, while
// the request path may carry an SD/project prefix. Find the FST entry whose
// relative path is the longest '/'-aligned tail of the request.
static bool IsoFind(const char* path, IsoEntry& out)
{
    if (sIsoFiles.empty()) return false;

    std::string p;
    for (const char* c = path; *c != '\0'; ++c)
        p += (*c == '\\') ? '/' : (char)tolower((unsigned char)*c);

    size_t pos = 0;
    for (;;)
    {
        auto it = sIsoFiles.find(p.substr(pos));
        if (it != sIsoFiles.end()) { out = it->second; return true; }
        size_t nx = p.find('/', pos);
        if (nx == std::string::npos)
            return false;
        pos = nx + 1;
    }
}

// Does the mounted ISO contain this asset? (Locates the ISO lazily.)
static bool IsoHasFile(const char* path)
{
    if (!IsoMounted() && sIsoAttempts < 16) { sIsoAttempts++; IsoLocate(); }
    if (!IsoMounted()) return false;
    IsoEntry ent;
    return IsoFind(path, ent);
}

// Directory enumeration over the ISO FST, so directory-scanning code (e.g. the
// Lua script discovery in ScriptUtils::LoadScriptDirectory) sees files bundled in
// the disc image, not just loose files on the SD. Lists the immediate children of
// whichever FST directory the request path resolves to (longest-suffix match).
struct IsoDirEnum { std::vector<std::pair<std::string, bool>> entries; size_t index; };

static bool IsoListDir(const char* dirPath, std::vector<std::pair<std::string, bool>>& out)
{
    if (sIsoFiles.empty()) return false;

    // lowercase + '/' + drop "." components + ensure trailing '/'
    std::string raw;
    for (const char* c = dirPath; *c != '\0'; ++c)
        raw += (*c == '\\') ? '/' : (char)tolower((unsigned char)*c);
    raw += '/';
    std::string p, comp;
    for (char c : raw)
    {
        if (c == '/') { if (!comp.empty() && comp != ".") { p += comp; p += '/'; } comp.clear(); }
        else comp += c;
    }

    // Longest suffix of p that is a prefix of some FST key = the matching ISO dir.
    std::string prefix;
    for (size_t pos = 0; pos <= p.size(); )
    {
        std::string cand = p.substr(pos);
        if (!cand.empty())
        {
            for (auto& kv : sIsoFiles)
                if (kv.first.size() > cand.size() && kv.first.compare(0, cand.size(), cand) == 0)
                { prefix = cand; break; }
        }
        if (!prefix.empty()) break;
        size_t nx = p.find('/', pos);
        if (nx == std::string::npos) break;
        pos = nx + 1;
    }
    if (prefix.empty()) return false;

    // Immediate children under the prefix (files + subdirs, de-duplicated).
    std::unordered_map<std::string, bool> seen;
    for (auto& kv : sIsoFiles)
    {
        const std::string& key = kv.first;
        if (key.size() <= prefix.size() || key.compare(0, prefix.size(), prefix) != 0) continue;
        std::string rest = key.substr(prefix.size());
        size_t slash = rest.find('/');
        if (slash == std::string::npos)
        {
            if (seen.emplace(rest, false).second) out.push_back({ rest, false });
        }
        else
        {
            std::string sub = rest.substr(0, slash);
            if (seen.emplace(sub + "/", true).second) out.push_back({ sub, true });
        }
    }
    return !out.empty();
}

void SYS_AcquireFileData(const char* path, bool isAsset, int32_t maxSize, char*& outData, uint32_t& outSize)
{
    // Need to init fat in case opening the Engine.ini in OctPreInitialize()
    InitFAT();

    outData = nullptr;
    outSize = 0;

    // On-disc streaming: serve assets from the ISO's FST if one is mounted (SD or DVD).
    if (isAsset)
    {
        if (!IsoMounted() && sIsoAttempts < 16)
        {
            sIsoAttempts++;
            IsoLocate();
        }

        IsoEntry ent;
        if (IsoMounted() && IsoFind(path, ent))
        {
            int32_t fileSize = (int32_t)ent.size;
            if (maxSize > 0)
            {
                fileSize = glm::min(fileSize, maxSize);
            }

#if OCT_ISOLOG_FILES
            {
                // The log writes to the SD: hold the file I/O lock, as other threads may be reading the card.
                SCOPED_LOCK(GetIsoMutex());
                IsoLog("H %s", path);
            }
#endif

            if (sIsoMode == ISO_DVD && (ent.offset & 31u) == 0)
            {
                // Aligned fast path: DMA straight off the disc into an aligned, length-
                // padded buffer (the packer 32-byte-aligns file data, so offsets align).
                uint32_t alignedLen = ((uint32_t)fileSize + 31u) & ~31u;
                outData = (char*)memalign(32, alignedLen);
                outSize = uint32_t(fileSize);
                bool read = false;
                if (outData != nullptr)
                {
                    // Serialize with IsoReadRaw: the DI reader is shared with other threads
                    // (async loads, video and game streams reading off the disc).
                    SCOPED_LOCK(GetIsoMutex());
                    read = OctDvdReadAligned(ent.offset, outData, alignedLen);
                }
                if (read)
                    return;
                if (outData != nullptr) { free(outData); outData = nullptr; outSize = 0; }
            }
            else
            {
                // SD (fseek/fread) or DVD with a misaligned offset (bounce via IsoReadRaw).
                outData = (char*)malloc(fileSize);
                outSize = uint32_t(fileSize);
                if (outData != nullptr && IsoReadRaw(ent.offset, outData, (uint32_t)fileSize))
                    return;
                if (outData != nullptr) { free(outData); outData = nullptr; outSize = 0; }
            }
        }
    }

    // Fall back to a loose file on the SD (under the file I/O lock, like every other SD access).
    SCOPED_LOCK(GetIsoMutex());
    FILE* file = fopen(path, "rb");

    if (file != nullptr)
    {
#if OCT_ISOLOG_FILES
        if (isAsset) IsoLog("L %s", path);
#endif

        int32_t fileSize = 0;
        fseek(file, 0, SEEK_END);
        fileSize = ftell(file);
        fseek(file, 0, SEEK_SET);

        if (maxSize > 0)
        {
            fileSize = glm::min(fileSize, maxSize);
        }

        outData = (char*)malloc(fileSize);
        outSize = uint32_t(fileSize);
        fread(outData, fileSize, 1, file);

        fclose(file);
        file = nullptr;
    }
    else
    {
        if (isAsset) IsoLog("F %s", path);      // a FAILED read is rare and always worth a line
        LogError("Failed to open file: %s", path);
    }
}

void SYS_ReleaseFileData(char* data)
{
    if (data != nullptr)
    {
        free(data);
    }
}

// Loose-file handle kept open between range reads (e.g. a video streaming from the
// SD), so each read doesn't pay for a FAT open. Guarded by the ISO mutex.
static FILE* sRangeFile = nullptr;
static std::string sRangePath;

// The size of an asset on the disc image, without reading it. False when it is not on one (a
// loose file on the SD): the caller then reads it whole, as before.
bool SYS_GetAssetFileSize(const char* path, uint32_t& outSize)
{
    if (!IsoMounted() && sIsoAttempts < 16)
    {
        sIsoAttempts++;
        IsoLocate();
    }

    IsoEntry ent;
    if (IsoMounted() && IsoFind(path, ent))
    {
        outSize = ent.size;
        return true;
    }

    return false;
}

bool SYS_ReadFileRange(const char* path, bool isAsset, uint32_t offset, uint32_t size, char* outData)
{
    if (outData == nullptr)
    {
        return false;
    }

    if (isAsset)
    {
        if (!IsoMounted() && sIsoAttempts < 16)
        {
            sIsoAttempts++;
            IsoLocate();
        }

        IsoEntry ent;
        if (IsoMounted() && IsoFind(path, ent))
        {
            if (uint64_t(offset) + size > ent.size)
            {
                return false;
            }

            return IsoReadRaw(ent.offset + offset, outData, size);
        }
    }

    SCOPED_LOCK(GetIsoMutex());

    if (sRangeFile == nullptr || sRangePath != path)
    {
        if (sRangeFile != nullptr)
        {
            fclose(sRangeFile);
        }

        sRangeFile = fopen(path, "rb");
        sRangePath = (sRangeFile != nullptr) ? path : "";
    }

    return sRangeFile != nullptr &&
           fseek(sRangeFile, long(offset), SEEK_SET) == 0 &&
           fread(outData, 1, size, sRangeFile) == size;
}

// Writes a line to the local SD diagnostic log (IsoLog -> /octiso.log) from outside
// this file, e.g. video playback stats. A no-op unless IsoLog_local.h is present.
//
// The line is QUEUED, and a thread below every other (priority 10) writes it -- under the ISO mutex,
// so the SD writes don't interleave with streaming reads. Written here, on the caller's thread, a
// line cost the game a frame: opening, appending to and closing a file on the SD card took 50-130 ms
// on hardware, every five seconds for the perf log, and showed as a hitch that only the log made.
// The thread runs whenever everything else waits (the main thread waits on the GPU and the retrace
// every frame), so lines reach the card within a frame or so; one written just before a hard crash
// can be lost. A full queue drops lines (and says how many) rather than wait.
#if __has_include("IsoLog_local.h")
namespace
{
    const uint32_t kLogLines = 32;
    char sLogQueue[kLogLines][512];
    volatile uint32_t sLogHead = 0;         // next to write to the card
    volatile uint32_t sLogTail = 0;         // next free
    volatile uint32_t sLogDropped = 0;
    sem_t sLogSem = LWP_SEM_NULL;
    lwp_t sLogThread = LWP_THREAD_NULL;
    bool sLogThreadTried = false;

    void* LogThread(void*)
    {
        while (true)
        {
            LWP_SemWait(sLogSem);
            while (sLogHead != sLogTail)
            {
                const uint32_t dropped = sLogDropped;
                {
                    SCOPED_LOCK(GetIsoMutex());
                    if (dropped != 0)
                    {
                        IsoLog("(%u log lines dropped: the queue was full)", dropped);
                    }
                    IsoLog("%s", sLogQueue[sLogHead % kLogLines]);
                }
                if (dropped != 0)
                {
                    sLogDropped -= dropped;
                }
                sLogHead = sLogHead + 1;
            }
        }
        return nullptr;
    }

    // 64 KB of stack: fopen/fwrite go through libfat and the SD driver (see SYS_CreateThread).
    bool StartLogThread()
    {
        if (!sLogThreadTried)
        {
            sLogThreadTried = true;
            if (LWP_SemInit(&sLogSem, 0, kLogLines + 1) != 0)
            {
                sLogSem = LWP_SEM_NULL;
            }
            else if (LWP_CreateThread(&sLogThread, LogThread, nullptr, nullptr, 64 * 1024, 10) != 0)
            {
                LWP_SemDestroy(sLogSem);
                sLogSem = LWP_SEM_NULL;
            }
        }
        return sLogSem != LWP_SEM_NULL;
    }
}
#endif

void OctLog(const char* format, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

#if __has_include("IsoLog_local.h")
    if (StartLogThread())
    {
        u32 level;
        _CPU_ISR_Disable(level);
        const bool full = (sLogTail - sLogHead) >= kLogLines;
        if (!full)
        {
            memcpy(sLogQueue[sLogTail % kLogLines], buffer, sizeof(buffer));
            sLogTail = sLogTail + 1;
        }
        else
        {
            sLogDropped = sLogDropped + 1;
        }
        _CPU_ISR_Restore(level);
        if (!full)
        {
            LWP_SemPost(sLogSem);
        }
        return;
    }
#endif

    SCOPED_LOCK(GetIsoMutex());
    IsoLog("%s", buffer);
}

// Serialize file I/O with the engine's ISO reads and log writes. For game code that reads the SD
// (e.g. the ISO through its own FILE handles) from another thread: the SD driver keeps shared
// per-card state, and overlapping use from two threads hangs it.
void OctLockFileIo()
{
    SYS_LockMutex(GetIsoMutex());
}

void OctUnlockFileIo()
{
    SYS_UnlockMutex(GetIsoMutex());
}

std::string SYS_GetCurrentDirectoryPath()
{
    char path[MAX_PATH_SIZE] = {};
    getcwd(path, MAX_PATH_SIZE);
    return std::string(path) + "/";
}

std::string SYS_GetAbsolutePath(const std::string& relativePath)
{
    std::string absPath;
    char* resolvedPath = realpath(relativePath.c_str(), nullptr);
    if (resolvedPath != nullptr)
    {
        absPath = resolvedPath;
        free(resolvedPath);
    }

    if (absPath != "" && DoesDirExist(absPath.c_str()))
        absPath += "/";

    return absPath;
}

void SYS_SetWorkingDirectory(const std::string& dirPath)
{
    chdir(dirPath.c_str());
}

bool SYS_CreateDirectory(const char* dirPath)
{
    return (mkdir(dirPath, 0777) == 0);
}

void SYS_RemoveDirectory(const char* dirPath)
{

}

void SYS_RemoveFile(const char* path)
{
    remove(path);
}

bool SYS_Rename(const char* oldPath, const char* newPath)
{
    return (rename(oldPath, newPath) == 0);
}

void SYS_OpenDirectory(const std::string& dirPath, DirEntry& outDirEntry)
{
    strncpy(outDirEntry.mDirectoryPath, dirPath.c_str(), MAX_PATH_SIZE);
    outDirEntry.mIsoEnum = nullptr;

    // Locate the ISO lazily -- a directory scan may be the first thing requested.
    if (!IsoMounted() && sIsoAttempts < 16) { sIsoAttempts++; IsoLocate(); }

    // Prefer the ISO's FST for directories that live inside the mounted disc image.
    if (IsoMounted())
    {
        IsoDirEnum* en = new IsoDirEnum();
        en->index = 0;
        if (IsoListDir(dirPath.c_str(), en->entries))
        {
            {
                SCOPED_LOCK(GetIsoMutex());
                IsoLog("ISO dir: %s -> %u entries", dirPath.c_str(), (uint32_t)en->entries.size());
            }
            outDirEntry.mIsoEnum = en;
            outDirEntry.mDir = nullptr;
            strncpy(outDirEntry.mFilename, en->entries[0].first.c_str(), MAX_PATH_SIZE);
            outDirEntry.mDirectory = en->entries[0].second;
            outDirEntry.mValid = true;
            return;
        }
        delete en;
    }

    outDirEntry.mDir = opendir(dirPath.c_str());
    if (outDirEntry.mDir == nullptr)
    {
        LogError("Could not open directory.");
        closedir(outDirEntry.mDir);
        return;
    }

    dirent* ent = readdir(outDirEntry.mDir);
    if (ent == nullptr)
    {
        outDirEntry.mValid = false;
    }
    else
    {
        memcpy(outDirEntry.mFilename, ent->d_name, MAX_PATH_SIZE);

        struct stat statbuf;
        std::string fullPath = dirPath + outDirEntry.mFilename;
        stat(fullPath.c_str(), &statbuf);

        outDirEntry.mDirectory = S_ISDIR(statbuf.st_mode);
        outDirEntry.mValid = true;
    }
}

void SYS_IterateDirectory(DirEntry& dirEntry)
{
    if (dirEntry.mIsoEnum != nullptr)
    {
        IsoDirEnum* en = (IsoDirEnum*)dirEntry.mIsoEnum;
        en->index++;
        if (en->index >= en->entries.size())
        {
            dirEntry.mValid = false;
        }
        else
        {
            strncpy(dirEntry.mFilename, en->entries[en->index].first.c_str(), MAX_PATH_SIZE);
            dirEntry.mDirectory = en->entries[en->index].second;
            dirEntry.mValid = true;
        }
        return;
    }

    dirent* ent = readdir(dirEntry.mDir);
    if (ent == nullptr)
    {
        dirEntry.mValid = false;
    }
    else
    {
        memcpy(dirEntry.mFilename, ent->d_name, MAX_PATH_SIZE);

        struct stat statbuf;
        std::string fullPath = std::string(dirEntry.mDirectoryPath) + dirEntry.mFilename;
        stat(fullPath.c_str(), &statbuf);

        dirEntry.mDirectory = S_ISDIR(statbuf.st_mode);
        dirEntry.mValid = true;
    }
}

void SYS_CloseDirectory(DirEntry& dirEntry)
{
    if (dirEntry.mIsoEnum != nullptr)
    {
        delete (IsoDirEnum*)dirEntry.mIsoEnum;
        dirEntry.mIsoEnum = nullptr;
        return;
    }

    closedir(dirEntry.mDir);
    dirEntry.mDir = nullptr;
}

std::vector<std::string> SYS_OpenFileDialog()
{
    return {};
}

std::string SYS_SaveFileDialog()
{
    return "";
}

std::string SYS_SelectFolderDialog()
{
    return "";
}

// Threads
ThreadObject* SYS_CreateThread(ThreadFuncFP func, void* arg)
{
    ThreadObject* retThread = new ThreadObject();

    int32_t createStatus = 0;

    createStatus = LWP_CreateThread(
        retThread,     /* thread handle */
        func,           /* code */
        arg,            /* arg pointer for thread */
        nullptr,        /* stack base */
        64 * 1024,      /* stack size: 16 KB overflowed on hardware once a thread read the SD card
                           (fread -> libfat -> SD driver); the async asset loader does exactly that */
        40              /* thread priority: BELOW the main thread's 64. SD and disc reads busy-wait,
                           so a loader at the main thread's priority held the CPU whenever the main
                           thread woke from vsync, and its frames waited on the loader's reads */);

    if (createStatus != 0)
    {
        LogError("Failed to create Thread");
    }

    return retThread;
}

void SYS_JoinThread(ThreadObject* thread)
{
    LWP_JoinThread(*thread, nullptr);
}

void SYS_DestroyThread(ThreadObject* thread)
{
    delete thread;
}

MutexObject* SYS_CreateMutex()
{
    MutexObject* retMutex = new MutexObject();

    // Pass true in second param to allow recursive locking
    int32_t status = LWP_MutexInit(retMutex, true);

    if (status != 0)
    {
        LogError("Failed to create Mutex");
    }

    return retMutex;
}
std::string SYS_GetOctavePath()
{
    return "";
}
std::string SYS_GetExecutablePath()
{
    return "";
}
void SYS_LockMutex(MutexObject* mutex)
{
    LWP_MutexLock(*mutex);
}

void SYS_UnlockMutex(MutexObject* mutex)
{
    LWP_MutexUnlock(*mutex);
}

void SYS_DestroyMutex(MutexObject* mutex)
{
    LWP_MutexDestroy(*mutex);
    delete mutex;
}

void SYS_Sleep(uint32_t milliseconds)
{
    usleep(milliseconds * 1000);
}

// Time
uint64_t SYS_GetTimeMicroseconds()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t microseconds = 1000000 * tv.tv_sec + tv.tv_usec;
    return microseconds;
}


std::string SYS_GetFileName(const std::string& relativePath)
{
 size_t slash = relativePath.find_last_of("/\\");
    size_t start = (slash == std::string::npos) ? 0 : slash + 1;

    // Strip extension (last '.' after the last slash)
    size_t dot = relativePath.find_last_of('.');
    if (dot == std::string::npos || dot < start) {
        dot = relativePath.size(); // no extension
    }

    return relativePath.substr(start, dot - start);

}
void SYS_CopyDirectory(const char* sourceDir, const char* destDir)
{

}

bool SYS_CopyDirectoryRecursive(const std::string& sourceDir,
                                const std::string& destDir)
{
    return false;
}

void SYS_CopyFile(const char* sourcePath, const char* destPath)
{

}

void SYS_MoveDirectory(const char* sourceDir, const char* destDir)
{

}

void SYS_MoveFile(const char* sourcePath, const char* destPath)
{

}

// Process
void SYS_Exec(const char* cmd, std::string* output)
{

}

// Memory
void* SYS_AlignedMalloc(uint32_t size, uint32_t alignment)
{
    return memalign(alignment, size);
}

void SYS_AlignedFree(void* pointer)
{
    OCT_ASSERT(pointer != nullptr);
    free(pointer);
}

std::vector<MemoryStat> SYS_GetMemoryStats()
{
    std::vector<MemoryStat> stats;

    uint64_t mainBytes = SYS_GetArena1Size();

#if PLATFORM_WII
    mainBytes += SYS_GetArena2Size();
#endif

    {
        MemoryStat stat;
        stat.mName = "Main";
        stat.mBytesFree = mainBytes;
        stat.mBytesAllocated = 0;
        stats.push_back(stat);
    }

    return stats;
}

static bool IsMemoryCardMounted()
{
    return GetEngineState()->mSystem.mMemoryCardMounted;
}

#if PLATFORM_GAMECUBE
static void UnmountMemoryCard(int32_t channel, int32_t result)
{
    LogWarning("Memory Card was removed from Slot %c", (channel == 0) ? 'A' : 'B');
    SYS_UnmountMemoryCard();
}

// The last CARD_Mount's result: why slot A is not mounted, when it is not.
static int32_t sCardMountResult = CARD_ERROR_NOCARD;

static void MountMemoryCard()
{
    if (!IsMemoryCardMounted())
    {
        // The work area is made once and kept. Mounting is tried again every time a save is
        // touched while no card is in, and a fresh 40 KB each try was never given back.
        if (GetEngineState()->mSystem.mMemoryCardMountArea == nullptr)
        {
            LogDebug("Initializing CARD");
            GetEngineState()->mSystem.mMemoryCardMountArea = SYS_AlignedMalloc(CARD_WORKAREA_SIZE, 32);
            CARD_Init("OCTA","00");
        }
        int errorSlotA = CARD_Mount(CARD_SLOTA, GetEngineState()->mSystem.mMemoryCardMountArea, UnmountMemoryCard);
        sCardMountResult = errorSlotA;
        LogDebug("Memory card code: %d", errorSlotA);

        if (errorSlotA >= 0)
        {
            GetEngineState()->mSystem.mMemoryCardMounted = true;
        }
        else
        {
            // A damaged or foreign card can be left half attached; let go of it, so the next
            // try (another card put in) starts clean.
            CARD_Unmount(CARD_SLOTA);
        }
    }
}

// What the memory card's own menu shows for a save: a 32-character title and a 32-character
// description, an icon and, if given, a 96 x 32 banner in CI8 (GX's 8 x 4 tiles) followed by its
// 256-colour RGB5A3 palette. The icon is either STILL -- 32 x 32 RGB5A3 (GX's 4 x 4 tiles,
// big-endian), 2048 bytes -- or ANIMATED: 1 to 8 frames of 32 x 32 CI8 (1024 bytes each) followed
// by the one RGB5A3 palette they share (512 bytes), played in a loop. Set by the game with
// System.SetSaveInfo; with no icon set, saves are written as they always were, bare.
#define SAVE_COMMENT_SIZE 64
#define SAVE_ICON_RGB_SIZE (CARD_ICON_W * CARD_ICON_H * 2)
#define SAVE_ICON_CI_SIZE (CARD_ICON_W * CARD_ICON_H)
#define SAVE_TLUT_SIZE (256 * 2)
#define SAVE_BANNER_SIZE (CARD_BANNER_W * CARD_BANNER_H + SAVE_TLUT_SIZE)
static char sSaveComment[SAVE_COMMENT_SIZE] = {};
static uint8_t sSaveIcon[CARD_MAXICONS * SAVE_ICON_CI_SIZE + SAVE_TLUT_SIZE] = {};
static uint32_t sSaveIconSize = 0;
static uint32_t sSaveIconFrames = 0;       // 0: one still RGB5A3 icon; n: n CI8 frames and a palette
static uint8_t sSaveBanner[SAVE_BANNER_SIZE] = {};
static bool sSaveInfoSet = false;
static bool sSaveBannerSet = false;

// iconFrames 0: icon is one RGB5A3 picture; 1-8: that many CI8 frames, then their palette.
void SYS_SetSaveInfo(const char* title, const char* description, const uint8_t* icon, uint32_t iconFrames, const uint8_t* bannerCI8)
{
    memset(sSaveComment, 0, sizeof(sSaveComment));
    strncpy(sSaveComment, title, 31);
    strncpy(sSaveComment + 32, description, 31);
    sSaveIconFrames = (iconFrames <= CARD_MAXICONS) ? iconFrames : CARD_MAXICONS;
    sSaveIconSize = (sSaveIconFrames == 0) ? SAVE_ICON_RGB_SIZE : sSaveIconFrames * SAVE_ICON_CI_SIZE + SAVE_TLUT_SIZE;
    memcpy(sSaveIcon, icon, sSaveIconSize);
    sSaveBannerSet = (bannerCI8 != nullptr);
    if (sSaveBannerSet)
    {
        memcpy(sSaveBanner, bannerCI8, SAVE_BANNER_SIZE);
    }
    sSaveInfoSet = true;
}

// A save's file, in whole blocks. With save info: the comment at 0, the pictures from 64 -- the
// banner and its palette first, if there is one, then the icon's frames (and their palette), as
// the card's menu reads them -- and the data after them. The pictures must start in the file's
// first 512 bytes (libogc refuses the status otherwise). Without save info: the data at 0, as
// saves always were. Reading works out where the data starts from the file's own picture formats
// (SaveDataOffset), so a save with no icon, a still one or an animated one all read back.
static uint32_t SaveInfoSize()
{
    if (!sSaveInfoSet)
    {
        return 0;
    }
    return SAVE_COMMENT_SIZE + (sSaveBannerSet ? SAVE_BANNER_SIZE : 0) + sSaveIconSize;
}

static uint32_t SaveDataOffset(int32_t fileNo)
{
    card_stat stat;
    if (CARD_GetStatus(CARD_SLOTA, fileNo, &stat) < 0 ||
        stat.icon_addr != SAVE_COMMENT_SIZE || stat.comment_addr != 0)
    {
        return 0;                               // a bare save: the data from the start
    }
    uint32_t at = SAVE_COMMENT_SIZE;
    const uint32_t bannerFmt = CARD_GetBannerFmt(&stat);
    if (bannerFmt == CARD_BANNER_CI) at += SAVE_BANNER_SIZE;
    else if (bannerFmt == CARD_BANNER_RGB) at += CARD_BANNER_W * CARD_BANNER_H * 2;
    bool sharedTlut = false;
    for (uint32_t n = 0; n < CARD_MAXICONS; ++n)
    {
        if (((stat.icon_speed >> (2 * n)) & CARD_SPEED_MASK) == CARD_SPEED_END)
        {
            break;                              // the animation's last frame was the one before
        }
        const uint32_t fmt = CARD_GetIconFmt(&stat, n);
        if (fmt == CARD_ICON_RGB) at += SAVE_ICON_RGB_SIZE;
        else if (fmt == CARD_ICON_CI) { at += SAVE_ICON_CI_SIZE; sharedTlut = true; }
    }
    if (sharedTlut) at += SAVE_TLUT_SIZE;
    return at;
}

static uint32_t SaveFileSize(uint32_t dataBytes, uint32_t sectorSize)
{
    uint32_t bytes = dataBytes + SaveInfoSize();
    return ((bytes + sectorSize - 1) / sectorSize) * sectorSize;
}

static const char* CardErrorName(int32_t error)
{
    switch (error)
    {
    case CARD_ERROR_NOCARD: return "nocard";
    case CARD_ERROR_WRONGDEVICE: return "wrongdevice";
    case CARD_ERROR_BROKEN: return "damaged";
    case CARD_ERROR_ENCODING: return "encoding";
    case CARD_ERROR_BUSY: return "busy";
    default: return "error";
    }
}

// Whether a save of dataBytes can go on the card in slot A, before it is written.
//   "exists"  a save of that name is there and will be written over
//   "ready"   there is room for it, a new file of blocksNeeded blocks
//   "full"    there is not: too few free blocks, or all 127 files used
//   "nocard", "wrongdevice", "damaged" (unformatted or corrupt), "encoding" (a card of the
//   other region's), "busy", "error": why the card cannot be used at all
const char* SYS_GetSaveCardState(const char* saveName, uint32_t dataBytes, int32_t& blocksNeeded, int32_t& blocksFree)
{
    blocksNeeded = 0;
    blocksFree = 0;
    MountMemoryCard();
    if (!IsMemoryCardMounted())
    {
        return CardErrorName(sCardMountResult);
    }

    uint32_t sectorSize = 8192;
    CARD_GetSectorSize(CARD_SLOTA, &sectorSize);
    uint32_t fileSize = SaveFileSize(dataBytes, sectorSize);
    blocksNeeded = (int32_t)(fileSize / sectorSize);

    u16 freeBlocks = 0;
    int32_t err = CARD_GetFreeBlocks(CARD_SLOTA, &freeBlocks);
    if (err < 0)
    {
        return CardErrorName(err);
    }
    blocksFree = freeBlocks;

    card_file cardFile;
    if (CARD_Open(CARD_SLOTA, saveName, &cardFile) >= 0)
    {
        uint32_t have = cardFile.len;
        CARD_Close(&cardFile);
        // Written over in place; a save that has outgrown its file is made again, and that
        // needs the difference free.
        if (have >= fileSize || (int32_t)((fileSize - have) / sectorSize) <= blocksFree)
        {
            return "exists";
        }
        return "full";
    }

    int32_t files = 0;
    card_dir dir;
    for (int32_t r = CARD_FindFirst(CARD_SLOTA, &dir, true); r >= 0; r = CARD_FindNext(&dir))
    {
        files++;
    }
    if (files >= CARD_MAXFILES || blocksNeeded > blocksFree)
    {
        return "full";
    }
    return "ready";
}
#endif

bool SYS_ReadSave(const char* saveName, Stream& outStream)
{
    // This needs to be different between GameCube and Wii, since GameCube uses memory cards and Wii uses SD cards.
    bool success = false;

#if PLATFORM_WII
    if (GetEngineState()->mProjectDirectory != "")
    {
        if (SYS_DoesSaveExist(saveName))
        {
            std::string savePath = GetEngineState()->mProjectDirectory + "Saves/" + saveName;
            outStream.ReadFile(savePath.c_str(), false);
            success = true;
        }
        else
        {
            LogError("Failed to read save.");
        }
    }
    else
    {
        LogError("Failed to read save. Project directory is unset.");
    }
#else

    LogDebug("READ SAVE");
    MountMemoryCard();

    if (IsMemoryCardMounted())
    {
        uint32_t sectorSize = 0;
        CARD_GetSectorSize(CARD_SLOTA, &sectorSize);

        card_file cardFile;
        int32_t cardError = CARD_Open(CARD_SLOTA, saveName, &cardFile);

        if (cardError >= 0)
        {
            int32_t fileSize = ((cardFile.len + sectorSize - 1) / sectorSize) * sectorSize;
            uint32_t dataAt = SaveDataOffset(cardFile.filenum);

            char* cardBuffer = (char*)SYS_AlignedMalloc(fileSize, 32);
            cardError = CARD_Read(&cardFile, cardBuffer, fileSize, 0);
            success = (cardError >= 0);

            outStream.SetPos(0);
            if (success)
            {
                outStream.WriteBytes((uint8_t*) cardBuffer + dataAt, fileSize - dataAt);
            }
            outStream.SetPos(0);
            SYS_AlignedFree(cardBuffer);

            CARD_Close(&cardFile);
        }
    }
#endif

    return success;
}

bool SYS_WriteSave(const char* saveName, Stream& stream)
{
    bool success = false;
#if PLATFORM_WII
    
    if (GetEngineState()->mProjectDirectory != "")
    {
        std::string saveDir = GetEngineState()->mProjectDirectory + "Saves";

        // In the embedded case, might need to create a Project directory
        if (!DoesDirExist(GetEngineState()->mProjectDirectory.c_str()))
        {
            SYS_CreateDirectory(GetEngineState()->mProjectDirectory.c_str());
        }
        
        bool saveDirExists = DoesDirExist(saveDir.c_str());

        if (!saveDirExists)
        {
            saveDirExists = SYS_CreateDirectory(saveDir.c_str());
        }

        if (saveDirExists)
        {
            std::string savePath = saveDir + "/" + saveName;
            stream.WriteFile(savePath.c_str());
            success = true;
            LogDebug("Save written: %s (%d bytes)", saveName, stream.GetSize());
        }
        else
        {
            LogError("Failed to open Saves directory");
        }
    }
    else
    {
        LogError("Failed to write save");
    }
#else

    LogDebug("WRITE SAVE");
    MountMemoryCard();

    if (IsMemoryCardMounted())
    {
        uint32_t sectorSize = 0;
        CARD_GetSectorSize(CARD_SLOTA, &sectorSize);

        int32_t fileSize = (int32_t)SaveFileSize(stream.GetSize(), sectorSize);

        card_file cardFile;
        int32_t cardError = CARD_Open(CARD_SLOTA, saveName, &cardFile);

        if (cardError >= 0 && (int32_t)cardFile.len < fileSize)
        {
            // Outgrown (an old save with no room for the icon, or a one-block save now that the
            // icon is animated): made again, bigger -- but only if the card has the room. Deleted
            // first and then not made, the save would be lost; left alone, it keeps what it had.
            const int32_t have = (int32_t)cardFile.len;
            CARD_Close(&cardFile);
            u16 freeBlocks = 0;
            const int32_t more = (int32_t)((fileSize - have + (int32_t)sectorSize - 1) / (int32_t)sectorSize);
            if (CARD_GetFreeBlocks(CARD_SLOTA, &freeBlocks) < 0 || (int32_t)freeBlocks < more)
            {
                LogError("Save %s needs %d more block(s) on the memory card; left as it was", saveName, (int)more);
                return false;
            }
            CARD_Delete(CARD_SLOTA, saveName);
            cardError = CARD_ERROR_NOFILE;
        }

        if (cardError < 0)
        {
            // File not found. Create it.
            cardError = CARD_Create(CARD_SLOTA, saveName, fileSize, &cardFile);

            if (cardError < 0)
            {
                LogError("Failed to create save data on memory card. Error code = %d", cardError);
            }
        }

        if (cardError >= 0)
        {
            char* cardBuffer = (char*)SYS_AlignedMalloc(fileSize, 32);

            //LogDebug("fileSize = %d, cardFile.len = %d, stream.GetSize() = %d", fileSize, cardFile.len, stream.GetSize());
            //OCT_ASSERT(fileSize == cardFile.len);
            OCT_ASSERT(fileSize >= (int32_t)(stream.GetSize() + SaveInfoSize()));
            memset(cardBuffer, 0, fileSize);
            uint32_t commentAt = 0;
            uint32_t picturesAt = SAVE_COMMENT_SIZE;
            uint32_t bannerAt = picturesAt;
            uint32_t iconAt = picturesAt + (sSaveBannerSet ? SAVE_BANNER_SIZE : 0);
            uint32_t dataAt = SaveInfoSize();
            if (sSaveInfoSet)
            {
                memcpy(cardBuffer + commentAt, sSaveComment, SAVE_COMMENT_SIZE);
                if (sSaveBannerSet)
                {
                    memcpy(cardBuffer + bannerAt, sSaveBanner, SAVE_BANNER_SIZE);
                }
                memcpy(cardBuffer + iconAt, sSaveIcon, sSaveIconSize);
            }
            memcpy(cardBuffer + dataAt, stream.GetData(), stream.GetSize());

            // A block at a time: given more than one, libogc's CARD_Write writes the first and
            // reports success (measured in Dolphin: a two-block save came back with its second
            // block blank).
            for (int32_t at = 0; at < fileSize && cardError >= 0; at += (int32_t)sectorSize)
            {
                cardError = CARD_Write(&cardFile, cardBuffer + at, sectorSize, at);
            }
            success = (cardError >= 0);

            if (cardError < 0)
            {
                LogError("Failed to write save to memory card. Error code = %d", cardError);
            }
            else if (sSaveInfoSet)
            {
                // Where the card's menu finds the name and the pictures: the banner (CI8, its
                // palette right after it) if there is one, then the icon -- one still RGB5A3
                // picture, or its CI8 frames and their shared palette, each frame shown for 12
                // retraces (CARD_SPEED_SLOW) in a loop. The card finds them all from the one
                // address. (libogc's CARD_SetIconSpeed macro reads icon_fmt; set it directly.)
                card_stat stat;
                if (CARD_GetStatus(CARD_SLOTA, cardFile.filenum, &stat) >= 0)
                {
                    stat.banner_fmt = (sSaveBannerSet ? CARD_BANNER_CI : CARD_BANNER_NONE) | CARD_ANIM_LOOP;
                    stat.icon_addr = picturesAt;
                    stat.icon_fmt = 0;
                    stat.icon_speed = 0;
                    const uint32_t frames = (sSaveIconFrames == 0) ? 1 : sSaveIconFrames;
                    for (uint32_t n = 0; n < frames; ++n)
                    {
                        const uint32_t fmt = (sSaveIconFrames == 0) ? CARD_ICON_RGB : CARD_ICON_CI;
                        stat.icon_fmt |= (u16)(fmt << (2 * n));
                        stat.icon_speed |= (u16)(CARD_SPEED_SLOW << (2 * n));
                    }
                    stat.comment_addr = commentAt;
                    int32_t statError = CARD_SetStatus(CARD_SLOTA, cardFile.filenum, &stat);
                    if (statError < 0)
                    {
                        LogError("Failed to set the save's icon. Error code = %d", statError);
                    }
                }
            }

            SYS_AlignedFree(cardBuffer);
            CARD_Close(&cardFile);
        }
    }
#endif

    return success;
}

bool SYS_DoesSaveExist(const char* saveName)
{
    bool exists = false;

#if PLATFORM_WII
    if (GetEngineState()->mProjectDirectory != "")
    {
        std::string savePath = GetEngineState()->mProjectDirectory + "Saves/" + saveName;

        FILE* file = fopen(savePath.c_str(), "rb");

        if (file != nullptr)
        {
            exists = true;
            fclose(file);
            file = nullptr;
        }
    }
#else

    LogDebug("CHECK SAVE");
    MountMemoryCard();

    if (IsMemoryCardMounted())
    {
        card_file cardFile;
        int32_t cardError = CARD_Open(CARD_SLOTA, saveName, &cardFile);

        if (cardError >= 0)
        {
            exists = true;
            CARD_Close(&cardFile);
        }
    }
#endif

    return exists;
}

bool SYS_DeleteSave(const char* saveName)
{
    bool success = false;

#if PLATFORM_WII
    if (GetEngineState()->mProjectDirectory != "")
    {
        std::string savePath = GetEngineState()->mProjectDirectory + "Saves/" + saveName;
        SYS_RemoveFile(savePath.c_str());
        success = true;
    }
#else
    LogDebug("DELETE SAVE");
    MountMemoryCard();

    if (IsMemoryCardMounted())
    {
        int32_t cardError = CARD_Delete(CARD_SLOTA, saveName);

        if (cardError >= 0)
        {
            success = true;
        }
    }
#endif

    return success;
}

void SYS_UnmountMemoryCard()
{
    LogDebug("Unmounting Memory Card");
    if (IsMemoryCardMounted())
    {
        CARD_Unmount(CARD_SLOTA);
        GetEngineState()->mSystem.mMemoryCardMounted = false;
        // The work area is kept for the next mount (see MountMemoryCard). This also runs from
        // the card's detach callback, an interrupt, where freeing memory was never safe.
    }
}

// Clipboard
void SYS_SetClipboardText(const std::string& str)
{

}

std::string SYS_GetClipboardText()
{
    return "";
}

// Misc
// 1 = route engine logging to OSReport so it shows in Dolphin's log. KEEP AT 0 for anything
// that runs on hardware: OSReport overwrites the RTC counter (see SYS_Log below).
#ifndef OCT_DOLPHIN_EMU_LOG
#define OCT_DOLPHIN_EMU_LOG 0
#endif

void SYS_Log(LogSeverity severity, const char* format, va_list arg)
{
    // NOTE: do NOT route logging through SYS_Report() on console. Per Extrems, libogc's
    // debug-output (OSReport/SYS_Report) path OVERWRITES THE RTC COUNTER on every call --
    // a low-memory timekeeping field. To be precise about the mechanism: SYS_Report
    // clobbers the RTC counter, NOT the OS low-memory globals in general -- corruption of
    // the broader OS globals is a separate libogc issue, not this debug path. Either way,
    // overwriting the RTC counter is real low-mem state corruption, so logging is a no-op
    // on console; for ISO/asset tracing use the local git-ignored file logger
    // (IsoLog_local.h -> /octiso.log), which touches neither the RTC counter nor the OS globals.
#if OCT_DOLPHIN_EMU_LOG
    // DOLPHIN ONLY, for a diagnostic build: never ship this to a console (see above).
    (void)severity;
    char line[512];
    vsnprintf(line, sizeof(line), format, arg);
    SYS_Report("%s\n", line);
#else
    (void)severity; (void)format; (void)arg;
#endif
}

void SYS_Assert(const char* exprString, const char* fileString, uint32_t lineNumber)
{
    const char* fileName = strrchr(fileString, '/') ? strrchr(fileString, '/') + 1 : fileString;
    char str[256];
    snprintf(str, 256, "[Assert] %s, %s, line %d", exprString, fileName, lineNumber);

    SYS_Alert(str);
}

void SYS_Alert(const char* message)
{
    // Display alert message in console view and wait for player to hit A button.
    LogError("%s", message);

#if ENABLE_LIBOGC_CONSOLE
    EnableConsole(true);
    SYS_Sleep(500);
    INP_Update();
    while (!IsGamepadButtonJustDown(GAMEPAD_A, 0))
    {
        SYS_Sleep(5);
        INP_Update();
    }

    // Add some small feedback to show that 
    // user is attempting to proceed.
    LogError(">>>");
    SYS_Sleep(100);

    EnableConsole(false);
#endif
}

void SYS_UpdateConsole()
{
#if ENABLE_LIBOGC_CONSOLE
    SystemState& system = GetEngineState()->mSystem;
    void* fb = GetEngineState()->mConsoleMode ? system.mConsoleBuffer : system.mFrameBuffers[system.mFrameIndex];

    VIDEO_SetNextFramebuffer(fb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
#endif
}

int32_t SYS_GetPlatformTier()
{
    return 1;
}

void SYS_SetWindowTitle(const char* title)
{
    
}

bool SYS_DoesWindowHaveFocus()
{
    return true;
}

void SYS_SetScreenOrientation(ScreenOrientation orientation)
{

}

ScreenOrientation SYS_GetScreenOrientation()
{
    return ScreenOrientation::Landscape;
}

void SYS_SetFullscreen(bool fullscreen)
{

}

bool SYS_IsFullscreen()
{
    return true;
}

void SYS_ExplorerOpenDirectory(const std::string& dirPath)
{
}

#endif