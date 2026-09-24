#include "Profiler.h"
#include "Engine.h"
#include "System/System.h"
#include "Maths.h"
#include "Clock.h"
#include "Log.h"
#include "Assertion.h"

#include "Graphics/Graphics.h"

static Profiler* sProfiler = nullptr;

#if PLATFORM_DOLPHIN && PROFILING_ENABLED
// The console has no profiler window, so where the frame went is written to the SD diagnostic log
// (OctLog -> /octiso.log, a no-op unless that local logger is switched on): one line every few
// seconds, the average of every frame stat over them and then the same stats for the single worst
// frame. One line, not one a frame, because writing to the card is itself a hitch.
#include <stdio.h>
#include <string.h>
#include <malloc.h>
void OctLog(const char* format, ...);

static const float kPerfLogPeriod = 5.0f;
// The last period's two halves, kept for System.GetPerfReport(): a game can put them on screen
// where the card cannot be written (or read back) conveniently.
static char sPerfAverage[256] = "";
static char sPerfWorst[256] = "";
const char* GetPerfAverageLine() { return sPerfAverage; }
const char* GetPerfWorstLine() { return sPerfWorst; }
static const uint32_t kPerfMaxStats = 24;

// ---- LUA: where the game's scripts spend the frame. Every script's Tick is timed by name
// (Script::CallTick), and a script can time its own parts: System.PerfBegin(name) / PerfEnd().
// Each gets, a frame: its average and worst ms, and the Lua memory it allocated and the garbage
// collector freed while it ran (net, from the Lua heap's size before and after) -- garbage made
// every frame is collector work later, which comes back as uneven frames. Logged with the PERF
// lines as LUA lines. Times include what is nested inside them.
namespace
{
    struct LuaPerf
    {
        char mName[24];
        double mSumMs;
        float mWorstMs;
        float mFrameMs;             // this frame's so far
        uint32_t mFrame;            // which frame mFrameMs is
        double mAllocKb;
        double mFreedKb;
    };
    const uint32_t kMaxLuaPerf = 24;
    LuaPerf sLuaPerf[kMaxLuaPerf];
    uint32_t sNumLuaPerf = 0;

    struct LuaPerfOpen
    {
        LuaPerf* mEntry;
        uint64_t mStartUs;
        int64_t mStartBytes;
    };
    LuaPerfOpen sLuaOpen[16];
    uint32_t sNumLuaOpen = 0;

    LuaPerf* FindLuaPerf(const char* name)
    {
        for (uint32_t i = 0; i < sNumLuaPerf; ++i)
        {
            if (strncmp(sLuaPerf[i].mName, name, sizeof(sLuaPerf[i].mName) - 1) == 0)
            {
                return &sLuaPerf[i];
            }
        }
        if (sNumLuaPerf == kMaxLuaPerf)
        {
            return nullptr;
        }
        LuaPerf* e = &sLuaPerf[sNumLuaPerf++];
        memset(e, 0, sizeof(*e));
        strncpy(e->mName, name, sizeof(e->mName) - 1);
        return e;
    }

    int64_t LuaHeapBytes()
    {
        lua_State* L = GetLua();
        return L ? (int64_t(lua_gc(L, LUA_GCCOUNT, 0)) * 1024 + lua_gc(L, LUA_GCCOUNTB, 0)) : 0;
    }

    void LuaPerfAdd(LuaPerf* e, uint64_t us, int64_t bytes)
    {
        const uint32_t frame = GetEngineState()->mFrameNumber;
        if (e->mFrame != frame)
        {
            e->mWorstMs = glm::max(e->mWorstMs, e->mFrameMs);
            e->mFrameMs = 0.0f;
            e->mFrame = frame;
        }
        const float ms = us / 1000.0f;
        e->mFrameMs += ms;
        e->mSumMs += ms;
        if (bytes >= 0) e->mAllocKb += bytes / 1024.0;
        else e->mFreedKb += -bytes / 1024.0;
    }

    void LogLuaPerf(uint32_t frames)
    {
        if (sNumLuaPerf == 0 || frames == 0)
        {
            return;
        }
        char line[500];
        int at = snprintf(line, sizeof(line), "LUA heap=%dK", int(LuaHeapBytes() / 1024));
        for (uint32_t i = 0; i < sNumLuaPerf; ++i)
        {
            LuaPerf& e = sLuaPerf[i];
            const float worst = glm::max(e.mWorstMs, e.mFrameMs);
            char item[96];
            snprintf(item, sizeof(item), " %s=%.2f/%.1fms+%.1fK-%.1fK", e.mName, e.mSumMs / frames, worst,
                     e.mAllocKb / frames, e.mFreedKb / frames);
            if (at + int(strlen(item)) >= int(sizeof(line)) - 1)
            {
                OctLog("%s", line);                         // too long for one line: go on in another
                at = snprintf(line, sizeof(line), "LUA+");
            }
            at += snprintf(line + at, sizeof(line) - at, "%s", item);
            e.mSumMs = 0.0;
            e.mWorstMs = 0.0f;
            e.mFrameMs = 0.0f;
            e.mAllocKb = e.mFreedKb = 0.0;
        }
        OctLog("%s", line);
    }
}

