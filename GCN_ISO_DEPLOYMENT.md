# GameCube ISO Deployment for Octave (libogc)

Package an Octave project into a **real bootable GameCube disc image** (`.iso` / `.gcm`)
where the game reads its assets **straight off the disc** — no SD card, no assets
baked into RAM. This is the proper hardware-distribution path: burn it, boot it,
done.

---

## Why this matters

Before this, an Octave GameCube build could ship two ways, and both had a catch:

| Mode | How it works | Catch |
|------|--------------|-------|
| **Embedded DOL** | All assets compiled *into* the `.dol` | Everything must fit in RAM — hard ceiling on content |
| **SD-root** | Thin `.dol` + `Assets/` folder on an SD card | Requires an SD card + Swiss/Homebrew Channel to run |

Neither gives you a **single file you can put on a disc and run on a real console**
the way a retail game does. That's what this adds: assets live in the disc's own
filesystem and stream off it at runtime, so there's no RAM limit and no SD card.

---

## What it produces

Package Project → **GameCube** now emits, alongside the usual `.dol` and cooked
`Packaged/GameCube/` folder:

```
YourProject.iso        ← a genuine GameCube disc image
```

Boot it in Dolphin or burn it for real hardware. It's a standard GCM: disc header,
apploader, `main.dol`, and a File String Table (FST) pointing at every asset.

---

## How it works

Three cooperating pieces:

### 1. The apploader (`Standalone/Tools/apploader/`)
The GameCube's boot ROM can't run a game directly — it runs a small **apploader**
off the disc that loads everything into memory. This is a minimal, from-source,
public-domain apploader (compiled by devkitPPC, ~1 KB). It reads the disc's boot
header, loads the FST and the game's `main.dol`, and hands control to the game.

It's shipped **as source you can read**, not an opaque binary lifted from a retail
disc.

### 2. The disc builder (in the editor)
When you package for GameCube, the editor lays the cooked asset tree into a real
disc filesystem — every file 32-byte aligned (a DVD hardware requirement) — and
stitches together `boot header + apploader + main.dol + FST + file data` into the
`.iso`.

### 3. The runtime disc reader (`System_Dolphin.cpp`)
At runtime the game reads assets off the disc through libogc's DVD interface. It
parses the disc's FST to map an asset path (e.g. `Engine/Assets/Meshes/SM_Cube.oct`)
to its exact offset and length on the disc, then reads it. This sits behind the
same file-loading path the engine already uses, so **nothing else in the engine
had to change** — and SD-card builds still work exactly as before (the SD path is
tried first; only a real disc boot falls through to the DVD reader).

---

## How to use it

1. Open your project in the Octave editor.
2. **Package Project → GameCube.**
3. Grab `Packaged/GameCube/YourProject.iso`.
4. Boot it in Dolphin, or write it to a disc / load it via a disc-based loader on
   real hardware.

That's it. The runtime fix is compiled into the DOL during packaging, so any
project picks it up automatically.

---

## The interesting bugs (for the curious)

Getting a hand-rolled apploader + disc reader working on real GameCube boot flow
took untangling three non-obvious things, each found by reading the emulator's
boot log:

1. **The boot header isn't where you'd expect.** The apploader normally reads the
   DOL/FST offsets from low memory (`0x80000420+`), but not every loader populates
   that. Fix: the apploader reads the boot header straight from disc offset 0 and
   parses the offsets itself. (Also correct on real hardware.)

2. **libogc wipes the FST pointer.** The apploader records where it loaded the FST
   in an OS global (`0x80000038`) and lowers the memory arena to protect it — but
   libogc's own startup clears that global and resets the arena to the top of RAM,
   clobbering the in-memory FST. Fix: the runtime re-reads the FST from disc rather
   than trusting either.

3. **`DVD_Init()` isn't enough.** Disc reads returned `-1` until the drive was
   explicitly **mounted** (`DVD_Mount()` — a reset + disc-ID read). And the very
   first asset access happens before the drive is ready, so the reader retries
   until the mount succeeds.

---

## Limitations / notes

- **GameCube only** for now. Wii discs use encrypted partitions and are a separate
  (much larger) job.
- Tested in **Dolphin** (which emulates the boot flow and DVD reads faithfully).
  Real-hardware disc boot should work the same, but confirm on your own setup.
- The apploader is compiled from the bundled source; rebuild it with `make` in
  `Standalone/Tools/apploader/` only if you change it.
