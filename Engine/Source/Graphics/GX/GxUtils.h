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

#endif