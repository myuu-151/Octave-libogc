#if API_GX

#include "Graphics/Graphics.h"
#include "Graphics/GraphicsTypes.h"
#include "Graphics/GX/GxTypes.h"
#include "Graphics/GX/GxUtils.h"

#include "System/System.h"

#include "Engine.h"
#include "Renderer.h"
#include "Log.h"
#include "Maths.h"
#include "Assets/Material.h"
#include "Assets/SkeletalMesh.h"
#include "Assets/StaticMesh.h"
#include "Assets/Material.h"
#include "Assets/Texture.h"
#include "Nodes/Widgets/Quad.h"
#include "Nodes/Widgets/Text.h"
#include "Assets/Font.h"

#include "Nodes/3D/StaticMesh3d.h"
#include "Nodes/3D/SkeletalMesh3d.h"
#include "Nodes/3D/Particle3d.h"
#include "Nodes/3D/ShadowMesh3d.h"
#include "Nodes/3D/TextMesh3d.h"

#include "Assertion.h"
#include <stdlib.h>
#include <stdio.h>
#include <malloc.h>
#include <vector>

#include <gccore.h>
#define DEFAULT_FIFO_SIZE    (256*1024)

GxContext gGxContext;

void GFX_Initialize()
{
    LogDebug("GFX_Initialize");

    EngineState* engineState = GetEngineState();
    SystemState* systemState = &engineState->mSystem;
    GXRModeObj* rmode = &systemState->mGxRmode;

    // Alloc fifo
    gGxContext.mGpFifo = memalign(32, DEFAULT_FIFO_SIZE);
    memset(gGxContext.mGpFifo, 0, DEFAULT_FIFO_SIZE);

    GX_Init(gGxContext.mGpFifo, DEFAULT_FIFO_SIZE);

    GX_SetCopyClear({ 0, 0, 0, 0 }, GX_MAX_Z24);


    // other gx setup
    GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
    float yscale = GX_GetYScaleFactor(rmode->efbHeight, rmode->xfbHeight);
    uint32_t xfbHeight = GX_SetDispCopyYScale(yscale);
    GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetDispCopyDst(rmode->fbWidth, xfbHeight);
    GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
    GX_SetFieldMode(rmode->field_rendering, ((rmode->viHeight == 2 * rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));
    GX_SetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);

    GX_SetCullMode(GX_CULL_FRONT);
    GX_CopyDisp(systemState->mFrameBuffers[systemState->mFrameIndex], GX_TRUE);
    GX_SetDispCopyGamma(GX_GM_1_0);

    Mtx view;
    Mtx44 perspective;

    // setup our camera at the origin
    // looking down the -z axis with y up
    guVector cam = { 0.0F, 0.0F, 10.0F },
        up = { 0.0F, 1.0F, 0.0F },
        look = { 0.0F, 0.0F, -1.0F };
    guLookAt(view, &cam, &up, &look);


    // setup our projection matrix
    // this creates a perspective matrix with a view angle of 90,
    // and aspect ratio based on the display resolution
    f32 w = rmode->fbWidth;
    f32 h = rmode->efbHeight;
    guPerspective(perspective, 45, (f32)w / h, 0.1F, 300.0F);
    GX_LoadProjectionMtx(perspective, GX_PERSPECTIVE);

    GX_InvVtxCache();
    GX_InvalidateTexAll();
}

void GFX_Shutdown()
{

}

void GFX_BeginFrame()
{
    gGxContext.mWorld = Renderer::Get()->GetCurrentWorld();

    GXRModeObj* rmode = &GetEngineState()->mSystem.mGxRmode;

    SetupLights();

    GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);

    Mtx44 projection;
    Camera3D* camera = gGxContext.mWorld->GetActiveCamera();

    if (camera != nullptr)
    {
        glm::mat4 camProjection = camera->GetProjectionMatrix();
        memcpy(&projection, &camProjection, sizeof(Mtx44));

        if (camera->GetProjectionMode() == ProjectionMode::ORTHOGRAPHIC)
        {
            GX_LoadProjectionMtx(projection, GX_ORTHOGRAPHIC);
        }
        else
        {
            GX_LoadProjectionMtx(projection, GX_PERSPECTIVE);
        }
    }

    gGxContext.mColorScale = Renderer::Get()->GetColorScale();
    gGxContext.mInvColorScale = Renderer::Get()->GetColorScaleInverse();
    gGxContext.mColorScaleEnum = GX_CS_SCALE_1;
    if (gGxContext.mColorScale == 2.0f)
    {
        gGxContext.mColorScaleEnum = GX_CS_SCALE_2;
    }
    else if (gGxContext.mColorScale == 4.0f)
    {
        gGxContext.mColorScaleEnum = GX_CS_SCALE_4;
    }
}

