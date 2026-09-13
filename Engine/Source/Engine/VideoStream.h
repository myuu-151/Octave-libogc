#pragma once

#include "System/SystemTypes.h"

#include <cstdint>
#include <deque>
#include <vector>

class VideoClip;

// Decodes a VideoClip on a worker thread. Each record's JPEG is decoded to RGBA8 and
// queued (a few frames deep) together with the PCM audio that plays under it. The
// owner pops frames by presentation time and drains audio on the main thread.
class VideoStream
{
public:

    struct Frame
    {
        uint8_t* mPixels = nullptr;     // RGBA8, width * height * 4. Release with FreeFrame().
        double mTime = 0.0;
    };

    VideoStream();
    ~VideoStream();

    bool Open(VideoClip* clip);
    void Close();

    // Restarts decoding at the frame nearest `seconds`. Returns that frame's time.
    double Seek(double seconds);

    // Pops the newest queued frame with a time <= maxTime, dropping older ones.
    bool PopFrame(double maxTime, Frame& outFrame);
    static void FreeFrame(Frame& frame);
    bool HasQueuedFrames();

    // Appends all decoded audio not yet taken to outAudio.
    void PopAudio(std::vector<uint8_t>& outAudio);

    // True once every frame has been decoded and popped.
    bool IsEndOfStream();
    bool HasError();

private:

    static ThreadFuncRet ThreadMain(void* arg);
    void WorkerLoop();
    void ClearQueues();

    VideoClip* mClip = nullptr;
    ThreadObject* mThread = nullptr;
    MutexObject* mMutex = nullptr;

    // Guarded by mMutex
    std::deque<Frame> mFrames;
    std::vector<uint8_t> mAudio;
    uint32_t mNextRecord = 0;
    uint32_t mGeneration = 0;
    int32_t mSeekRecord = -1;
    bool mDecodedAll = false;
    bool mError = false;
    bool mExit = false;
};
