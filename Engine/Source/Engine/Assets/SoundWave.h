#pragma once

#include "Asset.h"

class SoundWave : public Asset
{
public:

    DECLARE_ASSET(SoundWave, Asset);

    SoundWave();
    ~SoundWave();

    virtual void LoadStream(Stream& stream, Platform platform) override;
    virtual int32_t GetFileReadLimit(const char* path) override;
    virtual void SaveStream(Stream& stream, Platform platform) override;
    virtual void Create() override;
    virtual void Destroy() override;
    virtual bool Import(const std::string& path, ImportOptions* options) override;
    virtual void GatherProperties(std::vector<Property>& outProps) override;
    virtual glm::vec4 GetTypeColor() override;
    virtual const char* GetTypeName() override;
    virtual const char* GetTypeImportExt() override;

    void SetPcmData(uint8_t* data, uint32_t size, uint32_t numSamples, uint32_t bitsPerSample, uint32_t numChannels, uint32_t sampleRate);

    void SetVolumeMultiplier(float volume);
    float GetVolumeMultiplier() const;

    void SetPitchMultiplier(float pitch);
    float GetPitchMultiplier() const;

    void SetAudioClass(int8_t audioClass);
    int8_t GetAudioClass() const;

    // How many copies of this sound may play at once; 0 (the default) is any number. Playing one
    // more stops the oldest (AudioManager), so 1 makes a sound cut itself off.
    void SetMaxInstances(uint8_t maxInstances) { mMaxInstances = maxInstances; }
    uint8_t GetMaxInstances() const { return mMaxInstances; }

    uint8_t* GetWaveData() const;
    uint32_t GetWaveDataSize() const;
    uint32_t GetNumChannels() const;
    uint32_t GetBitsPerSample() const;
    uint32_t GetSampleRate() const;
    uint32_t GetNumSamples() const;
    uint32_t GetBlockAlign() const;
    uint32_t GetByteRate() const;

    float GetDuration() const;

    bool GetStream() const;
    const uint8_t* GetCompressedData() const;
    uint32_t GetCompressedSize() const;

    // A Stream sound on console, loaded from a file: the compressed audio is NOT in memory.
    // It is this many bytes of the asset's own file, from this offset, read while it plays.
    const std::string& GetDiscPath() const { return mDiscPath; }
    uint32_t GetDiscOffset() const { return mDiscOffset; }
    uint32_t GetDiscSize() const { return mDiscSize; }

    // GameCube: the PCM moved to ARAM (see Audio_Dolphin.cpp), at this ARAM address; 0 while it
    // is in main memory. GetWaveData() is null once it has moved; GetWaveDataSize() still holds.
    uint32_t GetAramAddress() const { return mAramAddress; }
    void MoveWaveDataToAram(uint32_t aramAddress);

protected:

    static bool HandlePropChange(Datum* datum, uint32_t index, const void* newValue);

    uint8_t* mWaveData = nullptr;
    uint32_t mWaveDataSize = 0;

    uint8_t* mCompressedData = nullptr;
    uint32_t mCompressedSize = 0;

    std::string mDiscPath;
    uint32_t mDiscOffset = 0;
    uint32_t mDiscSize = 0;

    uint32_t mAramAddress = 0;

    // Properties
    float mVolumeMultiplier = 1.0f;
    float mPitchMultiplier = 1.0f;
    int8_t mAudioClass = 0;
    bool mCompress = false;
    bool mCompressInternal = false;
    bool mStream = false;   // Stream-decode compressed audio on console (music)
    uint8_t mMaxInstances = 0;  // copies playing at once; 0 is any number

    // Soundwave Format
    uint32_t mNumChannels = 1;
    uint32_t mBitsPerSample = 8;
    uint32_t mSampleRate = 22050;
    uint32_t mNumSamples = 0;
    uint32_t mBlockAlign = 0;
    uint32_t mByteRate = 0;
};
