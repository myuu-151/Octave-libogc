#include "VideoStream.h"

#include "JpegYuvDecoder.h"
#include "Assets/VideoClip.h"
#include "System/System.h"

#if PLATFORM_DOLPHIN
#include <gccore.h>
#else
#if !EDITOR
// Editor builds get the full stb_image implementation from stb_implementation.cpp.
// Other runtime builds only need baseline JPEG for video frames.
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
#endif

#include <cstring>

// Decoded frames waiting to be shown. Kept small: the decoder only needs to stay
// slightly ahead of playback, and on GameCube every frame costs precious RAM.
#if PLATFORM_DOLPHIN
static constexpr size_t kMaxQueuedFrames = 2;
#else
static constexpr size_t kMaxQueuedFrames = 3;
#endif

// Records read ahead of decoding (about half a second at 30 fps). Their audio is
// delivered as soon as they're read, so this is also how far audio can run ahead.
static constexpr size_t kMaxPendingRecords = 15;

// Records in a row that fail to load or decode before playback gives up.
static constexpr uint32_t kMaxConsecutiveFailures = 30;

static inline uint32_t ReadU32LE(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static inline uint8_t Clamp8(int32_t x)
{
    if (uint32_t(x) > 255)
    {
        return (x < 0) ? 0 : 255;
    }
    return uint8_t(x);
}

// Full-range (JPEG) YCbCr to GX_TF_RGBA8 texels: 4x4 blocks of 16 (A,R) byte pairs
// then 16 (G,B) pairs. Dimensions are multiples of 16.
static void ConvertYuvToGxRgba8(
    const uint8_t* yPlane,
    const uint8_t* cbPlane,
    const uint8_t* crPlane,
    uint32_t width,
    uint32_t height,
    uint8_t* dst)
{
    const uint32_t chromaWidth = width / 2;
    uint8_t* block = dst;

    for (uint32_t by = 0; by < height / 4; ++by)
    {
        for (uint32_t bx = 0; bx < width / 4; ++bx)
        {
            uint8_t* arPlane = block;
            uint8_t* gbPlane = block + 32;

            for (uint32_t py = 0; py < 4; ++py)
            {
                const uint32_t y = by * 4 + py;
                const uint8_t* lumaRow = yPlane + y * width;
                const uint32_t chromaRow = (y / 2) * chromaWidth;

                for (uint32_t px = 0; px < 4; ++px)
                {
                    const uint32_t x = bx * 4 + px;
                    const int32_t luma = lumaRow[x];
                    const int32_t cb = int32_t(cbPlane[chromaRow + x / 2]) - 128;
                    const int32_t cr = int32_t(crPlane[chromaRow + x / 2]) - 128;
                    const uint32_t i = (py * 4 + px) * 2;

                    arPlane[i] = 255;
                    arPlane[i + 1] = Clamp8(luma + ((91881 * cr) >> 16));
                    gbPlane[i] = Clamp8(luma - ((22554 * cb + 46802 * cr) >> 16));
                    gbPlane[i + 1] = Clamp8(luma + ((116130 * cb) >> 16));
                }
            }

            block += 64;
        }
    }
}

VideoStream::VideoStream()
{

}

VideoStream::~VideoStream()
{
    Close();
}

bool VideoStream::Open(VideoClip* clip, VideoFrameFormat format)
{
    Close();

    if (clip == nullptr || clip->GetNumFrames() == 0)
    {
        return false;
    }

    mClip = clip;
    mFormat = format;
    mWidth = clip->GetWidth();
    mHeight = clip->GetHeight();

    // The GX formats decode with JpegYuvDecoder, which needs whole 16x16 macroblocks.
    if (format != VideoFrameFormat::Rgba8 && ((mWidth % 16) != 0 || (mHeight % 16) != 0))
    {
        mClip = nullptr;
        return false;
    }

    mNextRecord = 0;
    mGeneration = 0;
    mSeekRecord = -1;
    mDroppedFrames = 0;
    mConsecutiveFailures = 0;
    mPlaybackTime = 0.0;
    mStats = VideoStreamStats();
    mDecodedAll = false;
    mError = false;
    mExit = false;

    if (!AllocateBuffers())
    {
        FreeBuffers();
        mClip = nullptr;
        return false;
    }

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
        ClearQueuesLocked();
        SYS_DestroyMutex(mMutex);
        mMutex = nullptr;
    }

    FreeBuffers();
    mClip = nullptr;
}

