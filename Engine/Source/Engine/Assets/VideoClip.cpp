#include "Assets/VideoClip.h"

#include "Log.h"
#include "Stream.h"
#include "Property.h"
#include "Engine.h"
#include "Maths.h"

#include "System/System.h"

#if EDITOR
#include <stb_image.h>
#include <cstdio>
#include <cstdlib>
#endif

#include <algorithm>
#include <cstring>

FORCE_LINK_DEF(VideoClip);
DEFINE_ASSET(VideoClip);

static const char* sCookPresetStrings[] =
{
    "Custom",
    "NTSC",
    "PAL"
};
static_assert(int32_t(VideoCookPreset::Count) == 3, "Need to update cook preset string table");

bool VideoClip::HandlePropChange(Datum* datum, uint32_t index, const void* newValue)
{
    Property* prop = static_cast<Property*>(datum);
    bool handled = false;

    if (prop->mName == "Recook")
    {
#if EDITOR
        VideoClip* clip = static_cast<VideoClip*>(prop->mOwner);
        clip->Cook();
#endif
        handled = true;
    }

    HandleAssetPropChange(datum, index, newValue);

    return handled;
}

VideoClip::VideoClip()
{
    mType = VideoClip::GetStaticType();
}

VideoClip::~VideoClip()
{

}

int32_t VideoClip::GetFileReadLimit(const char* path)
{
#if PLATFORM_DOLPHIN && !EDITOR
    // Only load the metadata; records are read from this file while playing.
    // payloadOffset sits right after the header and name, so peek at the start.
    mStreamPath = path;

    Stream head;
    if (!head.ReadFile(path, true, 1024))
    {
        return 0;
    }

    AssetHeader header = ReadHeader(head);
    head.SetAssetVersion(header.mVersion);

    std::string name;
    head.ReadString(name);

    if (head.GetPos() + sizeof(uint32_t) > head.GetSize())
    {
        return 0;
    }

    return int32_t(head.ReadUint32());
#else
    (void)path;
    return 0;
#endif
}

void VideoClip::LoadStream(Stream& stream, Platform platform)
{
    Asset::LoadStream(stream, platform);

    mPayloadOffset = stream.ReadUint32();

    stream.ReadString(mSourcePath);
    mCookPreset = stream.ReadInt32();
    mCookWidth = stream.ReadInt32();
    mCookHeight = stream.ReadInt32();
    mCookFps = stream.ReadInt32();
    mCookQuality = stream.ReadInt32();
    mCookAudioChannels = stream.ReadInt32();

    mWidth = stream.ReadUint32();
    mHeight = stream.ReadUint32();
    mFrameRateMilli = stream.ReadUint32();
    mNumFrames = stream.ReadUint32();
    mAudioSampleRate = stream.ReadUint32();
    mAudioNumChannels = stream.ReadUint32();
    mMaxRecordSize = stream.ReadUint32();

    uint32_t numOffsets = stream.ReadUint32();
    mRecordOffsets.resize(numOffsets);
    for (uint32_t i = 0; i < numOffsets; ++i)
    {
        mRecordOffsets[i] = stream.ReadUint32();
    }

    mPayload.clear();
    mStreamed = false;

    const uint32_t payloadSize = (numOffsets > 0) ? mRecordOffsets.back() : 0;
    bool valid = (mPayloadOffset != 0 &&
                  mNumFrames > 0 &&
                  numOffsets == mNumFrames + 1 &&
                  mFrameRateMilli > 0);

    if (valid)
    {
        if (uint64_t(mPayloadOffset) + payloadSize <= stream.GetSize())
        {
            mPayload.resize(payloadSize);
            memcpy(mPayload.data(), stream.GetData() + mPayloadOffset, payloadSize);
        }
        else if (!mStreamPath.empty())
        {
            // Only the metadata was read (see GetFileReadLimit()).
            mStreamed = true;
        }
        else
        {
            LogError("VideoClip %s: cooked video data is missing.", mName.c_str());
            valid = false;
        }
    }

    if (!valid)
    {
        mNumFrames = 0;
        mRecordOffsets.clear();
    }

    mRevision++;
}

