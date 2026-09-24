#include "Assets/StaticMesh.h"
#if API_GX && !EDITOR
#include <gccore.h>
#include "System/System.h"
#endif
#include "Renderer.h"
#include "Vertex.h"
#include "AssetManager.h"
#include "Utilities.h"
#include "Log.h"
#include <new>

#include "Graphics/Graphics.h"
#if API_GX
#include "Graphics/GX/GxUtils.h"
#endif
#include "Assets/Material.h"

#if EDITOR
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include "EditorUtils.h"
#endif

#include "btBulletDynamicsCommon.h"
#include "BulletCollision/CollisionDispatch/btInternalEdgeUtility.h"

using namespace std;

FORCE_LINK_DEF(StaticMesh);
DEFINE_ASSET(StaticMesh);

bool StaticMesh::HandlePropChange(Datum* datum, uint32_t index, const void* newValue)
{
    Property* prop = static_cast<Property*>(datum);
    OCT_ASSERT(prop != nullptr);
    StaticMesh* mesh = static_cast<StaticMesh*>(prop->mOwner);

    bool handled = false;

    if (prop->mName == "Generate Triangle Collision Mesh")
    {
        mesh->SetGenerateTriangleCollisionMesh(*((bool*)newValue));
        handled = true;
    }

    HandleAssetPropChange(datum, index, newValue);

    return handled;
}

StaticMesh::StaticMesh() :
    mMaterial(nullptr),
    mNumVertices(0),
    mNumIndices(0),
    mNumUvMaps(2),
    mVertices(nullptr),
    mIndices(nullptr),
    mCollisionShape(nullptr),
    mTriangleCollisionShape(nullptr),
    mTriangleIndexVertexArray(nullptr),
    mTriangleInfoMap(nullptr),
    mGenerateTriangleCollisionMesh(true),
    mHasVertexColor(false)
{
    mType = StaticMesh::GetStaticType();
}

StaticMesh::~StaticMesh()
{

}

void StaticMesh::CreateRaw(
    uint32_t numVertices,
    Vertex* vertices,
    uint32_t numIndices,
    IndexType* indices)
{
    mNumVertices = numVertices;
    mNumIndices = numIndices;
    mHasVertexColor = false;
    ResizeVertexArray(numVertices);
    ResizeIndexArray(numIndices);

    memcpy(mVertices, vertices, numVertices * GetVertexSize());
    memcpy(mIndices, indices, numIndices * sizeof(IndexType));

    Create();
}

StaticMeshResource* StaticMesh::GetResource()
{
    return &mResource;
}

Material* StaticMesh::GetMaterial()
{
    return mMaterial.Get<Material>();
}

void StaticMesh::SetMaterial(class Material* newMaterial)
{
    mMaterial = newMaterial;
}

