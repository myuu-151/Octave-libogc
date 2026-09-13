#include "VideoPlayer.h"

#include "Assets/VideoClip.h"
#include "Assets/Texture.h"
#include "VideoStream.h"
#include "AssetManager.h"
#include "Log.h"

#include "Audio/Audio.h"
#include "System/System.h"

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

    if (mStream->HasError())
    {
        LogError("Failed to decode video %s", clip->GetName().c_str());
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

    VideoStream::Frame frame;
    if (mStream->PopFrame(mTime, frame))
    {
        Texture* texture = mTexture.Get<Texture>();
        if (texture != nullptr)
        {
            texture->UpdatePixels(frame.mPixels);
        }

        VideoStream::FreeFrame(frame);
    }

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

bool VideoPlayer::Open(VideoClip* clip)
{
    Close();

    if (clip == nullptr || clip->GetNumFrames() == 0)
    {
        return false;
    }

    mStream = new VideoStream();
    if (!mStream->Open(clip))
    {
        delete mStream;
        mStream = nullptr;
        return false;
    }

    Texture* texture = NewTransientAsset<Texture>();
    texture->SetName("VideoTexture");
    texture->InitDynamic(clip->GetWidth(), clip->GetHeight());
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
    mTime = 0.0;
    mAudioStartTime = 0.0;
    mLastAudioPlayed = 0;
    mAudioActiveUs = 0;
    mClockRunning = false;

    return true;
}

void VideoPlayer::Close()
{
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
    mLastAudioPlayed = 0;
    mAudioActiveUs = 0;
    mClockRunning = false;

    if (mAudioStream != 0)
    {
        AUD_FlushStream(mAudioStream);
        AUD_SetStreamPaused(mAudioStream, true);
    }
}
