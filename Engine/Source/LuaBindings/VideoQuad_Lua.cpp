#include "LuaBindings/VideoQuad_Lua.h"
#include "LuaBindings/Quad_Lua.h"
#include "LuaBindings/Node_Lua.h"
#include "LuaBindings/Asset_Lua.h"
#include "LuaBindings/LuaUtils.h"

#include "Assets/VideoClip.h"

#if LUA_ENABLED

int VideoQuad_Lua::SetVideoClip(lua_State* L)
{
    VideoQuad* node = CHECK_VIDEO_QUAD(L, 1);
    VideoClip* clip = nullptr;

    if (!lua_isnil(L, 2))
    {
        Asset* asset = CHECK_ASSET(L, 2);
        if (asset == nullptr || asset->GetType() != VideoClip::GetStaticType())
        {
            return luaL_error(L, "SetVideoClip() expects a VideoClip");
        }
        clip = static_cast<VideoClip*>(asset);
    }

    node->SetVideoClip(clip);

    return 0;
}

int VideoQuad_Lua::GetVideoClip(lua_State* L)
{
    VideoQuad* node = CHECK_VIDEO_QUAD(L, 1);

    Asset_Lua::Create(L, node->GetVideoClip());
    return 1;
}

int VideoQuad_Lua::PlayVideo(lua_State* L)
{
    VideoQuad* node = CHECK_VIDEO_QUAD(L, 1);

    node->PlayVideo();

    return 0;
}

int VideoQuad_Lua::PauseVideo(lua_State* L)
{
    VideoQuad* node = CHECK_VIDEO_QUAD(L, 1);

    node->PauseVideo();

    return 0;
}

int VideoQuad_Lua::StopVideo(lua_State* L)
{
    VideoQuad* node = CHECK_VIDEO_QUAD(L, 1);

    node->StopVideo();

    return 0;
}

int VideoQuad_Lua::SeekVideo(lua_State* L)
{
    VideoQuad* node = CHECK_VIDEO_QUAD(L, 1);
    float seconds = CHECK_NUMBER(L, 2);

    node->SeekVideo(seconds);

    return 0;
}

int VideoQuad_Lua::IsPlaying(lua_State* L)
{
    VideoQuad* node = CHECK_VIDEO_QUAD(L, 1);

    lua_pushboolean(L, node->IsPlaying());
    return 1;
}

int VideoQuad_Lua::GetPlayTime(lua_State* L)
{
    VideoQuad* node = CHECK_VIDEO_QUAD(L, 1);

    lua_pushnumber(L, node->GetPlayTime());
    return 1;
}

int VideoQuad_Lua::GetDuration(lua_State* L)
{
    VideoQuad* node = CHECK_VIDEO_QUAD(L, 1);

    lua_pushnumber(L, node->GetDuration());
    return 1;
}

int VideoQuad_Lua::SetLoop(lua_State* L)
{
    VideoQuad* node = CHECK_VIDEO_QUAD(L, 1);
    bool value = CHECK_BOOLEAN(L, 2);

    node->SetLoop(value);

    return 0;
}

int VideoQuad_Lua::GetLoop(lua_State* L)
{
    VideoQuad* node = CHECK_VIDEO_QUAD(L, 1);

    lua_pushboolean(L, node->GetLoop());
    return 1;
}

int VideoQuad_Lua::SetAutoPlay(lua_State* L)
{
    VideoQuad* node = CHECK_VIDEO_QUAD(L, 1);
    bool value = CHECK_BOOLEAN(L, 2);

    node->SetAutoPlay(value);

    return 0;
}

int VideoQuad_Lua::GetAutoPlay(lua_State* L)
{
    VideoQuad* node = CHECK_VIDEO_QUAD(L, 1);

    lua_pushboolean(L, node->GetAutoPlay());
    return 1;
}

int VideoQuad_Lua::SetVolume(lua_State* L)
{
    VideoQuad* node = CHECK_VIDEO_QUAD(L, 1);
    float value = CHECK_NUMBER(L, 2);

    node->SetVolume(value);

    return 0;
}

int VideoQuad_Lua::GetVolume(lua_State* L)
{
    VideoQuad* node = CHECK_VIDEO_QUAD(L, 1);

    lua_pushnumber(L, node->GetVolume());
    return 1;
}

int VideoQuad_Lua::SetAudioEnabled(lua_State* L)
{
    VideoQuad* node = CHECK_VIDEO_QUAD(L, 1);
    bool value = CHECK_BOOLEAN(L, 2);

    node->SetAudioEnabled(value);

    return 0;
}

int VideoQuad_Lua::IsAudioEnabled(lua_State* L)
{
    VideoQuad* node = CHECK_VIDEO_QUAD(L, 1);

    lua_pushboolean(L, node->IsAudioEnabled());
    return 1;
}

int VideoQuad_Lua::SetFillScreen(lua_State* L)
{
    VideoQuad* node = CHECK_VIDEO_QUAD(L, 1);
    bool value = CHECK_BOOLEAN(L, 2);

    node->SetFillScreen(value);

    return 0;
}

int VideoQuad_Lua::GetFillScreen(lua_State* L)
{
    VideoQuad* node = CHECK_VIDEO_QUAD(L, 1);

    lua_pushboolean(L, node->GetFillScreen());
    return 1;
}

void VideoQuad_Lua::Bind()
{
    lua_State* L = GetLua();
    int mtIndex = CreateClassMetatable(
        VIDEO_QUAD_LUA_NAME,
        VIDEO_QUAD_LUA_FLAG,
        QUAD_LUA_NAME);

    Node_Lua::BindCommon(L, mtIndex);

    REGISTER_TABLE_FUNC(L, mtIndex, SetVideoClip);

    REGISTER_TABLE_FUNC(L, mtIndex, GetVideoClip);

    REGISTER_TABLE_FUNC(L, mtIndex, PlayVideo);

    REGISTER_TABLE_FUNC(L, mtIndex, PauseVideo);

    REGISTER_TABLE_FUNC(L, mtIndex, StopVideo);

    REGISTER_TABLE_FUNC(L, mtIndex, SeekVideo);

    REGISTER_TABLE_FUNC(L, mtIndex, IsPlaying);

    REGISTER_TABLE_FUNC(L, mtIndex, GetPlayTime);

    REGISTER_TABLE_FUNC(L, mtIndex, GetDuration);

    REGISTER_TABLE_FUNC(L, mtIndex, SetLoop);

    REGISTER_TABLE_FUNC(L, mtIndex, GetLoop);

    REGISTER_TABLE_FUNC(L, mtIndex, SetAutoPlay);

    REGISTER_TABLE_FUNC(L, mtIndex, GetAutoPlay);

    REGISTER_TABLE_FUNC(L, mtIndex, SetVolume);

    REGISTER_TABLE_FUNC(L, mtIndex, GetVolume);

    REGISTER_TABLE_FUNC(L, mtIndex, SetAudioEnabled);

    REGISTER_TABLE_FUNC(L, mtIndex, IsAudioEnabled);

    REGISTER_TABLE_FUNC(L, mtIndex, SetFillScreen);

    REGISTER_TABLE_FUNC(L, mtIndex, GetFillScreen);

    lua_pop(L, 1);
    OCT_ASSERT(lua_gettop(L) == 0);
}

#endif
