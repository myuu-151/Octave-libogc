#include "LuaBindings/StaticMesh_Lua.h"
#include "LuaBindings/Asset_Lua.h"
#include "LuaBindings/Material_Lua.h"

#include "LuaBindings/Vector_Lua.h"

#include "TableDatum.h"

#if LUA_ENABLED

int StaticMesh_Lua::GetMaterial(lua_State* L)
{
    StaticMesh* mesh = CHECK_STATIC_MESH(L, 1);

    Material* ret = mesh->GetMaterial();

    Asset_Lua::Create(L, ret);
    return 1;
}

int StaticMesh_Lua::SetMaterial(lua_State* L)
{
    StaticMesh* mesh = CHECK_STATIC_MESH(L, 1);
    Material* material = nullptr;
    if (!lua_isnil(L, 2)) { material = CHECK_MATERIAL(L, 2); }

    mesh->SetMaterial(material);

    return 0;
}

// mesh:StageColorsFrom(name, at, maxVertices) -> next, total, and mesh:ApplyStagedColors() -> bool:
// see StaticMesh::StageColorsFrom. next is -1 when it cannot (not a GameCube, not compact, another mesh).
int StaticMesh_Lua::StageColorsFrom(lua_State* L)
{
    StaticMesh* mesh = CHECK_STATIC_MESH(L, 1);
    const char* name = CHECK_STRING(L, 2);
    uint32_t at = (uint32_t)CHECK_INTEGER(L, 3);
    uint32_t maxVertices = (uint32_t)CHECK_INTEGER(L, 4);

    uint32_t total = 0;
    int32_t next = mesh->StageColorsFrom(name, at, maxVertices, total);
    lua_pushinteger(L, next);
    lua_pushinteger(L, total);
    return 2;
}

int StaticMesh_Lua::ApplyStagedColors(lua_State* L)
{
    StaticMesh* mesh = CHECK_STATIC_MESH(L, 1);
    lua_pushboolean(L, mesh->ApplyStagedColors());
    return 1;
}

