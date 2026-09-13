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
#include <vector>
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
    uint8_t*       memOwned = nullptr;   // private copy of the compressed Vorbis
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
    if (sv->memOwned) { free(sv->memOwned); sv->memOwned = nullptr; }
    sv->active = false;
    sv->eof = false;
}

// Keep-alive callback: non-NULL so an underrun parks the voice in SND_WAITING
// instead of ending it. Feeding happens on the main thread in AUD_Update.
static void StreamCallback(s32 voice) { (void)voice; }

// ---------------------------------------------------------------------------
// Push-PCM streams (AUD_OpenStream)
//
// Audio produced at runtime (e.g. a video soundtrack) is queued into a FIFO and
// fed to ASND the same way as the Vorbis streams above: one buffer playing, one
// queued with ASND_AddVoice. These use ASND voices above AUD_Play's range.
// ---------------------------------------------------------------------------

#define AUD_MAX_PCM_STREAMS 4

struct PcmStream
{
    bool     active = false;
    bool     started = false;           // ASND_SetVoice issued
    bool     paused = true;
    int32_t  voice = 0;
    int32_t  format = 0;
    uint32_t sampleRate = 0;
    uint32_t frameBytes = 4;
    int32_t  volume = MID_VOLUME;
    uint8_t* buf[2] = { nullptr, nullptr };
    uint32_t bufCapacity = 0;
    uint32_t inFlight[2] = { 0, 0 };    // bytes in the playing buffer, then the queued one
    int32_t  numInFlight = 0;
    int32_t  nextBuf = 0;
    std::vector<uint8_t> pending;
    uint32_t pendingHead = 0;
    uint64_t playedBytes = 0;
};

static PcmStream sPcmStreams[AUD_MAX_PCM_STREAMS];

static PcmStream* GetPcmStream(uint32_t streamId)
{
    if (streamId == 0 || streamId > AUD_MAX_PCM_STREAMS)
        return nullptr;

    PcmStream* stream = &sPcmStreams[streamId - 1];
    return stream->active ? stream : nullptr;
}

static uint32_t PcmPendingBytes(const PcmStream& s)
{
    return uint32_t(s.pending.size()) - s.pendingHead;
}

// Take up to maxBytes (whole sample frames) from the FIFO.
static uint32_t PcmTake(PcmStream& s, uint8_t* dst, uint32_t maxBytes)
{
    uint32_t n = PcmPendingBytes(s);
    if (n > maxBytes) n = maxBytes;
    n -= n % s.frameBytes;

    if (n > 0)
    {
        memcpy(dst, s.pending.data() + s.pendingHead, n);
        s.pendingHead += n;
    }

    if (s.pendingHead == s.pending.size())
    {
        s.pending.clear();
        s.pendingHead = 0;
    }
    else if (s.pendingHead > 64 * 1024)
    {
        s.pending.erase(s.pending.begin(), s.pending.begin() + s.pendingHead);
        s.pendingHead = 0;
    }

    return n;
}

// Count buffers ASND has finished playing.
static void PcmUpdatePlayed(PcmStream& s)
{
    if (!s.started || s.paused)
        return;

    s32 status = ASND_StatusVoice(s.voice);

    if (status != SND_WORKING)
    {
        // Drained (the keep-alive callback parks it in SND_WAITING): all played.
        for (int32_t i = 0; i < s.numInFlight; ++i)
            s.playedBytes += s.inFlight[i];
        s.numInFlight = 0;

        if (status == SND_UNUSED)
            s.started = false;
    }
    else if (s.numInFlight == 2 && ASND_TestVoiceBufferReady(s.voice) == 1)
    {
        // The queued buffer moved up, so the first one finished.
        s.playedBytes += s.inFlight[0];
        s.inFlight[0] = s.inFlight[1];
        s.numInFlight = 1;
    }
}

static void PcmFeed(PcmStream& s)
{
    PcmUpdatePlayed(s);

    if (s.paused)
        return;

    if (!s.started)
    {
        // Wait for a full buffer so playback doesn't start with an underrun.
        if (PcmPendingBytes(s) < s.bufCapacity)
            return;

        uint32_t n = PcmTake(s, s.buf[0], s.bufCapacity);
        DCFlushRange(s.buf[0], s.bufCapacity);

        if (ASND_SetVoice(s.voice, s.format, s.sampleRate, 0, s.buf[0], n, s.volume, s.volume, StreamCallback) != SND_OK)
            return;

        s.started = true;
        s.inFlight[0] = n;
        s.numInFlight = 1;
        s.nextBuf = 1;
    }

    while (s.numInFlight < 2 && ASND_TestVoiceBufferReady(s.voice) == 1)
    {
        // Queue full buffers. If the voice has run dry, send whatever is left so
        // the end of the stream still plays.
        uint32_t avail = PcmPendingBytes(s);
        if (avail < s.bufCapacity && !(s.numInFlight == 0 && avail >= s.frameBytes))
            break;

        uint8_t* dst = s.buf[s.nextBuf];
        uint32_t n = PcmTake(s, dst, s.bufCapacity);
        if (n == 0)
            break;

        DCFlushRange(dst, s.bufCapacity);

        if (ASND_AddVoice(s.voice, dst, n) != SND_OK)
            break;

        s.inFlight[s.numInFlight++] = n;
        s.nextBuf ^= 1;
    }
}

void AUD_Initialize()
{
    ASND_Init();
    ASND_Pause(0);
}