void StaticMesh::LoadStream(Stream& stream, Platform platform)
{
    Asset::LoadStream(stream, platform);

    mNumVertices = stream.ReadUint32();
    mNumIndices = stream.ReadUint32();

    // IndexType is 16 bits on the console backends, because GX addresses its vertex arrays with
    // GX_INDEX16 and cannot reach past 65536 entries. Indices are stored 32 bits wide on disc and
    // narrowed below, so a mesh over that many vertices has every high index silently wrap to the
    // start of the array -- the bulk of the mesh still draws, and the few wrapped triangles stretch
    // across the scene as huge stray geometry with nothing logged to explain it.
    //
    // The existing assert for this lives in Create() and is measured against MAX_MESH_VERTEX_COUNT,
    // which is 4294967295 under Vulkan. The editor therefore never trips it, and the problem only
    // ever appears on hardware. Say so here, where the narrowing actually happens.
    if (mNumVertices > MAX_MESH_VERTEX_COUNT)
    {
        LogError("Mesh %s has %u vertices, over this platform's limit of %u. Indices above the "
                 "limit will wrap and render as stray geometry. Split the mesh.",
                 GetName().c_str(), mNumVertices, (uint32_t)MAX_MESH_VERTEX_COUNT);
    }
    mNumUvMaps = stream.ReadUint32();

    stream.ReadAsset(mMaterial);

    mGenerateTriangleCollisionMesh = stream.ReadBool();
    mHasVertexColor = stream.ReadBool();

    mCompactVertices = false;
#if API_GX && !EDITOR
    // READ STRAIGHT INTO THE COMPACT FORM when the mesh will be drawn compact (GxUtils.cpp,
    // BindStaticMesh): position and colour, 16 bytes a vertex instead of 44. Made compact after
    // loading, a pipe piece still needed its full 430 KB array for a moment, in one block, and on
    // a GameCube some stages in there was no such block. The material has to be known now to
    // decide; it is when it is already loaded (the stage's is: the rings use it). If it is not,
    // the mesh loads the ordinary way and may still be made compact when it is created.
    if (mHasVertexColor && !mGenerateTriangleCollisionMesh && GFX_GetCompactUnlitMeshes() &&
        GFX_MaterialAllowsCompact(GetMaterial()))
    {
        struct CompactVertex { float mX, mY, mZ; uint32_t mColor; };
        mVertices = malloc(sizeof(CompactVertex) * (mNumVertices > 0 ? mNumVertices : 1));
        if (mVertices == nullptr)
        {
            LogError("Mesh %s: out of memory for %u vertices", GetName().c_str(), mNumVertices);
            throw std::bad_alloc();
        }
        mCompactVertices = true;

        uint32_t colorScale = GetEngineConfig()->mColorScale;
        uint32_t shiftCount = (colorScale != 1) ? (colorScale >> 1) : 0;
        CompactVertex* vertices = (CompactVertex*)mVertices;
        for (uint32_t i = 0; i < mNumVertices; ++i)
        {
            glm::vec3 position = stream.ReadVec3();
            stream.ReadVec2();                      // texture coordinates: not drawn
            stream.ReadVec2();
            stream.ReadVec3();                      // normal: not drawn
            uint32_t color = stream.ReadUint32();
            if (shiftCount != 0)
            {
                uint8_t* color8 = (uint8_t*)(&color);
                color8[0] = color8[0] >> shiftCount;
                color8[1] = color8[1] >> shiftCount;
                color8[2] = color8[2] >> shiftCount;
                color8[3] = color8[3] >> shiftCount;
            }
            vertices[i] = { position.x, position.y, position.z, color };
        }
    }
    else
#endif
    {
        ResizeVertexArray(mNumVertices);
    }

#if EDITOR
    mPureVertexColors.clear();
#endif

    if (mCompactVertices)
    {
        // read above
    }
    else if (mHasVertexColor)
    {
        VertexColor* vertices = GetColorVertices();
        for (uint32_t i = 0; i < mNumVertices; ++i)
        {
            vertices[i].mPosition = stream.ReadVec3();
            vertices[i].mTexcoord0 = stream.ReadVec2();
            vertices[i].mTexcoord1 = stream.ReadVec2();
            vertices[i].mNormal = stream.ReadVec3();
            vertices[i].mColor = stream.ReadUint32();
#if EDITOR
            // Cache these to save later. Should not be affected by color scale config.
            mPureVertexColors.push_back(vertices[i].mColor);
#endif
        }

        // Only allow vertex colors to go beyond 1.0 when painted.
        // For meshes with vertex colors, convert to reduced color space.
        uint32_t colorScale = GetEngineConfig()->mColorScale;
        if (colorScale != 1)
        {
            uint32_t shiftCount = (colorScale >> 1);
            for (uint32_t i = 0; i < mNumVertices; ++i)
            {
                uint8_t* color8 = (uint8_t*)(&vertices[i].mColor);
                color8[0] = color8[0] >> shiftCount;
                color8[1] = color8[1] >> shiftCount;
                color8[2] = color8[2] >> shiftCount;
                color8[3] = color8[3] >> shiftCount;
            }
        }
    }
    else
    {
        Vertex* vertices = GetVertices();
        for (uint32_t i = 0; i < mNumVertices; ++i)
        {
            vertices[i].mPosition = stream.ReadVec3();
            vertices[i].mTexcoord0 = stream.ReadVec2();
            vertices[i].mTexcoord1 = stream.ReadVec2();
            vertices[i].mNormal = stream.ReadVec3();
        }
    }

    // Indices are stored 32 bits wide and narrowed to IndexType here. On a platform where that is
    // 16 bits, an index past the limit wraps to the start of the vertex array rather than failing,
    // so the triangle that used it is drawn between three unrelated vertices and stretches across
    // the scene. Drop those triangles instead of drawing known-bad geometry: the mesh loses the
    // faces it could never have addressed, which reads as a small hole, rather than gaining a
    // stray polygon over everything else.
    //
    // Nothing is dropped on a 32-bit-index platform, where the limit is UINT32_MAX.
    ResizeIndexArray(mNumIndices);

    uint32_t dstIndex = 0;
    uint32_t droppedTriangles = 0;
    uint32_t i = 0;

    for (; i + 2 < mNumIndices; i += 3)
    {
        uint32_t i0 = stream.ReadUint32();
        uint32_t i1 = stream.ReadUint32();
        uint32_t i2 = stream.ReadUint32();

        if (i0 > MAX_MESH_VERTEX_COUNT ||
            i1 > MAX_MESH_VERTEX_COUNT ||
            i2 > MAX_MESH_VERTEX_COUNT)
        {
            droppedTriangles++;
            continue;
        }

        mIndices[dstIndex++] = (IndexType) i0;
        mIndices[dstIndex++] = (IndexType) i1;
        mIndices[dstIndex++] = (IndexType) i2;
    }

    // A trailing partial triangle should not exist, but the stream still has to be drained.
    for (; i < mNumIndices; ++i)
    {
        uint32_t index = stream.ReadUint32();

        if (index <= MAX_MESH_VERTEX_COUNT)
        {
            mIndices[dstIndex++] = (IndexType) index;
        }
    }

    if (droppedTriangles > 0)
    {
        LogError("Mesh %s: dropped %u triangle(s) indexing past vertex %u.",
                 GetName().c_str(), droppedTriangles, (uint32_t)MAX_MESH_VERTEX_COUNT);
    }

    mNumIndices = dstIndex;

    // Collision shapes
    bool compound = stream.ReadBool();
    uint32_t numCollisionShapes = stream.ReadUint32();
    std::vector<btCollisionShape*> collisionShapes;
    std::vector<btTransform> collisionTransforms;

    collisionShapes.resize(numCollisionShapes);
    collisionTransforms.resize(numCollisionShapes);

    for (uint32_t i = 0; i < numCollisionShapes; ++i)
    {
        CollisionShape shapeType = CollisionShape(stream.ReadUint32());

        switch (shapeType)
        {
        case CollisionShape::Box:
        {
            glm::vec3 halfExtents(0, 0, 0);
            halfExtents.x = stream.ReadFloat();
            halfExtents.y = stream.ReadFloat();
            halfExtents.z = stream.ReadFloat();
            collisionShapes[i] = new btBoxShape(btVector3(halfExtents.x, halfExtents.y, halfExtents.z));
            break;
        }
        case CollisionShape::Sphere:
        {
            float radius = stream.ReadFloat();
            collisionShapes[i] = new btSphereShape(radius);
            break;
        }
        case CollisionShape::ConvexHull:
        {
            uint32_t numPoints = stream.ReadUint32();
            std::vector<glm::vec3> points;
            points.reserve(numPoints);

            for (uint32_t i = 0; i < numPoints; ++i)
            {
                points.push_back(stream.ReadVec3());
            }

            collisionShapes[i] = new btConvexHullShape(
                reinterpret_cast<float*>(points.data()),
                numPoints,
                sizeof(glm::vec3));
            break;
        }
        case CollisionShape::Empty:
        {
            collisionShapes[i] = new btEmptyShape();
            break;
        }
        default:
        {
            LogWarning("Unrecognized shape type in StaticMesh::LoadStream()");
            break;
        }
        }

        if (compound)
        {
            // Read rotation and position to reconstruct transform.
            btVector3 origin = GlmToBullet(stream.ReadVec3());
            btQuaternion rotation = GlmToBullet(stream.ReadQuat());
            collisionTransforms[i].setIdentity();
            collisionTransforms[i].setRotation(rotation);
            collisionTransforms[i].setOrigin(origin);
        }
    }

    SetCollisionShapes(numCollisionShapes, collisionShapes.data(), collisionTransforms.data(), compound);

    mBounds.mCenter = stream.ReadVec3();
    mBounds.mRadius = stream.ReadFloat();
}

