#pragma once

#include "EngineTypes.h"

class Stream;
class SoundWave;
class Audio3D;

struct PcmFormat
{
    uint32_t mNumChannels = 2;
    uint32_t mSampleRate = 44100;
    uint32_t mBytesPerSample = 2;
};

void AUD_Initialize();
void AUD_Shutdown();
void AUD_Update();

void AUD_Play(
    uint32_t voiceIndex,
    SoundWave* soundWave,
    float volume,
    float pitch,
    bool loop,
    float startTime,
    bool spatial);

void AUD_Stop(uint32_t voiceIndex);
bool AUD_IsPlaying(uint32_t voiceIndex);
void AUD_SetVolume(uint32_t voiceIndex, float leftVolume, float rightVolume);
void AUD_SetPitch(uint32_t voiceIndex, float pitch);

uint8_t* AUD_AllocWaveBuffer(uint32_t size);
void AUD_FreeWaveBuffer(void* buffer);
void AUD_ProcessWaveBuffer(SoundWave* soundWave);

#if PLATFORM_GAMECUBE
// Sound effects live in ARAM (see Audio_Dolphin.cpp). Frees a SoundWave's ARAM block.
void AUD_FreeAram(uint32_t aramAddress);
// ARAM: total and free bytes, and the bytes of sound held there.
void AUD_GetAramStats(uint32_t& total, uint32_t& free, uint32_t& sounds);
#endif

// Streaming PCM voices: push audio that is produced at runtime (e.g. a video's
// soundtrack). Data is 16-bit signed little-endian interleaved PCM. Streams start
// paused. AUD_OpenStream() returns 0 if no stream could be opened.
uint32_t AUD_OpenStream(uint32_t sampleRate, uint32_t numChannels);
void AUD_CloseStream(uint32_t streamId);
void AUD_QueueStreamData(uint32_t streamId, const uint8_t* data, uint32_t size);
uint64_t AUD_GetStreamPlayedFrames(uint32_t streamId);   // since open / last flush
void AUD_SetStreamPaused(uint32_t streamId, bool paused);
void AUD_SetStreamVolume(uint32_t streamId, float volume);
void AUD_FlushStream(uint32_t streamId);

// Platform Independent
void AUD_EncodeVorbis(Stream& inStream, Stream& outStream, PcmFormat format);
void AUD_DecodeVorbis(Stream& inStream, Stream& outStream, PcmFormat format);
