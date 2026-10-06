#pragma once

#include "EngineTypes.h"

#if LUA_ENABLED

#define LIGHT_LAYER_LUA_NAME "LightLayer"

// LightLayer: a layer of light for a 2D game (Engine/LightLayer.h).
//   texture = LightLayer.Init(width, height)       the layer's size in pixels; its texture, for a Quad
//   LightLayer.Clear()                             each frame: no lights
//   LightLayer.AddLight(x, y, radius, strength)    a light, in the layer's pixels
//   LightLayer.Commit()                            sum them, cut into bands, into the texture
//   LightLayer.SetBands(n), LightLayer.SetDarkness(d), LightLayer.GetTexture()
struct LightLayer_Lua
{
    static int Init(lua_State* L);
    static int GetTexture(lua_State* L);
    static int SetBands(lua_State* L);
    static int SetDarkness(lua_State* L);
    static int Clear(lua_State* L);
    static int AddLight(lua_State* L);
    static int Commit(lua_State* L);

    static void Bind();
};

#endif