void StaticMesh::SaveStream(Stream& stream, Platform platform)
{
    Asset::SaveStream(stream, platform);
    
#if EDITOR
    stream.WriteUint32(mNumVertices);
    stream.WriteUint32(mNumIndices);
    stream.WriteUint32(mNumUvMaps);

    stream.WriteAsset(mMaterial);
    stream.WriteBool(mGenerateTriangleCollisionMesh);
    stream.WriteBool(mHasVertexColor);

    if (mHasVertexColor)
    {
        VertexColor* vertices = GetColorVertices();
        for (uint32_t i = 0; i < mNumVertices; ++i)
        {
            stream.WriteVec3(vertices[i].mPosition);
            stream.WriteVec2(vertices[i].mTexcoord0);
            stream.WriteVec2(vertices[i].mTexcoord1);
            stream.WriteVec3(vertices[i].mNormal);

            // Save pure vertex color, unaffected by color scale config setting.
            stream.WriteUint32(mPureVertexColors[i]);
        }
    }
    else
    {
        Vertex* vertices = GetVertices();
        for (uint32_t i = 0; i < mNumVertices; ++i)
        {
            stream.WriteVec3(vertices[i].mPosition);
            stream.WriteVec2(vertices[i].mTexcoord0);
            stream.WriteVec2(vertices[i].mTexcoord1);
            stream.WriteVec3(vertices[i].mNormal);
        }
    }

    for (uint32_t i = 0; i < mNumIndices; ++i)
    {
        stream.WriteUint32(mIndices[i]);
    }

    // Collision shapes
    uint32_t numCollisionShapes = 0;
    std::vector<btCollisionShape*> collisionShapes;
    btCompoundShape* compoundShape = nullptr;

    if (mCollisionShape != nullptr)
    {
        if (mCollisionShape->getShapeType() == COMPOUND_SHAPE_PROXYTYPE)
        {
            compoundShape = static_cast<btCompoundShape*>(mCollisionShape);
            numCollisionShapes = (uint32_t) compoundShape->getNumChildShapes();
            OCT_ASSERT(numCollisionShapes >= 1);

            for (uint32_t i = 0; i < numCollisionShapes; ++i)
            {
                collisionShapes.push_back(compoundShape->getChildShape(i));
            }
        }
        else
        {
            collisionShapes.push_back(mCollisionShape);
            numCollisionShapes = 1;
        }
    }

    OCT_ASSERT(collisionShapes.size() == numCollisionShapes);

    stream.WriteBool(compoundShape != nullptr);
    stream.WriteUint32(numCollisionShapes);

    for (uint32_t i = 0; i < numCollisionShapes; ++i)
    {
        int32_t shapeType = collisionShapes[i]->getShapeType();

        switch (shapeType)
        {
        case BOX_SHAPE_PROXYTYPE:
        {
            btBoxShape* boxShape = static_cast<btBoxShape*>(collisionShapes[i]);
            btVector3 halfExtents = boxShape->getHalfExtentsWithMargin();
            stream.WriteUint32(uint32_t(CollisionShape::Box));
            stream.WriteFloat(halfExtents.x());
            stream.WriteFloat(halfExtents.y());
            stream.WriteFloat(halfExtents.z());
            break;
        }
        case SPHERE_SHAPE_PROXYTYPE:
        {
            btSphereShape* sphereShape = static_cast<btSphereShape*>(collisionShapes[i]);
            stream.WriteUint32(uint32_t(CollisionShape::Sphere));
            stream.WriteFloat(sphereShape->getRadius());
            break;
        }
        case CONVEX_HULL_SHAPE_PROXYTYPE:
        {
            btConvexHullShape* convexShape = static_cast<btConvexHullShape*>(collisionShapes[i]);
            stream.WriteUint32(uint32_t(CollisionShape::ConvexHull));

            uint32_t numPoints = convexShape->getNumPoints();
            stream.WriteUint32(numPoints);

            const btVector3* points = convexShape->getPoints();
            for (uint32_t p = 0; p < numPoints; ++p)
            {
                glm::vec3 glmPoint = { points[p].x(), points[p].y(), points[p].z() };
                stream.WriteVec3(glmPoint);
            }
            break;
        }
        case EMPTY_SHAPE_PROXYTYPE:
        {
            stream.WriteUint32(uint32_t(CollisionShape::Empty));
            break;
        }
        default:
        {
            stream.WriteUint32(uint32_t(CollisionShape::Num));
            LogWarning("Unrecognized shape type in StaticMesh::SaveStream()");
        }
        }

        if (compoundShape != nullptr)
        {
            const btTransform& transform = compoundShape->getChildTransform(i);
            glm::vec3 origin = BulletToGlm(transform.getOrigin());
            glm::quat rotation = BulletToGlm(transform.getRotation());
            stream.WriteVec3(origin);
            stream.WriteQuat(rotation);

        }
    }

    stream.WriteVec3(mBounds.mCenter);
    stream.WriteFloat(mBounds.mRadius);
#endif
}

void StaticMesh::Create()
{
    Asset::Create();

    OCT_ASSERT(mNumVertices <= MAX_MESH_VERTEX_COUNT); // Vertex index must fit into IndexType width.

    // Before the GPU resource: on GX a compact mesh lets go of its vertex array just after.
    ComputeBounds();

    GFX_CreateStaticMeshResource(
        this,
        mHasVertexColor,
        mNumVertices,
        mHasVertexColor ? (void*)GetColorVertices() : (void*)GetVertices(),
        mNumIndices,
        mIndices);

    if (ShouldGenerateTriangleCollision())
    {
        CreateTriangleCollisionShape();
    }

#if API_GX
    if (GetResource()->mCompact)
    {
        ReleaseSourceArrays();
    }
#endif
}