// mesh:SetVertexData(xyz, rgba) -> bool: every vertex's position and colour, set anew (see
// StaticMesh::SetVertexData). xyz is a flat table of 3 numbers a vertex; rgba of 4 a vertex, 0-1.
int StaticMesh_Lua::SetVertexData(lua_State* L)
{
    StaticMesh* mesh = CHECK_STATIC_MESH(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TTABLE);
    uint32_t count = mesh->GetNumVertices();
    if ((uint32_t)luaL_len(L, 2) != count * 3 || (uint32_t)luaL_len(L, 3) != count * 4)
    {
        lua_pushboolean(L, false);
        return 1;
    }
    static std::vector<float> xyz;
    static std::vector<uint32_t> rgba;
    xyz.resize(count * 3);
    rgba.resize(count);
    for (uint32_t i = 0; i < count * 3; ++i)
    {
        lua_rawgeti(L, 2, (lua_Integer)(i + 1));
        xyz[i] = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t packed = 0;
        for (uint32_t c = 0; c < 4; ++c)
        {
            lua_rawgeti(L, 3, (lua_Integer)(i * 4 + c + 1));
            float v = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
            v = (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
            packed |= (uint32_t)(v * 255.0f + 0.5f) << (8 * c);
        }
        rgba[i] = packed;
    }
    lua_pushboolean(L, mesh->SetVertexData(xyz.data(), rgba.data(), count));
    return 1;
}

int StaticMesh_Lua::GetNumIndices(lua_State* L)
{
    StaticMesh* mesh = CHECK_STATIC_MESH(L, 1);

    uint32_t ret = mesh->GetNumIndices();

    lua_pushinteger(L, (int)ret);
    return 1;
}

int StaticMesh_Lua::GetNumFaces(lua_State* L)
{
    StaticMesh* mesh = CHECK_STATIC_MESH(L, 1);

    uint32_t ret = mesh->GetNumFaces();

    lua_pushinteger(L, (int)ret);
    return 1;
}

int StaticMesh_Lua::GetNumVertices(lua_State* L)
{
    StaticMesh* mesh = CHECK_STATIC_MESH(L, 1);

    uint32_t ret = mesh->GetNumVertices();

    lua_pushinteger(L, (int)ret);
    return 1;
}

int StaticMesh_Lua::HasVertexColor(lua_State* L)
{
    StaticMesh* mesh = CHECK_STATIC_MESH(L, 1);

    bool ret = mesh->HasVertexColor();

    lua_pushboolean(L, ret);
    return 1;
}

int StaticMesh_Lua::GetVertices(lua_State* L)
{
    StaticMesh* mesh = CHECK_STATIC_MESH(L, 1);

    int32_t numVerts = (int32_t)mesh->GetNumVertices();
    bool hasColor = mesh->HasVertexColor();
    Vertex* verts = hasColor ? nullptr : mesh->GetVertices();
    VertexColor* cverts = hasColor ? mesh->GetColorVertices() : nullptr;

    lua_newtable(L);
    int vertArrayIdx = lua_gettop(L);

    for (int32_t i = 0; i < numVerts; ++i)
    {
        lua_newtable(L);
        int vertTableIdx = lua_gettop(L);

        Vector_Lua::Create(L, hasColor ? cverts[i].mPosition : verts[i].mPosition);
        lua_setfield(L, vertTableIdx, "position");
        Vector_Lua::Create(L, hasColor ? cverts[i].mTexcoord0 : verts[i].mTexcoord0);
        lua_setfield(L, vertTableIdx, "texcoord0");
        Vector_Lua::Create(L, hasColor ? cverts[i].mTexcoord1 : verts[i].mTexcoord1);
        lua_setfield(L, vertTableIdx, "texcoord1");
        Vector_Lua::Create(L, hasColor ? cverts[i].mNormal : verts[i].mNormal);
        lua_setfield(L, vertTableIdx, "normal");

        if (hasColor)
        {
            lua_pushinteger(L, (int)cverts[i].mColor);
            lua_setfield(L, vertTableIdx, "color");
        }

        lua_seti(L, vertArrayIdx, i + 1);
    }

    // vertArray table should be on top
    return 1;
}

int StaticMesh_Lua::GetIndices(lua_State* L)
{
    StaticMesh* mesh = CHECK_STATIC_MESH(L, 1);

    IndexType* indices = mesh->GetIndices();
    int32_t numIndices = (int32_t)mesh->GetNumIndices();

    lua_newtable(L);
    int indexArrayIdx = lua_gettop(L);

    for (int32_t i = 0; i < numIndices; ++i)
    {
        lua_pushinteger(L, (int32_t)indices[i]);
        lua_seti(L, indexArrayIdx, i + 1);
    }

    // indexArray table should be on top.
    return 1;
}

int StaticMesh_Lua::HasTriangleMeshCollision(lua_State* L)
{
    StaticMesh* mesh = CHECK_STATIC_MESH(L, 1);

    bool ret = (mesh->GetTriangleCollisionShape() != nullptr);

    lua_pushboolean(L, ret);
    return 1;
}

int StaticMesh_Lua::EnableTriangleMeshCollision(lua_State* L)
{
    StaticMesh* mesh = CHECK_STATIC_MESH(L, 1);
    bool value = CHECK_BOOLEAN(L, 2);

    mesh->SetGenerateTriangleCollisionMesh(value);

    return 0;
}


void StaticMesh_Lua::Bind()
{
    lua_State* L = GetLua();
    int mtIndex = CreateClassMetatable(
        STATIC_MESH_LUA_NAME,
        STATIC_MESH_LUA_FLAG,
        ASSET_LUA_NAME);

    Asset_Lua::BindCommon(L, mtIndex);

    REGISTER_TABLE_FUNC(L, mtIndex, GetMaterial);

    REGISTER_TABLE_FUNC(L, mtIndex, SetMaterial);

    REGISTER_TABLE_FUNC(L, mtIndex, GetNumIndices);

    REGISTER_TABLE_FUNC(L, mtIndex, GetNumFaces);

    REGISTER_TABLE_FUNC(L, mtIndex, GetNumVertices);

    REGISTER_TABLE_FUNC(L, mtIndex, HasVertexColor);

    REGISTER_TABLE_FUNC(L, mtIndex, GetVertices);

    REGISTER_TABLE_FUNC(L, mtIndex, GetIndices);

    REGISTER_TABLE_FUNC(L, mtIndex, HasTriangleMeshCollision);

    REGISTER_TABLE_FUNC(L, mtIndex, EnableTriangleMeshCollision);

    REGISTER_TABLE_FUNC(L, mtIndex, StageColorsFrom);

    REGISTER_TABLE_FUNC(L, mtIndex, ApplyStagedColors);

    REGISTER_TABLE_FUNC(L, mtIndex, SetVertexData);

    lua_pop(L, 1);
    OCT_ASSERT(lua_gettop(L) == 0);
}

#endif
