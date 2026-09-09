#if PLATFORM_DOLPHIN

#include "Audio/Audio.h"
#include "Audio/AudioConstants.h"

#include "Assets/SoundWave.h"
#include "Log.h"

#include "System/System.h"

#include <gccore.h>
#include <asndlib.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include "vorbis/vorbisfile.h"

static int32_t sSampleRates[AUDIO_MAX_VOICES] = {};

// ---------------------------------------------------------------------------
// Streaming (decode-on-demand) voices
//
// A SoundWave flagged Stream keeps its compressed Vorbis in RAM instead of
// decoding to full PCM. Here we open it with vorbisfile (over an in-RAM buffer)
// and decode it a chunk at a time into a double PCM buffer, feeding ASND via
// ASND_AddVoice. Decode runs on the main thread (AUD_Update), never in the audio
// interrupt. A long track costs its compressed size (~KB) instead of MBs of PCM.
// ---------------------------------------------------------------------------

struct MemSource
{
    const uint8_t* data;
    uint32_t size;
    uint32_t pos;
};

struct StreamVoice
{
    bool           active = false;
    OggVorbis_File vf;
    MemSource      mem;
    uint8_t*       buf[2] = { nullptr, nullptr };
    uint32_t       bufSize = 0;
    int            nextBuf = 0;
    int            channels = 2;
    int            bytesPerSample = 1;   // 1 = 8-bit unsigned, 2 = 16-bit LE
    bool           loop = false;
    bool           eof = false;
};

static StreamVoice sStreams[AUDIO_MAX_VOICES];

// ov_callbacks over an in-RAM compressed buffer.
static size_t MemRead(void* ptr, size_t size, size_t nmemb, void* ds)
{
    MemSource* m = (MemSource*)ds;
    size_t want = size * nmemb;
    size_t avail = m->size - m->pos;
    if (want > avail) want = avail;
    if (want > 0) memcpy(ptr, m->data + m->pos, want);
    m->pos += (uint32_t)want;
    return (size > 0) ? (want / size) : 0;
}
static int MemSeek(void* ds, ogg_int64_t off, int whence)
{
    MemSource* m = (MemSource*)ds;
    int64_t np = (whence == SEEK_CUR) ? (int64_t)m->pos + off
               : (whence == SEEK_END) ? (int64_t)m->size + off
               : off;
    if (np < 0) np = 0;
    if (np > (int64_t)m->size) np = m->size;
    m->pos = (uint32_t)np;
    return 0;
}
static long MemTell(void* ds) { return (long)((MemSource*)ds)->pos; }
static int  MemClose(void* ds) { (void)ds; return 0; }

// Decode up to sv->bufSize bytes into dst. Returns bytes written (0 = ended).
static uint32_t StreamFill(StreamVoice* sv, uint8_t* dst)
{
    uint32_t written = 0;
    while (written < sv->bufSize)
    {
        uint32_t remain = sv->bufSize - written;
        long ret;

        if (sv->bytesPerSample == 2)
        {
            // ASND wants little-endian 16-bit -> decode straight into dst.
            ret = ov_read(&sv->vf, (char*)(dst + written), (int)remain, 0, 2, 1, NULL);
            if (ret == 0) { if (sv->loop) { ov_pcm_seek(&sv->vf, 0); continue; } sv->eof = true; break; }
            if (ret < 0) continue;
            written += (uint32_t)ret;
        }
        else
        {
            // 8-bit unsigned target: decode native-endian 16-bit, then convert.
            int16_t tmp[1024];
            long cap = (long)sizeof(tmp);
            if (cap > (long)(remain * 2)) cap = (long)(remain * 2);
            ret = ov_read(&sv->vf, (char*)tmp, (int)cap, 1, 2, 1, NULL);
            if (ret == 0) { if (sv->loop) { ov_pcm_seek(&sv->vf, 0); continue; } sv->eof = true; break; }
            if (ret < 0) continue;
            int n = (int)(ret / 2);
            for (int i = 0; i < n; ++i)
                dst[written++] = (uint8_t)(((int)tmp[i] + 32768) >> 8);
        }
    }
    return written;
}

static void StreamStop(StreamVoice* sv)
{
    if (!sv->active) return;
    ov_clear(&sv->vf);
    if (sv->buf[0]) { free(sv->buf[0]); sv->buf[0] = nullptr; }
    if (sv->buf[1]) { free(sv->buf[1]); sv->buf[1] = nullptr; }
    sv->active = false;
    sv->eof = false;
}

// Keep-alive callback: non-NULL so an underrun parks the voice in SND_WAITING
// instead of ending it. Feeding happens on the main thread in AUD_Update.
static void StreamCallback(s32 voice) { (void)voice; }

void AUD_Initialize()
{
    ASND_Init();
    ASND_Pause(0);
}

void AUD_Shutdown()
{
    ASND_End();
}

void AUD_Update()
{
    // Keep streamed voices fed (decode next chunk on the main thread).
    for (int32_t v = 0; v < AUDIO_MAX_VOICES; ++v)
    {
        StreamVoice* sv = &sStreams[v];
        if (!sv->active) continue;

        if (ASND_StatusVoice(v) == SND_UNUSED)
        {
            StreamStop(sv);   // voice finished / drained
            continue;
        }

        while (!sv->eof && ASND_TestVoiceBufferReady(v))
        {
            uint8_t* buf = sv->buf[sv->nextBuf];
            uint32_t n = StreamFill(sv, buf);
            if (n == 0) { sv->eof = true; break; }
            DCFlushRange(buf, sv->bufSize);
            ASND_AddVoice(v, buf, n);
            sv->nextBuf ^= 1;
        }
    }
}