glm::vec3 StaticMesh::GetVertexPosition(uint32_t index)
{
    if (mCompactVertices)
    {
        const float* f = (const float*)((const uint8_t*)mVertices + index * 16);
        return glm::vec3(f[0], f[1], f[2]);
    }

    return mHasVertexColor ? GetColorVertices()[index].mPosition : GetVertices()[index].mPosition;
}

// WHY. A marathon changes colours every zone, and on the GameCube the pipe's palettes are the same
// meshes painted differently. Loading another palette's meshes to swap them in held two sets at
// once (2.8 MB more) and cut the heap up; here the new colours are read off the disc, 4 bytes a
// vertex, and written into the compact vertex array the display lists already point at.
int32_t StaticMesh::StageColorsFrom(const std::string& assetName, uint32_t at, uint32_t maxVertices, uint32_t& outTotal)
{
    outTotal = 0;
#if API_GX && !EDITOR
    AssetStub* stub = AssetManager::Get()->GetAssetStub(assetName);
    if (!IsLoaded() || !GetResource()->mCompact || GetResource()->mCompactVertices == nullptr ||
        stub == nullptr || stub->mPath.empty())
    {
        return -1;
    }

    if (at == 0 || mStagedFrom != assetName)
    {
        // The header, up to the first vertex: the name, the counts, the material, two flags.
        char head[512];
        if (!SYS_ReadFileRange(stub->mPath.c_str(), true, 0, sizeof(head), head))
        {
            return -1;
        }
        Stream stream(head, sizeof(head));
        AssetHeader header = Asset::ReadHeader(stream);
        if (header.mType != GetType())
        {
            return -1;
        }
        stream.SetAssetVersion(header.mVersion);
        std::string name;
        stream.ReadString(name);
        uint32_t numVertices = stream.ReadUint32();
        stream.ReadUint32();                                // indices
        stream.ReadUint32();                                // uv maps
        MaterialRef material;
        stream.ReadAsset(material);                         // (already loaded: this mesh uses it)
        stream.ReadBool();                                  // triangle collision
        bool hasColor = stream.ReadBool();
        if (numVertices != mNumVertices || !hasColor || stream.GetPos() >= sizeof(head))
        {
            LogWarning("Mesh %s: cannot take %s's colours (a different mesh)", GetName().c_str(), assetName.c_str());
            mStagedFrom.clear();
            return -1;
        }
        mStagedFrom = assetName;
        mStagedDataAt = stream.GetPos();
        mStagedColors.clear();
        mStagedColors.resize(mNumVertices);
    }

    outTotal = mNumVertices;
    if (at >= mNumVertices)
    {
        return (int32_t)mNumVertices;
    }

    // A vertex on the disc: position, two texture coordinates, normal, colour -- 44 bytes.
    const uint32_t kVertexBytes = 44;
    const uint32_t kMost = (32 * 1024) / kVertexBytes;
    static char sPiece[kMost * kVertexBytes];
    uint32_t colorScale = GetEngineConfig()->mColorScale;
    uint32_t shiftCount = (colorScale != 1) ? (colorScale >> 1) : 0;
    uint32_t stop = (maxVertices >= mNumVertices - at) ? mNumVertices : at + maxVertices;
    for (uint32_t from = at; from < stop; from += kMost)
    {
        uint32_t n = (stop - from < kMost) ? (stop - from) : kMost;
        if (!SYS_ReadFileRange(stub->mPath.c_str(), true, mStagedDataAt + from * kVertexBytes, n * kVertexBytes, sPiece))
        {
            LogError("Mesh %s: reading %s's colours failed", GetName().c_str(), assetName.c_str());
            mStagedFrom.clear();
            return -1;
        }
        Stream stream(sPiece, n * kVertexBytes);
        for (uint32_t i = 0; i < n; ++i)
        {
            stream.ReadVec3();
            stream.ReadVec2();
            stream.ReadVec2();
            stream.ReadVec3();
            uint32_t color = stream.ReadUint32();
            if (shiftCount != 0)
            {
                uint8_t* color8 = (uint8_t*)(&color);
                color8[0] = color8[0] >> shiftCount;
                color8[1] = color8[1] >> shiftCount;
                color8[2] = color8[2] >> shiftCount;
                color8[3] = color8[3] >> shiftCount;
            }
            mStagedColors[from + i] = color;
        }
    }
    return (int32_t)stop;
#else
    (void)assetName; (void)at; (void)maxVertices;
    return -1;
#endif
}

bool StaticMesh::ApplyStagedColors()
{
#if API_GX && !EDITOR
    StaticMeshResource* resource = GetResource();
    if (mStagedColors.size() != mNumVertices || !resource->mCompact || resource->mCompactVertices == nullptr)
    {
        return false;
    }
    // position (12 bytes), colour (4): the colour is the last word of each 16, in GX's byte order
    // (reversed, as GFX_CreateStaticMeshResource does for a mesh as it is loaded)
    uint8_t* compact = (uint8_t*)resource->mCompactVertices;
    for (uint32_t i = 0; i < mNumVertices; ++i)
    {
        uint32_t color = mStagedColors[i];
        ReverseColorUint32(color);
        memcpy(compact + i * 16 + 12, &color, 4);
    }
    DCFlushRange(compact, mNumVertices * 16);
    GX_InvVtxCache();
    std::vector<uint32_t>().swap(mStagedColors);
    mStagedFrom.clear();
    return true;
#else
    return false;
#endif
}

void* StaticMesh::TakeVertexArray()
{
    void* vertices = mVertices;
    mVertices = nullptr;
    return vertices;
}

