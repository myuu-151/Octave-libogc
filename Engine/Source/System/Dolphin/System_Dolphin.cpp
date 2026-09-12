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
#include <vector>
#include <unordered_map>
#include <ctype.h>
#include <stdarg.h>

#define ENABLE_LIBOGC_CONSOLE 0

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

// Diagnostic log of ISO asset streaming, written to /octiso.log on the SD. Off by
// default; set ISO_LOG_ENABLED to 1 to trace mount/hit/miss when debugging.
#define ISO_LOG_ENABLED 0
static void IsoLog(const char* fmt, ...)
{
#if ISO_LOG_ENABLED
    FILE* lf = fopen("/octiso.log", "a");
    if (lf == nullptr) return;
    va_list ap; va_start(ap, fmt); vfprintf(lf, fmt, ap); va_end(ap);
    fputc('\n', lf);
    fclose(lf);
#else
    (void)fmt;
#endif
}

static bool IsoOpen(const char* isoPath)
{
    FILE* f = fopen(isoPath, "rb");
    if (f == nullptr) return false;

    uint8_t hdr[0x440];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fclose(f); return false; }
    if (IsoRead32(hdr + 0x1C) != 0xC2339F3D) { fclose(f); return false; }  // GC disc magic

    uint32_t fstOff  = IsoRead32(hdr + 0x424);
    uint32_t fstSize = IsoRead32(hdr + 0x428);
    if (fstOff == 0 || fstSize < 12) { fclose(f); return false; }

    std::vector<uint8_t> fst(fstSize);
    fseek(f, fstOff, SEEK_SET);
    if (fread(fst.data(), 1, fstSize, f) != fstSize) { fclose(f); return false; }

    uint32_t numEntries = IsoRead32(&fst[8]);            // root entry length = entry count
    if ((uint64_t)numEntries * 12 > fstSize || numEntries == 0) { fclose(f); return false; }
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

    sIso = f;
    LogDebug("ISO mounted: %s (%u files in FST)", isoPath, (uint32_t)sIsoFiles.size());
    IsoLog("ISO MOUNTED: %s  files=%u", isoPath, (uint32_t)sIsoFiles.size());
    int32_t sample = 0;
    for (auto& kv : sIsoFiles) { IsoLog("  fst: %s (%u bytes)", kv.first.c_str(), kv.second.size); if (++sample >= 8) break; }
    return true;
}

// Try known locations for the project's disc image. The ISO is whatever Swiss
// booted; we look for <ProjectName>.iso near the project dir / SD root.
static void IsoLocate()
{
    EngineState* es = GetEngineState();
    const std::string& pn = es->mProjectName;
    const std::string& pd = es->mProjectDirectory;
    if (pn.empty()) return;   // project name not set yet -- retry on a later call

    std::string cands[] = {
        pd + pn + ".iso",
        pd + "../" + pn + ".iso",
        "/" + pn + ".iso",
        pn + ".iso",
    };
    for (const std::string& c : cands)
    {
        IsoLog("ISO try: %s", c.c_str());
        if (IsoOpen(c.c_str())) return;
    }
    IsoLog("ISO: none of the candidate paths opened (project '%s', dir '%s')", pn.c_str(), pd.c_str());
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
    if (sIso == nullptr && sIsoAttempts < 16) { sIsoAttempts++; IsoLocate(); }
    if (sIso == nullptr) return false;
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

    // On-disc streaming: serve assets from the ISO's FST if one is present.
    if (isAsset)
    {
        if (sIso == nullptr && sIsoAttempts < 16)
        {
            sIsoAttempts++;
            IsoLocate();
        }

        IsoEntry ent;
        if (sIso != nullptr && IsoFind(path, ent))
        {
            int32_t fileSize = (int32_t)ent.size;
            if (maxSize > 0)
            {
                fileSize = glm::min(fileSize, maxSize);
            }

            IsoLog("H %s", path);

            outData = (char*)malloc(fileSize);
            outSize = uint32_t(fileSize);
            fseek(sIso, ent.offset, SEEK_SET);
            fread(outData, fileSize, 1, sIso);
            return;
        }
    }

    // Fall back to a loose file on the SD.
    FILE* file = fopen(path, "rb");

    if (file != nullptr)
    {
        if (isAsset) IsoLog("L %s", path);

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
        if (isAsset) IsoLog("F %s", path);
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
    outDirEntry.mIsoEnum = nullptr;

    // Locate the ISO lazily -- a directory scan may be the first thing requested.
    if (sIso == nullptr && sIsoAttempts < 16) { sIsoAttempts++; IsoLocate(); }

    // Prefer the ISO's FST for directories that live inside the mounted disc image.
    if (sIso != nullptr)
    {
        IsoDirEnum* en = new IsoDirEnum();
        en->index = 0;
        if (IsoListDir(dirPath.c_str(), en->entries))
        {
            IsoLog("ISO dir: %s -> %u entries", dirPath.c_str(), (uint32_t)en->entries.size());
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