void AUD_Shutdown()
{
    for (uint32_t i = 0; i < AUD_MAX_PCM_STREAMS; ++i)
    {
        if (sPcmStreams[i].active)
            AUD_CloseStream(i + 1);
    }

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

    for (uint32_t i = 0; i < AUD_MAX_PCM_STREAMS; ++i)
    {
        if (sPcmStreams[i].active)
            PcmFeed(sPcmStreams[i]);
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

        // Own a private copy of the compressed Vorbis so the stream is independent
        // of the SoundWave asset's lifetime. In non-embedded builds the asset can be
        // swept (ref count -> 0) while the music is still playing; keeping the asset's
        // pointer would dangle, and the freed block -- reused for other allocations --
        // corrupts memory (scene-wide geometry collapse). Embedded never unloads
        // assets, which is why this only bit non-embedded builds.
        uint32_t compressedSize = soundWave->GetCompressedSize();
        sv->memOwned = (uint8_t*)malloc(compressedSize);
        if (sv->memOwned == nullptr)
        {
            LogError("SoundWave '%s': failed to allocate %u-byte stream copy.",
                     soundWave->GetName().c_str(), compressedSize);
            return;
        }
        memcpy(sv->memOwned, soundWave->GetCompressedData(), compressedSize);

        sv->mem.data = sv->memOwned;
        sv->mem.size = compressedSize;
        sv->mem.pos  = 0;

        ov_callbacks cb = { MemRead, MemSeek, MemClose, MemTell };
        if (ov_open_callbacks(&sv->mem, &sv->vf, NULL, 0, cb) < 0)
        {
            LogError("SoundWave '%s': failed to open Vorbis stream.", soundWave->GetName().c_str());
            free(sv->memOwned);
            sv->memOwned = nullptr;
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

uint32_t AUD_OpenStream(uint32_t sampleRate, uint32_t numChannels)
{
    if (sampleRate == 0 || numChannels < 1 || numChannels > 2)
        return 0;

    OCT_ASSERT(AUDIO_MAX_VOICES + AUD_MAX_PCM_STREAMS <= MAX_SND_VOICES);

    for (uint32_t i = 0; i < AUD_MAX_PCM_STREAMS; ++i)
    {
        PcmStream& s = sPcmStreams[i];
        if (s.active)
            continue;

        s = PcmStream();
        s.voice = AUDIO_MAX_VOICES + i;
        s.format = (numChannels == 2) ? VOICE_STEREO_16BIT_LE : VOICE_MONO_16BIT_LE;
        s.sampleRate = sampleRate;
        s.frameBytes = 2 * numChannels;

        // ~100 ms per buffer.
        s.bufCapacity = (sampleRate * s.frameBytes) / 10;
        s.bufCapacity -= s.bufCapacity % s.frameBytes;

        uint32_t allocSize = (s.bufCapacity + 31) & ~31u;
        s.buf[0] = (uint8_t*)memalign(32, allocSize);
        s.buf[1] = (uint8_t*)memalign(32, allocSize);

        if (s.buf[0] == nullptr || s.buf[1] == nullptr)
        {
            LogError("AUD_OpenStream: failed to allocate stream buffers.");
            if (s.buf[0]) free(s.buf[0]);
            if (s.buf[1]) free(s.buf[1]);
            s = PcmStream();
            return 0;
        }

        s.active = true;
        return i + 1;
    }

    return 0;
}

void AUD_CloseStream(uint32_t streamId)
{
    PcmStream* s = GetPcmStream(streamId);
    if (s == nullptr)
        return;

    ASND_StopVoice(s->voice);

    if (s->buf[0]) free(s->buf[0]);
    if (s->buf[1]) free(s->buf[1]);

    *s = PcmStream();
}

void AUD_QueueStreamData(uint32_t streamId, const uint8_t* data, uint32_t size)
{
    PcmStream* s = GetPcmStream(streamId);
    if (s == nullptr || data == nullptr || size == 0)
        return;

    s->pending.insert(s->pending.end(), data, data + size);
    PcmFeed(*s);
}

uint64_t AUD_GetStreamPlayedFrames(uint32_t streamId)
{
    PcmStream* s = GetPcmStream(streamId);
    if (s == nullptr)
        return 0;

    PcmUpdatePlayed(*s);
    return s->playedBytes / s->frameBytes;
}

void AUD_SetStreamPaused(uint32_t streamId, bool paused)
{
    PcmStream* s = GetPcmStream(streamId);
    if (s == nullptr || s->paused == paused)
        return;

    if (paused)
        PcmUpdatePlayed(*s);

    s->paused = paused;

    if (s->started)
        ASND_PauseVoice(s->voice, paused ? 1 : 0);

    if (!paused)
        PcmFeed(*s);
}

void AUD_SetStreamVolume(uint32_t streamId, float volume)
{
    PcmStream* s = GetPcmStream(streamId);
    if (s == nullptr)
        return;

    int32_t volumeInt = int32_t(glm::clamp(volume, 0.0f, 2.0f) * MID_VOLUME);
    if (volumeInt == s->volume)
        return;

    s->volume = volumeInt;

    if (s->started)
        ASND_ChangeVolumeVoice(s->voice, volumeInt, volumeInt);
}

void AUD_FlushStream(uint32_t streamId)
{
    PcmStream* s = GetPcmStream(streamId);
    if (s == nullptr)
        return;

    ASND_StopVoice(s->voice);
    s->started = false;
    s->numInFlight = 0;
    s->pending.clear();
    s->pendingHead = 0;
    s->playedBytes = 0;
}

#endif