void StaticMesh::ReleaseSourceArrays()
{
    // mNumVertices and mNumIndices stay: they still say what the GPU resource holds.
    mCompactVertices = false;
    ResizeVertexArray(0);
    ResizeIndexArray(0);
}

#if EDITOR
void StaticMesh::ApplyScale(glm::vec3 scale)
{
    if (scale == glm::vec3(1.0f))
    {
        return;
    }

    if (scale.x == 0.0f || scale.y == 0.0f || scale.z == 0.0f)
    {
        LogError("Mesh %s: cannot apply a zero scale.", GetName().c_str());
        return;
    }

    // Normals do not transform like positions under a non-uniform scale -- they need the inverse,
    // renormalized afterwards. For a uniform scale this reduces to leaving them alone, but doing it
    // unconditionally keeps the one path correct for both.
    const glm::vec3 invScale = 1.0f / scale;

    if (mHasVertexColor)
    {
        VertexColor* verts = GetColorVertices();
        for (uint32_t i = 0; i < mNumVertices; ++i)
        {
            verts[i].mPosition *= scale;
            verts[i].mNormal = glm::normalize(verts[i].mNormal * invScale);
        }
    }
    else
    {
        Vertex* verts = GetVertices();
        for (uint32_t i = 0; i < mNumVertices; ++i)
        {
            verts[i].mPosition *= scale;
            verts[i].mNormal = glm::normalize(verts[i].mNormal * invScale);
        }
    }

    // Authored collision shapes carry their own dimensions, so they have to be scaled alongside the
    // geometry or the mesh would keep colliding at its old size. SaveStream reads these back out of
    // the live Bullet shapes (getHalfExtentsWithMargin, getRadius), and both of those already fold
    // in local scaling, so setting it here is what persists.
    const btVector3 btScale(scale.x, scale.y, scale.z);

    if (mCollisionShape != nullptr)
    {
        if (mCollisionShape->getShapeType() == COMPOUND_SHAPE_PROXYTYPE)
        {
            btCompoundShape* compound = static_cast<btCompoundShape*>(mCollisionShape);

            for (int32_t i = 0; i < compound->getNumChildShapes(); ++i)
            {
                btCollisionShape* child = compound->getChildShape(i);
                child->setLocalScaling(child->getLocalScaling() * btScale);

                // The child's offset from the mesh origin scales too, otherwise the pieces of a
                // compound keep their old spacing around correctly resized shapes.
                btTransform childTransform = compound->getChildTransform(i);
                childTransform.setOrigin(childTransform.getOrigin() * btScale);
                compound->updateChildTransform(i, childTransform, false);
            }

            compound->recalculateLocalAabb();
        }
        else
        {
            mCollisionShape->setLocalScaling(mCollisionShape->getLocalScaling() * btScale);
        }

        // A sphere has one radius and cannot represent a non-uniform scale, so Bullet will pick a
        // single axis and the collision will no longer match what is drawn.
        if (scale.x != scale.y || scale.y != scale.z)
        {
            LogWarning("Mesh %s: non-uniform scale applied to collision; sphere shapes cannot "
                       "represent this and will not match the visual mesh.", GetName().c_str());
        }
    }

    // The triangle collision mesh is built from the vertices, so it just needs rebuilding.
    DestroyTriangleCollisionShape();

    if (ShouldGenerateTriangleCollision())
    {
        CreateTriangleCollisionShape();
    }

    // Re-upload rather than going through Destroy()/Create(): Destroy() frees the vertex arrays and
    // clears the material, which would throw away the geometry this just rewrote.
    GFX_DestroyStaticMeshResource(this);
    GFX_CreateStaticMeshResource(
        this,
        mHasVertexColor,
        mNumVertices,
        mHasVertexColor ? (void*)GetColorVertices() : (void*)GetVertices(),
        mNumIndices,
        mIndices);

    ComputeBounds();
}
#endif

void StaticMesh::Destroy()
{
    Asset::Destroy();

#if CREATE_CONVEX_COLLISION_MESH
    for (StaticMesh* mesh : mCollisionMeshes)
    {
        delete mesh;
    }
    mCollisionMeshes.clear();
#endif

    GFX_DestroyStaticMeshResource(this);

    if (mCollisionShape != nullptr)
    {
        DestroyCollisionShape(mCollisionShape);
        mCollisionShape = nullptr;
    }

    DestroyTriangleCollisionShape();

    ResizeVertexArray(0);
    ResizeIndexArray(0);

    mMaterial = nullptr;
}

bool StaticMesh::Import(const std::string& path, ImportOptions* options)
{
    bool success = Asset::Import(path, options);
    if (!success)
    {
        return false;
    }

#if EDITOR
    // Loads a .DAE file and loads the first mesh in the mesh library.
    if (mResource.mVertexBuffer == VK_NULL_HANDLE)
    {
        Assimp::Importer importer;

        int32_t meshIndex = -1;

        if (options)
        {
            if (options->HasOption("meshIndex"))
            {
                meshIndex = options->GetOptionValue("meshIndex").GetInteger();
            }
        }

        // TODO: If supporting lightmap textures and automatic lightmap UV generation, then do not
        // join identical vertices. Auto-generated lightmap UVs will have all-unique vertices.
        const aiScene* scene = importer.ReadFile(path, aiProcess_FlipUVs | aiProcess_JoinIdenticalVertices | aiProcess_Triangulate);

        if (scene == nullptr)
        {
            LogError("Failed to load dae file");
            success = false;
        }

        if (scene->mNumMeshes < 1)
        {
            LogError("Failed to find any meshes in dae file");
            success = false;
        }

        if (meshIndex != -1 &&
            int32_t(scene->mNumMeshes) <= meshIndex)
        {
            LogError("Out of bounds mesh index??? Aborting mesh import.");
            success = false;
        }

        const bool singleMeshImport = (meshIndex == -1);

        if (success)
        {
            std::vector<const aiMesh*> collisionMeshes;

            // Single mesh import mode (allows for custom simplified collision)
            if (meshIndex == -1)
            {
                for (uint32_t i = 0; i < scene->mNumMeshes; ++i)
                {
                    const aiMesh* mesh = scene->mMeshes[i];
                    const char* meshName = mesh->mName.C_Str();

                    if (strncmp(meshName, "UCX", 3) == 0 ||
                        strncmp(meshName, "UBX", 3) == 0 ||
                        strncmp(meshName, "USP", 3) == 0)
                    {
                        collisionMeshes.push_back(mesh);
                    }
                    else
                    {
                        OCT_ASSERT(meshIndex == -1);
                        if (meshIndex == -1)
                        {
                            meshIndex = i;
                        }
                        else
                        {
                            LogError("More than one non-collision mesh found");
                        }
                    }
                }
            }

            Create(scene, *scene->mMeshes[meshIndex], (uint32_t)collisionMeshes.size(), collisionMeshes.data());

            if (!singleMeshImport)
            {
                // Make the name unique
                std::string newName = GenerateUniqueMeshName(GetName(), scene, meshIndex);
                SetName(newName);
            }
        }
    }
#endif

    return success;
}

