#include "EngineTypes.h"
#include "Log.h"
#include "Engine.h"
#include "Clock.h"
#include "Utilities.h"

#include "System/System.h"

#include "LuaBindings/System_Lua.h"
#include "LuaBindings/Stream_Lua.h"

#if LUA_ENABLED

int System_Lua::WriteSave(lua_State* L)
{
    const char* saveName = CHECK_STRING(L, 1);
    Stream& stream = CHECK_STREAM(L, 2);

    SYS_WriteSave(saveName, stream);

    return 0;
}

int System_Lua::ReadSave(lua_State* L)
{
    const char* saveName = CHECK_STRING(L, 1);
    Stream& stream = CHECK_STREAM(L, 2);

    SYS_ReadSave(saveName, stream);

    return 0;
}

int System_Lua::DoesSaveExist(lua_State* L)
{
    const char* saveName = CHECK_STRING(L, 1);

    bool ret = SYS_DoesSaveExist(saveName);

    lua_pushboolean(L, ret);
    return 1;
}

int System_Lua::DeleteSave(lua_State* L)
{
    const char* saveName = CHECK_STRING(L, 1);

    SYS_DeleteSave(saveName);

    return 0;
}

int System_Lua::UnmountMemoryCard(lua_State* L)
{
    SYS_UnmountMemoryCard();

    return 0;
}

int System_Lua::SetScreenOrientation(lua_State* L)
{
    int value = CHECK_INTEGER(L, 1);
    ScreenOrientation orientation = (ScreenOrientation)value;

    ::SetScreenOrientation(orientation);

    return 0;
}

int System_Lua::GetScreenOrientation(lua_State* L)
{
    ScreenOrientation ori = ::GetScreenOrientation();
    int32_t ret = (int32_t)ori;

    lua_pushinteger(L, ret);
    return 1;
}

int System_Lua::SetFullscreen(lua_State* L)
{
    bool fullscreen = CHECK_BOOLEAN(L, 1);

    SYS_SetFullscreen(fullscreen);

    return 0;
}

int System_Lua::IsFullscreen(lua_State* L)
{
    bool ret = SYS_IsFullscreen();

    lua_pushboolean(L, ret);
    return 1;
}

int System_Lua::SetWindowTitle(lua_State* L)
{
    const char* value = CHECK_STRING(L, 1);

    SYS_SetWindowTitle(value);

    return 0;
}

// System.GetFreeMemory() -> bytes the heap can still hand out, or 0 where that is not known.
// On the consoles this is the number to watch: what malloc holds free, plus what is left of the
// arena it grows into. A game that leaks shows up here long before it freezes.
#if PLATFORM_DOLPHIN
#include <malloc.h>
#include <ogc/system.h>
#endif

int System_Lua::GetFreeMemory(lua_State* L)
{
    lua_Integer freeBytes = 0;

#if PLATFORM_DOLPHIN
    struct mallinfo info = mallinfo();
    freeBytes = lua_Integer(info.fordblks) +
                lua_Integer((char*)SYS_GetArena1Hi() - (char*)SYS_GetArena1Lo());
#endif

    lua_pushinteger(L, freeBytes);
    return 1;
}

void System_Lua::Bind()
{
    lua_State* L = GetLua();

    lua_newtable(L);
    int tableIdx = lua_gettop(L);

    REGISTER_TABLE_FUNC(L, tableIdx, WriteSave);

    REGISTER_TABLE_FUNC(L, tableIdx, ReadSave);

    REGISTER_TABLE_FUNC(L, tableIdx, DoesSaveExist);

    REGISTER_TABLE_FUNC(L, tableIdx, DeleteSave);

    REGISTER_TABLE_FUNC(L, tableIdx, UnmountMemoryCard);

    REGISTER_TABLE_FUNC(L, tableIdx, GetFreeMemory);

    REGISTER_TABLE_FUNC(L, tableIdx, SetScreenOrientation);

    REGISTER_TABLE_FUNC(L, tableIdx, GetScreenOrientation);

    REGISTER_TABLE_FUNC(L, tableIdx, SetFullscreen);

    REGISTER_TABLE_FUNC(L, tableIdx, IsFullscreen);

    REGISTER_TABLE_FUNC(L, tableIdx, SetWindowTitle);

    lua_setglobal(L, "System");

    OCT_ASSERT(lua_gettop(L) == 0);
}

#endif
