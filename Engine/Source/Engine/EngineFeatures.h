#pragma once

// Parts of Octave a game can leave out of its engine library, to save the
// console's memory: their code (and what they allocate at start) stays out of
// the game. All on by default; a game turns one off in its Makefile_GCN /
// Makefile_Wii (OCT_PHYSICS := 0 and so on), which builds the engine library
// it links with that way (Engine/Makefile_GCN: Build/GCN_<what's off>).
//
//   OCT_PHYSICS     Bullet: collision shapes, rigid bodies, ray and sweep
//                   tests. Off, nodes have no collision and the tests hit
//                   nothing.
//   OCT_NAVIGATION  Recast/Detour: World::FindNavPath and the rest. Off, they
//                   find no path.
//   OCT_VORBIS      Ogg Vorbis sounds. Off, a Vorbis SoundWave is silent (PCM
//                   and ADPCM play as before).
//   OCT_NETWORK     Online play: NetworkManager and its messages. Off, the
//                   game is never networked (NET_* stay; a game may stub them).
//   OCT_VIDEO       Video playback: VideoPlayer, VideoStream, VideoQuad,
//                   Video3D, VideoClip and the JPEG decoder.
//   OCT_SPLINES     Spline3D nodes.
//   OCT_SKELETAL    Skeletal meshes (SkeletalMesh, SkeletalMesh3D) and their
//                   drawing.
//   OCT_PARTICLES   Particle systems (ParticleSystem, Particle3D) and their
//                   drawing.
//   OCT_INSTANCING  InstancedMesh3D and its drawing.
//   OCT_TEXT3D      TextMesh3D (text as 3D geometry) and its drawing.
//   OCT_UI_EXTRAS   The widgets beyond text, quads and canvases: Button,
//                   ArrayWidget, Poly, PolyRect.
//   OCT_CONSOLE     The on-screen console (Console; CONSOLE_ENABLED).
//   OCT_STATS       The stats overlay (StatsOverlay).
//   OCT_LUA         Lua: the VM, the bindings and Script nodes (LUA_ENABLED).
//                   Off, the engine runs no scripts at all -- for a game written
//                   wholly in C++.
//
// Each part's node types are left unregistered with it, so a scene or script
// asking for one gets nothing. Off, nothing that refers to a part may be
// linked: Octave pulls a whole class in (its registration with it) for any
// reference, so every use is behind its switch.
//
// A 2D game drawing everything itself (Castle Crashers on the GameCube) uses
// none of them: about 830 KB of code and Bullet's 240 KB of pools at start for
// the first three, about 440 KB more for the rest.

#ifndef OCT_PHYSICS
#define OCT_PHYSICS 1
#endif

#ifndef OCT_NAVIGATION
#define OCT_NAVIGATION 1
#endif

#ifndef OCT_VORBIS
#define OCT_VORBIS 1
#endif

#ifndef OCT_NETWORK
#define OCT_NETWORK 1
#endif

#ifndef OCT_VIDEO
#define OCT_VIDEO 1
#endif

#ifndef OCT_SPLINES
#define OCT_SPLINES 1
#endif

#ifndef OCT_SKELETAL
#define OCT_SKELETAL 1
#endif

#ifndef OCT_PARTICLES
#define OCT_PARTICLES 1
#endif

#ifndef OCT_INSTANCING
#define OCT_INSTANCING 1
#endif

#ifndef OCT_TEXT3D
#define OCT_TEXT3D 1
#endif

#ifndef OCT_UI_EXTRAS
#define OCT_UI_EXTRAS 1
#endif

#ifndef OCT_CONSOLE
#define OCT_CONSOLE 1
#endif

#ifndef OCT_STATS
#define OCT_STATS 1
#endif

#ifndef OCT_LUA
#define OCT_LUA 1
#endif