void StaticMesh::GatherProperties(std::vector<Property>& outProps)
{
    Asset::GatherProperties(outProps);
    outProps.push_back(Property(DatumType::Asset, "Material", this, &mMaterial, 1, HandleAssetPropChange, int32_t(Material::GetStaticType())));
    outProps.push_back(Property(DatumType::Bool, "Generate Triangle Collision Mesh", this, &mGenerateTriangleCollisionMesh, 1, HandlePropChange));
}

glm::vec4 StaticMesh::GetTypeColor()
{
    return glm::vec4(0.3f, 1.0f, 0.8f, 1.0f);
}

const char* StaticMesh::GetTypeName()
{
    return "StaticMesh";
}

const char* StaticMesh::GetTypeImportExt()
{
    return ".dae";
}

uint32_t StaticMesh::GetNumIndices() const
{
    return mNumIndices;
}

uint32_t StaticMesh::GetNumFaces() const
{
    return mNumIndices / 3;
}

uint32_t StaticMesh::GetNumVertices() const
{
    return mNumVertices;
}

bool StaticMesh::HasVertexColor() const
{
    return mHasVertexColor;
}

Vertex* StaticMesh::GetVertices()
{
    OCT_ASSERT(!mHasVertexColor);
    return reinterpret_cast<Vertex*>(mVertices);
}

VertexColor* StaticMesh::GetColorVertices()
{
    OCT_ASSERT(mHasVertexColor);
    return reinterpret_cast<VertexColor*>(mVertices);
}

IndexType* StaticMesh::GetIndices()
{
    return mIndices;
}

Bounds StaticMesh::GetBounds() const
{
    return mBounds;
}

btBvhTriangleMeshShape* StaticMesh::GetTriangleCollisionShape()
{
    return ShouldGenerateTriangleCollision() ? mTriangleCollisionShape : nullptr;
}

btCollisionShape* StaticMesh::GetCollisionShape()
{
    return mCollisionShape;
}

void StaticMesh::SetCollisionShape(btCollisionShape* shape)
{
    if (mCollisionShape != nullptr)
    {
        delete mCollisionShape;
        mCollisionShape = nullptr;
    }

    mCollisionShape = shape;
}

void StaticMesh::SetCollisionShapes(uint32_t numCollisionShapes, btCollisionShape** collisionShapes, btTransform* transforms, bool compound)
{
    if (numCollisionShapes > 0)
    {
        if (numCollisionShapes == 1 && !compound)
        {
            SetCollisionShape(collisionShapes[0]);
        }
        else
        {
            btCompoundShape* compoundShape = new btCompoundShape();

            for (uint32_t i = 0; i < numCollisionShapes; ++i)
            {
                compoundShape->addChildShape(transforms[i], collisionShapes[i]);
            }

            // Do we want to initialize the principle axis transform? Not really sure what it is.
            // I think it's related to physics simulation.
#if 0
            btScalar masses[3] = { 1, 1, 1 };
            btTransform principal;
            btVector3 inertia;
            compoundShape->calculatePrincipalAxisTransform(masses, principal, inertia);
#endif

            SetCollisionShape(compoundShape);
        }

#if CREATE_CONVEX_COLLISION_MESH
        OCT_ASSERT(mCollisionMeshes.size() == 0);
        for (uint32_t i = 0; i < numCollisionShapes; ++i)
        {
            CreateCollisionMesh(collisionShapes[i]);
        }
#endif
    }
}

void StaticMesh::SetGenerateTriangleCollisionMesh(bool generate)
{
    if (mGenerateTriangleCollisionMesh != generate)
    {
        mGenerateTriangleCollisionMesh = generate;

        if (ShouldGenerateTriangleCollision())
        {
            CreateTriangleCollisionShape();
        }
        else
        {
            DestroyTriangleCollisionShape();
        }
    }
}

bool StaticMesh::IsTriangleCollisionMeshEnabled() const
{
    return mGenerateTriangleCollisionMesh;
}

uint32_t StaticMesh::GetVertexSize() const
{
    return mHasVertexColor ? sizeof(VertexColor) : sizeof(Vertex);
}

bool StaticMesh::ShouldGenerateTriangleCollision() const
{
#if EDITOR
    // Always generate it in Editor. For vertex color and instance painting, we want to use the 
    // triangle collision data for placing the paint sphere reticle.
    return true;
#else
    return mGenerateTriangleCollisionMesh;
#endif
}

