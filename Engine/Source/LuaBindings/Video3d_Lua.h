#pragma once

#include "EngineTypes.h"
#include "Log.h"
#include "Engine.h"

#include "Nodes/3D/Video3d.h"

#include "LuaBindings/Node_Lua.h"
#include "LuaBindings/LuaUtils.h"

#if LUA_ENABLED

#define VIDEO_3D_LUA_NAME "Video3D"
#define VIDEO_3D_LUA_FLAG "cfVideo3D"
#define CHECK_VIDEO_3D(L, arg) static_cast<Video3D*>(CheckNodeLuaType(L, arg, VIDEO_3D_LUA_NAME, VIDEO_3D_LUA_FLAG));

struct Video3D_Lua
{
    static int SetVideoClip(lua_State* L);
    static int GetVideoClip(lua_State* L);
    static int PlayVideo(lua_State* L);
    static int PauseVideo(lua_State* L);
    static int StopVideo(lua_State* L);
    static int SeekVideo(lua_State* L);
    static int IsPlaying(lua_State* L);
    static int GetPlayTime(lua_State* L);
    static int GetDuration(lua_State* L);
    static int SetLoop(lua_State* L);
    static int GetLoop(lua_State* L);
    static int SetAutoPlay(lua_State* L);
    static int GetAutoPlay(lua_State* L);
    static int SetVolume(lua_State* L);
    static int GetVolume(lua_State* L);
    static int SetAudioEnabled(lua_State* L);
    static int IsAudioEnabled(lua_State* L);
    static int GetVideoTexture(lua_State* L);

    static void Bind();
};

#endif
