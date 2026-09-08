# GameCube apploader (open source)

A minimal, from-source GameCube apploader used by **Package Project → GameCube**
to produce a bootable disc image (`.iso`/`.gcm`). It is compiled by devkitPPC and
bundled as `Standalone/Tools/gcn_apploader.img` — **not** shipped as an opaque
binary lifted from a retail disc.

## What it does

The console's IPL (BIOS) cannot run a game directly. It reads this apploader off
the disc into `0x81200000` and calls it. The apploader then:

1. Loads the disc **FST** into high RAM and records its address in the OS
   low-memory globals (`0x80000038` / `0x8000003C`), lowering the arena top
   (`0x80000034`) so nothing allocates over it. Octave's runtime
   (`System_Dolphin.cpp`) parses that FST to read assets straight off the disc.
2. Loads every section of `main.dol` to its target address.
3. Clears the DOL's BSS and returns the DOL entry point.

Protocol reference: the public-domain YAGCD "Apploader" chapter.

## Building

Requires devkitPPC. From the devkitPro MSYS2 shell (or any shell with
`DEVKITPPC` set and `powerpc-eabi-*` on PATH):

```
make            # -> ../gcn_apploader.img
make clean
```

`make` compiles `apploader.c` (freestanding, `-G0 -msdata=none` so it needs no
r2/r13 setup), links it at `0x81200000` via `apploader.ld` with `app_entry`
first, and `mkimg.py` stamps the apploader header (entry / body size / BSS size)
onto the raw body.

The committed `gcn_apploader.img` is the output of this build; rerun `make` only
if you change `apploader.c`.

## License

`apploader.c`, `apploader.ld`, `mkimg.py`, and `Makefile` are released into the
public domain (Unlicense). Do whatever you want with them.