void StaticMesh::CreateTriangleCollisionShape()
{
    OCT_ASSERT(mNumIndices % 3 == 0);

    // Don't do anything if we already have triangle collision data generated
    // Note: In EDITOR, we always generate triangle collision data even if it's disabled.
    if (mTriangleIndexVertexArray == nullptr &&
        mTriangleCollisionShape == nullptr)
    {
        mTriangleIndexVertexArray = new btTriangleIndexVertexArray();

        btIndexedMesh mesh;
        mesh.m_numTriangles = mNumIndices / 3;
        mesh.m_triangleIndexBase = (const unsigned char*)mIndices;
        mesh.m_triangleIndexStride = sizeof(IndexType) * 3;
        mesh.m_numVertices = mNumVertices;
        mesh.m_vertexBase = (const unsigned char*)mVertices;
        mesh.m_vertexStride = GetVertexSize();

        mTriangleIndexVertexArray->addIndexedMesh(mesh, sizeof(IndexType) == 2 ? PHY_SHORT : PHY_INTEGER);

        bool useQuantizedAabbCompression = true;

        mTriangleCollisionShape = new btBvhTriangleMeshShape(mTriangleIndexVertexArray, useQuantizedAabbCompression);
        mTriangleInfoMap = new btTriangleInfoMap();
        btGenerateInternalEdgeInfo(mTriangleCollisionShape, mTriangleInfoMap);
    }
}

void StaticMesh::DestroyTriangleCollisionShape()
{
    if (mTriangleInfoMap != nullptr)
    {
        delete mTriangleInfoMap;
        mTriangleInfoMap = nullptr;
    }

    if (mTriangleCollisionShape != nullptr)
    {
        delete mTriangleCollisionShape;
        mTriangleCollisionShape = nullptr;
    }

    if (mTriangleIndexVertexArray != nullptr)
    {
        delete mTriangleIndexVertexArray;
        mTriangleIndexVertexArray = nullptr;
    }
}

void StaticMesh::ResizeVertexArray(uint32_t newSize)
{
    if (mVertices != nullptr)
    {
        free(mVertices);
        mVertices = nullptr;
    }

    if (newSize > 0)
    {
        if (mHasVertexColor)
        {
            mVertices = malloc(sizeof(VertexColor) * newSize);
        }
        else
        {
            mVertices = malloc(sizeof(Vertex) * newSize);
        }

        // malloc says "out of memory" by returning null, where new throws. A null array was then
        // written through (a DSI on the GameCube, loading a big mesh with memory nearly gone).
        // Throw as new would: on the async loader that fails the load and leaves the asset unloaded.
        if (mVertices == nullptr)
        {
            LogError("Mesh %s: out of memory for %u vertices", GetName().c_str(), newSize);
            throw std::bad_alloc();
        }
    }
}

void StaticMesh::ResizeIndexArray(uint32_t newSize)
{
    if (mIndices != nullptr)
    {
        free(mIndices);
        mIndices = nullptr;
    }

    if (newSize > 0)
    {
        mIndices = (IndexType*)malloc(sizeof(IndexType) * newSize);

        if (mIndices == nullptr)
        {
            LogError("Mesh %s: out of memory for %u indices", GetName().c_str(), newSize);
            throw std::bad_alloc();       // see ResizeVertexArray
        }
    }
}

void StaticMesh::ComputeBounds()
{
    if (mNumVertices == 0)
    {
        mBounds.mCenter = { 0.0f, 0.0f, 0.0f };
        mBounds.mRadius = 0.0f;
        return;
    }

    bool hasColor = HasVertexColor();

    glm::vec3 boxMin = { FLT_MAX, FLT_MAX, FLT_MAX };
    glm::vec3 boxMax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

    for (uint32_t i = 0; i < mNumVertices; ++i)
    {
        glm::vec3 pos = 
            GetVertexPosition(i);

        boxMin = glm::min(boxMin, pos);
        boxMax = glm::max(boxMax, pos);
    }

    mBounds.mCenter = (boxMin + boxMax) / 2.0f;

    float maxDist = 0.0f;

    for (uint32_t i = 0; i < mNumVertices; ++i)
    {
        glm::vec3 pos =
            GetVertexPosition(i);

        float dist = glm::distance(pos, mBounds.mCenter);
        maxDist = glm::max(maxDist, dist);
    }

    mBounds.mRadius = maxDist;
}

#if EDITOR