void OctLuaPerfBegin(const char* name)
{
    if (sNumLuaOpen < sizeof(sLuaOpen) / sizeof(sLuaOpen[0]))
    {
        LuaPerfOpen& o = sLuaOpen[sNumLuaOpen++];
        o.mEntry = FindLuaPerf(name);
        o.mStartBytes = LuaHeapBytes();
        o.mStartUs = SYS_GetTimeMicroseconds();
    }
}

void OctLuaPerfEnd()
{
    if (sNumLuaOpen > 0)
    {
        const uint64_t nowUs = SYS_GetTimeMicroseconds();
        LuaPerfOpen& o = sLuaOpen[--sNumLuaOpen];
        if (o.mEntry != nullptr)
        {
            LuaPerfAdd(o.mEntry, nowUs - o.mStartUs, LuaHeapBytes() - o.mStartBytes);
        }
    }
}

static void LogFrameStats(const std::vector<CpuStat>& stats, float deltaTime)
{
    static float sSum[kPerfMaxStats] = {};
    static float sWorst[kPerfMaxStats] = {};
    static float sWorstFrame = 0.0f;
    static float sTime = 0.0f;
    static uint32_t sFrames = 0;

    uint32_t count = glm::min<uint32_t>(uint32_t(stats.size()), kPerfMaxStats);
    float frameMs = deltaTime * 1000.0f;

    for (uint32_t i = 0; i < count; ++i)
    {
        sSum[i] += stats[i].mTime;
    }

    if (frameMs > sWorstFrame)
    {
        sWorstFrame = frameMs;
        for (uint32_t i = 0; i < count; ++i)
        {
            sWorst[i] = stats[i].mTime;
        }
    }

    sTime += deltaTime;
    sFrames++;

    if (sTime >= kPerfLogPeriod)
    {
        char line[500];
        struct mallinfo info = mallinfo();
        int at = snprintf(line, sizeof(line), "PERF %.1ffps heap=%dK avg", sFrames / sTime, int(info.fordblks / 1024));

        for (uint32_t i = 0; i < count && at < int(sizeof(line)) - 24; ++i)
        {
            at += snprintf(line + at, sizeof(line) - at, " %s=%.1f", stats[i].mName, sSum[i] / sFrames);
        }

        // Two lines, the average and the worst frame: one no longer fits OctLog's 512 bytes.
        OctLog("%s", line);

        at = snprintf(line, sizeof(line), "PERF worst %.0fms", sWorstFrame);

        for (uint32_t i = 0; i < count && at < int(sizeof(line)) - 24; ++i)
        {
            at += snprintf(line + at, sizeof(line) - at, " %s=%.1f", stats[i].mName, sWorst[i]);
        }

        OctLog("%s", line);
        LogLuaPerf(sFrames);

        int a = snprintf(sPerfAverage, sizeof(sPerfAverage), "avg");
        int w = snprintf(sPerfWorst, sizeof(sPerfWorst), "worst %.0f:", sWorstFrame);
        for (uint32_t i = 0; i < count; ++i)
        {
            // Three letters of the name, and only what took a millisecond somewhere: it has to fit a screen.
            if (sSum[i] / sFrames < 1.0f && sWorst[i] < 1.0f) continue;
            if (a < int(sizeof(sPerfAverage)) - 16) a += snprintf(sPerfAverage + a, sizeof(sPerfAverage) - a, " %.3s %.0f", stats[i].mName, sSum[i] / sFrames);
            if (w < int(sizeof(sPerfWorst)) - 16) w += snprintf(sPerfWorst + w, sizeof(sPerfWorst) - w, " %.3s %.0f", stats[i].mName, sWorst[i]);
        }

        for (uint32_t i = 0; i < kPerfMaxStats; ++i)
        {
            sSum[i] = 0.0f;
            sWorst[i] = 0.0f;
        }
        sWorstFrame = 0.0f;
        sTime = 0.0f;
        sFrames = 0;
    }
}
#endif

void Profiler::BeginFrame()
{
#if PROFILING_ENABLED
    // Clear out the start/end/elapse time on all stats
    for (uint32_t i = 0; i < mCpuFrameStats.size(); ++i)
    {
        mCpuFrameStats[i].mTime = 0.0f;
        mCpuFrameStats[i].mStartTime = 0;
        mCpuFrameStats[i].mEndTime = 0;
    }
#endif
}

