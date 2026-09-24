#include "Clock.h"
#include "Log.h"

#include "System/System.h"

#if PLATFORM_DOLPHIN
#include <gccore.h>

// How long one video retrace lasts: 50 Hz for PAL, else NTSC's 59.94.
static float RetracePeriod()
{
    return (VIDEO_GetCurrentTvMode() == VI_PAL) ? (1.0f / 50.0f) : (1001.0f / 60000.0f);
}

// This libogc has no VIDEO_GetRetraceCount; the post-retrace callback is handed the count.
static volatile uint32_t sRetraceCount = 0;
static void OnRetrace(u32 retraceCount) { sRetraceCount = retraceCount; }

static uint32_t VIDEO_GetRetraceCount()
{
    static bool sHooked = false;
    if (!sHooked)
    {
        VIDEO_SetPostRetraceCallback(OnRetrace);
        sHooked = true;
    }
    return sRetraceCount;
}
#endif

Clock::Clock()
{

}

Clock::~Clock()
{

}

void Clock::Start()
{
    mActive = true;
    mDeltaTimeSeconds = 0.0f;

    mStartTimeUs = SYS_GetTimeMicroseconds();
    mPreviousTimeUs = mStartTimeUs;
    mCurrentTimeUs = mStartTimeUs;
#if PLATFORM_DOLPHIN
    mPreviousRetrace = VIDEO_GetRetraceCount();
#endif
}

void Clock::Stop()
{
    Update(); // Record the latest time.
    mActive = false;
}

void Clock::Update()
{
    if (mActive)
    {
        mCurrentTimeUs = SYS_GetTimeMicroseconds();
        if (mCurrentTimeUs > mPreviousTimeUs)
        {
            mDeltaTimeSeconds = (mCurrentTimeUs - mPreviousTimeUs) / 1000000.0f;
        }
        else
        {
            // Somehow on the Dolphin emulator, the current time was less than the previous time??
            // Didn't get to test this on console, but since we want the engine to work in Dolphin 5.0,
            // we'll keep this safety check in for now.
            mDeltaTimeSeconds = 0.0f;
        }

        mPreviousTimeUs = mCurrentTimeUs;

#if PLATFORM_DOLPHIN
        // A frame is on screen for a whole number of retraces, so the game moves by exactly that
        // much time. Measured time wobbles around it (16.2, 17.1, ... ms), and a frame that just
        // misses a retrace is SHOWN for two but measured as one and a bit: both read as judder.
        const uint32_t retrace = VIDEO_GetRetraceCount();
        const uint32_t retraces = retrace - mPreviousRetrace;
        const bool counted = (mPreviousRetrace != 0);   // 0: the callback had not run yet
        mPreviousRetrace = retrace;
        if (counted && retraces > 0)
        {
            mDeltaTimeSeconds = retraces * RetracePeriod();
        }
#endif

        mTimeSeconds = (mCurrentTimeUs - mStartTimeUs) / 1000000.0f;
    }
}

float Clock::DeltaTime() const
{
    return mDeltaTimeSeconds;
}

float Clock::GetTime() const
{
    return mTimeSeconds;
}
