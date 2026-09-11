# On-Disc ISO Asset Loading — Investigation Notes

*Branch: `dvd-loading`. Status: root cause proven, fix designed, not yet built.*

This is the running log of getting an Octave GameCube ISO to read its assets
**straight off the disc at runtime** on real hardware — not baked into the DOL,
not from an SD card, but from the disc image itself, the way a retail game does.

---

## TL;DR

- The disc-FST reader **works in Dolphin and would work on a real ODE.**
- On the target rig (**PicoBoot/PicoLoader + Swiss + a real, empty drive**) it
  **fails**: libogc's `DVD_ReadPrio` returns `rc=-1` or hangs.
- **Root cause:** Swiss serves disc reads by recognizing the **Nintendo SDK's**
  disc functions and hijacking them. Our game uses **libogc**, which is a
  structurally different implementation, so Swiss doesn't recognize it and our
  reads fall through to the real (empty) drive.
- **Fix (designed, not built):** fork Swiss and add a small standalone patch
  that recognizes libogc's synchronous `DVD_ReadPrio` and serves it from the SD
  ISO. The game stays 100% stock libogc; only Swiss changes.

---

## The three ways to load assets (and why we want the fourth)

1. **Embedded** — assets baked into the DOL, loaded from RAM. Works everywhere,
   but the whole game must fit in memory. (Currently working.)
2. **SD root** — assets read off an SD card via FAT at runtime. Works on the rig
   today. (Currently working.)
3. **On-disc (the goal)** — assets read off the disc FST at runtime. One
   self-contained ISO, no SD, nothing in RAM. This is what "vanilla Octave never
   managed."

---

## The hardware reality

### PicoLoader is a boot chip, not a runtime ODE

Straight from the author's repo:

> "This is an ODE-based modchip … **However, it is not a full optical drive
> emulator** and only allows executing small homebrew apps (e.g. swiss) from
> flash … The device emulates the disc drive during startup … **then re-enables
> the actual disc drive afterward.**"

So "ODE-based" describes *how it connects* (via the disc interface), **not** what
it does at runtime. Once it boots Swiss, it steps aside and the console is back
on its **real, empty drive**. That's why our reads make the laser seek and then
fail — they're hitting the physical drive, because nothing is emulating a disc
for the *running* game.

### What can actually feed a running game disc data

| | Boots homebrew | Serves ISO data while the game runs |
|---|---|---|
| **PicoLoader** | ✅ (its whole job) | ❌ (re-enables the real drive) |
| **Full ODE** (GC Loader, FlippyDrive, …) | ✅ | ✅ (hardware) |
| **Swiss** | — | ✅ (software emulation) |

On this rig, the only thing that can serve a running game is **Swiss's software
emulation**. Which is where the real problem lives.

---

## The bug

### Symptoms (in order of discovery)

- ISO boots via Swiss, then **crashes** / freezes; embedded builds masked it by
  falling back to RAM assets.