void Profiler::EndFrame()
{
#if PROFILING_ENABLED
    float deltaTime = GetAppClock()->DeltaTime();

    // Calculate the elapsed time in milliseconds from the start/end microsecond times
    for (uint32_t i = 0; i < mCpuFrameStats.size(); ++i)
    {
        mCpuFrameStats[i].mSmoothedTime = Maths::Damp(mCpuFrameStats[i].mSmoothedTime, mCpuFrameStats[i].mTime, 0.05f, deltaTime);
    }

#if PLATFORM_DOLPHIN
    LogFrameStats(mCpuFrameStats, deltaTime);
#endif

    for (uint32_t i = 0; i < mGpuStats.size(); ++i)
    {
        mGpuStats[i].mSmoothedTime = Maths::Damp(mGpuStats[i].mSmoothedTime, mGpuStats[i].mTime, 0.05f, deltaTime);
    }
#endif
}

void Profiler::BeginCpuStat(const char* name, bool persistent)
{
#if PROFILING_ENABLED
    CpuStat* stat = FindCpuStat(name, persistent);

    if (stat == nullptr)
    {
        CpuStat newStat;
        strncpy(newStat.mName, name, STAT_NAME_LENGTH);

        if (persistent)
        {
            mCpuPersistentStats.push_back(newStat);
            stat = &mCpuPersistentStats.back();
        }
        else
        {
            mCpuFrameStats.push_back(newStat);
            stat = &mCpuFrameStats.back();
        }
    }

    stat->mStartTime = SYS_GetTimeMicroseconds();
#endif
}

void Profiler::EndCpuStat(const char* name, bool persistent)
{
#if PROFILING_ENABLED
    CpuStat* stat = FindCpuStat(name, persistent);
    OCT_ASSERT(stat);

    if (stat)
    {
        stat->mEndTime = SYS_GetTimeMicroseconds();
        stat->mTime += (stat->mEndTime - stat->mStartTime) / 1000.0f;
    }
#endif
}

void Profiler::BeginGpuStat(const char* name)
{
#if PROFILING_ENABLED
    GFX_BeginGpuTimestamp(name);
#endif
}

void Profiler::EndGpuStat(const char* name)
{
#if PROFILING_ENABLED
    GFX_EndGpuTimestamp(name);
#endif
}

void Profiler::SetGpuStatTime(const char* name, float time)
{
    // The GFX implementation is responsible for calling this on all GPU stats at Frame's end.
#if PROFILING_ENABLED
    GpuStat* gpuStat = nullptr;
    for (uint32_t i = 0; i < mGpuStats.size(); ++i)
    {
        if (strncmp(mGpuStats[i].mName, name, STAT_NAME_LENGTH) == 0)
        {
            gpuStat = &mGpuStats[i];
            break;
        }
    }

    if (gpuStat == nullptr)
    {
        mGpuStats.push_back(GpuStat());
        gpuStat = &(mGpuStats.back());
        strncpy(gpuStat->mName, name, STAT_NAME_LENGTH);
    }

    gpuStat->mTime = time;
#endif
}

CpuStat* Profiler::FindCpuStat(const char* name, bool persistent)
{
    std::vector<CpuStat>& stats = persistent ? mCpuPersistentStats : mCpuFrameStats;
    CpuStat* retStat = nullptr;

#if PROFILING_ENABLED
    for (uint32_t i = 0; i < stats.size(); ++i)
    {
        if (strncmp(stats[i].mName, name, STAT_NAME_LENGTH) == 0)
        {
            retStat = &stats[i];
            break;
        }
    }
#endif

    return retStat;
}

const std::vector<CpuStat>& Profiler::GetCpuFrameStats() const
{
    return mCpuFrameStats;
}

const std::vector<CpuStat>& Profiler::GetCpuPersistentStats() const
{
    return mCpuPersistentStats;
}

const std::vector<GpuStat>& Profiler::GetGpuStats() const
{
    return mGpuStats;
}

void Profiler::LogPersistentStats()
{
    LogDebug("----- Persistent Stats -----");

    for (uint32_t i = 0; i < mCpuPersistentStats.size(); ++i)
    {
        LogDebug("%s: %f", mCpuPersistentStats[i].mName, mCpuPersistentStats[i].mTime);
    }

    LogDebug("----------------------------");
}

void Profiler::DumpPersistentStats()
{
    FILE* statFile = fopen("CpuStats.csv", "w");

    if (statFile != nullptr)
    {
        for (uint32_t i = 0; i < mCpuPersistentStats.size(); ++i)
        {
            fprintf(statFile, "%s, %f\n", mCpuPersistentStats[i].mName, mCpuPersistentStats[i].mTime);
        }

        fclose(statFile);
        statFile = nullptr;
    }
}

void CreateProfiler()
{
#if PROFILING_ENABLED
    if (sProfiler == nullptr)
    {
        sProfiler = new Profiler();
    }
#endif
}

void DestroyProfiler()
{
#if PROFILING_ENABLED
    if (sProfiler != nullptr)
    {
        delete sProfiler;
        sProfiler = nullptr;
    }
#endif
}

Profiler* GetProfiler()
{
#if PROFILING_ENABLED
    return sProfiler;
#else
    return nullptr;
#endif
}