void StaticMesh::Create(
    const aiScene* scene,
    const aiMesh& meshData,
    uint32_t numCollisionMeshes,
    const aiMesh** collisionMeshes)
{
    // First, handle the main mesh that we want to render.
    if (meshData.mNumVertices == 0 ||
        meshData.mNumFaces == 0)
    {
        return;
    }

    mNumVertices = meshData.mNumVertices;
    mNumIndices = meshData.mNumFaces * 3;
    
    // Always set the num uv maps to 2 since we are storing two sets of UVs no matter what.
    mNumUvMaps = 2;
    //mNumUvMaps = glm::clamp(meshData.GetNumUVChannels(), 0u, MAX_UV_MAPS - 1u);

    mHasVertexColor = meshData.GetNumColorChannels() > 0;

    // Get pointers to vertex attributes
    glm::vec3* positions = reinterpret_cast<glm::vec3*>(meshData.mVertices);
    glm::vec3* texcoords0 = meshData.HasTextureCoords(0) ? reinterpret_cast<glm::vec3*>(meshData.mTextureCoords[0]) : nullptr;
    glm::vec3* texcoords1 = meshData.HasTextureCoords(1) ? reinterpret_cast<glm::vec3*>(meshData.mTextureCoords[1]) : texcoords0;
    glm::vec3* normals = reinterpret_cast<glm::vec3*>(meshData.mNormals);
    glm::vec4* colors = mHasVertexColor ? reinterpret_cast<glm::vec4*>(meshData.mColors[0]) : nullptr;

    aiFace* faces = meshData.mFaces;

    ResizeVertexArray(mNumVertices);

    // Create an interleaved VBO

    mPureVertexColors.clear();

    if (mHasVertexColor)
    {
        VertexColor* vertices = GetColorVertices();
        for (uint32_t i = 0; i < mNumVertices; ++i)
        {
            vertices[i].mPosition = glm::vec3(positions[i].x, positions[i].y, positions[i].z);
            vertices[i].mTexcoord0 = texcoords0 ? glm::vec2(texcoords0[i].x, texcoords0[i].y) : glm::vec2(0.0f, 0.0f);
            vertices[i].mTexcoord1 = texcoords1 ? glm::vec2(texcoords1[i].x, texcoords1[i].y) : glm::vec2(0.0f, 0.0f);
            vertices[i].mNormal = glm::vec3(normals[i].x, normals[i].y, normals[i].z);

            glm::vec4 color4f = glm::vec4(colors[i].r, colors[i].g, colors[i].b, colors[i].a);
            vertices[i].mColor = ColorFloat4ToUint32(color4f);

            mPureVertexColors.push_back(vertices[i].mColor);
        }
    }
    else
    {
        Vertex* vertices = GetVertices();
        for (uint32_t i = 0; i < mNumVertices; ++i)
        {
            vertices[i].mPosition = glm::vec3(positions[i].x, positions[i].y, positions[i].z);
            vertices[i].mTexcoord0 = texcoords0 ? glm::vec2(texcoords0[i].x, texcoords0[i].y) : glm::vec2(0.0f, 0.0f);
            vertices[i].mTexcoord1 = texcoords1 ? glm::vec2(texcoords1[i].x, texcoords1[i].y) : glm::vec2(0.0f, 0.0f);
            vertices[i].mNormal = glm::vec3(normals[i].x, normals[i].y, normals[i].z);
        }
    }


    ResizeIndexArray(mNumIndices);

    for (uint32_t i = 0; i < meshData.mNumFaces; ++i)
    {
        // Enforce triangulated faces
        OCT_ASSERT(faces[i].mNumIndices == 3);
        mIndices[i * 3 + 0] = (IndexType) faces[i].mIndices[0];
        mIndices[i * 3 + 1] = (IndexType) faces[i].mIndices[1];
        mIndices[i * 3 + 2] = (IndexType) faces[i].mIndices[2];
    }

    // Next, create collision objects for the collision meshes.
    uint32_t numCollisionShapes = 0;
    std::vector<btCollisionShape*> collisionShapes;
    std::vector<btTransform> collisionTransforms;

    for (uint32_t i = 0; i < numCollisionMeshes; ++i)
    {
        const aiMesh* colMesh = collisionMeshes[i];

        const aiNode* node = FindMeshNode(scene, scene->mRootNode, colMesh);

        btVector3 bScale = { 1.0f, 1.0f, 1.0f };
        btQuaternion bRotation = btQuaternion(0.0f, 0.0f, 0.0f, 1.0f);
        btVector3 bPosition = { 0.0f, 0.0f, 0.0f };
        btTransform bTransform = btTransform::getIdentity();

        if (node != nullptr)
        {
            aiVector3D scale;
            aiQuaternion rotation;
            aiVector3D position;
            node->mTransformation.Decompose(scale, rotation, position);

            bScale = { scale.x, scale.y, scale.z };
            bRotation = btQuaternion(rotation.x, rotation.y, rotation.z, rotation.w);
            bPosition = { position.x, position.y, position.z };

            bTransform = btTransform(bRotation, bPosition);
        }
        else
        {
            LogWarning("Could not find collision mesh node. Please ensure mesh and node have exact same name.");
        }

        if (strncmp(colMesh->mName.C_Str(), "UBX", 3) == 0)
        {
            // Box collision shape
            collisionShapes.push_back(new btBoxShape(bScale));
            collisionTransforms.push_back(bTransform);
            ++numCollisionShapes;
        }
        else if (strncmp(colMesh->mName.C_Str(), "USP", 3) == 0)
        {
            // Sphere collision shape
            collisionShapes.push_back(new btSphereShape(bScale.x()));
            collisionTransforms.push_back(bTransform);
            ++numCollisionShapes;

        }
        else if (strncmp(colMesh->mName.C_Str(), "UCX", 3) == 0)
        {
            // Convex collision shape
            collisionShapes.push_back(new btConvexHullShape(
                reinterpret_cast<float*>(colMesh->mVertices),
                colMesh->mNumVertices,
                sizeof(aiVector3D)));
            collisionTransforms.push_back(btTransform(bRotation, bPosition));
            ++numCollisionShapes;
        }
        else
        {
            LogWarning("Unknown collision mesh type found");
        }
    }

    OCT_ASSERT(collisionShapes.size() == numCollisionShapes);
    OCT_ASSERT(collisionTransforms.size() == numCollisionShapes);

    // Any imported mesh will have a compound collision shape.
    // Engine assets like SM_Cube and SM_Sphere will still use non-compound shapes, as those are 
    // explicitly set up during editor initialization.
    // Note: Even when a mesh has only one collision shape, we still need to use a compound shape parent
    // so that it can receive there correct transform. Otherwise it will always be positioned at the origin.
    SetCollisionShapes(numCollisionShapes, collisionShapes.data(), collisionTransforms.data(), true);

    mMaterial = Renderer::Get()->GetDefaultMaterial();

    Create();
}

#endif // EDITOR


#if CREATE_CONVEX_COLLISION_MESH

void StaticMesh::CreateCollisionMesh(btCollisionShape* collisionShape)
{
    if (collisionShape->getShapeType() == CONVEX_HULL_SHAPE_PROXYTYPE)
    {
        btConvexHullShape* convexShape = static_cast<btConvexHullShape*>(collisionShape);

        uint32_t numPoints = convexShape->getNumPoints();
        const btVector3* points = convexShape->getPoints();
        std::vector<Vertex> vertices;
        std::vector<IndexType> indices;

        for (uint32_t f = 0; f < numPoints / 3; ++f)
        {
            for (uint32_t v = 0; v < 3; ++v)
            {
                Vertex vert = {};
                btVector3 point = points[f * 3 + v];
                vert.mPosition = { point.x(), point.y(), point.z() };

                vertices.push_back(vert);
                indices.push_back(f * 3 + v);
            }
        }

        StaticMesh* newStaticMesh = new StaticMesh();
        newStaticMesh->CreateRaw(uint32_t(vertices.size()), vertices.data(), uint32_t(indices.size()), indices.data());
        newStaticMesh->SetName("Debug Collision Mesh");
        mCollisionMeshes.push_back(newStaticMesh);
    }
}
#endif
