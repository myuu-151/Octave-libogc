# Swiss fork patch: serve libogc `DVD_ReadPrio` from the SD ISO

This directory is **not part of Octave**. It's the work-in-progress patch for a
**Swiss fork** that makes an on-disc Octave/libogc ISO actually read its assets
under Swiss (PicoLoader rig, no ODE). See `../DVD_LOADING_NOTES.md` for the full
investigation and *why* this is needed.

## The idea

Swiss serves disc reads by recognizing the Nintendo SDK's disc functions and
hijacking them. libogc isn't the SDK, so Swiss ignores it and libogc's reads hit
the real (empty) drive → `rc=-1`. Fix: teach our Swiss fork to recognize
libogc's **synchronous** `DVD_ReadPrio` and redirect it straight to Swiss's SD
reader. Synchronous means no interrupt/callback flow to emulate — just read the
bytes and return.

Game stays 100% stock libogc; only the Swiss `ipl.dol` changes.

## Files

- `dvd_readprio_libogc.S` — the redirect stub (libogc ABI in, `frag_read_complete`
  out, cache-invalidate, return). Fully commented with the verified ABI.

## Wiring it into the Swiss fork (TODO)

1. **Drop the stub into the patch image** — add `dvd_readprio_libogc.S` to
   `cube/patches/base` (or a small new patch), so it links against `frag.c`
   (`frag_read_complete`) and `reservedarea.h` (`VAR_CURRENT_DISC`), and gets
   emitted as a `_bin.h` via `bin2s` like the other patch blobs.

2. **Add the signature** in `cube/swiss/source/patcher.c`, near the other DVD
   `FuncPattern`s:
   ```c
   FuncPattern DVDReadPrioLibogcSig =
       { 60, 12, 11, 2, 6, 4, DVDReadPrio_libogc_bin,
         DVDReadPrio_libogc_bin_size, "DVD_ReadPrio (libogc)" };
   ```
   (Fingerprint measured from our binary with the `make_pattern` reimpl.)

3. **Scan + install** in the patch pass — a *standalone* check, independent of
   the SDK-cluster logic at patcher.c:4225:
   ```c
   if (compare_pattern(&fp, &DVDReadPrioLibogcSig)) {
       DVDReadPrioLibogcSig.offsetFoundAt = i;
       // install: place the blob in the reserved area and overwrite the
       // function entry with a branch to it (model on existing function-patch
       // installs, e.g. how the SD/DVD device patches get Calc_Address'd in).
   }
   ```

4. **Verify uniqueness** — confirm `{60,12,11,2,6,4}` matches only
   `DVD_ReadPrio` in the game image (no false-positive patches). If it collides,
   add a `findx_pattern` anchor check (a nearby call/const) like the SDK sigs do.

## Open questions to resolve at build time

- Does `VAR_CURRENT_DISC` resolve as an extern in the patch's link, or does it
  need the `reservedarea.h` macro form?
- Is a 32-byte cache line correct for GC (yes — 32B), and is the `dcbi` loop
  acceptable vs. calling an existing cache helper?
- `DVD_ReadPrio`'s `s64` offset high word (r7) is assumed 0 (ISOs < 4 GB). Fine
  for us; document the assumption.

## Test plan

Build the Swiss fork (`make dev` → `cube/swiss/swiss.dol`), deploy as the
gekkoboot `ipl.dol`, boot the non-embedded `testproj.iso`, and read `octlog.txt`:
success looks like `read boot.bin ... rc=0` / `MOUNTED OK: 79 entries`, and the
on-screen diagnostic goes **white → solid blue**.
