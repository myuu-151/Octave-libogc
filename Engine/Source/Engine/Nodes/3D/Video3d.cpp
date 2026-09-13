#include "Nodes/3D/Video3d.h"

#include "Assets/VideoClip.h"
#include "Assets/Texture.h"
#include "Assets/MaterialLite.h"
#include "Assets/StaticMesh.h"
#include "AssetManager.h"
#include "Engine.h"
#include "Log.h"

FORCE_LINK_DEF(Video3D);
DEFINE_NODE(Video3D, StaticMesh3D);

bool Video3D::HandlePropChange(Datum* datum, uint32_t index, const void* newValue)
{
    Property* prop = static_cast<Property*>(datum);
    OCT_ASSERT(prop != nullptr);
    Video3D* node = static_cast<Video3D*>(prop->mOwner);
    bool success = false;

    if (prop->mName == "Play")
    {
        if (*(bool*)newValue)
        {
            node->PlayVideo();
        }
        else
        {
            node->StopVideo();
        }

        success = true;
    }

    return success;
}

Video3D::Video3D()
{
    mName = "Video";
}

Video3D::~Video3D()
{

}

const char* Video3D::GetTypeName() const
{
    return "Video";
}

void Video3D::GatherProperties(std::vector<Property>& outProps)
{
    StaticMesh3D::GatherProperties(outProps);

    SCOPED_CATEGORY("Video");

    outProps.push_back(Property(DatumType::Bool, "Play", this, &mPlaying, 1, HandlePropChange));
    outProps.push_back(Property(DatumType::Asset, "Video Clip", this, &mVideoClip, 1, nullptr, int32_t(VideoClip::GetStaticType())));
    outProps.push_back(Property(DatumType::Bool, "Auto Play", this, &mAutoPlay));
    outProps.push_back(Property(DatumType::Bool, "Loop", this, &mLoop));
    outProps.push_back(Property(DatumType::Bool, "Audio Enabled", this, &mAudioEnabled));
    outProps.push_back(Property(DatumType::Float, "Volume", this, &mVolume));
    outProps.push_back(Property(DatumType::Bool, "Unlit", this, &mUnlit));
}

void Video3D::Create()
{
    StaticMesh3D::Create();

    if (GetStaticMesh() == nullptr)
    {
        SetStaticMesh(LoadAsset<StaticMesh>("SM_Plane"));
    }
}

void Video3D::Destroy()
{
    StopVideo();

    StaticMesh3D::Destroy();
}

void Video3D::Start()
{
    StaticMesh3D::Start();

    if (mAutoPlay)
    {
        PlayVideo();
    }
}

void Video3D::Stop()
{
    StopVideo();

    StaticMesh3D::Stop();
}

void Video3D::Tick(float deltaTime)
{
    StaticMesh3D::Tick(deltaTime);
    TickCommon();
}

void Video3D::EditorTick(float deltaTime)
{
    StaticMesh3D::EditorTick(deltaTime);
    TickCommon();
}

void Video3D::TickCommon()
{
    SyncPlayerSettings();
    mPlayer.Update(GetVideoClip());
    mPlaying = mPlayer.IsPlaying();
    ApplyVideoTexture();
}

void Video3D::SyncPlayerSettings()
{
    mPlayer.SetLoop(mLoop);
    mPlayer.SetVolume(mVolume);
    mPlayer.SetAudioEnabled(mAudioEnabled);
}

void Video3D::ApplyVideoTexture()
{
    Texture* texture = mPlayer.GetTexture();
    if (texture == mAppliedTexture)
    {
        return;
    }

    RestoreMaterial();

    if (texture != nullptr)
    {
        // Show the video through a copy of the current material, keeping its settings.
        mPrevMaterialOverride = GetMaterialOverride();
        MaterialLite* material = MaterialLite::New(GetMaterial());
        material->SetTexture(0, texture);
        if (mUnlit)
        {
            material->SetShadingModel(ShadingModel::Unlit);
        }
        mVideoMaterial = material;
        SetMaterialOverride(material);
    }

    mAppliedTexture = texture;
}

void Video3D::RestoreMaterial()
{
    if (mVideoMaterial.Get() != nullptr)
    {
        // Put back the material we replaced, so the transient video material never
        // gets saved with the scene.
        if (GetMaterialOverride() == mVideoMaterial.Get<Material>())
        {
            SetMaterialOverride(mPrevMaterialOverride.Get<Material>());
        }

        mVideoMaterial = nullptr;
    }

    mPrevMaterialOverride = nullptr;
}

void Video3D::SetVideoClip(VideoClip* clip)
{
    mVideoClip = clip;
}

VideoClip* Video3D::GetVideoClip()
{
    return mVideoClip.Get<VideoClip>();
}

void Video3D::PlayVideo()
{
    SyncPlayerSettings();
    mPlayer.Play(GetVideoClip());
    mPlaying = mPlayer.IsPlaying();
    ApplyVideoTexture();
}

void Video3D::PauseVideo()
{
    mPlayer.Pause();
    mPlaying = false;
}

void Video3D::StopVideo()
{
    mPlayer.Stop();
    mPlaying = false;
    ApplyVideoTexture();
}

void Video3D::SeekVideo(float seconds)
{
    SyncPlayerSettings();
    mPlayer.Seek(GetVideoClip(), seconds);
    ApplyVideoTexture();
}

bool Video3D::IsPlaying() const
{
    return mPlaying;
}

float Video3D::GetPlayTime() const
{
    return float(mPlayer.GetTime());
}

float Video3D::GetDuration()
{
    VideoClip* clip = GetVideoClip();
    return (clip != nullptr) ? clip->GetDuration() : 0.0f;
}

void Video3D::SetLoop(bool loop)
{
    mLoop = loop;
}

bool Video3D::GetLoop() const
{
    return mLoop;
}

void Video3D::SetAutoPlay(bool autoPlay)
{
    mAutoPlay = autoPlay;
}

bool Video3D::GetAutoPlay() const
{
    return mAutoPlay;
}

void Video3D::SetVolume(float volume)
{
    mVolume = volume;
}

float Video3D::GetVolume() const
{
    return mVolume;
}

void Video3D::SetAudioEnabled(bool enabled)
{
    mAudioEnabled = enabled;
}

bool Video3D::IsAudioEnabled() const
{
    return mAudioEnabled;
}

void Video3D::SetUnlit(bool unlit)
{
    mUnlit = unlit;
}

bool Video3D::IsUnlit() const
{
    return mUnlit;
}

Texture* Video3D::GetVideoTexture()
{
    return mPlayer.GetTexture();
}
