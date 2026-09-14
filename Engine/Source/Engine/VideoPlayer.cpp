#include "VideoPlayer.h"

#include "Assets/VideoClip.h"
#include "Assets/Texture.h"
#include "VideoStream.h"
#include "AssetManager.h"
#include "Log.h"

#include "Audio/Audio.h"
#include "Graphics/Graphics.h"
#include "System/System.h"

#if PLATFORM_DOLPHIN
// Local SD diagnostic log (System_Dolphin.cpp). A no-op unless the local logger is enabled.
void OctLog(const char* format, ...);
#endif

// Longest step the playback clock takes in one tick (e.g. after a hitch).
static constexpr double kMaxTickSeconds = 0.25;

// If the video clock drifts this far from the audio clock, jump to the audio clock.
// Kept loose because some audio backends only report progress per buffer.
static constexpr double kAudioResyncSeconds = 0.4;

VideoPlayer::VideoPlayer()
{

}

VideoPlayer::~VideoPlayer()
{
    if (mStream != nullptr)
    {
        mStream->Close();
        delete mStream;
        mStream = nullptr;
    }
}

void VideoPlayer::Update(VideoClip* clip)
{
    // Advance by real time rather than the tick's delta so time dilation doesn't
    // change playback speed and an extra update in a frame doesn't double-step.
    const uint64_t nowUs = SYS_GetTimeMicroseconds();
    double deltaTime = (mLastTickUs != 0 && nowUs > mLastTickUs) ? double(nowUs - mLastTickUs) / 1000000.0 : 0.0;
    deltaTime = glm::min(deltaTime, kMaxTickSeconds);
    mLastTickUs = nowUs;

    // Reopen if the clip was swapped or recooked while open.
    if (mStream != nullptr &&
        (clip != mOpenClip || clip == nullptr || clip->GetRevision() != mOpenRevision))
    {
        Close();
    }

    if (!mPlaying)
    {
        return;
    }

    if (mStream == nullptr && !Open(clip))
    {
        mPlaying = false;
        return;
    }

    const uint32_t dropped = mStream->TakeDroppedFrames();
    if (dropped > 0)
    {
        LogWarning("Video %s: skipped %u frame(s) that failed to load or decode", clip->GetName().c_str(), dropped);
    }

    if (mStream->HasError())
    {
        LogError("Video %s: too many frames failed to load or decode; stopping", clip->GetName().c_str());
        Stop();
        return;
    }

    mAudioScratch.clear();
    mStream->PopAudio(mAudioScratch);

    if (mAudioStream != 0)
    {
        if (!mAudioScratch.empty())
        {
            AUD_QueueStreamData(mAudioStream, mAudioScratch.data(), uint32_t(mAudioScratch.size()));
            mAudioSubmittedFrames += mAudioScratch.size() / (2 * clip->GetAudioNumChannels());
        }

        AUD_SetStreamVolume(mAudioStream, mVolume);
    }

    if (!mClockRunning)
    {
        // Hold the clock until the first frame is decoded so playback starts in sync.
        if (!mStream->HasQueuedFrames())
        {
            return;
        }

        mClockRunning = true;
        deltaTime = 0.0;

        if (mAudioStream != 0)
        {
            AUD_SetStreamPaused(mAudioStream, false);
        }
    }

    mTime += deltaTime;

    if (mAudioStream != 0)
    {
        const uint64_t played = AUD_GetStreamPlayedFrames(mAudioStream);

        if (played != mLastAudioPlayed)
        {
            mLastAudioPlayed = played;
            mAudioActiveUs = nowUs;
        }

        // Only follow the audio clock while audio is actually advancing, so a
        // soundtrack shorter than the video doesn't freeze the picture.
        const bool audioActive = played > 0 && (nowUs - mAudioActiveUs) < 500000;

        if (audioActive)
        {
            const double audioTime = mAudioStartTime + double(played) / clip->GetAudioSampleRate();

            if (glm::abs(audioTime - mTime) > kAudioResyncSeconds)
            {
                mTime = audioTime;
            }
        }
    }

    mStream->SetPlaybackTime(mTime);

    VideoStream::Frame frame;
    if (mStream->PopFrame(mTime, frame))
    {
        Texture* texture = mTexture.Get<Texture>();

#if PLATFORM_DOLPHIN
        // Zero copy: point the texture at the decoded frame's buffers. The frame on
        // screen is released only once it has been replaced.
        if (texture != nullptr)
        {
            GFX_SetTextureResourceData(texture, frame.mPlanes);
        }

        ReleaseDisplayedFrame();
        mDisplayedFrame = frame;
#else
        if (texture != nullptr)
        {
            texture->UpdatePixels(frame.mPlanes[0]);
        }

        mStream->ReleaseFrame(frame);
#endif
    }

#if PLATFORM_DOLPHIN
    // Once a second, log decoder timing and buffering to the SD diagnostic log.
    if (mLogUs == 0 || nowUs - mLogUs >= 1000000)
    {
        mLogUs = nowUs;

        VideoStreamStats stats;
        float audioBufferedMs = 0.0f;
        if (GetStats(stats, audioBufferedMs))
        {
            OctLog("VIDEO t=%.2f read=%.1fms decode=%.1fms decoded=%u late=%u failed=%u queued=%u audio=%.0fms",
                mTime,
                stats.mReadMs,
                stats.mDecodeMs,
                unsigned(stats.mDecodedFrames),
                unsigned(stats.mLateFrames),
                unsigned(stats.mFailedFrames),
                unsigned(stats.mQueuedFrames),
                audioBufferedMs);
        }
    }
#endif

    if (mStream->IsEndOfStream() && mTime >= clip->GetDuration())
    {
        if (mLoop)
        {
            SeekInternal(0.0);
        }
        else
        {
            // Hold the last frame.
            mPlaying = false;
            mClockRunning = false;

            if (mAudioStream != 0)
            {
                AUD_SetStreamPaused(mAudioStream, true);
            }
        }
    }
}

