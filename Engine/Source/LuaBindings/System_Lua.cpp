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

    bool ok = SYS_WriteSave(saveName, stream);

    lua_pushboolean(L, ok);
    return 1;
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
#if PLATFORM_GAMECUBE
size_t BigBlockCacheBytes();    // BigBlockCache_Dolphin.cpp: freed blocks kept for reuse
#endif

int System_Lua::GetFreeMemory(lua_State* L)
{
    lua_Integer freeBytes = 0;

#if PLATFORM_DOLPHIN
    struct mallinfo info = mallinfo();
    freeBytes = lua_Integer(info.fordblks) +
                lua_Integer((char*)SYS_GetArena1Hi() - (char*)SYS_GetArena1Lo());
#endif
#if PLATFORM_GAMECUBE
    freeBytes += lua_Integer(BigBlockCacheBytes());     // held for reuse, but free all the same
#endif

    lua_pushinteger(L, freeBytes);
    return 1;
}

// System.GetStorageMode() -> how the GameCube's SD card is being read: "dma27", "dma13.5", "pio27",
// "pio13.5", or "stock" when the engine's own SD driver mounted nothing (no card, or a disc).
// An empty string everywhere else. It is the only way to see, on the console itself, whether an
// adapter turned out to be semi-passive (DMA) or passive (PIO).
#if PLATFORM_GAMECUBE
extern "C" const char* OctSd_GetModeName(int chan);
#endif

// System.SetSaveInfo(title, description, iconHex [, bannerHex]) -- what the GameCube's memory
// card menu shows for the game's saves: a title and a description, 31 characters each, a 32 x 32
// icon given as 4096 hex digits, the 2048 bytes of an RGB5A3 texture in GX's tile order, and
// optionally a 96 x 32 banner as 7168 hex digits: 3072 bytes of CI8 in GX's tile order, then its
// 256-colour RGB5A3 palette. Saves written after this carry them. Does nothing on other platforms.
//
// System.GetSaveCard(saveName, dataBytes) -> state, blocksNeeded, blocksFree
// Whether a save of that size can be written to slot A's card; see SYS_GetSaveCardState for the
// states. "none" on platforms without memory cards.
#if PLATFORM_GAMECUBE
void SYS_SetSaveInfo(const char* title, const char* description, const uint8_t* iconRGB5A3, const uint8_t* bannerCI8);

static void HexToBytes(const char* hex, uint8_t* out, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        char pair[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
        out[i] = (uint8_t)strtoul(pair, nullptr, 16);
    }
}
const char* SYS_GetSaveCardState(const char* saveName, uint32_t dataBytes, int32_t& blocksNeeded, int32_t& blocksFree);
#endif

int System_Lua::SetSaveInfo(lua_State* L)
{
    const char* title = CHECK_STRING(L, 1);
    const char* description = CHECK_STRING(L, 2);
    size_t hexLen = 0;
    const char* hex = luaL_checklstring(L, 3, &hexLen);
    size_t bannerLen = 0;
    const char* bannerHex = lua_isstring(L, 4) ? lua_tolstring(L, 4, &bannerLen) : nullptr;
#if PLATFORM_GAMECUBE
    uint8_t icon[32 * 32 * 2];
    if (hexLen != sizeof(icon) * 2)
    {
        return luaL_error(L, "SetSaveInfo: the icon must be %d hex digits", (int)sizeof(icon) * 2);
    }
    HexToBytes(hex, icon, sizeof(icon));
    static uint8_t banner[96 * 32 + 256 * 2];
    if (bannerHex != nullptr && bannerLen != sizeof(banner) * 2)
    {
        return luaL_error(L, "SetSaveInfo: the banner must be %d hex digits", (int)sizeof(banner) * 2);
    }
    if (bannerHex != nullptr)
    {
        HexToBytes(bannerHex, banner, sizeof(banner));
    }
    SYS_SetSaveInfo(title, description, icon, bannerHex != nullptr ? banner : nullptr);
#else
    (void)title; (void)description; (void)hex; (void)hexLen; (void)bannerHex; (void)bannerLen;
#endif
    return 0;
}

int System_Lua::GetSaveCard(lua_State* L)
{
    const char* saveName = CHECK_STRING(L, 1);
    int32_t dataBytes = (int32_t)luaL_checkinteger(L, 2);
    const char* state = "none";
    int32_t needed = 0;
    int32_t freeBlocks = 0;
#if PLATFORM_GAMECUBE
    state = SYS_GetSaveCardState(saveName, (uint32_t)dataBytes, needed, freeBlocks);
#else
    (void)saveName; (void)dataBytes;
#endif
    lua_pushstring(L, state);
    lua_pushinteger(L, needed);
    lua_pushinteger(L, freeBlocks);
    return 3;
}

int System_Lua::GetStorageMode(lua_State* L)
{
#if PLATFORM_GAMECUBE
    lua_pushstring(L, OctSd_GetModeName(-1));
#else
    lua_pushstring(L, "");
#endif
    return 1;
}

// System.GetPerfReport() -> two strings: the average milliseconds of each frame stat over the last
// five seconds, and the same stats for the worst single frame of them. Empty off the consoles.
#if PLATFORM_DOLPHIN
const char* GetPerfAverageLine();
const char* GetPerfWorstLine();
#endif

// System.GetClockMs() -> the milliseconds since the game started, read now (not the frame's
// time): for timing work within a frame, and a seed. A whole number, and it wraps after 24 days.
int System_Lua::GetClockMs(lua_State* L)
{
    lua_pushinteger(L, (lua_Integer)((SYS_GetTimeMicroseconds() / 1000) & 0x7FFFFFFF));
    return 1;
}

int System_Lua::GetPerfReport(lua_State* L)
{
#if PLATFORM_DOLPHIN
    lua_pushstring(L, GetPerfAverageLine());
    lua_pushstring(L, GetPerfWorstLine());
#else
    lua_pushstring(L, "");
    lua_pushstring(L, "");
#endif
    return 2;
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
    REGISTER_TABLE_FUNC(L, tableIdx, GetStorageMode);
    REGISTER_TABLE_FUNC(L, tableIdx, SetSaveInfo);
    REGISTER_TABLE_FUNC(L, tableIdx, GetSaveCard);
    REGISTER_TABLE_FUNC(L, tableIdx, GetPerfReport);

    REGISTER_TABLE_FUNC(L, tableIdx, GetClockMs);

    REGISTER_TABLE_FUNC(L, tableIdx, SetScreenOrientation);

    REGISTER_TABLE_FUNC(L, tableIdx, GetScreenOrientation);

    REGISTER_TABLE_FUNC(L, tableIdx, SetFullscreen);

    REGISTER_TABLE_FUNC(L, tableIdx, IsFullscreen);

    REGISTER_TABLE_FUNC(L, tableIdx, SetWindowTitle);

    lua_setglobal(L, "System");

    OCT_ASSERT(lua_gettop(L) == 0);
}

#endif