void GFX_EndFrame()
{
    glm::vec4 clearColor = Renderer::Get()->GetClearColor();
    GXColor gxClear;
    gxClear.r = uint8_t(glm::clamp(clearColor.r * 255.0f, 0.0f, 255.0f));
    gxClear.g = uint8_t(glm::clamp(clearColor.g * 255.0f, 0.0f, 255.0f));
    gxClear.b = uint8_t(glm::clamp(clearColor.b * 255.0f, 0.0f, 255.0f));
    gxClear.a = uint8_t(glm::clamp(clearColor.a * 255.0f, 0.0f, 255.0f));
    GX_SetCopyClear(gxClear, GX_MAX_Z24);

    SystemState* systemState = &GetEngineState()->mSystem;
    systemState->mFrameIndex ^= 1; // flip framebuffer

    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
    GX_CopyDisp(systemState->mFrameBuffers[systemState->mFrameIndex], GX_TRUE);
    GX_DrawDone();

    VIDEO_SetNextFramebuffer(systemState->mFrameBuffers[systemState->mFrameIndex]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
}

void GFX_BeginScreen(uint32_t screenIndex)
{

}

void GFX_BeginView(uint32_t viewIndex)
{

}

bool GFX_ShouldCullLights()
{
    return false;
}

void GFX_BeginRenderPass(RenderPassId renderPassId)
{
    switch (renderPassId)
    {
    case RenderPassId::Forward:
        PrepareForwardRendering();
        break;
    case RenderPassId::Ui:
        PrepareUiRendering();
        break;
    default:
        break;
    }
}

void GFX_EndRenderPass()
{

}

void GFX_SetPipelineState(PipelineConfig config)
{

}

void GFX_SetViewport(int32_t x, int32_t y, int32_t width, int32_t height, bool handlePrerotation)
{
    GX_SetViewport((float) x, float(y), (float) width, float(height), 0, 1);
}

void GFX_SetScissor(int32_t x, int32_t y, int32_t width, int32_t height, bool handlePrerotation)
{
    GX_SetScissor(x, y, width, height);
}

glm::mat4 GFX_MakePerspectiveMatrix(float fovyDegrees, float aspectRatio, float zNear, float zFar)
{
    Mtx44 projection;
    guPerspective(projection, fovyDegrees, aspectRatio, zNear, zFar);
    glm::mat4 retMatrix = *reinterpret_cast<glm::mat4*>(&projection);
    return retMatrix;
}

glm::mat4 GFX_MakeOrthographicMatrix(float left, float right, float bottom, float top, float zNear, float zFar)
{
    Mtx44 projection;
    guOrtho(projection, top, bottom, left, right, zNear, zFar);
    glm::mat4 retMatrix = *reinterpret_cast<glm::mat4*>(&projection);
    return retMatrix;
}

void GFX_SetFog(const FogSettings& fogSettings)
{
    Camera3D* camera = gGxContext.mWorld->GetActiveCamera();

    float cameraNear = 0.0f;
    float cameraFar = 100.0f;
    uint8_t fogType = GX_FOG_NONE;

    if (camera)
    {
        cameraNear = camera->GetNearZ();
        cameraFar = camera->GetFarZ();

        if (camera->GetProjectionMode() == ProjectionMode::ORTHOGRAPHIC)
        {
            switch (fogSettings.mDensityFunc)
            {
            case FogDensityFunc::Linear: fogType = GX_FOG_ORTHO_LIN; break;
            case FogDensityFunc::Exponential: fogType = GX_FOG_ORTHO_EXP; break;
            default: break;
            }
        }
        else
        {
            switch (fogSettings.mDensityFunc)
            {
            case FogDensityFunc::Linear: fogType = GX_FOG_PERSP_LIN; break;
            case FogDensityFunc::Exponential: fogType = GX_FOG_PERSP_EXP; break;
            default: break;
            }
        }
    }

    glm::vec4 clampedColor = glm::clamp(fogSettings.mColor, 0.0f, 1.0f);
    GXColor fogColor =
    {
        uint8_t(clampedColor.r * 255.0f),
        uint8_t(clampedColor.g * 255.0f),
        uint8_t(clampedColor.b * 255.0f),
        uint8_t(clampedColor.a * 255.0f)
    };

    // Save off the fog state so that we can enable/disable on a per-material basis if needed.
    gGxContext.mFogType = fogSettings.mEnabled ? fogType : GX_FOG_NONE;
    gGxContext.mFogStartZ = fogSettings.mNear;
    gGxContext.mFogEndZ = fogSettings.mFar;
    gGxContext.mFogNearZ = cameraNear;
    gGxContext.mFogFarZ = cameraFar;
    gGxContext.mFogColor = fogColor;

    GX_SetFog(
        gGxContext.mFogType,
        gGxContext.mFogStartZ,
        gGxContext.mFogEndZ,
        gGxContext.mFogNearZ,
        gGxContext.mFogFarZ,
        gGxContext.mFogColor);
}

void GFX_DrawLines(const std::vector<Line>& lines)
{

}

void GFX_DrawFullscreen()
{

}

void GFX_ResizeWindow()
{

}

void GFX_Reset()
{

}

Node3D* GFX_ProcessHitCheck(World* world, int32_t x, int32_t y, uint32_t* outInstance)
{
    return nullptr;
}

uint32_t GFX_GetNumViews()
{
    return 1;
}

void GFX_SetFrameRate(int32_t frameRate)
{

}

void GFX_PathTrace()
{

}

void GFX_BeginLightBake()
{

}

void GFX_UpdateLightBake()
{

}

void GFX_EndLightBake()
{

}

bool GFX_IsLightBakeInProgress()
{
    return false;
}

float GFX_GetLightBakeProgress()
{
    return 0.0f;
}

void GFX_EnableMaterials(bool enable)
{

}

void GFX_BeginGpuTimestamp(const char* name)
{

}

void GFX_EndGpuTimestamp(const char* name)
{

}

// Texture
void GFX_CreateTextureResource(Texture* texture, std::vector<uint8_t>& data)
{
    TextureResource* resource = texture->GetResource();

    GX_InvalidateTexAll();

    if (texture->IsDynamic())
    {
        // Dynamic textures (e.g. video) use raw texel memory instead of a TPL: a
        // GX_TF_RGBA8 buffer, or for YUV textures three GX_TF_I8 planes that TEV
        // converts to RGB when drawn. GFX_SetTextureResourceData() can point them at
        // external memory instead. Clamp wrapping, since sizes needn't be powers of two.
        const uint32_t width = texture->GetWidth();
        const uint32_t height = texture->GetHeight();

        if (texture->IsYuv())
        {
            const uint32_t lumaSize = width * height;
            const uint32_t chromaSize = (width / 2) * (height / 2);
            resource->mDynamicData = SYS_AlignedMalloc(lumaSize, 32);
            resource->mDynamicCb = SYS_AlignedMalloc(chromaSize, 32);
            resource->mDynamicCr = SYS_AlignedMalloc(chromaSize, 32);

            if (resource->mDynamicData == nullptr || resource->mDynamicCb == nullptr || resource->mDynamicCr == nullptr)
            {
                LogError("Failed to allocate %ux%u YUV dynamic texture", width, height);
                GFX_DestroyTextureResource(texture);
                return;
            }

            // Black: Y = 0, Cb = Cr = 128.
            memset(resource->mDynamicData, 0, lumaSize);
            memset(resource->mDynamicCb, 128, chromaSize);
            memset(resource->mDynamicCr, 128, chromaSize);
            DCFlushRange(resource->mDynamicData, lumaSize);
            DCFlushRange(resource->mDynamicCb, chromaSize);
            DCFlushRange(resource->mDynamicCr, chromaSize);
            resource->mDynamicSize = lumaSize;
        }
        else
        {
            resource->mDynamicSize = ((width + 3) & ~3u) * ((height + 3) & ~3u) * 4;
            resource->mDynamicData = SYS_AlignedMalloc(resource->mDynamicSize, 32);

            if (resource->mDynamicData == nullptr)
            {
                LogError("Failed to allocate %u bytes for dynamic texture", resource->mDynamicSize);
                resource->mDynamicSize = 0;
                return;
            }

            memset(resource->mDynamicData, 0, resource->mDynamicSize);
        }

        GFX_SetTextureResourceData(texture, nullptr);

        if (!texture->IsYuv() && data.size() == width * height * 4)
        {
            GFX_UpdateTextureResourcePixels(texture, data.data());
        }
        return;
    }

    resource->mTplData = SYS_AlignedMalloc((uint32_t)data.size(), 32);

    // Running out of memory here used to be a write through a null pointer: the copy below went
    // ahead regardless and the game died inside memcpy at address 0, which says nothing about what
    // actually went wrong. Say which texture could not be allocated and how big it was, and leave
    // it untextured -- a missing texture is a far better failure than a corrupted heap, and it
    // names the thing to make smaller.
    if (resource->mTplData == nullptr)
    {
        LogError("Texture '%s': could not allocate %u bytes; out of memory.",
                 texture->GetName().c_str(), (uint32_t)data.size());
        return;
    }

    memcpy(resource->mTplData, data.data(), data.size());
    resource->mTplSize = (uint32_t)data.size();

    TPL_OpenTPLFromMemory(&resource->mTplFile, resource->mTplData, (uint32_t)data.size());
    TPL_GetTexture(&resource->mTplFile, 0, &resource->mGxTexObj);

    //uint32_t fmt = 0;
    //uint16_t width = 0;
    //uint16_t height = 0;
    //TPL_GetTextureInfo(&resource->mTplFile, 0, &fmt, &width, &height);
    //LogDebug("Loaded TPL ----- fmt: %d, w: %d, h: %d", fmt, width, height);

    bool mipmapped = texture->IsMipmapped();
    uint8_t minFilter = GX_LINEAR;
    uint8_t magFilter = GX_LINEAR;
    uint8_t wrap = GX_REPEAT;

    switch (texture->GetFilterType())
    {
    case FilterType::Nearest: minFilter = mipmapped ? GX_NEAR_MIP_LIN : GX_NEAR; magFilter = GX_NEAR; break;
    case FilterType::Linear: minFilter = mipmapped ? GX_LIN_MIP_LIN : GX_LINEAR; magFilter = GX_LINEAR; break;
    default: break;
    }

    switch (texture->GetWrapMode())
    {
    case WrapMode::Clamp: wrap = GX_CLAMP; break;
    case WrapMode::Repeat: wrap = GX_REPEAT; break;
    case WrapMode::Mirror: wrap = GX_MIRROR; break;
    default: break;
    }

    GX_InitTexObjWrapMode(&resource->mGxTexObj, wrap, wrap);
    GX_InitTexObjFilterMode(&resource->mGxTexObj, minFilter, magFilter);
}

void GFX_DestroyTextureResource(Texture* texture)
{
    TextureResource* resource = texture->GetResource();

    if (texture->IsDynamic())
    {
        void** buffers[3] = { &resource->mDynamicData, &resource->mDynamicCb, &resource->mDynamicCr };
        for (void** buffer : buffers)
        {
            if (*buffer != nullptr)
            {
                SYS_AlignedFree(*buffer);
                *buffer = nullptr;
            }
        }

        resource->mDynamicSize = 0;
        resource->mGxTexObj = { };
        resource->mGxTexObjCb = { };
        resource->mGxTexObjCr = { };
        return;
    }

    TPL_CloseTPLFile(&resource->mTplFile);

    if (resource->mTplData != nullptr)
    {
        SYS_AlignedFree(resource->mTplData);
        resource->mTplData = nullptr;
    }

    resource->mGxTexObj = { };
}

void GFX_UpdateTextureResourcePixels(Texture* texture, const uint8_t* rgba8)
{
    TextureResource* resource = texture->GetResource();
    if (resource->mDynamicData == nullptr || rgba8 == nullptr || texture->IsYuv())
    {
        return;
    }

    // GX_TF_RGBA8 stores texels in 4x4 blocks: 16 (A,R) byte pairs, then 16 (G,B)
    // pairs. Texels past the right/bottom edge repeat the edge pixel.
    const uint32_t width = texture->GetWidth();
    const uint32_t height = texture->GetHeight();
    const uint32_t blocksWide = (width + 3) / 4;
    const uint32_t blocksHigh = (height + 3) / 4;
    uint8_t* block = (uint8_t*)resource->mDynamicData;

    for (uint32_t by = 0; by < blocksHigh; ++by)
    {
        for (uint32_t bx = 0; bx < blocksWide; ++bx)
        {
            uint8_t* arPlane = block;
            uint8_t* gbPlane = block + 32;

            for (uint32_t py = 0; py < 4; ++py)
            {
                const uint32_t y = glm::min(by * 4 + py, height - 1);
                const uint8_t* srcRow = rgba8 + y * width * 4;

                for (uint32_t px = 0; px < 4; ++px)
                {
                    const uint32_t x = glm::min(bx * 4 + px, width - 1);
                    const uint8_t* src = srcRow + x * 4;
                    const uint32_t i = (py * 4 + px) * 2;

                    arPlane[i] = src[3];
                    arPlane[i + 1] = src[0];
                    gbPlane[i] = src[1];
                    gbPlane[i + 1] = src[2];
                }
            }

            block += 64;
        }
    }

    DCFlushRange(resource->mDynamicData, resource->mDynamicSize);
    GX_InvalidateTexAll();
}

void GFX_SetTextureResourceData(Texture* texture, uint8_t* const* planes)
{
    TextureResource* resource = texture->GetResource();
    if (resource->mDynamicData == nullptr)
    {
        return;
    }

    const uint32_t width = texture->GetWidth();
    const uint32_t height = texture->GetHeight();
    const uint8_t filter = (texture->GetFilterType() == FilterType::Nearest) ? GX_NEAR : GX_LINEAR;

    if (texture->IsYuv())
    {
        void* luma = planes ? planes[0] : resource->mDynamicData;
        void* cb = planes ? planes[1] : resource->mDynamicCb;
        void* cr = planes ? planes[2] : resource->mDynamicCr;

        GX_InitTexObj(&resource->mGxTexObj, luma, width, height, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GX_InitTexObjFilterMode(&resource->mGxTexObj, filter, filter);
        GX_InitTexObj(&resource->mGxTexObjCb, cb, width / 2, height / 2, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GX_InitTexObjFilterMode(&resource->mGxTexObjCb, filter, filter);
        GX_InitTexObj(&resource->mGxTexObjCr, cr, width / 2, height / 2, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GX_InitTexObjFilterMode(&resource->mGxTexObjCr, filter, filter);
    }
    else
    {
        void* texels = planes ? planes[0] : resource->mDynamicData;

        GX_InitTexObj(&resource->mGxTexObj, texels, width, height, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GX_InitTexObjFilterMode(&resource->mGxTexObj, filter, filter);
    }

    GX_InvalidateTexAll();
}

// Material
void GFX_CreateMaterialResource(Material* material)
{

}

void GFX_DestroyMaterialResource(Material* material)
{

}

// StaticMesh
void GFX_CreateStaticMeshResource(StaticMesh* staticMesh, bool hasColor, uint32_t numVertices, void* vertices, uint32_t numIndices, IndexType* indices)
{
    StaticMeshResource* resource = staticMesh->GetResource();

    // We need to rearrange the vertex color data?
    // Apparently dolphin expects rgba reversed
    // TODO: Do this when cooking assets for Dolphin platforms.
    if (hasColor && !staticMesh->HasCompactVertices())     // (a compact array is done below)
    {
        VertexColor* vertices = staticMesh->GetColorVertices();
        for (uint32_t i = 0; i < numVertices; ++i)
        {
            ReverseColorUint32(vertices[i].mColor);
        }
    }
    
    // Compact (opt in, GFX_SetCompactUnlitMeshes): vertex colours, an unlit untextured material,
    // no triangle collision to build from the full arrays. See BindStaticMesh.
    struct CompactVertex { float mX, mY, mZ; uint32_t mColor; };
    if (staticMesh->HasCompactVertices())
    {
        // read compact (StaticMesh::LoadStream): its colours into GX's order, and the array kept
        CompactVertex* compactVerts = (CompactVertex*)staticMesh->GetColorVertices();
        for (uint32_t i = 0; i < numVertices; ++i)
        {
            ReverseColorUint32(compactVerts[i].mColor);
        }
        DCFlushRange(compactVerts, numVertices * sizeof(CompactVertex));
        resource->mColorDisplayList = CreateMeshDisplayList(staticMesh, true, resource->mColorDisplayListSize, true);
        resource->mCompactVertices = staticMesh->TakeVertexArray();
        resource->mCompact = true;
        LogDebug("Mesh %s: compact (%u vertices)", staticMesh->GetName().c_str(), numVertices);
        return;
    }

    bool compact = GFX_GetCompactUnlitMeshes() && hasColor && numVertices > 0 &&
                   GFX_MaterialAllowsCompact(staticMesh->GetMaterial()) &&
                   !staticMesh->IsTriangleCollisionMeshEnabled();

    if (compact)
    {
        CompactVertex* out = (CompactVertex*)memalign(32, numVertices * sizeof(CompactVertex));
        if (out != nullptr)
        {
            const VertexColor* src = staticMesh->GetColorVertices();
            for (uint32_t i = 0; i < numVertices; ++i)
            {
                out[i].mX = src[i].mPosition.x;
                out[i].mY = src[i].mPosition.y;
                out[i].mZ = src[i].mPosition.z;
                out[i].mColor = src[i].mColor;          // already in GX's order (above)
            }
            DCFlushRange(out, numVertices * sizeof(CompactVertex));
            resource->mColorDisplayList = CreateMeshDisplayList(staticMesh, true, resource->mColorDisplayListSize, true);
            if (resource->mColorDisplayList != nullptr)
            {
                resource->mCompactVertices = out;
                resource->mCompact = true;
                return;
            }
            free(out);
        }
        // no room to make it compact: the ordinary way below
    }

    if (hasColor)
    {
        resource->mColorDisplayList = CreateMeshDisplayList(staticMesh, true, resource->mColorDisplayListSize);
    }
    else
    {
        resource->mDisplayList = CreateMeshDisplayList(staticMesh, false, resource->mDisplayListSize);
    }
}

void GFX_DestroyStaticMeshResource(StaticMesh* staticMesh)
{
    StaticMeshResource* resource = staticMesh->GetResource();

    if (resource->mDisplayList != nullptr)
    {
        DestroyMeshDisplayList(resource->mDisplayList);
        resource->mDisplayList = nullptr;
        resource->mDisplayListSize = 0;
    }

    if (resource->mColorDisplayList != nullptr)
    {
        DestroyMeshDisplayList(resource->mColorDisplayList);
        resource->mColorDisplayList = nullptr;
        resource->mColorDisplayListSize = 0;
    }

    if (resource->mCompactVertices != nullptr)
    {
        free(resource->mCompactVertices);
        resource->mCompactVertices = nullptr;
    }
    resource->mCompact = false;
}

// SkeletalMesh
void GFX_CreateSkeletalMeshResource(SkeletalMesh* skeletalMesh, uint32_t numVertices, VertexSkinned* vertices, uint32_t numIndices, IndexType* indices)
{

}

void GFX_DestroySkeletalMeshResource(SkeletalMesh* skeletalMesh)
{

}

// StaticMeshComp
void GFX_CreateStaticMeshCompResource(StaticMesh3D* staticMeshComp)
{

}

void GFX_DestroyStaticMeshCompResource(StaticMesh3D* staticMeshComp)
{

}

void GFX_UpdateStaticMeshCompResourceColors(StaticMesh3D* staticMeshComp)
{

}

void GFX_DrawStaticMeshComp(StaticMesh3D* staticMeshComp, StaticMesh* meshOverride)
{
    StaticMesh* mesh = meshOverride ? meshOverride : staticMeshComp->GetStaticMesh();

    if (mesh != nullptr)
    {
        StaticMeshResource* meshResource = mesh->GetResource();

        bool hasBakedLighting = staticMeshComp->HasBakedLighting();
        bool hasInstanceColors = staticMeshComp->HasInstanceColors();
        bool hasColor = mesh->HasVertexColor() || hasInstanceColors;
        
        uint32_t* instanceColors = nullptr;
        if (hasInstanceColors)
        {
            instanceColors = staticMeshComp->GetInstanceColors().data();
        }

        BindStaticMesh(mesh, instanceColors);

        MaterialLite* material = Material::AsLite(staticMeshComp->GetMaterial());

        if (material == nullptr)
        {
            material = Renderer::Get()->GetDefaultMaterial();
            OCT_ASSERT(material != nullptr);
        }

        BindMaterial(material, hasColor, hasBakedLighting);

        Mtx model;
        Mtx view;
        Mtx modelView;

        glm::mat4 modelSrc = glm::transpose(staticMeshComp->GetRenderTransform());
        glm::mat4 viewSrc = glm::transpose(gGxContext.mWorld->GetActiveCamera()->GetViewMatrix());

        memcpy(model, &modelSrc, sizeof(float) * 4 * 3);
        memcpy(view, &viewSrc, sizeof(float) * 4 * 3);
        guMtxConcat(view, model, modelView);

        GX_LoadPosMtxImm(modelView, GX_PNMTX0);

        Mtx modelViewInv;
        guMtxInverse(modelView, modelViewInv);
        guMtxTranspose(modelViewInv, modelView);
        GX_LoadNrmMtxImm(modelView, GX_PNMTX0);

        SetupLightMask(material->GetShadingModel(), staticMeshComp->GetLightingChannels(), hasBakedLighting);
        SetupLightingChannels();

        if (hasColor)
        {
            // Allow lazily creating display lists to convserve memory unless needed.
            // This scenario can happen when a static mesh asset has no vertex colors, but it
            // is being used by a component that has instance colors for baked lighting.
            if (meshResource->mColorDisplayList == nullptr)
            {
                meshResource->mColorDisplayList = CreateMeshDisplayList(mesh, true, meshResource->mColorDisplayListSize);
            }

            CallMeshDisplayList(meshResource->mColorDisplayList, meshResource->mColorDisplayListSize);
        }
        else
        {
            CallMeshDisplayList(meshResource->mDisplayList, meshResource->mDisplayListSize);
        }

        if (material->GetVertexColorMode() == VertexColorMode::TextureBlend)
        {
            // Reset swap tables used for texture blending.
            GX_SetTevSwapMode(0, GX_TEV_SWAP0, GX_TEV_SWAP0);
            GX_SetTevSwapMode(1, GX_TEV_SWAP0, GX_TEV_SWAP0);
            GX_SetTevSwapMode(2, GX_TEV_SWAP0, GX_TEV_SWAP0);
        }
    }
}

// SkeletalMeshComp
void GFX_CreateSkeletalMeshCompResource(SkeletalMesh3D* skeletalMeshComp)
{

}

void GFX_DestroySkeletalMeshCompResource(SkeletalMesh3D* skeletalMeshComp)
{

}

void GFX_ReallocateSkeletalMeshCompVertexBuffer(SkeletalMesh3D* skeletalMeshComp, uint32_t numVertices)
{

}

void GFX_UpdateSkeletalMeshCompVertexBuffer(SkeletalMesh3D* skeletalMeshComp, const std::vector<Vertex>& skinnedVertices)
{

}

void GFX_DrawSkeletalMeshComp(SkeletalMesh3D* skeletalMeshComp)
{
    SkeletalMesh* mesh = skeletalMeshComp->GetSkeletalMesh();

    if (mesh != nullptr)
    {
        bool cpuSkinned = IsCpuSkinningRequired(skeletalMeshComp);
        if (cpuSkinned)
        {
            Vertex* vertices = skeletalMeshComp->GetSkinnedVertices();
            uint32_t numVertices = skeletalMeshComp->GetNumSkinnedVertices();

            GX_ClearVtxDesc();
            GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
            GX_SetVtxDesc(GX_VA_NRM, GX_INDEX16);
            GX_SetVtxDesc(GX_VA_TEX0, GX_INDEX16);
            GX_SetVtxDesc(GX_VA_TEX1, GX_INDEX16);

            GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
            GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
            GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
            GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);

            GX_SetArray(GX_VA_POS, &vertices[0].mPosition, sizeof(Vertex));
            GX_SetArray(GX_VA_NRM, &vertices[0].mNormal, sizeof(Vertex));
            GX_SetArray(GX_VA_TEX0, &vertices[0].mTexcoord0, sizeof(Vertex));
            GX_SetArray(GX_VA_TEX1, &vertices[0].mTexcoord1, sizeof(Vertex));

            DCFlushRange(vertices, numVertices * sizeof(Vertex));
            GX_InvVtxCache();
        }
        else
        {
            BindSkeletalMesh(mesh);
        }

        MaterialLite* material = Material::AsLite(skeletalMeshComp->GetMaterial());

        if (material == nullptr)
        {
            material = Renderer::Get()->GetDefaultMaterial();
            OCT_ASSERT(material != nullptr);
        }

        BindMaterial(material, false, false);

        Mtx model;
        Mtx view;
        Mtx modelView;

        glm::mat4 modelSrc = glm::transpose(skeletalMeshComp->GetRenderTransform());
        glm::mat4 viewSrc = glm::transpose(gGxContext.mWorld->GetActiveCamera()->GetViewMatrix());

        memcpy(model, &modelSrc, sizeof(float) * 4 * 3);
        memcpy(view, &viewSrc, sizeof(float) * 4 * 3);
        guMtxConcat(view, model, modelView);

        Mtx modelViewInv;
        Mtx normalMtx;
        guMtxInverse(modelView, modelViewInv);
        guMtxTranspose(modelViewInv, normalMtx);

        if (cpuSkinned)
        {
            // Only need to set up a single position/normal matrix pair
            GX_LoadPosMtxImm(modelView, GX_PNMTX0);
            GX_LoadNrmMtxImm(normalMtx, GX_PNMTX0);
        }
        else
        {
            // Need to set up one position/normal matrix pair per bone.
            uint32_t numBones = skeletalMeshComp->GetNumBones();
            for (uint32_t i = 0; i < numBones; ++i)
            {
                Mtx fullTransform;
                Mtx bone;
                glm::mat4 boneSrc = glm::transpose(skeletalMeshComp->GetBoneTransform(i));
                memcpy(bone, &boneSrc, sizeof(float) * 4 * 3);
                guMtxConcat(modelView, bone, fullTransform);
                GX_LoadPosMtxImm(fullTransform, GX_PNMTX0 + i*3);

                Mtx normalTransform;
                guMtxConcat(normalMtx, bone, normalTransform);
                GX_LoadNrmMtxImm(normalTransform, GX_PNMTX0 + i*3);
            }
        }

        SetupLightMask(material->GetShadingModel(), skeletalMeshComp->GetLightingChannels(), false);
        SetupLightingChannels();

        // GX_Begin's vertex count is a u16, so a primitive tops out at 65535 vertices --
        // 21845 triangles. Anything larger wrapped silently and the GP read the surplus
        // vertex data as commands (see CreateMeshDisplayList for the same fix).
        const uint32_t numFaces = mesh->GetNumFaces();
        const uint32_t kMaxFacesPerBatch = 21845;
        const uint32_t numBatches = (numFaces + kMaxFacesPerBatch - 1) / kMaxFacesPerBatch;

        const uint16_t* indices = mesh->GetIndices();

        for (uint32_t batch = 0; batch < numBatches; ++batch)
        {
        const uint32_t firstFace = batch * kMaxFacesPerBatch;
        const uint32_t batchFaces = (numFaces - firstFace < kMaxFacesPerBatch)
                                  ? (numFaces - firstFace)
                                  : kMaxFacesPerBatch;

        GX_Begin(GX_TRIANGLES, GX_VTXFMT0, uint16_t(batchFaces * 3));

        if (cpuSkinned)
        {
            for (uint32_t i = firstFace; i < firstFace + batchFaces; ++i)
            {
                GX_Position1x16(indices[i * 3 + 0]);
                GX_Normal1x16(indices[i * 3 + 0]);
                GX_TexCoord1x16(indices[i * 3 + 0]);
                GX_TexCoord1x16(indices[i * 3 + 0]);

                GX_Position1x16(indices[i * 3 + 1]);
                GX_Normal1x16(indices[i * 3 + 1]);
                GX_TexCoord1x16(indices[i * 3 + 1]);
                GX_TexCoord1x16(indices[i * 3 + 1]);

                GX_Position1x16(indices[i * 3 + 2]);
                GX_Normal1x16(indices[i * 3 + 2]);
                GX_TexCoord1x16(indices[i * 3 + 2]);
                GX_TexCoord1x16(indices[i * 3 + 2]);
            }
        }
        else
        {
            const VertexSkinned* verts = mesh->GetVertices().data();

            for (uint32_t i = firstFace; i < firstFace + batchFaces; ++i)
            {
                uint8_t bone0 = 3 * verts[indices[i * 3 + 0]].mBoneIndices[0];
                uint8_t bone1 = 3 * verts[indices[i * 3 + 1]].mBoneIndices[0];
                uint8_t bone2 = 3 * verts[indices[i * 3 + 2]].mBoneIndices[0];

                GX_MatrixIndex1x8(bone0);
                GX_Position1x16(indices[i * 3 + 0]);
                GX_Normal1x16(indices[i * 3 + 0]);
                GX_TexCoord1x16(indices[i * 3 + 0]);
                GX_TexCoord1x16(indices[i * 3 + 0]);

                GX_MatrixIndex1x8(bone1);
                GX_Position1x16(indices[i * 3 + 1]);
                GX_Normal1x16(indices[i * 3 + 1]);
                GX_TexCoord1x16(indices[i * 3 + 1]);
                GX_TexCoord1x16(indices[i * 3 + 1]);

                GX_MatrixIndex1x8(bone2);
                GX_Position1x16(indices[i * 3 + 2]);
                GX_Normal1x16(indices[i * 3 + 2]);
                GX_TexCoord1x16(indices[i * 3 + 2]);
                GX_TexCoord1x16(indices[i * 3 + 2]);
            }
        }

        GX_End();
        }
    }
}

bool GFX_IsCpuSkinningRequired(SkeletalMesh3D* skeletalMeshComp)
{
    return IsCpuSkinningRequired(skeletalMeshComp);
}

// ShadowMeshComp
void GFX_DrawShadowMeshComp(ShadowMesh3D* shadowMeshComp)
{
    StaticMesh* mesh = shadowMeshComp->GetStaticMesh();

    if (mesh != nullptr)
    {
        BindStaticMesh(mesh, nullptr);

        // Shading
        gGxContext.mLighting.mEnabled = false;
        gGxContext.mLighting.mColorChannel = false;

        glm::vec4 shadowColor = gGxContext.mWorld->GetShadowColor();
        GX_SetChanMatColor(GX_COLOR0A0, { uint8_t(shadowColor.r * 255.0f),
                                            uint8_t(shadowColor.g * 255.0f),
                                            uint8_t(shadowColor.b * 255.0f),
                                            uint8_t(shadowColor.a * 255.f) });

        GX_SetNumTexGens(0);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);

        GX_SetNumTevStages(1);

        gGxContext.mLighting.mMaterialSrc = GX_SRC_REG;

        // Matrices
        Mtx model;
        Mtx view;
        Mtx modelView;

        glm::mat4 modelSrc = glm::transpose(shadowMeshComp->GetRenderTransform());
        glm::mat4 viewSrc = glm::transpose(gGxContext.mWorld->GetActiveCamera()->GetViewMatrix());

        memcpy(model, &modelSrc, sizeof(float) * 4 * 3);
        memcpy(view, &viewSrc, sizeof(float) * 4 * 3);
        guMtxConcat(view, model, modelView);

        GX_LoadPosMtxImm(modelView, GX_PNMTX0);

        Mtx modelViewInv;
        guMtxInverse(modelView, modelViewInv);
        guMtxTranspose(modelViewInv, modelView);
        GX_LoadNrmMtxImm(modelView, GX_PNMTX0);

        SetupLightingChannels();

        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
        GX_SetZCompLoc(GX_TRUE);

        // (1) Back faces
        GX_SetZMode(GX_TRUE, GX_GREATER, GX_FALSE);
        GX_SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
        GX_SetColorUpdate(GX_FALSE);
        GX_SetAlphaUpdate(GX_TRUE);
        GX_SetCullMode(GX_CULL_BACK); // Reverse triangle winding
        CallMeshDisplayList(mesh->GetResource()->mDisplayList, mesh->GetResource()->mDisplayListSize);

        // (2) Front faces
        GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
        GX_SetBlendMode(GX_BM_BLEND, GX_BL_DSTALPHA, GX_BL_INVDSTALPHA, GX_LO_CLEAR);
        GX_SetColorUpdate(GX_TRUE);
        GX_SetAlphaUpdate(GX_FALSE);
        GX_SetCullMode(GX_CULL_FRONT); // Reverse triangle winding
        CallMeshDisplayList(mesh->GetResource()->mDisplayList, mesh->GetResource()->mDisplayListSize);

        // (3) Clear alpha channel
        GX_SetZMode(GX_FALSE, GX_LEQUAL, GX_FALSE);
        GX_SetBlendMode(GX_BM_BLEND, GX_BL_ZERO, GX_BL_ZERO, GX_LO_CLEAR);
        GX_SetColorUpdate(GX_FALSE);
        GX_SetAlphaUpdate(GX_TRUE);
        GX_SetCullMode(GX_CULL_FRONT); // Reverse triangle winding
        CallMeshDisplayList(mesh->GetResource()->mDisplayList, mesh->GetResource()->mDisplayListSize);
    }
}

// InstancedMeshComp
void GFX_DrawInstancedMeshComp(InstancedMesh3D* instancedMeshComp)
{

}

// TextMeshComp
void GFX_CreateTextMeshCompResource(TextMesh3D* textMeshComp)
{

}

void GFX_DestroyTextMeshCompResource(TextMesh3D* textMeshComp)
{

}

void GFX_UpdateTextMeshCompVertexBuffer(TextMesh3D* textMeshComp, const std::vector<Vertex>& vertices)
{

}

void GFX_DrawTextMeshComp(TextMesh3D* textMeshComp)
{
    if (textMeshComp->GetNumVisibleCharacters() == 0)
        return;

    const Vertex* vertices = textMeshComp->GetVertices();
    uint32_t numChars = (uint32_t)textMeshComp->GetNumVisibleCharacters();
    uint32_t numVertices = numChars * 6;

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_NRM, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX1, GX_DIRECT);

    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);

    // TODO: Are both of these cache functions necessary to call?
    DCFlushRange((void*)vertices, numVertices * sizeof(Vertex));
    GX_InvVtxCache();

    MaterialLite* material = Material::AsLite(textMeshComp->GetMaterial());

    if (material == nullptr)
    {
        material = Renderer::Get()->GetDefaultMaterial();
        OCT_ASSERT(material != nullptr);
    }

    BindMaterial(material, false, false);

    Mtx model;
    Mtx view;
    Mtx modelView;

    glm::mat4 modelSrc = glm::transpose(textMeshComp->GetRenderTransform());
    glm::mat4 viewSrc = glm::transpose(gGxContext.mWorld->GetActiveCamera()->GetViewMatrix());

    memcpy(model, &modelSrc, sizeof(float) * 4 * 3);
    memcpy(view, &viewSrc, sizeof(float) * 4 * 3);
    guMtxConcat(view, model, modelView);

    GX_LoadPosMtxImm(modelView, GX_PNMTX0);

    Mtx modelViewInv;
    guMtxInverse(modelView, modelViewInv);
    guMtxTranspose(modelViewInv, modelView);
    GX_LoadNrmMtxImm(modelView, GX_PNMTX0);

    SetupLightMask(material->GetShadingModel(), textMeshComp->GetLightingChannels(), false);
    SetupLightingChannels();

    GX_Begin(GX_TRIANGLES, GX_VTXFMT0, numVertices);

    const Vertex* vert = nullptr;

    for (uint32_t i = 0; i < numVertices; i++)
    {
        vert = &vertices[i];
        GX_Position3f32(vert->mPosition.x, vert->mPosition.y, vert->mPosition.z);
        GX_Normal3f32(vert->mNormal.x, vert->mNormal.y, vert->mNormal.z);
        GX_TexCoord2f32(vert->mTexcoord0.x, vert->mTexcoord0.y);
        GX_TexCoord2f32(vert->mTexcoord1.x, vert->mTexcoord1.y);
    }

    GX_End();

    if (material->GetVertexColorMode() == VertexColorMode::TextureBlend)
    {
        // Reset swap tables used for texture blending.
        GX_SetTevSwapMode(0, GX_TEV_SWAP0, GX_TEV_SWAP0);
        GX_SetTevSwapMode(1, GX_TEV_SWAP0, GX_TEV_SWAP0);
        GX_SetTevSwapMode(2, GX_TEV_SWAP0, GX_TEV_SWAP0);
    }
}

// ParticleComp
void GFX_CreateParticleCompResource(Particle3D* particleComp)
{

}

void GFX_DestroyParticleCompResource(Particle3D* particleComp)
{

}

void GFX_UpdateParticleCompVertexBuffer(Particle3D* particleComp, const std::vector<VertexParticle>& vertices)
{

}

void GFX_DrawParticleComp(Particle3D* particleComp)
{
    if (particleComp->GetNumParticles() > 0)
    {
        const std::vector<VertexParticle>& vertices = particleComp->GetVertices();
        uint32_t numVertices = (uint32_t)vertices.size();

        GX_ClearVtxDesc();
        GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
        GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);

        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

        // TODO: Are both of these cache functions necessary to call?
        DCFlushRange((void*)vertices.data(), numVertices * sizeof(VertexParticle));
        GX_InvVtxCache();

        MaterialLite* material = Material::AsLite(particleComp->GetMaterial());

        if (material == nullptr)
        {
            material = Renderer::Get()->GetDefaultMaterial();
            OCT_ASSERT(material != nullptr);
        }

        BindMaterial(material, true, false);

        Mtx model;
        Mtx view;
        Mtx modelView;

        glm::mat4 modelSrc = particleComp->GetUseLocalSpace() ? glm::transpose(particleComp->GetTransform()) : glm::mat4(1);
        glm::mat4 viewSrc = glm::transpose(gGxContext.mWorld->GetActiveCamera()->GetViewMatrix());

        memcpy(model, &modelSrc, sizeof(float) * 4 * 3);
        memcpy(view, &viewSrc, sizeof(float) * 4 * 3);
        guMtxConcat(view, model, modelView);

        GX_LoadPosMtxImm(modelView, GX_PNMTX0);

        uint32_t numParticles = particleComp->GetNumParticles();
        OCT_ASSERT(numParticles * 4 == numVertices);

        SetupLightMask(material->GetShadingModel(), particleComp->GetLightingChannels(), false);
        SetupLightingChannels();

        GX_Begin(GX_TRIANGLES, GX_VTXFMT0, numParticles * 6);

        const VertexParticle* vert = nullptr;

        for (uint32_t i = 0; i < numParticles * 4; i += 4)
        {
            // A particle should have a constant color across all vertices.
            uint8_t* color = (uint8_t*)&(vertices[i + 0].mColor);

            vert = &vertices[i + 0];
            GX_Position3f32(vert->mPosition.x, vert->mPosition.y, vert->mPosition.z);
            GX_Color4u8(color[3], color[2], color[1], color[0]);
            GX_TexCoord2f32(vert->mTexcoord.x, vert->mTexcoord.y);

            vert = &vertices[i + 1];
            GX_Position3f32(vert->mPosition.x, vert->mPosition.y, vert->mPosition.z);
            GX_Color4u8(color[3], color[2], color[1], color[0]);
            GX_TexCoord2f32(vert->mTexcoord.x, vert->mTexcoord.y);

            vert = &vertices[i + 2];
            GX_Position3f32(vert->mPosition.x, vert->mPosition.y, vert->mPosition.z);
            GX_Color4u8(color[3], color[2], color[1], color[0]);
            GX_TexCoord2f32(vert->mTexcoord.x, vert->mTexcoord.y);

            vert = &vertices[i + 2];
            GX_Position3f32(vert->mPosition.x, vert->mPosition.y, vert->mPosition.z);
            GX_Color4u8(color[3], color[2], color[1], color[0]);
            GX_TexCoord2f32(vert->mTexcoord.x, vert->mTexcoord.y);

            vert = &vertices[i + 1];
            GX_Position3f32(vert->mPosition.x, vert->mPosition.y, vert->mPosition.z);
            GX_Color4u8(color[3], color[2], color[1], color[0]);
            GX_TexCoord2f32(vert->mTexcoord.x, vert->mTexcoord.y);

            vert = &vertices[i + 3];
            GX_Position3f32(vert->mPosition.x, vert->mPosition.y, vert->mPosition.z);
            GX_Color4u8(color[3], color[2], color[1], color[0]);
            GX_TexCoord2f32(vert->mTexcoord.x, vert->mTexcoord.y);
        }

        GX_End();
    }
}

