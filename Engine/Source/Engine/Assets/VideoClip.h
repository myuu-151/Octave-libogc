#pragma once

#include "Asset.h"

#include <string>
#include <vector>

enum class VideoCookPreset : int32_t
{
    Custom,     // Cook Width / Height / FPS
    NTSC,       // 640x480 @ 29.97 fps
    PAL,        // 640x528 @ 25 fps

    Count
};

// A cooked video: MJPEG frames plus 16-bit PCM audio, produced in the editor by
// running ffmpeg on a source video (mp4/mov/webm/...). The same cooked data plays
// on every platform.
//
// .oct layout after the base Asset header + name:
//   uint32 payloadOffset     absolute file offset of the payload (0 = not cooked)
//   metadata                 cook settings, frame format, record offset table
//   payload                  one record per video frame:
//                              uint32 audioBytes (LE), uint32 jpegBytes (LE),
//                              s16le interleaved PCM, baseline JPEG
//
// Each frame's audio is stored next to the frame so playback reads the file
// sequentially (no seeking between an audio block and the frames on a disc). On
// GameCube/Wii only the metadata is loaded into RAM; records are read from the
// file on demand while the video plays (see GetFileReadLimit()).
class VideoClip : public Asset
{
public:

    DECLARE_ASSET(VideoClip, Asset);

    VideoClip();
    ~VideoClip();

    virtual int32_t GetFileReadLimit(const char* path) override;
    virtual void LoadStream(Stream& stream, Platform platform) override;
    virtual void SaveStream(Stream& stream, Platform platform) override;
    virtual void Create() override;
    virtual void Destroy() override;
    virtual bool Import(const std::string& path, ImportOptions* options) override;
    virtual void GatherProperties(std::vector<Property>& outProps) override;
    virtual glm::vec4 GetTypeColor() override;
    virtual const char* GetTypeName() override;
    virtual const char* GetTypeImportExt() override;

    uint32_t GetWidth() const;
    uint32_t GetHeight() const;
    float GetFrameRate() const;
    uint32_t GetNumFrames() const;
    float GetDuration() const;

    bool HasAudio() const;
    uint32_t GetAudioSampleRate() const;
    uint32_t GetAudioNumChannels() const;

    uint32_t GetMaxRecordSize() const;
    uint32_t GetRecordSize(uint32_t frame) const;

    // Copies record `frame` into dst, which must hold GetRecordSize(frame) bytes.
    // Safe to call from a worker thread while the clip is not recooked or destroyed.
    bool ReadRecord(uint32_t frame, uint8_t* dst) const;

    // Changes whenever the cooked data changes, so players know to reopen.
    uint32_t GetRevision() const;

#if EDITOR
    bool Cook();
#endif

    static bool HandlePropChange(Datum* datum, uint32_t index, const void* newValue);

protected:

    // Properties (cook settings)
    std::string mSourcePath;
    int32_t mCookPreset = 0;         // VideoCookPreset: Custom uses the fields below
    int32_t mCookWidth = 320;
    int32_t mCookHeight = 0;         // 0 = keep the source aspect ratio
    int32_t mCookFps = 24;
    int32_t mCookQuality = 5;        // ffmpeg -q:v, 2 (best) to 31 (smallest)
    int32_t mCookAudioChannels = 2;
    // Keep the source's own values instead of the settings above (read with ffprobe).
    // Dimensions still round down to multiples of 16, and at most 1024.
    bool mNativeResolution = false;
    bool mNativeFrameRate = false;
    bool mNativeSampleRate = false;  // Otherwise audio is cooked at 44100 Hz

    // Cooked format
    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
    uint32_t mFrameRateMilli = 0;
    uint32_t mNumFrames = 0;
    uint32_t mAudioSampleRate = 0;
    uint32_t mAudioNumChannels = 0;
    uint32_t mMaxRecordSize = 0;
    std::vector<uint32_t> mRecordOffsets;   // mNumFrames + 1 entries, relative to the payload

    // Payload: held in memory, or read on demand from mStreamPath.
    std::vector<uint8_t> mPayload;
    std::string mStreamPath;
    uint32_t mPayloadOffset = 0;
    bool mStreamed = false;

    uint32_t mRevision = 0;
};