bool VideoStream::AllocateBuffers()
{
    const uint32_t lumaSize = mWidth * mHeight;
    const uint32_t chromaSize = (mWidth / 2) * (mHeight / 2);
    uint32_t planeSizes[3] = { 0, 0, 0 };

    if (mFormat == VideoFrameFormat::GxYuv420)
    {
        planeSizes[0] = lumaSize;
        planeSizes[1] = chromaSize;
        planeSizes[2] = chromaSize;
    }
    else
    {
        planeSizes[0] = lumaSize * 4;
    }

    if (mFormat != VideoFrameFormat::Rgba8)
    {
        mJpeg = new JpegYuvDecoder();
    }

    if (mFormat == VideoFrameFormat::GxRgba8)
    {
        // Row-major planes to decode into before converting to RGBA.
        const uint32_t scratchSizes[3] = { lumaSize, chromaSize, chromaSize };
        for (uint32_t p = 0; p < 3; ++p)
        {
            mScratchPlanes[p] = (uint8_t*)SYS_AlignedMalloc(scratchSizes[p], 32);
            if (mScratchPlanes[p] == nullptr)
            {
                return false;
            }
        }
    }

    // Frames that can exist at once: the queue, the one on screen, and one being decoded.
    const uint32_t numSlots = uint32_t(kMaxQueuedFrames) + 2;
    mSlots.resize(numSlots);
    mFreeSlots.clear();

    for (uint32_t i = 0; i < numSlots; ++i)
    {
        for (uint32_t p = 0; p < 3; ++p)
        {
            if (planeSizes[p] == 0)
            {
                continue;
            }

            mSlots[i].mPlanes[p] = (uint8_t*)SYS_AlignedMalloc(planeSizes[p], 32);
            mSlots[i].mSizes[p] = planeSizes[p];

            if (mSlots[i].mPlanes[p] == nullptr)
            {
                return false;
            }
        }

        mFreeSlots.push_back(int32_t(i));
    }

    mRecordBuffers.resize(kMaxPendingRecords);
    mFreeRecordBuffers.clear();

    for (uint32_t i = 0; i < kMaxPendingRecords; ++i)
    {
        mRecordBuffers[i].resize(mClip->GetMaxRecordSize());
        mFreeRecordBuffers.push_back(int32_t(i));
    }

    return true;
}

void VideoStream::FreeBuffers()
{
    for (Slot& slot : mSlots)
    {
        for (uint32_t p = 0; p < 3; ++p)
        {
            if (slot.mPlanes[p] != nullptr)
            {
                SYS_AlignedFree(slot.mPlanes[p]);
            }
        }
    }

    mSlots.clear();
    mFreeSlots.clear();

    mRecordBuffers.clear();
    mRecordBuffers.shrink_to_fit();
    mFreeRecordBuffers.clear();
    mPendingRecords.clear();

    for (uint32_t p = 0; p < 3; ++p)
    {
        if (mScratchPlanes[p] != nullptr)
        {
            SYS_AlignedFree(mScratchPlanes[p]);
            mScratchPlanes[p] = nullptr;
        }
    }

    delete mJpeg;
    mJpeg = nullptr;
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
    mConsecutiveFailures = 0;
    mPlaybackTime = record / frameRate;
    ClearQueuesLocked();

    return mPlaybackTime;
}

void VideoStream::SetPlaybackTime(double seconds)
{
    if (mMutex == nullptr)
    {
        return;
    }

    SCOPED_LOCK(mMutex);
    mPlaybackTime = seconds;
}

VideoStreamStats VideoStream::GetStats()
{
    if (mMutex == nullptr)
    {
        return VideoStreamStats();
    }

    SCOPED_LOCK(mMutex);
    VideoStreamStats stats = mStats;
    stats.mQueuedFrames = uint32_t(mFrames.size());
    return stats;
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
            ReleaseSlotLocked(outFrame.mSlot);
        }

        outFrame = mFrames.front();
        mFrames.pop_front();
        found = true;
    }

    return found;
}

