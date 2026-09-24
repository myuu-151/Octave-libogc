#pragma once

#if API_GX

class Texture;
class StaticMesh;
class SkeletalMesh;
class StaticMesh3D;
class SkeletalMesh3D;
class Widget;

#include "Assets/MaterialLite.h"

#include <gccore.h>

void SetupLights();
void SetupLightMask(ShadingModel shadingModel,  uint8_t lightingChannels, bool useBakedLight);
void SetupLightingChannels();

void PrepareForwardRendering();
void PrepareUiRendering();

bool IsCpuSkinningRequired(SkeletalMesh3D* component);

void BindMaterial(MaterialLite* material, bool useVertexColor, bool useBakedLighting);
void BindStaticMesh(StaticMesh* staticMesh, uint32_t* instanceColors);
void BindSkeletalMesh(SkeletalMesh* skeletalMesh);

uint8_t ConfigTev(uint8_t tevStage, uint32_t textureSlot, TevMode mode, bool vertexColorBlend);

void ApplyWidgetRotation(Mtx& mtx, Widget* widget);

void* CreateMeshDisplayList(StaticMesh* staticMesh, bool useColor, uint32_t& outSize, bool compact = false);
void GFX_SetCompactUnlitMeshes(bool compact);
bool GFX_GetCompactUnlitMeshes();
bool GFX_MaterialAllowsCompact(class Material* material);
void CallMeshDisplayList(void* displayList, uint32_t size);
void DestroyMeshDisplayList(void* displayList);

// BATCH CULLING -- written and tested (Dolphin, 2026-09-24) but NOT HOOKED UP: nothing calls it.
// A mesh's display lists are batches with bounding spheres; this draws only the batches in view.
// To use it: call GxSetCullFrustum in GFX_BeginFrame with the camera's projection (tangents
// 1 / P[0][0] and 1 / P[1][1], near and far), and CallMeshDisplayListCulled in place of
// CallMeshDisplayList in GFX_DrawStaticMeshComp with the model-view matrix (before it is turned
// into the normal matrix). Sonic Pipe Dream left out only ~8% of its batches: its camera looks
// down the pipe, so nearly all of it is in view.
void CallMeshDisplayListCulled(void* displayList, uint32_t size, const Mtx modelView);
void GxSetCullFrustum(bool enabled, float tanHalfX, float tanHalfY, float nearZ, float farZ);
// For a mesh whose vertices move after its lists are built: its batches are never left out.
void GxMeshListsNoCull(void* displayList);

// The GPU may still be drawing the last frame while the CPU works on the next (Graphics_GX.cpp).
// GxWaitGpu: wait until it has drawn everything queued so far -- before rewriting in place any
// memory it reads. GxDeferFree: free a block it may still be reading once it is done with it.
void GxWaitGpu();
void GxDeferFree(void* block);
void GxCountDraw(uint32_t verts, uint32_t tris);

#endif