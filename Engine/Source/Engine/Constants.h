#pragma once

#define ENGINE_NAME "Octave"
#define OCTAVE_VERSION 5
#define MAX_PATH_SIZE 260

#define DEFAULT_GAME_NAME "Octave"
#define DEFAULT_WINDOW_WIDTH 1280
#define DEFAULT_WINDOW_HEIGHT 720

#define DEFAULT_TEXTURE_DIRECTORY_NAME "Engine/Assets/Textures/"
#define DEFAULT_DIFFUSE_TEXTURE_NAME "T_White"
#define DEFAULT_SPECULAR_TEXTURE_NAME "T_Black"
#define DEFAULT_NORMAL_TEXTURE_NAME "T_DefaultNormal"
#define DEFAULT_REFLECTIVE_TEXTURE_NAME "T_Black"
#define DEFAULT_EMISSIVE_TEXTURE_NAME "T_Black"
#define DEFAULT_ORM_TEXTURE_NAME "T_DefaultORM"

#define DEFAULT_TEXTURE_SIZE 4
#define MATERIAL_LITE_MAX_TEXTURES 4
#define MAX_LIGHTS_PER_FRAME 32
#define MAX_LIGHTS_PER_DRAW 8
#define MAX_BONE_INFLUENCES 4
#define MAX_BONES 128
#define MAX_UV_MAPS 2

// A MaterialLite texture slot whose UV Map is this takes its coordinates from the surface's
// view-space normal instead of a UV channel (sphere map / matcap):
//   u = n_view.x * 0.5 + 0.5,  v = -n_view.y * 0.5 + 0.5
// View space is the camera's (+X right, +Y up, +Z towards the viewer). v = 0 is the top row of
// the image, so a matcap rendered of a sphere seen from the front maps as it looks: the top of
// the picture lights up-facing surfaces, its right side right-facing ones, and its middle the
// surfaces facing the camera. The slot's UV offset/scale do not apply. Keep in step with
// UV_MAP_ENVIRONMENT in Engine/Shaders/GLSL/src/Common.glsl.
#define UV_MAP_ENVIRONMENT 2

// Warp (indirect texture) slots: a MaterialLite texture slot whose TEV mode is TevMode::Warp is
// not drawn. Its texture is a tileable offset map, sampled at the slot's own UV map (with that UV
// set's offset/scale, so it can scroll), and it shifts the coordinates of every other slot:
//   uv' = uv + (byte(G) - 128, byte(B) - 128) / 256 * strength
// Green moves U, blue moves V; 128 is no offset. Red and alpha are ignored (the GameCube's
// indirect unit reads only a texture's A, B and G, and CMPR has no usable alpha). The offset is
// in the warped slot's own coordinates (1.0 = one repeat of its texture), applied after its UV
// offset/scale. strength is the material's Emission value, which no rasterizer uses (it only
// feeds the light baker, which ignores it on a warp material). strength <= 0 turns the warp off.
// Only slots 1..3 can be warp slots (slot 0 is always the base texture), and only the first one
// counts; a later one is treated as Pass. Keep in step with TEV_MODE_WARP in
// Engine/Shaders/GLSL/src/Common.glsl. The 3DS ignores warp slots.
#define MATERIAL_LITE_WARP_MIN_SLOT 1

#define DEFAULT_AMBIENT_LIGHT_COLOR glm::vec4(0.1f, 0.1f, 0.1f, 1.0f)
#define DEFAULT_SHADOW_COLOR glm::vec4(0.0f, 0.0f, 0.0f, 0.8f)

#define SHADOW_MAP_RESOLUTION 2048
#define SHADOW_RANGE 50.0f
#define SHADOW_RANGE_Z 400.0f

#define LOGGING_ENABLED 1
#include "EngineFeatures.h"
#define CONSOLE_ENABLED OCT_CONSOLE  // (EngineFeatures.h: a game can leave the console out)
#define DEBUG_DRAW_ENABLED 1

#define INVALID_TYPE_ID 0
#define INVALID_NET_ID 0
#define INVALID_NODE_ID 0

#define INVALID_HOST_ID 0
#define SERVER_HOST_ID 1
#define AUTHORITY_HOST_ID 1

#define MAX_NET_FUNC_PARAMS 8

#define OCT_SESSION_NAME_LEN 31
#define OCT_MAX_SESSION_LIST_SIZE 32

#define EMBED_ALL_ASSETS 1

#define LARGE_BOUNDS 10000.0f

#if EDITOR
#define ASSET_LIVE_REF_TRACKING 1
#else
#define ASSET_LIVE_REF_TRACKING 0
#endif

#define LUA_ENABLED 1
#define LUA_TYPE_CHECK 1