void VideoClip::SaveStream(Stream& stream, Platform platform)
{
    Asset::SaveStream(stream, platform);

#if EDITOR
    Stream meta;
    meta.WriteString(mSourcePath);
    meta.WriteInt32(mCookPreset);
    meta.WriteInt32(mCookWidth);
    meta.WriteInt32(mCookHeight);
    meta.WriteInt32(mCookFps);
    meta.WriteInt32(mCookQuality);
    meta.WriteInt32(mCookAudioChannels);

    meta.WriteUint32(mWidth);
    meta.WriteUint32(mHeight);
    meta.WriteUint32(mFrameRateMilli);
    meta.WriteUint32(mNumFrames);
    meta.WriteUint32(mAudioSampleRate);
    meta.WriteUint32(mAudioNumChannels);
    meta.WriteUint32(mMaxRecordSize);

    meta.WriteUint32(uint32_t(mRecordOffsets.size()));
    for (uint32_t offset : mRecordOffsets)
    {
        meta.WriteUint32(offset);
    }

    const bool hasPayload = !mPayload.empty();
    const uint32_t payloadOffset = hasPayload ? (stream.GetPos() + sizeof(uint32_t) + meta.GetSize()) : 0;

    stream.WriteUint32(payloadOffset);
    stream.WriteBytes((const uint8_t*)meta.GetData(), meta.GetSize());

    if (hasPayload)
    {
        stream.WriteBytes(mPayload.data(), uint32_t(mPayload.size()));
    }
#endif
}

void VideoClip::Create()
{
    Asset::Create();
}

void VideoClip::Destroy()
{
    Asset::Destroy();

    mPayload.clear();
    mPayload.shrink_to_fit();
    mRecordOffsets.clear();
}

bool VideoClip::Import(const std::string& path, ImportOptions* options)
{
    bool success = Asset::Import(path, options);
    if (!success)
    {
        return false;
    }

#if EDITOR
    mSourcePath = path;
    success = Cook();

    if (success)
    {
        Create();
    }
#endif

    return success;
}

void VideoClip::GatherProperties(std::vector<Property>& outProps)
{
    Asset::GatherProperties(outProps);

    outProps.push_back(Property(DatumType::String, "Source Path", this, &mSourcePath, 1, HandlePropChange));
    outProps.push_back(Property(DatumType::Integer, "Preset", this, &mCookPreset, 1, HandlePropChange, NULL_DATUM, int32_t(VideoCookPreset::Count), sCookPresetStrings));
    outProps.push_back(Property(DatumType::Integer, "Cook Width", this, &mCookWidth, 1, HandlePropChange));
    outProps.push_back(Property(DatumType::Integer, "Cook Height", this, &mCookHeight, 1, HandlePropChange));
    outProps.push_back(Property(DatumType::Integer, "Cook FPS", this, &mCookFps, 1, HandlePropChange));
    outProps.push_back(Property(DatumType::Integer, "JPEG Quality", this, &mCookQuality, 1, HandlePropChange));
    outProps.push_back(Property(DatumType::Integer, "Audio Channels", this, &mCookAudioChannels, 1, HandlePropChange));

    static bool sFakeRecook = false;
    outProps.push_back(Property(DatumType::Bool, "Recook", this, &sFakeRecook, 1, HandlePropChange));
}

glm::vec4 VideoClip::GetTypeColor()
{
    return glm::vec4(0.9f, 0.6f, 0.1f, 1.0f);
}

const char* VideoClip::GetTypeName()
{
    return "VideoClip";
}

const char* VideoClip::GetTypeImportExt()
{
    return ".mp4";
}

uint32_t VideoClip::GetWidth() const
{
    return mWidth;
}

uint32_t VideoClip::GetHeight() const
{
    return mHeight;
}

float VideoClip::GetFrameRate() const
{
    return mFrameRateMilli / 1000.0f;
}

uint32_t VideoClip::GetNumFrames() const
{
    return mNumFrames;
}

