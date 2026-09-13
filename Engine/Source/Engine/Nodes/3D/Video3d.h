#pragma once

#include "Nodes/3D/StaticMesh3d.h"
#include "AssetRef.h"
#include "VideoPlayer.h"

class VideoClip;
class Texture;

// A static mesh that plays a VideoClip on its surface. While a video is open, the
// node's material is replaced by a copy (MaterialLite) whose texture slot 0 is the
// video frame, so any mesh can be a screen. For 2D / fullscreen video use VideoQuad.
class Video3D : public StaticMesh3D
{
public:

    DECLARE_NODE(Video3D, StaticMesh3D);

    Video3D();
    ~Video3D();

    virtual const char* GetTypeName() const override;
    virtual void GatherProperties(std::vector<Property>& outProps) override;

    virtual void Create() override;
    virtual void Destroy() override;
    virtual void Start() override;
    virtual void Stop() override;
    virtual void Tick(float deltaTime) override;
    virtual void EditorTick(float deltaTime) override;

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

    void SetUnlit(bool unlit);
    bool IsUnlit() const;

    // The texture receiving video frames (null until the video is opened). Can be
    // assigned to other materials or widgets from script.
    Texture* GetVideoTexture();

    static bool HandlePropChange(Datum* datum, uint32_t index, const void* newValue);

protected:

    void TickCommon();
    void SyncPlayerSettings();
    void ApplyVideoTexture();
    void RestoreMaterial();

    // Properties
    AssetRef mVideoClip;
    bool mAutoPlay = true;
    bool mLoop = false;
    bool mAudioEnabled = true;
    bool mUnlit = true;
    float mVolume = 1.0f;

    // State
    VideoPlayer mPlayer;
    Texture* mAppliedTexture = nullptr;
    MaterialRef mVideoMaterial;
    MaterialRef mPrevMaterialOverride;
    bool mPlaying = false;
};
