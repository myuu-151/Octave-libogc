# Audio Streaming Roadmap (GameCube / libogc)

## The problem

On console, a **compressed** `SoundWave` is fully **Vorbis-decoded to PCM in RAM
on load**, and the decode is **double-buffered** (decoder fills a growing stream,
then a second full-size buffer is allocated and copied). So peak RAM ≈ **2× the
decoded PCM size**, and the on-disk compressed size is irrelevant to RAM cost.

Reference: `Engine/Source/Engine/Assets/SoundWave.cpp` `LoadStream()` — the
compressed branch calls `AUD_DecodeVorbis(...)` into `outStream`, then
`AUD_AllocWaveBuffer(size)` + `memcpy`. `AUD_AllocWaveBuffer` is a plain
`SYS_AlignedMalloc` with no null-check, so an allocation failure writes to a null
buffer and crashes.

### Observed case

| Track | Length | On disk (compressed) | Decoded PCM | Double-buffered peak | Result |
|-------|--------|----------------------|-------------|----------------------|--------|
| track9  | ~12 s   | 50 KB   | ~1 MB   | ~2 MB   | loads fine |
| track10 | 82.9 s  | 421 KB  | ~7 MB   | ~14 MB  | **crashes** (`Invalid write to 0x00000004`) |

Both are compressed 8-bit stereo 44.1 kHz. track10's ~14 MB transient spike
exceeds free MEM1 (24 MB total, minus the DOL + engine + other assets) → the
wave-buffer allocation fails → null-pointer write → crash. "Compressed" only
shrank the ISO; the RAM cost is the *decoded* size, doubled.

The root limitation: **audio is loaded whole into RAM as decoded PCM. There is no
streaming.** A minutes-long track cannot fit.

---

## Roadmap A — Stream-decode from RAM  *(recommended, achievable)*

Keep the **compressed Vorbis in RAM** (e.g. 421 KB) instead of decoding it to
full PCM (7 MB), and **decode on the fly during playback** into a small
double-buffered PCM ring that feeds the audio voice.

**What it achieves**
- RAM cost drops from ~14 MB → **~0.5 MB** per streamed track (compressed blob +
  a small ring buffer).
- Kills the ceiling for **any realistic music length** — a 10-minute track is
  still only a few MB compressed.
- **No DVD async required** — the data is already resident in RAM, so this is
  pure CPU decode. It sidesteps the disc-streaming mountain entirely.

**What it takes**
1. **Keep `mCompressedData` on console** for sounds flagged to stream (today it's
   retained only under `#if EDITOR`).
2. **A streaming voice**: `ov_open` on the in-RAM Vorbis, a decode callback that
   keeps a double-buffered PCM ring filled, wired into libogc's **ASND voice
   callback** (ASND asks for more PCM; decode just enough to hand back).
3. **Keep the current decode-to-PCM path for short SFX** — they're tiny; no
   reason to stream them. Only music / long clips stream.

**The real risk: CPU, not RAM.** Vorbis decode on a 486 MHz Gekko in real time is
fine for one or two stereo streams; a dozen simultaneous would choke. So this is
a **"streamed" flag on the `SoundWave`**, used for music, not every sound.

**Interim safety (do regardless):** null-check `AUD_AllocWaveBuffer` in
`SoundWave::LoadStream` so an oversized clip fails gracefully (skips the sound +
logs) instead of hard-crashing on a null write.

---

## Roadmap B — Stream from disc  *(the full retail mountain, overkill here)*

Don't even hold the compressed data in RAM — read compressed chunks off the
**disc** as you decode, keeping only a tiny read buffer + PCM ring resident.

**What it achieves**
- RAM cost approaches zero regardless of track length or compressed size.
- Matches how retail GameCube games stream music/audio off the disc.

**What it takes (and why it's hard)**
- **DVD async** (`DVD_ReadAbsAsyncPrio` + callbacks / a background reader thread)
  on top of the decode-streaming from Roadmap A.
- **Cache coherency** on every DMA'd chunk (invalidate CPU cache for the region).
- **Memory staging / double-buffering**, often routed through ARAM.
- **Seek-aware disc layout** so the read head isn't thrashing.

**When it's actually needed:** only if the *compressed* audio itself won't fit in
RAM — which for music basically never happens (10 min Vorbis ≈ a few MB). So for
this engine's use case it's overkill; Roadmap A already removes the ceiling.

---

## Recommendation

Do **Roadmap A** (stream-decode from RAM). It's the meaningful-but-bounded fix:
it removes the audio RAM ceiling for any realistic music, needs no DVD async, and
is a genuine feature rather than a band-aid. Add the `AUD_AllocWaveBuffer`
null-check immediately so long clips stop hard-crashing in the meantime.

Reserve **Roadmap B** for the day compressed audio alone won't fit in RAM — an
edge case this engine is unlikely to hit.
