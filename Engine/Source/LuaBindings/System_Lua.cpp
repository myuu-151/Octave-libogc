#include "EngineTypes.h"
#include "Log.h"
#include "Engine.h"
#include "Clock.h"
#include "Utilities.h"

#include "System/System.h"

#include "LuaBindings/System_Lua.h"
#include "LuaBindings/Stream_Lua.h"
#include "AssetManager.h"
#include "Assets/Texture.h"
#include "Assets/StaticMesh.h"
#include "Assets/SkeletalMesh.h"
#include "Assets/SoundWave.h"
#include "Assets/Font.h"
#include <algorithm>
#include <unordered_map>

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

// System.PinBlocks(bytes): on the GameCube, freed blocks of this size (to an eighth over) are kept
// for the next allocation of it and never given back to the heap (BigBlockCache_Dolphin.cpp) -- for
// what is streamed in and out all the time at one size, a sky's frames. 0 stops. Nothing elsewhere.
#if PLATFORM_GAMECUBE
void BigBlockCachePin(size_t size);
#endif
int System_Lua::PinBlocks(lua_State* L)
{
    lua_Integer bytes = luaL_checkinteger(L, 1);
#if PLATFORM_GAMECUBE
    BigBlockCachePin(bytes > 0 ? (size_t)bytes : 0);
#else
    (void)bytes;
#endif
    return 0;
}

// System.MemoryCensus([top]) -- DIAGNOSTIC: logs every loaded asset's memory (textures' texel data,
// meshes' arrays and display lists, sounds' samples), largest first (the first `top`, default 40),
// the totals by type, and the heap: free, and the largest single block malloc can still give.
// Returns the total bytes counted.
int System_Lua::MemoryCensus(lua_State* L)
{
    int top = (int)luaL_optinteger(L, 1, 40);
    struct Row { std::string name; std::string type; uint32_t bytes; };
    std::vector<Row> rows;
    std::unordered_map<std::string, uint32_t> byType;
    uint64_t total = 0;
    for (auto& it : AssetManager::Get()->GetAssetMap())
    {
        Asset* asset = it.second ? it.second->mAsset : nullptr;
        if (asset == nullptr) continue;
        uint32_t bytes = 0;
        TypeId type = asset->GetType();
        if (type == Texture::GetStaticType())
        {
#if API_GX
            TextureResource* r = static_cast<Texture*>(asset)->GetResource();
            bytes = r->mTplSize + r->mDynamicSize;
#endif
        }
        else if (type == StaticMesh::GetStaticType())
        {
            StaticMesh* m = static_cast<StaticMesh*>(asset);
            void* verts = m->HasVertexColor() ? (void*)m->GetColorVertices() : (void*)m->GetVertices();
            if (verts != nullptr) bytes += m->GetNumVertices() * m->GetVertexSize();
            if (m->GetIndices() != nullptr) bytes += m->GetNumIndices() * sizeof(IndexType);
#if API_GX
            StaticMeshResource* r = m->GetResource();
            bytes += r->mDisplayListSize + r->mColorDisplayListSize;
            if (r->mCompact && r->mCompactVertices != nullptr) bytes += m->GetNumVertices() * 16;
#endif
        }
        else if (type == SkeletalMesh::GetStaticType())
        {
            SkeletalMesh* m = static_cast<SkeletalMesh*>(asset);
            bytes = m->GetNumVertices() * sizeof(VertexSkinned) + m->GetNumIndices() * sizeof(IndexType);
        }
        else if (type == SoundWave::GetStaticType())
        {
            bytes = static_cast<SoundWave*>(asset)->GetWaveDataSize();
        }
        else if (type == Font::GetStaticType())
        {
#if API_GX
            Texture* t = static_cast<Font*>(asset)->GetTexture();
            if (t != nullptr && t->GetResource() != nullptr) bytes = t->GetResource()->mTplSize;
#endif
        }
        rows.push_back({ asset->GetName(), asset->GetTypeName(), bytes });
        byType[asset->GetTypeName()] += bytes;
        total += bytes;
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.bytes > b.bytes; });
    LogDebug("CENSUS %d assets loaded, %u KB counted", (int)rows.size(), (unsigned)(total / 1024));
    for (auto& t : byType) LogDebug("CENSUS type %-14s %6u KB", t.first.c_str(), t.second / 1024);
    for (int i = 0; i < (int)rows.size() && i < top; ++i)
        LogDebug("CENSUS %6u KB  %-12s %s", rows[i].bytes / 1024, rows[i].type.c_str(), rows[i].name.c_str());
    LogDebug("CENSUS lua %d KB", lua_gc(L, LUA_GCCOUNT, 0));
#if PLATFORM_DOLPHIN
    struct mallinfo info = mallinfo();
    size_t arena = (size_t)((char*)SYS_GetArena1Hi() - (char*)SYS_GetArena1Lo());
    // the largest block malloc can still give: halving down from 8 MB, then refining
    size_t lo = 0, hi = 8 * 1024 * 1024;
    while (hi - lo > 4096)
    {
        size_t mid = (lo + hi) / 2;
        void* p = malloc(mid);
        if (p != nullptr) { free(p); lo = mid; } else { hi = mid; }
    }
    LogDebug("CENSUS heap free %d KB (+ arena %u KB), largest block %u KB",
             info.fordblks / 1024, (unsigned)(arena / 1024), (unsigned)(lo / 1024));
#endif
    lua_pushinteger(L, (lua_Integer)total);
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
    REGISTER_TABLE_FUNC(L, tableIdx, MemoryCensus);
    REGISTER_TABLE_FUNC(L, tableIdx, PinBlocks);
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
