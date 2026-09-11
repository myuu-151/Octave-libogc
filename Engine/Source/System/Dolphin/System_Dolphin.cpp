#if PLATFORM_DOLPHIN

#include "System/System.h"

#include "Engine.h"
#include "Renderer.h"
#include "Log.h"
#include "Input/Input.h"
#include "Constants.h"
#include "InputDevices.h"

#include <gccore.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <malloc.h>
#include <fat.h>
#include <string>
#include <string.h>
#include <stdarg.h>

#define ENABLE_LIBOGC_CONSOLE 0

// DIAGNOSTIC: append one line to a log file on the SD root (the default FAT
// device). Opens+closes every call so the log survives a later crash -- boot,
// let it fail, pull the SD, read octlog.txt on a PC.
static void DvdLog(const char* fmt, ...)
{
    FILE* f = fopen("octlog.txt", "a");
    if (f == nullptr) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static bool sFatInit = false;
static void InitFAT()
{
    if (!sFatInit)
    {
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

static void SYS_ShowDvdDiag();   // DIAGNOSTIC: paint FST-mount result as a screen color

void SYS_Initialize()
{
    EngineState& engine = *GetEngineState();
    SystemState& system = engine.mSystem;

    system.mFrameIndex = 0;

    VIDEO_Init();
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

    // Clear both framebuffers to black up front. A freshly allocated XFB is
    // uninitialized, and all-zero YUV shows as green -- without this you get a
    // green flash before the first frame is rendered.
    VIDEO_ClearFrameBuffer(rmode, system.mFrameBuffers[0], COLOR_BLACK);
    VIDEO_ClearFrameBuffer(rmode, system.mFrameBuffers[1], COLOR_BLACK);

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

    SYS_ShowDvdDiag();   // DIAGNOSTIC: hold a status color on-screen (~3s) before boot
}

void SYS_Shutdown()
{

}

void SYS_Update()
{
    GetEngineState()->mQuit = !SYS_MainLoop();
}

// ---------------------------------------------------------------------------
// Disc (GCM/ISO) asset loading
//
// When the game boots from a real GameCube disc -- or a GCM in Dolphin/Swiss --
// there is no SD card, and the cooked assets live inside the disc filesystem.
// The apploader loads the disc's FST (File String Table) into main memory and
// stores its address/size in the OS low-memory globals at 0x80000038/0x8000003C.
// We copy that FST into our own buffer (the apploader's copy may sit in memory
// the engine later allocates over), parse it to map an asset's relative path to
// its absolute disc offset+length, and read it with DVD_ReadPrio.
//
// This is only used as a fallback: fopen() (libfat / SD) is always tried first,
// so the existing SD-root layout and Dolphin's emulated SD keep working. Only a
// real disc boot -- where fopen fails -- falls through to the FST reader.
// ---------------------------------------------------------------------------

static bool        sDvdActive = false;   // a valid boot FST was found
static volatile int sDvdDiag  = 0;       // DIAGNOSTIC: how far InitDVD got (screen color)
static bool        sDvdVideoReady = false; // DIAGNOSTIC: gate DVD access until video is up
static u8*         sFst       = nullptr; // our owned copy of the FST
static const char* sFstStr    = nullptr; // string table within sFst
static u32         sFstCount  = 0;       // number of FST entries
static dvdcmdblk   sDvdBlk;

static bool FstIsDir(const u8* e)          { return e[0] != 0; }
static u32  FstNameOff(const u8* e)        { return (u32(e[1]) << 16) | (u32(e[2]) << 8) | u32(e[3]); }
static u32  FstU32(const u8* e, int at)
{
    const u8* p = e + at;
    return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}

static bool StrEqCI(const char* a, const char* b)
{
    while (*a && *b)
    {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == 0 && *b == 0;
}

static int sDvdAttempts = 0;

static void InitDVD()
{
    // DIAGNOSTIC: never touch the drive before video is up, so a fault here is
    // visible as a frozen screen color instead of a pre-video "no signal".
    if (!sDvdVideoReady) return;
    if (sDvdActive) return;             // already mounted
    if (sDvdAttempts >= 32) return;     // give up (e.g. SD-only system, no disc)
    sDvdAttempts++;

    DvdLog("InitDVD: attempt %d", sDvdAttempts);

    // Set up the DVD driver. This alone does NOT reset/spin the physical drive.
    DVD_Init();
    DvdLog("  DVD_Init() returned");

    // Read the disc boot header (boot.bin) to locate the FST. Try the absolute
    // read WITHOUT mounting first: under Swiss's DI emulation the disc is already
    // "present", so this read is intercepted -- no drive reset, no real laser
    // seek. On Dolphin this first read returns -1 and we fall back to DVD_Mount()
    // (which resets the drive) only then. This avoids the reset that was leaking
    // to the real drive under Swiss and faulting on hardware.
    u8* boot = (u8*)memalign(32, 0x440);
    if (boot == nullptr) { DvdLog("  boot memalign FAILED"); sDvdDiag = 5; return; }

    bool usedMount = false;
    s32 rc = DVD_ReadPrio(&sDvdBlk, boot, 0x440, 0, 2);
    DvdLog("  read boot.bin (no mount): rc=%ld", (long)rc);
    if (rc < 0)
    {
        DvdLog("  first read failed -> DVD_Mount()");
        DVD_Mount();        // drive reset + disc-ID read (needed on Dolphin)
        usedMount = true;
        rc = DVD_ReadPrio(&sDvdBlk, boot, 0x440, 0, 2);
        DvdLog("  read boot.bin (after mount): rc=%ld", (long)rc);
        if (rc < 0)
        {
            free(boot);
            sDvdDiag = 1;   // RED: read failed even after mount
            return;
        }
    }

    DvdLog("  boot[0..3]=%02X %02X %02X %02X  magic@1C=%02X%02X%02X%02X",
           boot[0], boot[1], boot[2], boot[3],
           boot[0x1C], boot[0x1D], boot[0x1E], boot[0x1F]);

    u32 fstOffset = FstU32(boot, 0x424);
    u32 fstSize   = FstU32(boot, 0x428);
    free(boot);
    DvdLog("  fstOffset=0x%08lX fstSize=0x%08lX", (unsigned long)fstOffset, (unsigned long)fstSize);

    if (fstOffset < 0x440 || fstSize < 12)
    {
        sDvdDiag = 4;   // WHITE: read succeeded but boot header offsets are garbage
        return;
    }

    u32 fstAligned = (fstSize + 31) & ~31u;
    sFst = (u8*)memalign(32, fstAligned);
    if (sFst == nullptr) { DvdLog("  FST memalign(%lu) FAILED", (unsigned long)fstAligned); sDvdDiag = 5; return; }

    rc = DVD_ReadPrio(&sDvdBlk, sFst, fstAligned, (s64)fstOffset, 2);
    DvdLog("  read FST: rc=%ld", (long)rc);
    if (rc < 0)
    {
        free(sFst);
        sFst = nullptr;
        sDvdDiag = 1;   // RED: FST read failed
        return;
    }

    // Root entry must be a directory; its length field holds the total entry count.
    if (!FstIsDir(sFst))
    {
        DvdLog("  FST root not a dir (byte0=%02X)", sFst[0]);
        free(sFst); sFst = nullptr; sDvdDiag = 4; return;   // WHITE: FST corrupt
    }
    u32 count = FstU32(sFst, 8);
    DvdLog("  FST root count=%lu", (unsigned long)count);
    if (count == 0 || (u64)count * 12 > fstSize)
    {
        free(sFst); sFst = nullptr; sDvdDiag = 4; return;   // WHITE: FST corrupt
    }

    sFstCount = count;
    sFstStr   = (const char*)(sFst + count * 12);
    sDvdActive = true;
    DvdLog("  MOUNTED OK: %lu entries (usedMount=%d)", (unsigned long)count, usedMount ? 1 : 0);
    // GREEN: served without a mount (Swiss path).  BLUE: needed DVD_Mount (Dolphin).
    sDvdDiag  = usedMount ? 2 : 10;

    LogDebug("Disc FST mounted: %u entries.", count);
}

// ---------------------------------------------------------------------------
// DIAGNOSTIC (temporary): now that video is up, force a disc-FST mount attempt
// and show the result on-screen. COLORBLIND-SAFE: uses only WHITE / BLUE /
// YELLOW (no red or green), and SOLID vs FLASHING to carry the sub-detail so it
// reads with no color discrimination at all.
//
//   Step 1: a ~1.5s WHITE flash  = "reached the diagnostic, video OK".
//   Step 2: the result, held ~4s:
//     SOLID  BLUE   = FST mounted WITHOUT a drive reset (the Swiss win!)
//     FLASH  BLUE   = FST mounted, but needed DVD_Mount's reset (laser path)
//     SOLID  YELLOW = DVD read failed entirely
//     FLASH  YELLOW = read OK but FST/offsets bad (or out of memory)
//     NO SIGNAL after the white = InitDVD itself faults (DVD calls crash HW)
//     NO WHITE at all           = crash before video / diagnostic never reached
//
//   Simple read: BLUE = mounted (good), YELLOW = failed (bad);
//                SOLID = clean, FLASHING = "but with a caveat".
// ---------------------------------------------------------------------------
static void SysPaint(SystemState& system, u32 color, int frames)
{
    VIDEO_ClearFrameBuffer(&system.mGxRmode, system.mFrameBuffers[0], color);
    VIDEO_ClearFrameBuffer(&system.mGxRmode, system.mFrameBuffers[1], color);
    VIDEO_SetNextFramebuffer(system.mFrameBuffers[system.mFrameIndex]);
    VIDEO_Flush();
    for (int f = 0; f < frames; ++f) VIDEO_WaitVSync();
}

static void SysFlash(SystemState& system, u32 color, int cycles)
{
    for (int c = 0; c < cycles; ++c)
    {
        SysPaint(system, color,       18);   // ~0.3s on
        SysPaint(system, COLOR_BLACK, 12);   // ~0.2s off
    }
}

static void SYS_ShowDvdDiag()
{
    SystemState& system = GetEngineState()->mSystem;

    sDvdVideoReady = true;                 // video is up -- allow DVD access now

    DvdLog("==== boot: DVD diagnostic reached (video up, SD writable) ====");
    SysPaint(system, COLOR_WHITE, 90);     // ~1.5s "reached diagnostic" flash

    InitDVD();                             // the risky part
    DvdLog("==== InitDVD returned: sDvdDiag=%d sDvdActive=%d ====", sDvdDiag, sDvdActive ? 1 : 0);

    switch (sDvdDiag)
    {
    case 10: SysPaint(system, COLOR_BLUE,   240); break;  // SOLID BLUE  : mounted, no reset (WIN)
    case 2:  SysFlash(system, COLOR_BLUE,   8);   break;  // FLASH BLUE  : mounted, needed reset
    case 1:  SysPaint(system, COLOR_YELLOW, 240); break;  // SOLID YELLOW: read failed
    case 4:
    case 5:  SysFlash(system, COLOR_YELLOW, 8);   break;  // FLASH YELLOW: read OK but data bad
    default: SysPaint(system, COLOR_BLACK,  240); break;  // no status
    }
}

// Resolve a slash-separated path (relative to the disc root) to its data.
static bool FstFind(const char* relPath, u32& outOffset, u32& outLen)
{
    u32 dirIndex = 0;            // start at root
    u32 dirEnd   = sFstCount;    // root subtree spans every entry

    const char* p = relPath;
    while (*p)
    {
        char comp[256];
        int c = 0;
        while (*p && *p != '/' && *p != '\\') { if (c < 255) comp[c++] = *p; ++p; }
        comp[c] = 0;
        while (*p == '/' || *p == '\\') ++p;
        if (c == 0) continue;

        bool last = (*p == 0);
        bool found = false;

        u32 i = dirIndex + 1;
        while (i < dirEnd)
        {
            const u8* e = sFst + i * 12;
            bool isDir = FstIsDir(e);
            const char* name = sFstStr + FstNameOff(e);

            if (StrEqCI(name, comp))
            {
                if (last)
                {
                    if (isDir) return false;    // wanted a file, hit a directory
                    outOffset = FstU32(e, 4);
                    outLen    = FstU32(e, 8);
                    return true;
                }
                if (!isDir) return false;       // wanted a directory, hit a file
                dirIndex = i;
                dirEnd   = FstU32(e, 8);        // dir's "next index" bounds its subtree
                found = true;
                break;
            }

            i = isDir ? FstU32(e, 8) : (i + 1); // skip nested subtrees
        }

        if (!found) return false;
    }

    return false;
}

// Normalize an engine path (strip device/drive/leading-dot-slash) and resolve it
// against the FST, tolerating any absolute prefix by retrying on shorter tails.
static bool FstResolve(const char* path, u32& outOffset, u32& outLen)
{
    if (!sDvdActive) return false;

    std::string s = path;
    for (char& ch : s) { if (ch == '\\') ch = '/'; }

    size_t colon = s.find(":/");           // strip "sd:/", "fat:/", "C:/" ...
    if (colon != std::string::npos) s = s.substr(colon + 2);

    while (!s.empty())
    {
        if (s[0] == '/')                                  s = s.substr(1);
        else if (s.size() >= 2 && s[0] == '.' && s[1] == '/') s = s.substr(2);
        else break;
    }

    while (!s.empty())
    {
        if (FstFind(s.c_str(), outOffset, outLen)) return true;
        size_t slash = s.find('/');
        if (slash == std::string::npos) return false;
        s = s.substr(slash + 1);           // drop leading component, retry the tail
    }

    return false;
}

static bool DVD_ReadAsset(const char* path, int32_t maxSize, char*& outData, uint32_t& outSize)
{
    u32 offset = 0, len = 0;
    if (!FstResolve(path, offset, len)) return false;

    u32 readLen = len;
    if (maxSize > 0 && (u32)maxSize < readLen) readLen = (u32)maxSize;

    // DVD_ReadPrio requires 32-byte aligned buffer/length; the packer 32-byte
    // aligns every file's data (and pads the image tail), so reading up to the
    // next boundary never crosses into another file's meaningful bytes.
    u32 aligned = (readLen + 31) & ~31u;
    char* buf = (char*)memalign(32, aligned);
    if (buf == nullptr) return false;

    if (DVD_ReadPrio(&sDvdBlk, buf, aligned, (s64)offset, 2) < 0)
    {
        free(buf);
        return false;
    }

    outData = buf;
    outSize = readLen;
    return true;
}

// Files
bool SYS_DoesFileExist(const char* path, bool isAsset)
{
    struct stat info;
    int32_t retStatus = stat(path, &info);

    if (retStatus == 0)
    {
        // If the file is actually a directory, than return false.
        return !(info.st_mode & S_IFDIR);
    }

    // Fall back to the disc filesystem (disc boot with no SD).
    InitDVD();
    if (sDvdActive)
    {
        u32 offset = 0, len = 0;
        if (FstResolve(path, offset, len)) return true;
    }

    return false;
}

void SYS_AcquireFileData(const char* path, bool isAsset, int32_t maxSize, char*& outData, uint32_t& outSize)
{
    // Need to init fat in case opening the Engine.ini in OctPreInitialize()
    InitFAT();

    outData = nullptr;
    outSize = 0;

    FILE* file = fopen(path, "rb");

    if (file != nullptr)
    {
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
        // No SD / file not on the mounted filesystem -- try the disc FST.
        InitDVD();
        if (DVD_ReadAsset(path, maxSize, outData, outSize))
        {
            return;
        }

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
        16 * 1024,      /* stack size */
        64              /* thread priority */);

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

static void MountMemoryCard()
{
    if (!IsMemoryCardMounted())
    {
        LogDebug("Initializing CARD");
        GetEngineState()->mSystem.mMemoryCardMountArea = SYS_AlignedMalloc(CARD_WORKAREA_SIZE, 32);
        CARD_Init("OCTA","00");
        int errorSlotA = CARD_Mount(CARD_SLOTA, GetEngineState()->mSystem.mMemoryCardMountArea, UnmountMemoryCard);
        LogDebug("Memory card code: %d", errorSlotA);

        if (errorSlotA >= 0)
        {
            GetEngineState()->mSystem.mMemoryCardMounted = true;
        }
    }
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

            char* cardBuffer = (char*)SYS_AlignedMalloc(fileSize, 32);
            CARD_Read(&cardFile, cardBuffer, sectorSize, 0);
            success = true;

            outStream.SetPos(0);
            outStream.WriteBytes((uint8_t*) cardBuffer, fileSize);
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

        int32_t fileSize = ((stream.GetSize() + sectorSize - 1) / sectorSize) * sectorSize;

        card_file cardFile;
        int32_t cardError = CARD_Open(CARD_SLOTA, saveName, &cardFile);

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
            OCT_ASSERT(fileSize >= (int32_t)stream.GetSize());
            memcpy(cardBuffer, stream.GetData(), stream.GetSize());

            cardError = CARD_Write(&cardFile, cardBuffer, fileSize, 0);
            success = true;

            if (cardError < 0)
            {
                LogError("Failed to write save to memory card. Error code = %d", cardError);
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
        SYS_AlignedFree(GetEngineState()->mSystem.mMemoryCardMountArea);
        GetEngineState()->mSystem.mMemoryCardMountArea = nullptr;
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
void SYS_Log(LogSeverity severity, const char* format, va_list arg)
{
    // SYS_Report() allows logging in Dolphin with a .dol file.
    // Printf logging requires .elf.
    char logBuffer[256];
    vsnprintf(logBuffer, 255, format, arg);

    SYS_Report(logBuffer);
    SYS_Report("\n");

    // I'm not sure if printf() is needed for the libogc console, but the console
    // is currently broken right now and causes octave to crash.
    //vprintf(format, arg);
    //printf("\n");
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