void VideoStream::ReleaseFrame(Frame& frame)
{
    if (mMutex != nullptr && frame.mSlot >= 0)
    {
        SCOPED_LOCK(mMutex);
        ReleaseSlotLocked(frame.mSlot);
    }

    frame = Frame();
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

uint32_t VideoStream::TakeDroppedFrames()
{
    if (mMutex == nullptr)
    {
        return 0;
    }

    SCOPED_LOCK(mMutex);
    const uint32_t dropped = mDroppedFrames;
    mDroppedFrames = 0;
    return dropped;
}

ThreadFuncRet VideoStream::ThreadMain(void* arg)
{
#if PLATFORM_DOLPHIN
    // The main thread runs at priority 64. Decoding below it means the decoder only
    // gets time the main thread spends blocked (e.g. waiting for vsync), so video
    // never stalls rendering.
    LWP_SetThreadPriority(LWP_GetSelf(), 40);
#endif

    static_cast<VideoStream*>(arg)->WorkerLoop();

    THREAD_RETURN();
}

void VideoStream::WorkerLoop()
{
    while (true)
    {
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
        }

        // Read before decoding, so audio keeps flowing even when decoding falls behind.
        const bool read = ReadNextRecord();
        const bool decoded = DecodeNextFrame();

        if (!read && !decoded)
        {
            SYS_Sleep(2);
        }
    }
}

bool VideoStream::ReadNextRecord()
{
    uint32_t index = 0;
    uint32_t generation = 0;
    int32_t bufferIndex = -1;

    {
        SCOPED_LOCK(mMutex);

        if (mError || mFreeRecordBuffers.empty() || mNextRecord >= mClip->GetNumFrames())
        {
            return false;
        }

        index = mNextRecord;
        generation = mGeneration;
        bufferIndex = mFreeRecordBuffers.back();
        mFreeRecordBuffers.pop_back();
    }

    std::vector<uint8_t>& buffer = mRecordBuffers[bufferIndex];

    const uint64_t readStartUs = SYS_GetTimeMicroseconds();
    const uint32_t recordSize = mClip->GetRecordSize(index);
    const bool readOk = recordSize >= 8 &&
                        recordSize <= buffer.size() &&
                        mClip->ReadRecord(index, buffer.data());
    const uint64_t readEndUs = SYS_GetTimeMicroseconds();

    uint32_t audioBytes = 0;
    uint32_t jpegBytes = 0;
    bool valid = false;

    if (readOk)
    {
        audioBytes = ReadU32LE(buffer.data());
        jpegBytes = ReadU32LE(buffer.data() + 4);
        valid = (uint64_t(8) + audioBytes + jpegBytes) <= recordSize;
    }

    SCOPED_LOCK(mMutex);

    if (generation != mGeneration)
    {
        // A seek happened while reading; this record belongs to the old position.
        mFreeRecordBuffers.push_back(bufferIndex);
        return true;
    }

    mNextRecord = index + 1;

    if (!valid)
    {
        mFreeRecordBuffers.push_back(bufferIndex);
        RecordFailureLocked();
        return true;
    }

    const float readMs = float(double(readEndUs - readStartUs) / 1000.0);
    mStats.mReadMs += (readMs - mStats.mReadMs) * 0.1f;

    mAudio.insert(mAudio.end(), buffer.data() + 8, buffer.data() + 8 + audioBytes);

    PendingRecord pending;
    pending.mIndex = index;
    pending.mBuffer = bufferIndex;
    pending.mJpegOffset = 8 + audioBytes;
    pending.mJpegSize = jpegBytes;
    mPendingRecords.push_back(pending);

    return true;
}

