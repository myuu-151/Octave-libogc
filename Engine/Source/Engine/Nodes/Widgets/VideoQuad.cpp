#include "Nodes/Widgets/VideoQuad.h"

#include "Assets/VideoClip.h"
#include "Assets/Texture.h"
#include "Renderer.h"
#include "Log.h"

#include "Graphics/Graphics.h"

#if EDITOR
#include "EditorState.h"
#include "Viewport2d.h"
#endif

FORCE_LINK_DEF(VideoQuad);
DEFINE_NODE(VideoQuad, Quad);

bool VideoQuad::HandleVideoPropChange(Datum* datum, uint32_t index, const void* newValue)
{
    Property* prop = static_cast<Property*>(datum);
    OCT_ASSERT(prop != nullptr);
    VideoQuad* node = static_cast<VideoQuad*>(prop->mOwner);
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

VideoQuad::VideoQuad()
{
    SetName("Video");
}

VideoQuad::~VideoQuad()
{

}

void VideoQuad::Destroy()
{
    StopVideo();

    Quad::Destroy();
}

void VideoQuad::Start()
{
    Quad::Start();

    if (mAutoPlay)
    {
        PlayVideo();
    }
}

void VideoQuad::Stop()
{
    StopVideo();

    Quad::Stop();
}

void VideoQuad::Tick(float deltaTime)
{
    Quad::Tick(deltaTime);
    TickCommon();
}

void VideoQuad::EditorTick(float deltaTime)
{
    Quad::EditorTick(deltaTime);
    TickCommon();
}

void VideoQuad::GatherProperties(std::vector<Property>& outProps)
{
    // Skip Quad's Texture property: the texture comes from the video and is transient,
    // so it must not be saved with the scene.
    Widget::GatherProperties(outProps);

    {
        SCOPED_CATEGORY("Quad");
        outProps.push_back(Property(DatumType::Vector2D, "UV Scale", this, &mUvScale, 1, Quad::HandlePropChange));
        outProps.push_back(Property(DatumType::Vector2D, "UV Offset", this, &mUvOffset, 1, Quad::HandlePropChange));
    }

    SCOPED_CATEGORY("Video");

    outProps.push_back(Property(DatumType::Bool, "Play", this, &mPlaying, 1, HandleVideoPropChange));
    outProps.push_back(Property(DatumType::Asset, "Video Clip", this, &mVideoClip, 1, nullptr, int32_t(VideoClip::GetStaticType())));
    outProps.push_back(Property(DatumType::Bool, "Auto Play", this, &mAutoPlay));
    outProps.push_back(Property(DatumType::Bool, "Loop", this, &mLoop));
    outProps.push_back(Property(DatumType::Bool, "Audio Enabled", this, &mAudioEnabled));
    outProps.push_back(Property(DatumType::Float, "Volume", this, &mVolume));
    outProps.push_back(Property(DatumType::Bool, "Fill Screen", this, &mFillScreen));
    outProps.push_back(Property(DatumType::Bool, "GPU Color Conversion", this, &mGpuColorConversion));
}

void VideoQuad::PreRender()
{
    if (!mFillScreen)
    {
        Quad::PreRender();
        return;
    }

    // Lay out as usual, then draw over the whole screen instead. Only the computed
    // rect is replaced; the saved layout (anchor, offset, size) is never touched, so
    // turning Fill Screen off restores the widget's own placement.
    MarkDirty();
    Widget::PreRender();
    mRect = GetScreenRect();
    UpdateVertexData();
    GFX_UpdateQuadResourceVertexData(this);
}

Rect VideoQuad::GetScreenRect()
{
    // The area a top-level widget lays out in: the viewport, or in the 2D editor the
    // wrapper widget that frames the game screen (mirrors Widget::UpdateRect).
    glm::uvec4 vp = Renderer::Get()->GetViewport();
    Rect screenRect(0.0f, 0.0f, (float)vp.z, (float)vp.w);

#if EDITOR
    if (GetEditorState()->GetEditorMode() == EditorMode::Scene2D)
    {
        Widget* wrapper = GetEditorState()->GetViewport2D()->GetWrapperWidget();
        if (wrapper != nullptr && wrapper != this)
        {
            screenRect = wrapper->GetRect();
        }
    }
#endif

    return screenRect;
}

void VideoQuad::Render()
{
    if (mPlayer.GetTexture() != nullptr)
    {
        Quad::Render();
    }
    else
    {
        // No video open: don't draw an untextured (solid color) quad over the screen.
        Widget::Render();
    }
}

void VideoQuad::TickCommon()
{
    SyncPlayerSettings();
    mPlayer.Update(GetVideoClip());
    mPlaying = mPlayer.IsPlaying();
    ApplyVideoTexture();
}

void VideoQuad::SyncPlayerSettings()
{
    mPlayer.SetLoop(mLoop);
    mPlayer.SetVolume(mVolume);
    mPlayer.SetAudioEnabled(mAudioEnabled);

    // Widgets draw with their own TEV setup, so on GameCube/Wii frames can stay YUV
    // and be converted to RGB on the GPU. Off uses CPU conversion instead.
    mPlayer.SetUseYuv(mGpuColorConversion);
}

void VideoQuad::ApplyVideoTexture()
{
    Texture* texture = mPlayer.GetTexture();
    if (texture != GetTexture())
    {
        SetTexture(texture);
    }
}

void VideoQuad::SetVideoClip(VideoClip* clip)
{
    mVideoClip = clip;
}

VideoClip* VideoQuad::GetVideoClip()
{
    return mVideoClip.Get<VideoClip>();
}

void VideoQuad::PlayVideo()
{
    SyncPlayerSettings();
    mPlayer.Play(GetVideoClip());
    mPlaying = mPlayer.IsPlaying();
    ApplyVideoTexture();
}

void VideoQuad::PauseVideo()
{
    mPlayer.Pause();
    mPlaying = false;
}

void VideoQuad::StopVideo()
{
    mPlayer.Stop();
    mPlaying = false;
    ApplyVideoTexture();
}

void VideoQuad::SeekVideo(float seconds)
{
    SyncPlayerSettings();
    mPlayer.Seek(GetVideoClip(), seconds);
    ApplyVideoTexture();
}

bool VideoQuad::IsPlaying() const
{
    return mPlaying;
}

float VideoQuad::GetPlayTime() const
{
    return float(mPlayer.GetTime());
}

float VideoQuad::GetDuration()
{
    VideoClip* clip = GetVideoClip();
    return (clip != nullptr) ? clip->GetDuration() : 0.0f;
}

void VideoQuad::SetLoop(bool loop)
{
    mLoop = loop;
}

bool VideoQuad::GetLoop() const
{
    return mLoop;
}

void VideoQuad::SetAutoPlay(bool autoPlay)
{
    mAutoPlay = autoPlay;
}

bool VideoQuad::GetAutoPlay() const
{
    return mAutoPlay;
}

void VideoQuad::SetVolume(float volume)
{
    mVolume = volume;
}

float VideoQuad::GetVolume() const
{
    return mVolume;
}

void VideoQuad::SetAudioEnabled(bool enabled)
{
    mAudioEnabled = enabled;
}

bool VideoQuad::IsAudioEnabled() const
{
    return mAudioEnabled;
}

void VideoQuad::SetFillScreen(bool fillScreen)
{
    mFillScreen = fillScreen;
    MarkDirty();
}

bool VideoQuad::GetFillScreen() const
{
    return mFillScreen;
}