float VideoClip::GetDuration() const
{
    return (mFrameRateMilli > 0) ? (mNumFrames * 1000.0f / mFrameRateMilli) : 0.0f;
}

bool VideoClip::HasAudio() const
{
    return mAudioSampleRate > 0 && mAudioNumChannels > 0;
}

uint32_t VideoClip::GetAudioSampleRate() const
{
    return mAudioSampleRate;
}

uint32_t VideoClip::GetAudioNumChannels() const
{
    return mAudioNumChannels;
}

uint32_t VideoClip::GetMaxRecordSize() const
{
    return mMaxRecordSize;
}

uint32_t VideoClip::GetRecordSize(uint32_t frame) const
{
    if (frame >= mNumFrames)
    {
        return 0;
    }

    return mRecordOffsets[frame + 1] - mRecordOffsets[frame];
}

bool VideoClip::ReadRecord(uint32_t frame, uint8_t* dst) const
{
    if (frame >= mNumFrames || dst == nullptr)
    {
        return false;
    }

    const uint32_t offset = mRecordOffsets[frame];
    const uint32_t size = mRecordOffsets[frame + 1] - offset;

    if (mStreamed)
    {
        return SYS_ReadFileRange(mStreamPath.c_str(), true, mPayloadOffset + offset, size, (char*)dst);
    }

    if (uint64_t(offset) + size > mPayload.size())
    {
        return false;
    }

    memcpy(dst, mPayload.data() + offset, size);
    return true;
}

uint32_t VideoClip::GetRevision() const
{
    return mRevision;
}

#if EDITOR

static std::string GetFFmpegPath()
{
    const char* envPath = getenv("OCTAVE_FFMPEG");
    if (envPath != nullptr && envPath[0] != '\0')
    {
        return envPath;
    }

#if PLATFORM_WINDOWS
    std::string bundledPath = SYS_GetOctavePath() + "External/ffmpeg/bin/ffmpeg.exe";
#else
    std::string bundledPath = SYS_GetOctavePath() + "External/ffmpeg/bin/ffmpeg";
#endif

    if (SYS_DoesFileExist(bundledPath.c_str(), false))
    {
        return bundledPath;
    }

    // Fall back to an ffmpeg on the PATH.
    return "ffmpeg";
}

// Runs ffmpeg with the given arguments, sending its error output to logPath.
static bool RunFFmpeg(const std::string& args, const std::string& logPath)
{
    std::string cmd = "\"" + GetFFmpegPath() + "\" " + args + " 2> \"" + logPath + "\"";

#if PLATFORM_WINDOWS
    // cmd.exe /c strips the outer pair of quotes when a command starts and ends with
    // one, which would mangle the quoted exe path. Wrap everything in one more pair.
    cmd = "\"" + cmd + "\"";
#endif

    LogDebug("[Exec] %s", cmd.c_str());
    return system(cmd.c_str()) == 0;
}

static void LogFFmpegOutput(const std::string& logPath)
{
    if (!SYS_DoesFileExist(logPath.c_str(), false))
    {
        return;
    }

    Stream logStream;
    if (logStream.ReadFile(logPath.c_str(), false) && logStream.GetSize() > 0)
    {
        std::string text(logStream.GetData(), logStream.GetSize());
        if (text.size() > 1024)
        {
            text = text.substr(text.size() - 1024);
        }
        LogError("ffmpeg: %s", text.c_str());
    }
}

static std::string GetFramePath(const std::string& prefix, uint32_t frameNumber)
{
    char suffix[32];
    snprintf(suffix, sizeof(suffix), "_%06u.jpg", frameNumber);
    return prefix + suffix;
}

static void AppendU32LE(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(uint8_t(value));
    out.push_back(uint8_t(value >> 8));
    out.push_back(uint8_t(value >> 16));
    out.push_back(uint8_t(value >> 24));
}

