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

// Decoder timing and counts, for diagnosing playback performance.
struct VideoStreamStats
{
    float mReadMs = 0.0f;           // Recent average time to read a record
    float mDecodeMs = 0.0f;         // Recent average time to decode a frame
    uint32_t mDecodedFrames = 0;
    uint32_t mLateFrames = 0;       // Not decoded because they were already late
    uint32_t mFailedFrames = 0;     // Couldn't be read or decoded
    uint32_t mQueuedFrames = 0;
};

// Streams a VideoClip on a worker thread. Records are read ahead of playback into a
// small queue, delivering their audio right away, and their frames are decoded from
// that queue into buffers allocated once when the video opens. Reading doesn't wait
// on decoding, so audio keeps flowing when the CPU can't decode every frame: frames
// that are already late are skipped instead. A record that can't be read or decoded
// is skipped rather than stopping playback.
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

    // The current playback time, used to skip decoding frames that are already late.
    void SetPlaybackTime(double seconds);

    // Pops the newest queued frame with a time <= maxTime, dropping older ones. The
    // frame's buffers stay valid until it is passed to ReleaseFrame().
    bool PopFrame(double maxTime, Frame& outFrame);
    void ReleaseFrame(Frame& frame);
    bool HasQueuedFrames();

    // Appends all audio read but not yet taken to outAudio.
    void PopAudio(std::vector<uint8_t>& outAudio);

    // True once every frame has been decoded (or skipped) and popped.
    bool IsEndOfStream();

    // True if too many records in a row failed to load or decode.
    bool HasError();

    // Frames that failed to load or decode since the last call.
    uint32_t TakeDroppedFrames();

    VideoStreamStats GetStats();

private:

    struct Slot
    {
        uint8_t* mPlanes[3] = { nullptr, nullptr, nullptr };
        uint32_t mSizes[3] = { 0, 0, 0 };
    };

    // A record that has been read, whose frame hasn't been decoded yet.
    struct PendingRecord
    {
        uint32_t mIndex = 0;
        int32_t mBuffer = -1;
        uint32_t mJpegOffset = 0;
        uint32_t mJpegSize = 0;
    };

    static ThreadFuncRet ThreadMain(void* arg);
    void WorkerLoop();
    bool ReadNextRecord();      // Returns true if it did any work
    bool DecodeNextFrame();     // Returns true if it did any work
    bool DecodeFrame(const uint8_t* jpeg, uint32_t jpegSize, Slot& slot);
    bool AllocateBuffers();
    void FreeBuffers();
    void ReleaseSlotLocked(int32_t slot);
    void RecordFailureLocked();
    void ClearQueuesLocked();

    VideoClip* mClip = nullptr;
    VideoFrameFormat mFormat = VideoFrameFormat::Rgba8;
    uint32_t mWidth = 0;
    uint32_t mHeight = 0;

    ThreadObject* mThread = nullptr;
    MutexObject* mMutex = nullptr;

    // Worker-only
    JpegYuvDecoder* mJpeg = nullptr;
    uint8_t* mScratchPlanes[3] = { nullptr, nullptr, nullptr };

    std::vector<Slot> mSlots;
    std::vector<std::vector<uint8_t>> mRecordBuffers;

    // Guarded by mMutex
    std::vector<int32_t> mFreeSlots;
    std::vector<int32_t> mFreeRecordBuffers;
    std::deque<PendingRecord> mPendingRecords;
    std::deque<Frame> mFrames;
    std::vector<uint8_t> mAudio;
    uint32_t mNextRecord = 0;
    uint32_t mGeneration = 0;
    int32_t mSeekRecord = -1;
    uint32_t mDroppedFrames = 0;
    uint32_t mConsecutiveFailures = 0;
    double mPlaybackTime = 0.0;
    VideoStreamStats mStats;
    bool mDecodedAll = false;
    bool mError = false;
    bool mExit = false;
};
