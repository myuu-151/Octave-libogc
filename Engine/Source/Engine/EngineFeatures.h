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
//
// A 2D game drawing everything itself (Castle Crashers on the GameCube) uses
// none of them: about 830 KB of code, and Bullet's 240 KB of pools at start.

#ifndef OCT_PHYSICS
#define OCT_PHYSICS 1
#endif

#ifndef OCT_NAVIGATION
#define OCT_NAVIGATION 1
#endif

#ifndef OCT_VORBIS
#define OCT_VORBIS 1
#endif
