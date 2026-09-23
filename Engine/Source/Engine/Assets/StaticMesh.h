#pragma once

#include <string>

#include "Assets/Material.h"
#include "Asset.h"
#include "Vertex.h"

#include "Graphics/GraphicsTypes.h"

#define CREATE_CONVEX_COLLISION_MESH (PLATFORM_WINDOWS || PLATFORM_LINUX)

#if EDITOR
#include <assimp/scene.h>
#endif

class StaticMesh : public Asset
{
public:

    DECLARE_ASSET(StaticMesh, Asset);

    StaticMesh();
    ~StaticMesh();

    void CreateRaw(
        uint32_t numVertices,
        Vertex* vertices,
        uint32_t numIndices,
        IndexType* indices);

    StaticMeshResource* GetResource();

    // Asset Interface
    virtual void LoadStream(Stream& stream, Platform platform) override;
    virtual bool CanLoadWindowed() const override { return true; }
    virtual void SaveStream(Stream& stream, Platform platform) override;
    virtual void Create() override;
    virtual void Destroy() override;
    virtual bool Import(const std::string& path, ImportOptions* options) override;
    virtual void GatherProperties(std::vector<Property>& outProps) override;
    virtual glm::vec4 GetTypeColor() override;
    virtual const char* GetTypeName() override;
    virtual const char* GetTypeImportExt() override;

    class Material* GetMaterial();
    void SetMaterial(class Material* newMaterial);

    uint32_t GetNumIndices() const;
    uint32_t GetNumFaces() const;
    uint32_t GetNumVertices() const;
    bool HasVertexColor() const;

    Vertex* GetVertices();
    VertexColor* GetColorVertices();
    IndexType* GetIndices();

    Bounds GetBounds() const;

    btBvhTriangleMeshShape* GetTriangleCollisionShape();
    btCollisionShape* GetCollisionShape();
    void SetCollisionShape(btCollisionShape* shape);
    void SetCollisionShapes(uint32_t numCollisionShapes, btCollisionShape** collisionShapes, btTransform* transforms, bool compound);

    void SetGenerateTriangleCollisionMesh(bool generate);
    bool IsTriangleCollisionMeshEnabled() const;
    // GX: the mesh was made compact (GFX_SetCompactUnlitMeshes) and needs its full arrays no more.
    void ReleaseSourceArrays();
    // GX: the vertices were read straight into the compact form (position, colour: 16 bytes).
    bool HasCompactVertices() const { return mCompactVertices; }
    // Hands the vertex array over (the compact renderer keeps it) and forgets it.
    void* TakeVertexArray();
    glm::vec3 GetVertexPosition(uint32_t index);
    uint32_t GetVertexSize() const;

    static bool HandlePropChange(Datum* datum, uint32_t index, const void* newValue);

#if EDITOR
    // Bake a scale into the geometry itself, so a node can carry it at scale 1 without changing
    // size. Rewrites vertex positions and normals, rescales the collision shapes and bounds, and
    // re-uploads the mesh. Editor-only: it modifies the asset, which then needs saving.
    void ApplyScale(glm::vec3 scale);
#endif

private:

    bool ShouldGenerateTriangleCollision() const;

    void CreateTriangleCollisionShape();
    void DestroyTriangleCollisionShape();

    void ResizeVertexArray(uint32_t newSize);
    void ResizeIndexArray(uint32_t newSize);

    void ComputeBounds();

    MaterialRef mMaterial;
    uint32_t mNumVertices;
    uint32_t mNumIndices;
    uint32_t mNumUvMaps;

    void* mVertices;
    IndexType* mIndices;

    Bounds mBounds;

    btCollisionShape* mCollisionShape;
    btBvhTriangleMeshShape* mTriangleCollisionShape;
    btTriangleIndexVertexArray* mTriangleIndexVertexArray;
    btTriangleInfoMap* mTriangleInfoMap;
    bool mGenerateTriangleCollisionMesh;
    bool mHasVertexColor;
    bool mCompactVertices = false;

    // Graphics Resource
    StaticMeshResource mResource;

#if EDITOR
public:
    void Create(
        const aiScene* scene,
        const aiMesh& meshData,
        uint32_t numCollisionMeshes,
        const aiMesh** collisionMeshes);
protected:
    std::vector<uint32_t> mPureVertexColors;
#endif

#if CREATE_CONVEX_COLLISION_MESH
public:
    void CreateCollisionMesh(btCollisionShape* collisionShape);
    std::vector<StaticMesh*> mCollisionMeshes;
#endif // EDITOR
};