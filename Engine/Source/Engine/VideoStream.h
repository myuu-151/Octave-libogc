#pragma once

#include "System/SystemTypes.h"

#include <cstdint>
#include <deque>
#include <vector>

class VideoClip;
class JpegYuvDecoder;

enum class VideoFrameFormat : uint8_t
{
    Rgba8,      // RGBA8 rows, uploaded by copying (Vulkan)
    GxRgba8,    // GX_TF_RGBA8 texel layout, used directly as a GameCube texture
    GxYuv420    // Y, Cb, Cr GX_TF_I8 planes, converted to RGB by TEV when drawn
};

// Decodes a VideoClip on a worker thread into frame buffers allocated once when the
// video opens. Each record's frame is queued (a few deep) together with the PCM
// audio that plays under it. The owner pops frames by presentation time and drains
// audio on the main thread. A record that can't be read or decoded is skipped (its
// audio is kept) instead of stopping playback.
class VideoStream
{
public:

    struct Frame
    {
        uint8_t* mPlanes[3] = { nullptr, nullptr, nullptr };    // RGBA formats use mPlanes[0]
        double mTime = 0.0;
        int32_t mSlot = -1;
    };

    VideoStream();
    ~VideoStream();

    bool Open(VideoClip* clip, VideoFrameFormat format);
    void Close();

    // Restarts decoding at the frame nearest `seconds`. Returns that frame's time.
    double Seek(double seconds);

    // Pops the newest queued frame with a time <= maxTime, dropping older ones. The
    // frame's buffers stay valid until it is passed to ReleaseFrame().
    bool PopFrame(double maxTime, Frame& outFrame);
    void ReleaseFrame(Frame& frame);
    bool HasQueuedFrames();

    // Appends all decoded audio not yet taken to outAudio.
    void PopAudio(std::vector<uint8_t>& outAudio);

    // True once every frame has been decoded and popped.
    bool IsEndOfStream();

    // True if too many records in a row failed to load or decode.
    bool HasError();

    // Frames skipped since the last call.
    uint32_t TakeDroppedFrames();

private:

    struct Slot
    {
        uint8_t* mPlanes[3] = { nullptr, nullptr, nullptr };
        uint32_t mSizes[3] = { 0, 0, 0 };
    };

    static ThreadFuncRet ThreadMain(void* arg);
    void WorkerLoop();
    bool DecodeFrame(const uint8_t* jpeg, uint32_t jpegSize, Slot& slot);
    bool AllocateBuffers();
    void FreeBuffers();
    void ReleaseSlotLocked(int32_t slot);
    void ClearQueuesLocked();

    VideoClip* mClip = nullptr;
    VideoFrameFormat mFormat = VideoFrameFormat::Rgba8;
    uint32_t mWidth = 0;
    uint32_t mHeight = 0;

    ThreadObject* mThread = nullptr;
    MutexObject* mMutex = nullptr;

    // Worker-only
    JpegYuvDecoder* mJpeg = nullptr;
    std::vector<uint8_t> mRecord;
    uint8_t* mScratchPlanes[3] = { nullptr, nullptr, nullptr };

    std::vector<Slot> mSlots;

    // Guarded by mMutex
    std::vector<int32_t> mFreeSlots;
    std::deque<Frame> mFrames;
    std::vector<uint8_t> mAudio;
    uint32_t mNextRecord = 0;
    uint32_t mGeneration = 0;
    int32_t mSeekRecord = -1;
    uint32_t mDroppedFrames = 0;
    uint32_t mConsecutiveFailures = 0;
    bool mDecodedAll = false;
    bool mError = false;
    bool mExit = false;
};
