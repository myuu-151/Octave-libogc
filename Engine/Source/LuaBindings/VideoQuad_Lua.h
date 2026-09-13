#pragma once

#include "EngineTypes.h"
#include "Log.h"

#include "Nodes/Widgets/VideoQuad.h"

#include "LuaBindings/LuaUtils.h"

#if LUA_ENABLED

#define VIDEO_QUAD_LUA_NAME "VideoQuad"
#define VIDEO_QUAD_LUA_FLAG "cfVideoQuad"
#define CHECK_VIDEO_QUAD(L, arg) static_cast<VideoQuad*>(CheckNodeLuaType(L, arg, VIDEO_QUAD_LUA_NAME, VIDEO_QUAD_LUA_FLAG));

struct VideoQuad_Lua
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
    static int SetFillScreen(lua_State* L);
    static int GetFillScreen(lua_State* L);

    static void Bind();
};

#endif