bool VideoClip::Cook()
{
    if (mSourcePath.empty() || !SYS_DoesFileExist(mSourcePath.c_str(), false))
    {
        LogError("VideoClip %s: source video not found: %s", mName.c_str(), mSourcePath.c_str());
        return false;
    }

    // Frame dimensions must be multiples of 4: GX RGBA8 textures are laid out in 4x4 blocks.
    // Height -4 tells ffmpeg to keep the aspect ratio and round to a multiple of 4.
    int32_t width = glm::clamp(mCookWidth, 16, 1024) & ~3;
    int32_t height = (mCookHeight > 0) ? (glm::clamp(mCookHeight, 16, 1024) & ~3) : -4;
    uint32_t fpsMilli = uint32_t(glm::clamp(mCookFps, 1, 60)) * 1000;
    std::string fpsFilter = std::to_string(fpsMilli / 1000);

    switch (VideoCookPreset(mCookPreset))
    {
    case VideoCookPreset::NTSC:
        width = 640;
        height = 480;
        fpsMilli = 29970;
        fpsFilter = "30000/1001";
        break;
    case VideoCookPreset::PAL:
        width = 640;
        height = 528;
        fpsMilli = 25000;
        fpsFilter = "25";
        break;
    default:
        break;
    }

    const int32_t quality = glm::clamp(mCookQuality, 2, 31);
    // Fixed at 44100 Hz to match the rate the rest of the audio pipeline (Vorbis) supports.
    const int32_t audioRate = 44100;
    const int32_t audioChannels = (mCookAudioChannels == 1) ? 1 : 2;

    std::string tempDir = GetEngineState()->mProjectDirectory + "Intermediate";
    SYS_CreateDirectory(tempDir.c_str());
    tempDir += "/VideoCook";
    SYS_CreateDirectory(tempDir.c_str());

    const std::string prefix = tempDir + "/" + mName;
    const std::string audioPath = prefix + ".pcm";
    const std::string logPath = prefix + "_ffmpeg.txt";

    // Clear out anything left behind by a previous cook of this clip.
    for (uint32_t i = 1; SYS_DoesFileExist(GetFramePath(prefix, i).c_str(), false); ++i)
    {
        SYS_RemoveFile(GetFramePath(prefix, i).c_str());
    }
    if (SYS_DoesFileExist(audioPath.c_str(), false))
    {
        SYS_RemoveFile(audioPath.c_str());
    }

    LogDebug("VideoClip %s: cooking %dx%d @ %.2f fps (quality %d)...",
        mName.c_str(), width, (height > 0) ? height : 0, fpsMilli / 1000.0f, quality);

    // Pass 1: video frames as baseline JPEGs.
    std::string videoArgs = "-y -hide_banner -loglevel error";
    videoArgs += " -i \"" + mSourcePath + "\" -an";
    videoArgs += " -vf \"fps=" + fpsFilter +
                 ",scale=" + std::to_string(width) + ":" + std::to_string(height) +
                 ":flags=lanczos,format=yuvj420p\"";
    videoArgs += " -q:v " + std::to_string(quality);
    videoArgs += " -f image2 \"" + prefix + "_%06d.jpg\"";

    if (!RunFFmpeg(videoArgs, logPath))
    {
        LogError("VideoClip %s: ffmpeg failed to extract video frames.", mName.c_str());
        LogFFmpegOutput(logPath);
        return false;
    }

    // Pass 2: audio as raw 16-bit little-endian PCM. Fails if there is no audio track.
    std::string audioArgs = "-y -hide_banner -loglevel error";
    audioArgs += " -i \"" + mSourcePath + "\" -vn";
    audioArgs += " -ac " + std::to_string(audioChannels);
    audioArgs += " -ar " + std::to_string(audioRate);
    audioArgs += " -f s16le -acodec pcm_s16le \"" + audioPath + "\"";

    const uint32_t audioFrameBytes = 2 * audioChannels;
    Stream audio;
    bool hasAudio = RunFFmpeg(audioArgs, logPath) &&
                    SYS_DoesFileExist(audioPath.c_str(), false) &&
                    audio.ReadFile(audioPath.c_str(), false) &&
                    audio.GetSize() >= audioFrameBytes;
    const uint64_t totalAudioFrames = hasAudio ? (audio.GetSize() / audioFrameBytes) : 0;

    uint32_t numFrames = 0;
    while (SYS_DoesFileExist(GetFramePath(prefix, numFrames + 1).c_str(), false))
    {
        ++numFrames;
    }

    if (numFrames == 0)
    {
        LogError("VideoClip %s: ffmpeg produced no frames.", mName.c_str());
        LogFFmpegOutput(logPath);
        return false;
    }

    // Mux: one record per frame, holding the audio that plays during that frame.
    std::vector<uint8_t> payload;
    std::vector<uint32_t> offsets;
    offsets.reserve(numFrames + 1);
    uint32_t maxRecordSize = 0;
    int32_t frameWidth = 0;
    int32_t frameHeight = 0;
    bool success = true;

    for (uint32_t i = 0; i < numFrames; ++i)
    {
        const std::string framePath = GetFramePath(prefix, i + 1);
        Stream jpeg;

        if (!jpeg.ReadFile(framePath.c_str(), false) || jpeg.GetSize() == 0)
        {
            LogError("VideoClip %s: failed to read %s", mName.c_str(), framePath.c_str());
            success = false;
            break;
        }

        if (i == 0)
        {
            int comps = 0;
            if (!stbi_info_from_memory((const stbi_uc*)jpeg.GetData(), int(jpeg.GetSize()), &frameWidth, &frameHeight, &comps))
            {
                LogError("VideoClip %s: could not read the frame size.", mName.c_str());
                success = false;
                break;
            }
        }

        uint64_t audioStart = std::min<uint64_t>(uint64_t(i) * audioRate * 1000 / fpsMilli, totalAudioFrames);
        uint64_t audioEnd = std::min<uint64_t>(uint64_t(i + 1) * audioRate * 1000 / fpsMilli, totalAudioFrames);
        const uint32_t audioBytes = uint32_t((audioEnd - audioStart) * audioFrameBytes);
        const uint32_t jpegBytes = jpeg.GetSize();

        if (payload.size() + 8 + audioBytes + jpegBytes > 0xF0000000ull)
        {
            LogError("VideoClip %s: cooked video is too large.", mName.c_str());
            success = false;
            break;
        }

        offsets.push_back(uint32_t(payload.size()));
        AppendU32LE(payload, audioBytes);
        AppendU32LE(payload, jpegBytes);

        if (audioBytes > 0)
        {
            const uint8_t* audioData = (const uint8_t*)audio.GetData() + audioStart * audioFrameBytes;
            payload.insert(payload.end(), audioData, audioData + audioBytes);
        }

        const uint8_t* jpegData = (const uint8_t*)jpeg.GetData();
        payload.insert(payload.end(), jpegData, jpegData + jpegBytes);

        maxRecordSize = std::max<uint32_t>(maxRecordSize, 8 + audioBytes + jpegBytes);
    }

    // Clean up intermediates.
    for (uint32_t i = 1; i <= numFrames; ++i)
    {
        SYS_RemoveFile(GetFramePath(prefix, i).c_str());
    }
    if (SYS_DoesFileExist(audioPath.c_str(), false))
    {
        SYS_RemoveFile(audioPath.c_str());
    }

    if (!success)
    {
        return false;
    }

    offsets.push_back(uint32_t(payload.size()));

    mWidth = uint32_t(frameWidth);
    mHeight = uint32_t(frameHeight);
    mFrameRateMilli = fpsMilli;
    mNumFrames = numFrames;
    mAudioSampleRate = hasAudio ? uint32_t(audioRate) : 0;
    mAudioNumChannels = hasAudio ? uint32_t(audioChannels) : 0;
    mMaxRecordSize = maxRecordSize;
    mRecordOffsets.swap(offsets);
    mPayload.swap(payload);
    mStreamed = false;
    mRevision++;

    SetDirtyFlag();

    LogDebug("VideoClip %s: cooked %u frames at %ux%u, %s, %.2f MB.",
        mName.c_str(), mNumFrames, mWidth, mHeight,
        hasAudio ? "with audio" : "no audio",
        mPayload.size() / (1024.0f * 1024.0f));

    return true;
}

#endif
