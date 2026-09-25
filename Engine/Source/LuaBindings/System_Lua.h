#pragma once

#include "Engine.h"

#if LUA_ENABLED

#define SYSTEM_LUA_NAME "System"

struct System_Lua
{
    static int WriteSave(lua_State* L);
    static int ReadSave(lua_State* L);
    static int DoesSaveExist(lua_State* L);
    static int DeleteSave(lua_State* L);
    static int UnmountMemoryCard(lua_State* L);
    static int GetFreeMemory(lua_State* L);
    static int GetAramStats(lua_State* L);
    static int MemoryCensus(lua_State* L);
    static int PinBlocks(lua_State* L);
    static int GetStorageMode(lua_State* L);
    static int SetSaveInfo(lua_State* L);
    static int GetSaveCard(lua_State* L);
    static int GetPerfReport(lua_State* L);
    static int GetClockMs(lua_State* L);
    static int PerfBegin(lua_State* L);
    static int PerfEnd(lua_State* L);

    static int SetScreenOrientation(lua_State* L);
    static int GetScreenOrientation(lua_State* L);
    static int SetFullscreen(lua_State* L);
    static int IsFullscreen(lua_State* L);

    static int SetWindowTitle(lua_State* L);

    static void Bind();
};

#endif

