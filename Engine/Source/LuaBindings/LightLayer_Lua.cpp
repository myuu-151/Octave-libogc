#include "LightLayer.h"
#include "Assets/Texture.h"

#include "LuaBindings/LuaUtils.h"
#include "LuaBindings/LightLayer_Lua.h"
#include "LuaBindings/Asset_Lua.h"

#if LUA_ENABLED

int LightLayer_Lua::Init(lua_State* L)
{
    uint32_t width = (uint32_t)CHECK_INTEGER(L, 1);
    uint32_t height = (uint32_t)CHECK_INTEGER(L, 2);

    LightLayer* layer = LightLayer::Get();
    if (layer->Init(width, height))
    {
        Asset_Lua::Create(L, layer->GetTexture());
    }
    else
    {
        lua_pushnil(L);
    }
    return 1;
}

int LightLayer_Lua::GetTexture(lua_State* L)
{
    Asset_Lua::Create(L, LightLayer::Get()->GetTexture(), true);
    return 1;
}

int LightLayer_Lua::SetBands(lua_State* L)
{
    int32_t bands = CHECK_INTEGER(L, 1);
    LightLayer::Get()->SetBands((uint32_t)bands);
    return 0;
}

int LightLayer_Lua::SetDarkness(lua_State* L)
{
    float darkest = CHECK_NUMBER(L, 1);
    LightLayer::Get()->SetDarkness(darkest);
    return 0;
}

int LightLayer_Lua::Clear(lua_State* L)
{
    LightLayer::Get()->Clear();
    return 0;
}

int LightLayer_Lua::AddLight(lua_State* L)
{
    float x = CHECK_NUMBER(L, 1);
    float y = CHECK_NUMBER(L, 2);
    float radius = CHECK_NUMBER(L, 3);
    float strength = 1.0f;
    if (!lua_isnone(L, 4)) { strength = CHECK_NUMBER(L, 4); }

    LightLayer::Get()->AddLight(x, y, radius, strength);
    return 0;
}

int LightLayer_Lua::Commit(lua_State* L)
{
    LightLayer::Get()->Commit();
    return 0;
}

void LightLayer_Lua::Bind()
{
    lua_State* L = GetLua();

    lua_newtable(L);
    int tableIdx = lua_gettop(L);

    REGISTER_TABLE_FUNC(L, tableIdx, Init);
    REGISTER_TABLE_FUNC(L, tableIdx, GetTexture);
    REGISTER_TABLE_FUNC(L, tableIdx, SetBands);
    REGISTER_TABLE_FUNC(L, tableIdx, SetDarkness);
    REGISTER_TABLE_FUNC(L, tableIdx, Clear);
    REGISTER_TABLE_FUNC(L, tableIdx, AddLight);
    REGISTER_TABLE_FUNC(L, tableIdx, Commit);

    lua_setglobal(L, LIGHT_LAYER_LUA_NAME);

    OCT_ASSERT(lua_gettop(L) == 0);
}

#endif