// Quad
void GFX_CreateQuadResource(Quad* quad)
{

}

void GFX_DestroyQuadResource(Quad* quad)
{

}

void GFX_UpdateQuadResourceVertexData(Quad* quad)
{

}

// TEV setup for YUV textures (video): converts full-range (JPEG) YCbCr to RGB on
// the GPU, then applies vertex color and the widget's uniform color.
//   R = Y + 1.402 (Cr - 0.5)
//   G = Y - 0.344 (Cb - 0.5) - 0.714 (Cr - 0.5)
//   B = Y + 1.772 (Cb - 0.5)
// Computed at half scale so every coefficient fits an 8-bit konst color, then
// doubled. Intermediate stages don't clamp, so negative values carry through.
static void SetupYuvTevStages(Texture* texture, GXColor uniformColor)
{
    TextureResource* resource = texture->GetResource();
    GX_LoadTexObj(&resource->mGxTexObj, GX_TEXMAP0);     // Y
    GX_LoadTexObj(&resource->mGxTexObjCb, GX_TEXMAP1);   // Cb
    GX_LoadTexObj(&resource->mGxTexObjCr, GX_TEXMAP2);   // Cr

    // Normalized texture coordinates are scaled per texcoord by the size of the texture
    // they're paired with, so the half-size chroma planes need their own texcoord
    // (sharing TEXCOORD0 with the luma plane samples one of them at the wrong scale).
    GX_SetNumTexGens(2);
    GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);

    GX_SetNumTevStages(8);

    GX_SetTevColorS10(GX_TEVREG0, { -90, 68, -113, 0 }); // Offsets / 2
    GX_SetTevColor(GX_TEVREG1, uniformColor);
    GX_SetTevKColor(GX_KCOLOR0, { 179, 0, 0, 0 });       // Cr -> R  (1.402 / 2)
    GX_SetTevKColor(GX_KCOLOR1, { 0, 91, 0, 0 });        // Cr -> G  (0.714 / 2)
    GX_SetTevKColor(GX_KCOLOR2, { 0, 0, 226, 0 });       // Cb -> B  (1.772 / 2)
    GX_SetTevKColor(GX_KCOLOR3, { 0, 44, 0, 0 });        // Cb -> G  (0.344 / 2)

    // Stage 0: Y / 2 + offsets
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
    GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_HALF, GX_CC_C0);
    GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
    GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

    // Stages 1-4: add / subtract each chroma channel times its per-channel coefficients
    const uint8_t chromaStages[4] = { GX_TEVSTAGE1, GX_TEVSTAGE2, GX_TEVSTAGE3, GX_TEVSTAGE4 };
    const uint32_t chromaMaps[4] = { GX_TEXMAP2, GX_TEXMAP2, GX_TEXMAP1, GX_TEXMAP1 };
    const uint8_t chromaKonst[4] = { GX_TEV_KCSEL_K0, GX_TEV_KCSEL_K1, GX_TEV_KCSEL_K2, GX_TEV_KCSEL_K3 };
    const uint8_t chromaOps[4] = { GX_TEV_ADD, GX_TEV_SUB, GX_TEV_ADD, GX_TEV_SUB };

    for (uint32_t i = 0; i < 4; ++i)
    {
        const uint8_t stage = chromaStages[i];
        GX_SetTevOrder(stage, GX_TEXCOORD1, chromaMaps[i], GX_COLORNULL);
        GX_SetTevKColorSel(stage, chromaKonst[i]);
        GX_SetTevColorIn(stage, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_CPREV);
        GX_SetTevColorOp(stage, chromaOps[i], GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
        GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    }

    // Stage 5: double back to full scale and clamp
    GX_SetTevOrder(GX_TEVSTAGE5, GX_TEXCOORDNULL, GX_TEXMAP_DISABLE, GX_COLORNULL);
    GX_SetTevColorIn(GX_TEVSTAGE5, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
    GX_SetTevColorOp(GX_TEVSTAGE5, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaIn(GX_TEVSTAGE5, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    GX_SetTevAlphaOp(GX_TEVSTAGE5, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

    // Stage 6: modulate by vertex color
    GX_SetTevOrder(GX_TEVSTAGE6, GX_TEXCOORDNULL, GX_TEXMAP_DISABLE, GX_COLOR0A0);
    GX_SetTevColorIn(GX_TEVSTAGE6, GX_CC_ZERO, GX_CC_CPREV, GX_CC_RASC, GX_CC_ZERO);
    GX_SetTevColorOp(GX_TEVSTAGE6, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaIn(GX_TEVSTAGE6, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
    GX_SetTevAlphaOp(GX_TEVSTAGE6, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

    // Stage 7: modulate by the widget's uniform color
    GX_SetTevOrder(GX_TEVSTAGE7, GX_TEXCOORDNULL, GX_TEXMAP_DISABLE, GX_COLOR0A0);
    GX_SetTevColorIn(GX_TEVSTAGE7, GX_CC_ZERO, GX_CC_C1, GX_CC_CPREV, GX_CC_ZERO);
    GX_SetTevColorOp(GX_TEVSTAGE7, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaIn(GX_TEVSTAGE7, GX_CA_ZERO, GX_CA_A1, GX_CA_APREV, GX_CA_ZERO);
    GX_SetTevAlphaOp(GX_TEVSTAGE7, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

void GFX_DrawQuad(Quad* quad)
{
    // Setup render state / TEVs
    Renderer* renderer = Renderer::Get();
    Texture* texture = quad->GetTexture() ? quad->GetTexture() : renderer->mWhiteTexture.Get<Texture>();

    glm::vec4 uniColor = glm::clamp(quad->GetColor(), 0.0f, 1.0f);
    GXColor uniColorGx = { uint8_t(uniColor.r * 255.0f),
                           uint8_t(uniColor.g * 255.0f),
                           uint8_t(uniColor.b * 255.0f),
                           uint8_t(uniColor.a * 255.0f) };

    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);

    if (texture->IsYuv())
    {
        SetupYuvTevStages(texture, uniColorGx);
    }
    else
    {
        GX_SetNumTevStages(2);
        GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GX_LoadTexObj(&texture->GetResource()->mGxTexObj, GX_TEXMAP0);

        // TEV1 applies uniform color modulation
        GX_SetTevColor(GX_TEVREG0, uniColorGx);

        GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_C0, GX_CC_CPREV, GX_CC_ZERO);
        GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_A0, GX_CA_APREV, GX_CA_ZERO);
        GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_DISABLE, GX_COLOR0A0);
    }

    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);

    // Draw verts
    //DCFlushRange(mVertices, 4 * sizeof(VertexUI));
    //GX_InvVtxCache();

    Mtx modelViewUI;
    guMtxIdentity(modelViewUI);
    ApplyWidgetRotation(modelViewUI, quad);
    GX_LoadPosMtxImm(modelViewUI, GX_PNMTX0);

    VertexUI* vertices = quad->GetVertices();

    gGxContext.mLighting.mEnabled = false;
    gGxContext.mLighting.mMaterialSrc = GX_SRC_VTX;
    SetupLightingChannels();

    GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT0, 4);

    for (uint32_t i = 0; i < 4; ++i)
    {
        GX_Position2f32(vertices[i].mPosition.x,
            vertices[i].mPosition.y);

        // Color needs to come before texcoord for some reason.
        uint8_t* color = (uint8_t*)&(vertices[i].mColor);
        GX_Color4u8(color[3], color[2], color[1], color[0]);

        GX_TexCoord2f32(vertices[i].mTexcoord.x,
            vertices[i].mTexcoord.y);
    }

    GX_End();
}

// Text
void GFX_CreateTextResource(Text* text)
{

}

void GFX_DestroyTextResource(Text* text)
{

}

void GFX_UpdateTextResourceUniformData(Text* text)
{

}

void GFX_UpdateTextResourceVertexData(Text* text)
{

}

void GFX_DrawText(Text* text)
{
    Renderer* renderer = Renderer::Get();

    Rect rect = text->GetRect();
    Font* font = text->GetFont();
    uint32_t numVisibleChars = text->GetNumVisibleCharacters();
    glm::vec2 justOff = text->GetJustifiedOffset();

    Mtx modelViewUI;
    guMtxIdentity(modelViewUI);
    glm::vec2 translation = glm::vec2(rect.mX + justOff.x, rect.mY + justOff.y);
    int32_t fontSize = font ? font->GetSize() : 32;
    float textScale = text->GetScaledTextSize() / fontSize;
    guMtxScale(modelViewUI, textScale, textScale, 1.0);
    guMtxTransApply(modelViewUI, modelViewUI, translation.x, translation.y, 0.0f);
    ApplyWidgetRotation(modelViewUI, text);
    GX_LoadPosMtxImm(modelViewUI, GX_PNMTX0);

    // Setup render state / TEVs
    Texture* texture = renderer->mWhiteTexture.Get<Texture>();

    if (font != nullptr &&
        font->GetTexture() != nullptr)
    {
        texture = font->GetTexture();
    }

    GX_SetNumTevStages(2);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);

    glm::vec4 uniColor = glm::clamp(text->GetColor(), 0.0f, 1.0f);
    GX_SetTevColor(GX_TEVREG0, { uint8_t(uniColor.r * 255.0f),
                                 uint8_t(uniColor.g * 255.0f),
                                 uint8_t(uniColor.b * 255.0f),
                                 uint8_t(uniColor.a * 255.0f) });

    GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_C0, GX_CC_CPREV, GX_CC_ZERO);
    GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_A0, GX_CA_APREV, GX_CA_ZERO);
    GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_DISABLE, GX_COLOR0A0);

    // Font's used to be RGB, but they are now stored RGBA so we don't need to swizzle.
    // Keeping the code here as a reference in case swizzling comes in handy in the future.
    //GX_SetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP1);
    //GX_SetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_RED);

    GX_LoadTexObj(&texture->GetResource()->mGxTexObj, GX_TEXMAP0);

    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);

    // Draw verts
    //DCFlushRange(mVertices, 4 * sizeof(VertexUI));
    //GX_InvVtxCache();

    VertexUI* vertices = text->GetVertices();

    gGxContext.mLighting.mEnabled = false;
    gGxContext.mLighting.mMaterialSrc = GX_SRC_VTX;
    SetupLightingChannels();

    GX_Begin(GX_TRIANGLES, GX_VTXFMT0, 6 * numVisibleChars);

    for (uint32_t i = 0; i < 6 * numVisibleChars; ++i)
    {
        GX_Position2f32(vertices[i].mPosition.x,
            vertices[i].mPosition.y);

        // Color needs to come before texcoord for some reason.
        uint8_t* color = (uint8_t*)&(vertices[i].mColor);
        GX_Color4u8(color[3], color[2], color[1], color[0]);

        GX_TexCoord2f32(vertices[i].mTexcoord.x,
            vertices[i].mTexcoord.y);
    }

    GX_End();

    //GX_SetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
    //GX_SetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
}

// Poly
void GFX_CreatePolyResource(Poly* poly)
{

}

void GFX_DestroyPolyResource(Poly* poly)
{

}

void GFX_UpdatePolyResourceUniformData(Poly* poly)
{

}

void GFX_UpdatePolyResourceVertexData(Poly* poly)
{

}

void GFX_DrawPoly(Poly* poly)
{

}

void GFX_DrawStaticMesh(StaticMesh* mesh, Material* material, const glm::mat4& transform, glm::vec4 color)
{
    // TODO: For debug drawing
}

void GFX_RenderPostProcessPasses()
{
    
}

#endif