void VideoPlayer::Play(VideoClip* clip)
{
    if (mStream == nullptr && !Open(clip))
    {
        mPlaying = false;
        return;
    }

    if (mStream->IsEndOfStream() && mTime >= mOpenClip->GetDuration())
    {
        // Finished: start over.
        SeekInternal(0.0);
    }

    mPlaying = true;
    mLastTickUs = 0;

    if (mClockRunning && mAudioStream != 0)
    {
        AUD_SetStreamPaused(mAudioStream, false);
    }
}

void VideoPlayer::Pause()
{
    mPlaying = false;

    if (mAudioStream != 0)
    {
        AUD_SetStreamPaused(mAudioStream, true);
    }
}

void VideoPlayer::Stop()
{
    Close();
    mPlaying = false;
    mTime = 0.0;
}

void VideoPlayer::Seek(VideoClip* clip, double seconds)
{
    if (mStream == nullptr && !Open(clip))
    {
        return;
    }

    SeekInternal(glm::max(seconds, 0.0));
}

bool VideoPlayer::IsPlaying() const
{
    return mPlaying;
}

double VideoPlayer::GetTime() const
{
    return mTime;
}

Texture* VideoPlayer::GetTexture() const
{
    return mTexture.Get<Texture>();
}

void VideoPlayer::SetLoop(bool loop)
{
    mLoop = loop;
}

void VideoPlayer::SetVolume(float volume)
{
    mVolume = volume;
}

void VideoPlayer::SetAudioEnabled(bool enabled)
{
    mAudioEnabled = enabled;
}

void VideoPlayer::SetUseYuv(bool useYuv)
{
    if (mUseYuv == useYuv)
    {
        return;
    }

    mUseYuv = useYuv;

    if (mStream != nullptr)
    {
        // Reopen with the new frame format on the next Update() (playback state is kept).
        Close();
    }
}

