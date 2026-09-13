#include "VideoStream.h"

#include "Assets/VideoClip.h"
#include "System/System.h"

#if PLATFORM_DOLPHIN
#include <gccore.h>
#endif

#if !EDITOR
// Editor builds get the full stb_image implementation from stb_implementation.cpp.
// Runtime builds only need baseline JPEG for video frames.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS
#endif

#if defined(__GNUC__)
// STBI_ONLY_JPEG leaves some of stb_image's static helpers unused.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <stb_image.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <cstring>

// Decoded frames waiting to be shown. Small, since each is width * height * 4 bytes
// and the decoder only needs to stay slightly ahead of playback.
static constexpr size_t kMaxQueuedFrames = 3;

static inline uint32_t ReadU32LE(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

VideoStream::VideoStream()
{

}

VideoStream::~VideoStream()
{
    Close();
}

bool VideoStream::Open(VideoClip* clip)
{
    Close();

    if (clip == nullptr || clip->GetNumFrames() == 0)
    {
        return false;
    }

    mClip = clip;
    mNextRecord = 0;
    mGeneration = 0;
    mSeekRecord = -1;
    mDecodedAll = false;
    mError = false;
    mExit = false;

    mMutex = SYS_CreateMutex();
    mThread = SYS_CreateThread(ThreadMain, this);

    return true;
}

void VideoStream::Close()
{
    if (mThread != nullptr)
    {
        {
            SCOPED_LOCK(mMutex);
            mExit = true;
        }

        SYS_JoinThread(mThread);
        SYS_DestroyThread(mThread);
        mThread = nullptr;
    }

    if (mMutex != nullptr)
    {
        ClearQueues();
        SYS_DestroyMutex(mMutex);
        mMutex = nullptr;
    }

    mClip = nullptr;
}

double VideoStream::Seek(double seconds)
{
    if (mMutex == nullptr || mClip == nullptr)
    {
        return 0.0;
    }

    const double frameRate = mClip->GetFrameRate();
    int32_t record = int32_t(seconds * frameRate + 0.5);
    record = glm::clamp(record, 0, int32_t(mClip->GetNumFrames()) - 1);

    SCOPED_LOCK(mMutex);
    mSeekRecord = record;
    mGeneration++;
    mDecodedAll = false;
    ClearQueues();

    return record / frameRate;
}

bool VideoStream::PopFrame(double maxTime, Frame& outFrame)
{
    if (mMutex == nullptr)
    {
        return false;
    }

    SCOPED_LOCK(mMutex);
    bool found = false;

    while (!mFrames.empty() && mFrames.front().mTime <= maxTime)
    {
        if (found)
        {
            FreeFrame(outFrame);
        }

        outFrame = mFrames.front();
        mFrames.pop_front();
        found = true;
    }

    return found;
}

void VideoStream::FreeFrame(Frame& frame)
{
    if (frame.mPixels != nullptr)
    {
        stbi_image_free(frame.mPixels);
        frame.mPixels = nullptr;
    }
}

bool VideoStream::HasQueuedFrames()
{
    if (mMutex == nullptr)
    {
        return false;
    }

    SCOPED_LOCK(mMutex);
    return !mFrames.empty();
}

void VideoStream::PopAudio(std::vector<uint8_t>& outAudio)
{
    if (mMutex == nullptr)
    {
        return;
    }

    SCOPED_LOCK(mMutex);
    outAudio.insert(outAudio.end(), mAudio.begin(), mAudio.end());
    mAudio.clear();
}

bool VideoStream::IsEndOfStream()
{
    if (mMutex == nullptr)
    {
        return true;
    }

    SCOPED_LOCK(mMutex);
    return mDecodedAll && mFrames.empty();
}

bool VideoStream::HasError()
{
    if (mMutex == nullptr)
    {
        return false;
    }

    SCOPED_LOCK(mMutex);
    return mError;
}

ThreadFuncRet VideoStream::ThreadMain(void* arg)
{
#if PLATFORM_DOLPHIN
    // The main thread runs at priority 64. Decoding below it means the decoder only
    // gets time the main thread spends blocked (e.g. waiting for vsync), so video
    // never stalls rendering. It drops frames instead if the CPU is too busy.
    LWP_SetThreadPriority(LWP_GetSelf(), 40);
#endif

    static_cast<VideoStream*>(arg)->WorkerLoop();

    THREAD_RETURN();
}

void VideoStream::WorkerLoop()
{
    const uint32_t numFrames = mClip->GetNumFrames();
    const uint32_t width = mClip->GetWidth();
    const uint32_t height = mClip->GetHeight();
    const double frameRate = mClip->GetFrameRate();

    std::vector<uint8_t> record(mClip->GetMaxRecordSize());

    while (true)
    {
        uint32_t index = 0;
        uint32_t generation = 0;
        bool idle = false;

        {
            SCOPED_LOCK(mMutex);

            if (mExit)
            {
                break;
            }

            if (mSeekRecord >= 0)
            {
                mNextRecord = uint32_t(mSeekRecord);
                mSeekRecord = -1;
            }

            index = mNextRecord;
            generation = mGeneration;

            if (mError || mFrames.size() >= kMaxQueuedFrames)
            {
                idle = true;
            }
            else if (index >= numFrames)
            {
                mDecodedAll = true;
                idle = true;
            }
        }

        if (idle)
        {
            SYS_Sleep(2);
            continue;
        }

        const uint32_t recordSize = mClip->GetRecordSize(index);
        bool success = recordSize >= 8 &&
                       recordSize <= record.size() &&
                       mClip->ReadRecord(index, record.data());

        uint32_t audioBytes = 0;
        uint8_t* pixels = nullptr;

        if (success)
        {
            audioBytes = ReadU32LE(record.data());
            const uint32_t jpegBytes = ReadU32LE(record.data() + 4);
            success = (uint64_t(8) + audioBytes + jpegBytes) <= recordSize;

            if (success)
            {
                int decWidth = 0;
                int decHeight = 0;
                int decComps = 0;
                pixels = stbi_load_from_memory(
                    record.data() + 8 + audioBytes,
                    int(jpegBytes),
                    &decWidth,
                    &decHeight,
                    &decComps,
                    4);

                success = pixels != nullptr &&
                          uint32_t(decWidth) == width &&
                          uint32_t(decHeight) == height;
            }
        }

        SCOPED_LOCK(mMutex);

        if (generation != mGeneration)
        {
            // A seek happened while decoding; this frame belongs to the old position.
            if (pixels != nullptr)
            {
                stbi_image_free(pixels);
            }
            continue;
        }

        if (!success)
        {
            if (pixels != nullptr)
            {
                stbi_image_free(pixels);
            }
            mError = true;
            continue;
        }

        Frame frame;
        frame.mPixels = pixels;
        frame.mTime = index / frameRate;
        mFrames.push_back(frame);

        mAudio.insert(mAudio.end(), record.data() + 8, record.data() + 8 + audioBytes);
        mNextRecord = index + 1;
    }
}

void VideoStream::ClearQueues()
{
    for (Frame& frame : mFrames)
    {
        FreeFrame(frame);
    }

    mFrames.clear();
    mAudio.clear();
}