- Non-embedded ISO (assets *only* on disc): boots → **null-pointer crash** in
  `Renderer::LoadDefaultMeshes()` because `AssetRef::Get<StaticMesh>()` returns
  null (the default meshes couldn't be read off the disc).
- The disc laser physically **seeks** during the read attempt — proof the reads
  reach the *real* drive, not Swiss.

### The on-screen diagnostic (colorblind-safe)

Because there's no USB Gecko, `SYS_ShowDvdDiag()` paints the FST-mount result as
screen colors, using blue/yellow + solid/flashing (no red/green):

- **White flash** — reached the diagnostic, video up.
- **Solid blue** — FST mounted with no drive reset (the win).
- **Flashing blue** — mounted, but needed `DVD_Mount`'s reset.
- **Solid yellow** — read failed.
- **Flashing yellow** — read OK but FST data bad.
- **White → no signal** — the DVD call itself faulted.

Observed: **white → solid yellow → red crash.** Read failed, then null-deref.

### The SD-card logger — ground truth

`DvdLog()` appends to `octlog.txt` on the SD, flushing every line so it survives
a crash. The decisive trace:

```
==== boot: DVD diagnostic reached (video up, SD writable) ====
InitDVD: attempt 1
  DVD_Init() returned
  read boot.bin (no mount): rc=-1
  first read failed -> DVD_Mount()
  read boot.bin (after mount): rc=-1
==== InitDVD returned: sDvdDiag=1 sDvdActive=0 ====
```

`DVD_ReadPrio` returns **`rc=-1`** (a hard drive error), or on some boots doesn't
return at all (**hangs** — the log stops right after `DVD_Init()`). Either way:
the read went to the real empty drive. Swiss never intercepted it.

### The exception dump

```
PC   = 0x800BB5E4   (StaticMesh::SetCollisionShape, StaticMesh.cpp:607)
insn = 80630068  ->  lwz r3, 0x68(r3)
r3   = 0x00000000   DAR = 0x00000068   DSISR = 0x04000000
```

Textbook null deref — the downstream symptom of "no assets came back," not the
disease.

---

## Why: Swiss recognizes the SDK, not libogc

### How Swiss serves disc reads

Swiss can't hand the game a real disc, so it **hijacks the game's disc-read
functions** and feeds them data from the SD ISO instead. Two mechanisms:

1. **Function patching (primary):** Swiss scans the loaded game for known disc
   functions (`DVDLowRead`, `Read`, `__DVDInterruptHandler`, `OSExceptionInit`,
   …) and rewrites them to route through its own SD reader. It also patches the
   SDK's `OSExceptionInit` so the game's own init doesn't wipe Swiss's hooks.
2. **DSI register trap (fallback):** Swiss unmaps the console's I/O so any raw
   hardware-register access faults into its emulator (`emulator.c`, DSI handler
   at `0x80000300`).

### Why our reads slip through both

- Swiss finds functions by an **instruction-fingerprint** — the SDK is one
  stable codebase, so its fingerprints are known and fixed. **libogc is a
  different implementation** (a state machine — `__dvd_statebusy`,
  `__dvd_stateready`, … — not the SDK's flat `DVDLowRead`). Swiss's scanner finds
  none of its fingerprints, so **none of the patches apply.**
- With nothing patched, libogc's low-level init rebuilds the console's memory
  map and reinstalls exception vectors, which **tears down Swiss's DSI trap.**
  So the fallback is gone too.

Result: unrecognized + trap torn down → reads hit the real drive → `rc=-1`.

Retail games work because they're SDK. It's not Nintendo-only, either — Swiss's
`patcher.c` even carries fingerprints for the **SN Systems ProDG** compiler. It's
just that nobody ever added libogc — a libogc game reading its *own* disc is
extremely rare.

### The measurement: libogc ≠ SDK (proven)

Swiss identifies functions by `make_pattern()` (patcher.c:499): a 6-tuple of
`{Length, Loads, Stores, FCalls, Branch, Moves}` instruction counts. We
reimplemented it exactly and measured libogc in our binary:

| function | libogc fingerprint | nearest Swiss SDK pattern | match |
|---|---|---|---|
| `__Read` | `{41,10,14,1,0,4}` | `Read {54,22,17,3,2,4}` | ❌ |
| `__DVDInterruptHandler` | `{120,19,14,3,13,6}` | `{121,32,13,4,15,14}` | ❌ |
| `DVD_ReadPrio` | `{60,12,11,2,6,4}` | — | — |

No matches. So the easy "add libogc's signatures and let Swiss's existing patches
take over" shortcut is **off the table** — Swiss's patch *code* assumes SDK guts
libogc doesn't have.

---

## The fix plan: fork Swiss, redirect `DVD_ReadPrio`

### Why this route

- **Not libdvm** — we're deliberately staying off Extrems' libogc2/libdvm stack.
- **Not "fix the game"** — you can't steer a C compiler to hit Swiss's exact
  fingerprints, and mangling libogc would break the clean ODE path.
- **Patch Swiss** keeps the game **pure libogc** (portable to real ODEs), and
  Swiss is a separate tool you already swap as `ipl.dol` via gekkoboot. Forking
  it is GPL-legit and needs nobody's blessing — same as ProDG got added.

### The approach

`DVD_ReadPrio` is **synchronous** — it blocks until the read is done and returns
bytes/`-1`. That's the magic: no interrupt/callback flow to emulate. So we add a
**standalone** patch (not the SDK-cluster machinery at patcher.c:4225):

1. Scan the game for the `DVD_ReadPrio` fingerprint `{60,12,11,2,6,4}`.
2. Replace its body with a stub that:
   - reads its args (buffer, length, `s64` disc offset),
   - calls Swiss's SD-disc reader at that offset,
   - flushes/invalidates cache,
   - returns the length.

The engine's FST reader calls `DVD_ReadPrio` → Swiss's stub serves it from the
ISO → the FST mounts. Everything downstream (asset loads, scene) just works.

### Remaining work

- [ ] Write the redirect stub (libogc `DVD_ReadPrio` ABI → Swiss device read).
- [ ] Pin down Swiss's device-read entry point in its reserved-area runtime.
- [ ] Add the fingerprint scan + install to the Swiss fork's patcher.
- [ ] Build the Swiss fork, deploy as `ipl.dol`, boot-test with the SD logger.
- [ ] Verify the fingerprint is unique in the binary (no false-positive patches).

---

## Toolchain notes (building Swiss)

- Swiss builds against **libogc2** + `libdvm` + `ppc-{libdeflate,libmad,
  libpsoarchive,libxxhash,zlib-ng-compat}` + `gamecube-tools-git`. None of these
  are needed by *the game* — only to compile the Swiss tool.
- Minimal build target: `make dev` (compile-patches + compile) — skips the ISO
  packaging that needs `mkisofs`/`xorrisofs`. Output: `cube/swiss/swiss.dol`.
- devkitPro's package CDN (`pkg.devkitpro.org`) rate-limited/403'd this machine
  after a burst of requests. Notes: enable the curl `XferCommand` in msys2
  `pacman.conf`, and it **needs an explicit `-A` User-Agent** — the default curl
  UA gets 403. Cooldown or the devkitPro GUI updater also works.

---

## Debugging reference

- **On-screen colors:** white pre-marker → solid/flashing blue (mounted) or
  yellow (failed). Colorblind-safe (blue/yellow + solid/flashing, no red/green).
- **SD log:** `octlog.txt` at the SD root; appends per boot, flushes per line.
- **Fingerprint tool:** reimplements `make_pattern`; feed it `objdump -d` of the
  ELF to fingerprint any function in Swiss's format.
- **Dolphin can't help here:** it's too lenient to reproduce the failure, and
  Swiss's own low-level tricks don't run faithfully inside it (Swiss's input was
  dead when we tried). Real hardware + SD log is the debugging path.