void AUD_Play(
    uint32_t voiceIndex,
    SoundWave* soundWave,
    float volume,
    float pitch,
    bool loop,
    float startTime,
    bool spatial)
{
    //float startPercent = startTime / soundWave->GetDuration();
    //startPercent = glm::clamp(startPercent, 0.0f, 1.0f);
    //uint32_t startingSample = uint32_t(startPercent * soundWave->GetNumSamples());

    int32_t volumeInt = int32_t(volume * MID_VOLUME);
    int32_t pitchHz = int32_t(soundWave->GetSampleRate() * pitch);
    int32_t voiceFormat = 0;

    sSampleRates[voiceIndex] = soundWave->GetSampleRate();

    bool stereo = (soundWave->GetNumChannels() == 2);
    bool bit16 = (soundWave->GetBitsPerSample() == 16);

    // Wave data is stored in little endian format
    if (stereo)
    {
        voiceFormat = bit16 ? VOICE_STEREO_16BIT_LE : VOICE_STEREO_8BIT_U;
    }
    else
    {
        voiceFormat = bit16 ? VOICE_MONO_16BIT_LE : VOICE_MONO_8BIT_U;
    }

    // Streamed sound: decode-on-demand from the in-RAM compressed Vorbis rather
    // than playing a fully-decoded PCM buffer.
    if (soundWave->GetStream() && soundWave->GetCompressedData() != nullptr)
    {
        StreamVoice* sv = &sStreams[voiceIndex];
        StreamStop(sv);   // clean any previous stream on this voice

        sv->mem.data = soundWave->GetCompressedData();
        sv->mem.size = soundWave->GetCompressedSize();
        sv->mem.pos  = 0;

        ov_callbacks cb = { MemRead, MemSeek, MemClose, MemTell };
        if (ov_open_callbacks(&sv->mem, &sv->vf, NULL, 0, cb) < 0)
        {
            LogError("SoundWave '%s': failed to open Vorbis stream.", soundWave->GetName().c_str());
            return;
        }

        sv->active = true;
        sv->channels = (int)soundWave->GetNumChannels();
        sv->bytesPerSample = bit16 ? 2 : 1;
        sv->loop = loop;
        sv->eof = false;
        sv->nextBuf = 1;

        // ~0.2s double buffers.
        uint32_t bytesPerSec = soundWave->GetSampleRate() * sv->channels * sv->bytesPerSample;
        sv->bufSize = ((bytesPerSec / 5) + 31) & ~31u;
        sv->buf[0] = (uint8_t*)memalign(32, sv->bufSize);
        sv->buf[1] = (uint8_t*)memalign(32, sv->bufSize);
        if (sv->buf[0] == nullptr || sv->buf[1] == nullptr)
        {
            LogError("SoundWave '%s': failed to allocate stream buffers.", soundWave->GetName().c_str());
            StreamStop(sv);
            return;
        }

        uint32_t n0 = StreamFill(sv, sv->buf[0]);
        DCFlushRange(sv->buf[0], sv->bufSize);
        ASND_SetVoice(
            voiceIndex, voiceFormat, pitchHz, 0,
            sv->buf[0], n0,
            spatial ? 0 : volumeInt, spatial ? 0 : volumeInt,
            StreamCallback);
        return;
    }

    if (loop)
    {
        ASND_SetInfiniteVoice(
            voiceIndex,
            voiceFormat,
            pitchHz,
            0,
            soundWave->GetWaveData(),
            soundWave->GetWaveDataSize(),
            spatial ? 0 : volumeInt,
            spatial ? 0 : volumeInt);
    }
    else
    {
        ASND_SetVoice(
            voiceIndex,
            voiceFormat,
            pitchHz,
            0,
            soundWave->GetWaveData(),
            soundWave->GetWaveDataSize(),
            spatial ? 0 : volumeInt,
            spatial ? 0 : volumeInt,
            nullptr);
    }
}

void AUD_Stop(uint32_t voiceIndex)
{
    ASND_StopVoice(voiceIndex);

    if (voiceIndex < AUDIO_MAX_VOICES)
    {
        StreamStop(&sStreams[voiceIndex]);
    }
}

bool AUD_IsPlaying(uint32_t voiceIndex)
{
    int32_t status = ASND_StatusVoice(voiceIndex);
    return status == SND_WORKING;
}

void AUD_SetVolume(uint32_t voiceIndex, float leftVolume, float rightVolume)
{
    int32_t leftVolInt = int32_t(MID_VOLUME * leftVolume);
    int32_t rightVolInt = int32_t(MID_VOLUME * rightVolume);
    ASND_ChangeVolumeVoice(voiceIndex, leftVolInt, rightVolInt);
}

void AUD_SetPitch(uint32_t voiceIndex, float pitch)
{
    int32_t pitchHz = int32_t(pitch * sSampleRates[voiceIndex]);
    ASND_ChangePitchVoice(voiceIndex, pitchHz);
}

uint8_t* AUD_AllocWaveBuffer(uint32_t size)
{
    return (uint8_t*)SYS_AlignedMalloc(size, 32);
}

void AUD_FreeWaveBuffer(void* buffer)
{
    SYS_AlignedFree(buffer);
}

void AUD_ProcessWaveBuffer(SoundWave* soundWave)
{

}

#endif