bool VideoPlayer::Open(VideoClip* clip)
{
    Close();

    if (clip == nullptr || clip->GetNumFrames() == 0)
    {
        return false;
    }

#if PLATFORM_DOLPHIN
    const VideoFrameFormat format = mUseYuv ? VideoFrameFormat::GxYuv420 : VideoFrameFormat::GxRgba8;

    if ((clip->GetWidth() % 16) != 0 || (clip->GetHeight() % 16) != 0)
    {
        LogError("Video %s: frame size %ux%u is not a multiple of 16. Recook the clip.",
            clip->GetName().c_str(), clip->GetWidth(), clip->GetHeight());
        return false;
    }
#else
    const VideoFrameFormat format = VideoFrameFormat::Rgba8;
#endif

    mStream = new VideoStream();
    if (!mStream->Open(clip, format))
    {
        LogError("Video %s: failed to open (not enough memory for %ux%u frames?)",
            clip->GetName().c_str(), clip->GetWidth(), clip->GetHeight());
        delete mStream;
        mStream = nullptr;
        return false;
    }

    Texture* texture = NewTransientAsset<Texture>();
    texture->SetName("VideoTexture");

    if (format == VideoFrameFormat::GxYuv420)
    {
        texture->InitDynamicYuv(clip->GetWidth(), clip->GetHeight());
    }
    else
    {
        texture->InitDynamic(clip->GetWidth(), clip->GetHeight());
    }

    texture->Create();
    mTexture = texture;

    if (mAudioEnabled && clip->HasAudio())
    {
        mAudioStream = AUD_OpenStream(clip->GetAudioSampleRate(), clip->GetAudioNumChannels());

        if (mAudioStream != 0)
        {
            AUD_SetStreamPaused(mAudioStream, true);
            AUD_SetStreamVolume(mAudioStream, mVolume);
        }
    }

    mOpenClip = clip;
    mOpenRevision = clip->GetRevision();
    mAudioSubmittedFrames = 0;

#if PLATFORM_DOLPHIN
    mLogUs = 0;
    OctLog("VIDEO open %s %ux%u %.2ffps frames=%u maxRecord=%u audio=%uHz x%u format=%s",
        clip->GetName().c_str(),
        unsigned(clip->GetWidth()),
        unsigned(clip->GetHeight()),
        clip->GetFrameRate(),
        unsigned(clip->GetNumFrames()),
        unsigned(clip->GetMaxRecordSize()),
        unsigned(clip->GetAudioSampleRate()),
        unsigned(clip->GetAudioNumChannels()),
        (format == VideoFrameFormat::GxYuv420) ? "YUV" : "RGBA");
#endif
    mTime = 0.0;
    mAudioStartTime = 0.0;
    mLastAudioPlayed = 0;
    mAudioActiveUs = 0;
    mClockRunning = false;

    return true;
}

void VideoPlayer::Close()
{
#if PLATFORM_DOLPHIN
    // Stop the texture referencing frame buffers before they are freed.
    Texture* texture = mTexture.Get<Texture>();
    if (texture != nullptr)
    {
        GFX_SetTextureResourceData(texture, nullptr);
    }
#endif

    ReleaseDisplayedFrame();

    if (mStream != nullptr)
    {
        mStream->Close();
        delete mStream;
        mStream = nullptr;
    }

    if (mAudioStream != 0)
    {
        AUD_CloseStream(mAudioStream);
        mAudioStream = 0;
    }

    mTexture = nullptr;
    mOpenClip = nullptr;
    mClockRunning = false;
}

void VideoPlayer::SeekInternal(double seconds)
{
    if (mStream == nullptr)
    {
        return;
    }

    mTime = mStream->Seek(seconds);
    mAudioStartTime = mTime;
    mAudioSubmittedFrames = 0;
    mLastAudioPlayed = 0;
    mAudioActiveUs = 0;
    mClockRunning = false;

    if (mAudioStream != 0)
    {
        AUD_FlushStream(mAudioStream);
        AUD_SetStreamPaused(mAudioStream, true);
    }
}

void VideoPlayer::ReleaseDisplayedFrame()
{
    if (mStream != nullptr && mDisplayedFrame.mSlot >= 0)
    {
        mStream->ReleaseFrame(mDisplayedFrame);
    }

    mDisplayedFrame = VideoStream::Frame();
}

bool VideoPlayer::GetStats(VideoStreamStats& outStats, float& outAudioBufferedMs)
{
    if (mStream == nullptr || mOpenClip == nullptr)
    {
        return false;
    }

    outStats = mStream->GetStats();
    outAudioBufferedMs = 0.0f;

    if (mAudioStream != 0 && mOpenClip->GetAudioSampleRate() > 0)
    {
        const uint64_t played = AUD_GetStreamPlayedFrames(mAudioStream);
        const uint64_t buffered = (mAudioSubmittedFrames > played) ? (mAudioSubmittedFrames - played) : 0;
        outAudioBufferedMs = float(buffered * 1000.0 / mOpenClip->GetAudioSampleRate());
    }

    return true;
}
