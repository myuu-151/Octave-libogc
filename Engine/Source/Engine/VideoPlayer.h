#pragma once

#include "AssetRef.h"
#include "VideoStream.h"

#include <cstdint>
#include <vector>

class VideoClip;
class Texture;

// Plays a VideoClip: runs a VideoStream decoder, shows the current frame in a
// dynamic texture, streams the audio, and keeps the two in sync. Shared by the
// nodes that display video (Video3D on a mesh, VideoQuad as a widget), which just
// decide where GetTexture() is shown.
class VideoPlayer
{
public:

    VideoPlayer();
    ~VideoPlayer();

    // Advances playback. Call once per tick with the clip that should be playing;
    // a different or recooked clip reopens the video.
    void Update(VideoClip* clip);

    void Play(VideoClip* clip);
    void Pause();
    void Stop();
    void Seek(VideoClip* clip, double seconds);

    bool IsPlaying() const;
    double GetTime() const;

    // The texture receiving frames; null while no video is open. Stays valid (showing
    // the last frame) when paused or finished.
    Texture* GetTexture() const;

    void SetLoop(bool loop);
    void SetVolume(float volume);
    void SetAudioEnabled(bool enabled);   // Applies the next time a video opens

    // On GameCube/Wii, output YUV frames that the GPU converts to RGB (fastest; only
    // widgets can draw them). Otherwise frames are RGBA. Changing it reopens the video.
    void SetUseYuv(bool useYuv);

    // Decoder timing and how much audio is queued ahead of playback. Returns false
    // while no video is open.
    bool GetStats(VideoStreamStats& outStats, float& outAudioBufferedMs);

private:

    bool Open(VideoClip* clip);
    void Close();
    void SeekInternal(double seconds);
    void ReleaseDisplayedFrame();

    bool mLoop = false;
    float mVolume = 1.0f;
    bool mAudioEnabled = true;
    bool mUseYuv = false;

    VideoStream* mStream = nullptr;
    VideoClip* mOpenClip = nullptr;
    uint32_t mOpenRevision = 0;
    TextureRef mTexture;
    VideoStream::Frame mDisplayedFrame;
    uint32_t mAudioStream = 0;
    uint64_t mAudioSubmittedFrames = 0;     // Since open / last flush
    uint64_t mLogUs = 0;                    // Last SD diagnostic log line (GameCube/Wii)
    std::vector<uint8_t> mAudioScratch;

    double mTime = 0.0;
    double mAudioStartTime = 0.0;
    uint64_t mLastTickUs = 0;
    uint64_t mLastAudioPlayed = 0;
    uint64_t mAudioActiveUs = 0;
    bool mPlaying = false;
    bool mClockRunning = false;
};
