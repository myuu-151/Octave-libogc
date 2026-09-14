#pragma once

#include "Nodes/Widgets/Quad.h"
#include "AssetRef.h"
#include "VideoPlayer.h"

class VideoClip;
class Text;

// A widget that plays a VideoClip. With Fill Screen on (the default) it covers the
// whole screen regardless of its parent or anchor, for fullscreen cutscenes; turn it
// off to size/anchor it like any Quad (e.g. picture-in-picture). Draws nothing while
// no video is open.
class VideoQuad : public Quad
{
public:

    DECLARE_NODE(VideoQuad, Quad);

    VideoQuad();
    virtual ~VideoQuad();

    virtual void Destroy() override;
    virtual void Start() override;
    virtual void Stop() override;
    virtual void Tick(float deltaTime) override;
    virtual void EditorTick(float deltaTime) override;

    virtual void GatherProperties(std::vector<Property>& outProps) override;
    virtual void PreRender() override;
    virtual void Render() override;

    void SetVideoClip(VideoClip* clip);
    VideoClip* GetVideoClip();

    void PlayVideo();
    void PauseVideo();
    void StopVideo();
    void SeekVideo(float seconds);

    bool IsPlaying() const;
    float GetPlayTime() const;
    float GetDuration();

    void SetLoop(bool loop);
    bool GetLoop() const;

    void SetAutoPlay(bool autoPlay);
    bool GetAutoPlay() const;

    void SetVolume(float volume);
    float GetVolume() const;

    void SetAudioEnabled(bool enabled);
    bool IsAudioEnabled() const;

    void SetFillScreen(bool fillScreen);
    bool GetFillScreen() const;

    // Draws decoder timing and buffering over the video, to diagnose stutter.
    void SetShowStats(bool showStats);
    bool GetShowStats() const;

    static bool HandleVideoPropChange(Datum* datum, uint32_t index, const void* newValue);

protected:

    void TickCommon();
    void SyncPlayerSettings();
    void ApplyVideoTexture();
    Rect GetScreenRect();
    void UpdateStatsOverlay();
    void RemoveStatsOverlay();

    // Properties
    AssetRef mVideoClip;
    bool mAutoPlay = true;
    bool mLoop = false;
    bool mAudioEnabled = true;
    bool mFillScreen = true;
    bool mGpuColorConversion = true;
    bool mShowStats = false;
    float mVolume = 1.0f;

    // State
    VideoPlayer mPlayer;
    bool mPlaying = false;
    Text* mStatsText = nullptr;     // Transient child, never saved
    uint64_t mStatsUpdateUs = 0;
};