bool VideoStream::DecodeNextFrame()
{
    PendingRecord pending;
    int32_t slotIndex = -1;
    uint32_t generation = 0;
    double frameRate = 0.0;

    {
        SCOPED_LOCK(mMutex);

        if (mPendingRecords.empty())
        {
            if (mNextRecord >= mClip->GetNumFrames())
            {
                mDecodedAll = true;
            }
            return false;
        }

        if (mFrames.size() >= kMaxQueuedFrames)
        {
            return false;
        }

        frameRate = mClip->GetFrameRate();
        const double frameDuration = 1.0 / frameRate;
        pending = mPendingRecords.front();

        // A frame already a whole frame behind playback would be dropped as soon as
        // it arrived, so don't spend time decoding it. Its audio was delivered on read.
        if (pending.mIndex * frameDuration + frameDuration < mPlaybackTime)
        {
            mPendingRecords.pop_front();
            mFreeRecordBuffers.push_back(pending.mBuffer);
            mStats.mLateFrames++;
            return true;
        }

        if (mFreeSlots.empty())
        {
            return false;
        }

        mPendingRecords.pop_front();
        slotIndex = mFreeSlots.back();
        mFreeSlots.pop_back();
        generation = mGeneration;
    }

    const uint64_t decodeStartUs = SYS_GetTimeMicroseconds();
    const bool decoded = DecodeFrame(
        mRecordBuffers[pending.mBuffer].data() + pending.mJpegOffset,
        pending.mJpegSize,
        mSlots[slotIndex]);
    const uint64_t decodeEndUs = SYS_GetTimeMicroseconds();

    SCOPED_LOCK(mMutex);

    mFreeRecordBuffers.push_back(pending.mBuffer);

    if (generation != mGeneration)
    {
        // A seek happened while decoding; this frame belongs to the old position.
        ReleaseSlotLocked(slotIndex);
        return true;
    }

    if (!decoded)
    {
        // Skip the frame but keep going; the last good frame stays on screen.
        ReleaseSlotLocked(slotIndex);
        RecordFailureLocked();
        return true;
    }

    mConsecutiveFailures = 0;

    const float decodeMs = float(double(decodeEndUs - decodeStartUs) / 1000.0);
    mStats.mDecodeMs += (decodeMs - mStats.mDecodeMs) * 0.1f;
    mStats.mDecodedFrames++;

    Frame frame;
    frame.mSlot = slotIndex;
    frame.mTime = pending.mIndex / frameRate;
    for (uint32_t p = 0; p < 3; ++p)
    {
        frame.mPlanes[p] = mSlots[slotIndex].mPlanes[p];
    }

    mFrames.push_back(frame);
    return true;
}

bool VideoStream::DecodeFrame(const uint8_t* jpeg, uint32_t jpegSize, Slot& slot)
{
    switch (mFormat)
    {
    case VideoFrameFormat::GxYuv420:
    {
        JpegYuvPlanes planes;
        planes.mY = slot.mPlanes[0];
        planes.mCb = slot.mPlanes[1];
        planes.mCr = slot.mPlanes[2];
        planes.mGxLayout = true;

        if (!mJpeg->Decode(jpeg, jpegSize, mWidth, mHeight, planes))
        {
            return false;
        }
        break;
    }

    case VideoFrameFormat::GxRgba8:
    {
        JpegYuvPlanes planes;
        planes.mY = mScratchPlanes[0];
        planes.mCb = mScratchPlanes[1];
        planes.mCr = mScratchPlanes[2];

        if (!mJpeg->Decode(jpeg, jpegSize, mWidth, mHeight, planes))
        {
            return false;
        }

        ConvertYuvToGxRgba8(planes.mY, planes.mCb, planes.mCr, mWidth, mHeight, slot.mPlanes[0]);
        break;
    }

    case VideoFrameFormat::Rgba8:
    default:
    {
#if PLATFORM_DOLPHIN
        return false;
#else
        int decWidth = 0;
        int decHeight = 0;
        int decComps = 0;
        stbi_uc* pixels = stbi_load_from_memory(jpeg, int(jpegSize), &decWidth, &decHeight, &decComps, 4);

        if (pixels == nullptr)
        {
            return false;
        }

        const bool sizeMatches = uint32_t(decWidth) == mWidth && uint32_t(decHeight) == mHeight;
        if (sizeMatches)
        {
            memcpy(slot.mPlanes[0], pixels, mWidth * mHeight * 4);
        }

        stbi_image_free(pixels);

        if (!sizeMatches)
        {
            return false;
        }
#endif
        break;
    }
    }

#if PLATFORM_DOLPHIN
    // The GPU reads these buffers directly.
    for (uint32_t p = 0; p < 3; ++p)
    {
        if (slot.mPlanes[p] != nullptr)
        {
            DCFlushRange(slot.mPlanes[p], slot.mSizes[p]);
        }
    }
#endif

    return true;
}

void VideoStream::ReleaseSlotLocked(int32_t slot)
{
    if (slot >= 0)
    {
        mFreeSlots.push_back(slot);
    }
}

void VideoStream::RecordFailureLocked()
{
    mDroppedFrames++;
    mStats.mFailedFrames++;

    if (++mConsecutiveFailures >= kMaxConsecutiveFailures)
    {
        mError = true;
    }
}

void VideoStream::ClearQueuesLocked()
{
    for (Frame& frame : mFrames)
    {
        ReleaseSlotLocked(frame.mSlot);
    }

    for (PendingRecord& pending : mPendingRecords)
    {
        mFreeRecordBuffers.push_back(pending.mBuffer);
    }

    mFrames.clear();
    mPendingRecords.clear();
    mAudio.clear();
}
