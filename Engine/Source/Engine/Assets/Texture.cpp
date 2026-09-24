#include "Assets/Texture.h"
#include "System/System.h"
#include "AssetManager.h"
#if API_GX
#include <gccore.h>
#include "Graphics/GX/GxUtils.h"
#endif
#include "Assets/CmprEncoder.h"
#include "Renderer.h"
#include "Log.h"
#include "AssetManager.h"
#include "Engine.h"

#include <malloc.h>

#if EDITOR
#include "EditorUtils.h"
#include <stb_image.h>
#include <stb_image_write.h>
#include <stb_image_resize2.h>
#endif

using namespace std;

#define RGBA8_SIZE 4

#include "Graphics/Graphics.h"
#include "Graphics/GraphicsTypes.h"

static const char* sPixelFormatEnumStrings[] =
{
    "LA4",
    "RGB565",
    "RGBA8",
    "CMPR",
    "RGBA5551"
};
static_assert(
    uint32_t(PixelFormat::LA4) == 0 &&
    uint32_t(PixelFormat::RGB565) == 1 &&
    uint32_t(PixelFormat::RGBA8) == 2 &&
    uint32_t(PixelFormat::CMPR) == 3 &&
    uint32_t(PixelFormat::RGBA5551) == 4,
    "Need to update texture asset format string table");

const char* gFilterEnumStrings[] =
{
    "Nearest",
    "Linear"
};
static_assert(uint32_t(FilterType::Count) == 2, "Need to update filter type enum string table");

const char* gWrapEnumStrings[] =
{
    "Clamp",
    "Repeat",
    "Mirror"
};
static_assert(uint32_t(WrapMode::Count) == 3, "Need to update wrap mode enum string table");

FORCE_LINK_DEF(Texture);
DEFINE_ASSET(Texture);

bool Texture::HandlePropChange(Datum* datum, uint32_t index, const void* newValue)
{
    Property* prop = static_cast<Property*>(datum);
    OCT_ASSERT(prop != nullptr);
    Texture* texture = static_cast<Texture*>(prop->mOwner);
    bool success = false;

    if (prop->mName == "Mipmapped")
    {
        texture->mMipmapped = *(bool*)newValue;
        success = true;
    }
    else if (prop->mName == "Filter Type")
    {
        texture->mFilterType = *(FilterType*)newValue;
        success = true;
    }
    else if (prop->mName == "Wrap Mode")
    {
        texture->mWrapMode = *(WrapMode*)newValue;
        success = true;
    }

    if (success)
    {
#if EDITOR
        // Need to recreate the texture resource to show changes.
        GFX_DestroyTextureResource(texture);
        GFX_CreateTextureResource(texture, texture->mPixels);
#endif
    }

    HandleAssetPropChange(datum, index, newValue);

    return success;
}

bool UseCookedTextures(Platform platform)
{
    bool cook = false;

    if (platform == Platform::GameCube ||
        platform == Platform::Wii ||
        platform == Platform::N3DS)
    {
        cook = true;
    }

    return cook;
}

void CookTexture(
    Texture* texture,
    Platform platform,
    const std::vector<uint8_t>& srcPixels,
    std::vector<uint8_t>& outData,
    uint32_t& outWidth,
    uint32_t& outHeight,
    uint32_t& outNumMips)
{
#if EDITOR
    // (1) Save a temporary PNG in the Intermediate directory.
    std::string tempDir = GetEngineState()->mProjectDirectory + "Intermediate";
    const char* tempPng = "Temp.png";
    const char* tempOut = "Temp.tex";

    std::string pngPath = tempDir + "/" + tempPng;
    std::string outPath = tempDir + "/" + tempOut;
    if (!DoesDirExist(tempDir.c_str()))
    {
        CreateDir(tempDir.c_str());
    }

    // Check if texture is fully opaque, and if not, whether its alpha is a mask or a gradient.
    //
    // The distinction decides which formats can hold it. CMPR carries one bit of alpha, which is
    // exactly enough for a mask -- every texel either there or not -- and nothing like enough for
    // a gradient. Knowing which this is means a cutout texture can stay at 4 bits a texel instead
    // of being pushed up to 16 for an alpha channel it never needed.
    bool opaque = true;
    bool maskedAlpha = true;

    for (uint32_t i = 0; i < srcPixels.size(); i += 4)
    {
        const uint8_t alpha = srcPixels[i + 3];

        if (alpha != 0xff)
        {
            opaque = false;

            // Anything between counts as a gradient. The tolerance is for textures whose fully
            // transparent or fully opaque texels have been nudged a little by resizing or by a
            // lossy source.
            if (alpha > 8 && alpha < 247)
            {
                maskedAlpha = false;
                break;
            }
        }
    }

    std::vector<uint8_t> pixels = srcPixels;
    int32_t texWidth = texture->GetWidth();
    int32_t texHeight = texture->GetHeight();
    int32_t maxLod = 0;

    if (platform == Platform::GameCube ||
        platform == Platform::Wii ||
        platform == Platform::N3DS)
    {
        // The engine splash is exempt. It is drawn full screen for a moment at boot, where any
        // reduction is as visible as it ever gets, and it costs nothing after that -- it is not
        // competing for texture memory with a scene, because there is no scene yet. A project
        // turning its textures down to fit the hardware is not asking for its splash to be soft,
        // and having to remember to tick Force High Quality on it in every project is a trap.
        const bool isEngineSplash = (texture->GetName() == "T_OctaveSplash");

        bool forceHq = texture->IsForcedHighQuality() || isEngineSplash;
        int32_t downsampleFactor = texture->GetLowQualityDownsampleFactor();
        int32_t consoleMaxTextureSize = GetEngineConfig()->mLqMaxTextureSize;

        // A project can step every texture down a level at once with LqDownsampleFactor in
        // Config.ini, rather than setting the property on each texture by hand. A texture that
        // asks for more reduction in its own property still wins; Force High Quality still
        // exempts it entirely, since that check gates the whole block below.
        downsampleFactor = glm::max(downsampleFactor, GetEngineConfig()->mLqDownsampleFactor);

        if (!forceHq && (consoleMaxTextureSize != 0 || downsampleFactor > 1))
        {
            if (downsampleFactor > 1)
            {
                texWidth = glm::max(texWidth >> (downsampleFactor - 1), 1);
                texHeight = glm::max(texHeight >> (downsampleFactor - 1), 1);
            }

            float whRatio = float(texWidth) / float(texHeight);
            bool nonSquare = (texWidth != texHeight);

            texWidth = glm::max(texWidth, 1);
            texHeight = glm::max(texHeight, 1);

            if (consoleMaxTextureSize > 0)
            {
                texWidth = glm::min(texWidth, consoleMaxTextureSize);
                texHeight = glm::min(texHeight,consoleMaxTextureSize);
            }

            // Without a max size there is no upper bound to clamp against. Clamping to
            // consoleMaxTextureSize when it is 0 gives glm::clamp the range [1, 0], which returns
            // 0 and collapses the texture to nothing -- reachable whenever a downsample factor is
            // used on a non-square texture without also setting LqMaxTextureSize.
            const uint32_t maxDim = (consoleMaxTextureSize > 0) ? uint32_t(consoleMaxTextureSize) : 0xFFFFFFFFu;

            if (nonSquare)
            {
                if (texWidth > texHeight)
                {
                    float texHeightFloat = texWidth * (1 / whRatio);
                    texHeight = uint32_t(texHeightFloat + 0.5f);
                    texHeight = glm::clamp<uint32_t>(texHeight, 1, maxDim);
                }
                else
                {
                    float texWidthFloat = texHeight * whRatio;
                    texWidth = uint32_t(texWidthFloat + 0.5f);
                    texWidth = glm::clamp<uint32_t>(texWidth, 1, maxDim);
                }
            }
        }

        // TODO: Resize to a power-of-two?

        // Resize texture if downsampled for LQ, or non-power-of-two
        if (texWidth != texture->GetWidth() ||
            texHeight != texture->GetHeight())
        {
            pixels.resize(texWidth * texHeight * sizeof(uint32_t));

            stbir_resize_uint8_srgb(
                srcPixels.data(),
                texture->GetWidth(),
                texture->GetHeight(),
                texture->GetWidth() * sizeof(uint32_t),
                pixels.data(),
                texWidth,
                texHeight,
                texWidth * sizeof(uint32_t),
                stbir_pixel_layout::STBIR_RGBA);
        }
    }

    int32_t consoleEnableMips = GetEngineConfig()->mLqEnableMipMaps;

    const uint32_t comps = 4;
    stbi_flip_vertically_on_write(platform == Platform::N3DS);
    stbi_write_png(pngPath.c_str(), texWidth, texHeight, comps, pixels.data(), texWidth * 4);

    // (2) Exec platform-specific texture converter with relevant args, and output to another temp file in Intermediate.
    // Set when the GameCube/Wii branch decides this texture is CMPR that we
    // can encode ourselves. Declared out here because the format itself is
    // scoped to that branch.
    bool encodeCmprHere = false;

    std::string cookCmd = "";

    switch (platform)
    {
    case Platform::GameCube:
    case Platform::Wii:
    {
        cookCmd += GetDevkitproPath() + "/tools/bin/gxtexconv";

        cookCmd += " -i ";
        cookCmd += pngPath.c_str();
        cookCmd += " -o ";
        cookCmd += outPath.c_str();
        cookCmd += " colfmt=";

        PixelFormat format = texture->GetFormat();

        // A project can force one colour format on every console texture with LqTextureFormat in
        // Config.ini, rather than setting it per asset. Textures default to RGBA8, which is 32 bits
        // a texel -- a single 512x512 would fill Flipper's 1 MB texture cache on its own -- so a
        // project targeting this hardware almost always wants CMPR instead, at 4 bits.
        // Force High Quality opts a texture out, as it does for the downsampling above.
        // The engine splash opts out of this as well as of the downsampling. It is a full screen
        // image, which is where block compression shows worst, and it is gone before anything else
        // needs the memory.
        const bool isEngineSplash = (texture->GetName() == "T_OctaveSplash");

        const int32_t forcedFormat = GetEngineConfig()->mLqTextureFormat;
        if (forcedFormat >= 0 && !texture->IsForcedHighQuality() && !isEngineSplash)
        {
            format = (PixelFormat)forcedFormat;
        }

        // CMPR carries at most 1 bit of alpha through gxtexconv, so a texture with any
        // translucency in it loses that alpha entirely and reaches the TEV as a solid 255. A
        // Translucent material then has nothing left to blend with except the opacity slider,
        // and renders opaque at opacity 1 no matter what its texture looks like.
        //
        // Fall back to RGB5A3 (RGBA5551 here) for those, which spends 16 bits a texel instead of
        // 4 but gives translucent texels 3 bits of alpha. Opaque textures are untouched and stay
        // at 4 bits, so this only costs memory where alpha is actually used.
        //
        // This used to be gated to Wii. There is no hardware difference behind that -- GameCube's
        // Flipper and Wii's Hollywood share these texture formats and the same converter -- so a
        // GameCube build silently lost every soft alpha. The N3DS branch below already picks
        // etc1 vs etc1a4 on opacity alone, which is the same decision made correctly.
        //
        // A mask is the exception. CMPR is DXT1, which carries exactly one bit of alpha, so a
        // texture whose texels are only ever fully there or fully absent loses nothing by staying
        // compressed -- and stays at a quarter of the size it would be as RGB5A3. That is worth
        // having for a large cutout: a 512x1024 costs 2.7MB as RGBA8, 1.3MB as RGB5A3 and 340KB
        // as CMPR.
        // CMPR can carry a bit of alpha; gxtexconv does not put one there.
        //
        // The format is DXT1, which encodes transparency when its first
        // endpoint is not greater than its second -- index 3 then means
        // transparent. So a mask ought to survive compression, which is what
        // the comment above assumed.
        //
        // It does not. Feeding gxtexconv v1.0.6 a PNG that is half fully
        // transparent and asking for colfmt=14 gives back 256 blocks of which
        // every one is written in the mode that COULD encode transparency and
        // not one uses the transparent index. The same holds for real assets:
        // 65536 blocks of an alpha-masked texture, none transparent. There is
        // no flag for it either -- the converter takes only colfmt, mipmap,
        // lod bounds and size.
        //
        // So a cutout's transparent background arrives as solid black.
        //
        // That is invisible on a single-texture material and ruins any layered
        // one: each Decal stage blends by texture alpha, so an all-opaque
        // layer overwrites everything beneath it and only the last texture in
        // the material is ever seen.
        //
        // So CMPR with alpha is encoded here instead of by gxtexconv, which
        // keeps a mask at 4 bits a texel. That path does not do mipmaps, so a
        // mipmapped cutout still has to give up the compression.
        const bool wantMips = (texture->IsMipmapped() && consoleEnableMips);
        encodeCmprHere = (format == PixelFormat::CMPR) && !wantMips;

        if (format == PixelFormat::CMPR && !opaque && !encodeCmprHere)
        {
            format = PixelFormat::RGBA5551;
        }

        switch (format)
        {
        case PixelFormat::LA4: cookCmd += "2"; break;
        case PixelFormat::RGB565: cookCmd += "4"; break;
        case PixelFormat::RGBA5551: cookCmd += "5"; break;
        case PixelFormat::CMPR: cookCmd += "14"; break;   // DXT1; carries 1-bit alpha
        case PixelFormat::RGBA8: // Fallthrough to default
        default: cookCmd += "6"; break;
        }

        if (texture->IsMipmapped() && consoleEnableMips)
        {
            //int32_t maxLod = int32_t(texture->GetMipLevels()) - 1;
            maxLod = static_cast<int32_t>(floor(log2(std::min(texWidth, texHeight))) + 1) - 3;

            maxLod = glm::max(maxLod, 0);
            cookCmd += " mipmap=yes ";
            cookCmd += "minlod=0 ";
            cookCmd += "maxLod=";
            cookCmd += to_string(maxLod);
            cookCmd += " ";
        }
        else
        {
            cookCmd += " mipmap=no ";
        }

        cookCmd += "width=" + std::to_string(texWidth);
        cookCmd += " height=" + std::to_string(texHeight) + " ";

        break;
    }
    case Platform::N3DS:
    {
        cookCmd += GetDevkitproPath() + "/tools/bin/tex3ds";
        cookCmd += " -o ";
        cookCmd += outPath.c_str();
        cookCmd += " -f ";

        switch (texture->GetFormat())
        {
        case PixelFormat::LA4: cookCmd += "la4"; break;
        case PixelFormat::RGB565: cookCmd += "rgb565"; break;
        case PixelFormat::RGBA5551: cookCmd += "rgba5551"; break;
        case PixelFormat::CMPR: opaque ? (cookCmd += "etc1") : (cookCmd += "etc1a4"); break;
        case PixelFormat::RGBA8: // Fallthrough to default
        default: cookCmd += "rgba8"; break;
        }

        if (texture->IsMipmapped() && consoleEnableMips)
        {
            cookCmd += " -m triangle ";
        }
        else
        {
            cookCmd += " ";
        }

        cookCmd += pngPath.c_str();
        
        break;
    }

    default: OCT_ASSERT(0); break;
    }

    // Encode CMPR ourselves where we can, rather than shelling out.
    //
    // gxtexconv cannot put alpha in a CMPR texture, so anything masked would
    // otherwise have to be spent at 16 bits a texel. This also saves a process
    // launch per texture, which is most of what cooking a project costs.
    if (encodeCmprHere)
    {
        std::vector<uint8_t> tpl;

        if (EncodeCmprTpl(pixels.data(), texWidth, texHeight, tpl))
        {
            outData = tpl;
            outWidth = texWidth;
            outHeight = texHeight;
            outNumMips = 1;
            return;
        }

        // Falling through to gxtexconv loses the alpha, so say why rather than
        // letting a cutout quietly turn solid.
        LogWarning("Texture '%s': %ux%u cannot be CMPR encoded here; "
                   "falling back to gxtexconv, which will drop any alpha.",
                   texture->GetName().c_str(), texWidth, texHeight);
    }

    SYS_Exec(cookCmd.c_str());

    // (3) Use a stream to read the converted file, and copy all the data into outData
    Stream stream;
    stream.ReadFile(outPath.c_str(), false);
    outData.resize(stream.GetSize());
    memcpy(outData.data(), stream.GetData(), stream.GetSize());

    outWidth = texWidth;
    outHeight = texHeight;
    outNumMips = maxLod + 1;
#endif
}


Texture::Texture() :
    mWidth(0),
    mHeight(0),
    mMipLevels(1),
    mLayers(1),
    mFormat(PixelFormat::RGBA8),
    mFilterType(FilterType::Linear),
    mWrapMode(WrapMode::Repeat),
    mMipmapped(true),
    mRenderTarget(false),
    mSrgb(true),
    mForceHighQuality(false),
    mLowQualityDownsampleFactor(1)
{
    mType = Texture::GetStaticType();
}

Texture::~Texture()
{
    Destroy();
}

TextureResource* Texture::GetResource()
{
    return &mResource;
}

// WHY. A GameCube game that changes between skies frees eight 512 KB star frames and loads eight
// more of exactly the same size, each time. With malloc that cuts the heap up: a few changes in,
// with megabytes free, no single 512 KB block is left and the frames fail to load. The textures
// themselves are interchangeable -- same size, same format -- so here the new texels go into the
// buffer that is already there. The asset keeps its own name; only its picture changes.
bool Texture::ReloadFrom(const std::string& assetName)
{
    // All of it at once: straight into the buffer, 32 KB at a time.
    uint32_t total = 0;
    int32_t at = 0;
    do
    {
        at = ReloadPart(assetName, (uint32_t)at, 32 * 1024, total);
    } while (at >= 0 && (uint32_t)at < total);
    return at >= 0;
}

// ReloadFrom a piece at a time: `maxBytes` of the texels from byte `at`, so that refilling a big
// texture is spread over many frames (a marathon's change of sky, while it is being played). The
// texture shows a mix of the two pictures until the last piece is in: refill one that is not on
// the screen. Returns where the next piece starts (outTotal when done), or -1.
int32_t Texture::ReloadPart(const std::string& assetName, uint32_t at, uint32_t maxBytes, uint32_t& outTotal)
{
    outTotal = 0;
#if API_GX && !EDITOR
    TextureResource* resource = GetResource();
    AssetStub* stub = AssetManager::Get()->GetAssetStub(assetName);
    if (!IsLoaded() || IsDynamic() || resource->mTplData == nullptr || stub == nullptr || stub->mPath.empty())
    {
        return -1;
    }

    if (at == 0 || mReloadSource != assetName)
    {
        // The header: where the texels begin, and how many there are. It is well under 256 bytes.
        char head[256];
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

        uint32_t width = stream.ReadUint32();
        uint32_t height = stream.ReadUint32();
        uint32_t mips = stream.ReadUint32();
        stream.ReadUint32();                                // layers
        PixelFormat format = (PixelFormat)stream.ReadUint32();
        stream.ReadUint32();                                // filter
        stream.ReadUint32();                                // wrap
        stream.ReadBool();                                  // mipmapped
        stream.ReadBool();                                  // render target
        stream.ReadBool();                                  // sRGB
        if (header.mVersion >= ASSET_VERSION_TEXTURE_LOW_QUALITY)
        {
            stream.ReadBool();
            stream.ReadUint8();
        }
        if (header.mVersion >= ASSET_VERSION_TEXTURE_COOKED_PROPERTIES)
        {
            width = stream.ReadUint32();
            height = stream.ReadUint32();
            mips = stream.ReadUint32();
            stream.ReadUint32();                            // filter
        }
        uint32_t size = stream.ReadUint32();
        uint32_t offset = stream.GetPos();

        if (width != mWidth || height != mHeight || mips != mMipLevels || format != mFormat ||
            size != resource->mTplSize || offset >= sizeof(head))
        {
            LogWarning("Texture %s: cannot take %s's texels in place (a different size or format)",
                       GetName().c_str(), assetName.c_str());
            mReloadSource.clear();
            return -1;
        }
        mReloadSource = assetName;
        mReloadOffset = offset;
    }

    uint32_t size = resource->mTplSize;
    outTotal = size;
    if (at >= size)
    {
        return (int32_t)size;
    }

    // Straight into the buffer, 32 KB at a time (each read bounces through a buffer of that size).
    // The GPU may still be drawing the last frame with these texels: let it finish first.
    GxWaitGpu();
    const uint32_t kPiece = 32 * 1024;
    uint32_t stop = (maxBytes >= size - at) ? size : at + maxBytes;
    char* dst = (char*)resource->mTplData;
    for (uint32_t from = at; from < stop; from += kPiece)
    {
        uint32_t n = (stop - from < kPiece) ? (stop - from) : kPiece;
        if (!SYS_ReadFileRange(stub->mPath.c_str(), true, mReloadOffset + from, n, dst + from))
        {
            LogError("Texture %s: reading %s failed part way; the picture is mixed", GetName().c_str(),
                     assetName.c_str());
            return -1;
        }
    }

    DCFlushRange(dst + at, stop - at);
    if (stop >= size)
    {
        GX_InvalidateTexAll();
        mReloadSource.clear();
    }
    return (int32_t)stop;
#else
    (void)assetName; (void)at; (void)maxBytes;
    return -1;
#endif
}

void Texture::LoadStream(Stream& stream, Platform platform)
{
    Asset::LoadStream(stream, platform);

    mWidth = stream.ReadUint32();
    mHeight = stream.ReadUint32();
    mMipLevels = stream.ReadUint32();
    mLayers = stream.ReadUint32();
    mFormat = (PixelFormat)stream.ReadUint32();
    mFilterType = (FilterType)stream.ReadUint32();
    mWrapMode = (WrapMode)stream.ReadUint32();

    mMipmapped = stream.ReadBool();
    mRenderTarget = stream.ReadBool();
    mSrgb = stream.ReadBool();

    if (mVersion >= ASSET_VERSION_TEXTURE_LOW_QUALITY)
    {
        mForceHighQuality = stream.ReadBool();
        mLowQualityDownsampleFactor = stream.ReadUint8();
    }

    if (UseCookedTextures(platform))
    {
        if (mVersion >= ASSET_VERSION_TEXTURE_COOKED_PROPERTIES)
        {
            mWidth = stream.ReadUint32();
            mHeight = stream.ReadUint32();
            mMipLevels = stream.ReadUint32();
            mFilterType = (FilterType)stream.ReadUint32();
            mMipmapped = mMipLevels > 1;
        }

        uint32_t cookedDataSize = stream.ReadUint32();
        mPixels.resize(cookedDataSize);
        stream.ReadBytes(mPixels.data(), cookedDataSize);
    }
    else
    {
        int32_t size = (mWidth * mHeight * RGBA8_SIZE);
        mPixels.resize(size);

        for (int32_t i = 0; i < size; ++i)
        {
            mPixels[i] = stream.ReadUint8();
        }
    }
}

void Texture::SaveStream(Stream& stream, Platform platform)
{
    Asset::SaveStream(stream, platform);

#if EDITOR
    // For now, only allow saving in editor where mPixels is valid.
    // In the future, copy texture to buffer.
    stream.WriteUint32(mWidth);
    stream.WriteUint32(mHeight);
    stream.WriteUint32(mMipmapped ? mMipLevels : 1);
    stream.WriteUint32(mLayers);
    stream.WriteUint32(uint32_t(mFormat));
    stream.WriteUint32(uint32_t(mFilterType));
    stream.WriteUint32(uint32_t(mWrapMode));

    stream.WriteBool(mMipmapped);
    stream.WriteBool(mRenderTarget);
    stream.WriteBool(mSrgb);

    stream.WriteBool(mForceHighQuality);
    stream.WriteUint8(mLowQualityDownsampleFactor);

    if (UseCookedTextures(platform))
    {
        std::vector<uint8_t> cookedData;
        uint32_t texWidth = mWidth;
        uint32_t texHeight = mHeight;
        uint32_t numMips = mMipLevels;
        CookTexture(this, platform, mPixels, cookedData, texWidth, texHeight, numMips);

        stream.WriteUint32(texWidth);
        stream.WriteUint32(texHeight);
        stream.WriteUint32(numMips);
        // In the future, might support option for forcing filter type;
        stream.WriteUint32(uint32_t(mFilterType));

        uint32_t cookedDataSize = (uint32_t)cookedData.size();
        stream.WriteUint32(cookedDataSize);
        stream.WriteBytes(cookedData.data(), cookedDataSize);
    }
    else
    {
        // If not using an custom formats, just write out the raw RGBA8 pixels, uncompressed.
        OCT_ASSERT(mPixels.size() == (mWidth * mHeight * RGBA8_SIZE));
        for (int32_t i = 0; i < int32_t(mPixels.size()); ++i)
        {
            stream.WriteUint8(mPixels[i]);
        }
    }
#endif
}

void Texture::Create()
{
    Asset::Create();

    GFX_CreateTextureResource(this, mPixels);

#if !EDITOR
    // This pixel data is transferred to the GPU resource in GFX_CreateTextureResource(), so now 
    // we can clear the mPixels vector and shrink it so to free memory.
    // Keep copy of pixels when in editor so they can be saved without reading from the texture.
    mPixels.clear();
    mPixels.shrink_to_fit();
#endif
}

void Texture::Destroy()
{
    Asset::Destroy();

    GFX_DestroyTextureResource(this);
}

bool Texture::Import(const std::string& path, ImportOptions* options)
{
    bool success = Asset::Import(path, options);
    if (!success)
    {
        return false;
    }

#if EDITOR
    int32_t texWidth;
    int32_t texHeight;
    int32_t texChannels;
    PixelFormat format = PixelFormat::RGBA8;

    stbi_uc* pixels = stbi_load(path.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);

    uint32_t imageSize = texWidth * texHeight * 4;

    if (pixels == nullptr)
    {
        LogError("Failed to load texture image");
        success = false;
    }

    if (!Maths::IsPowerOfTwo(texWidth) || !Maths::IsPowerOfTwo(texHeight))
    {
        LogError("Texture dimensions must be power-of-two (e.g. 256x128, 64x64)");
        success = false;
    }

    if (success)
    {
        mPixels.resize(imageSize);
        memcpy(mPixels.data(), pixels, imageSize);

        mWidth = texWidth;
        mHeight = texHeight;
        mFormat = format;
        mRenderTarget = false;
        mMipmapped = true;
        mMipLevels = mMipmapped ? static_cast<int32_t>(floor(log2(std::max(mWidth, mHeight))) + 1) : 1;

        if (options != nullptr)
        {
            if (options->HasOption("mipmapped"))
            {
                mMipmapped = options->GetOptionValue("mipmapped");

                if (!mMipmapped)
                {
                    mMipLevels = 1;
                }
            }
        }

        Create();
    }

    stbi_image_free(pixels);
#endif

    return success;
}

void Texture::GatherProperties(std::vector<Property>& outProps)
{
    Asset::GatherProperties(outProps);

    outProps.push_back(Property(DatumType::Bool, "Mipmapped", this, &mMipmapped, 1, HandlePropChange));
    outProps.push_back(Property(DatumType::Bool, "sRGB", this, &mSrgb, 1, HandlePropChange));
    outProps.push_back(Property(DatumType::Integer, "Format", this, &mFormat, 1, Texture::HandlePropChange, NULL_DATUM, 5, sPixelFormatEnumStrings));
    outProps.push_back(Property(DatumType::Integer, "Filter Type", this, &mFilterType, 1, Texture::HandlePropChange, NULL_DATUM, int32_t(FilterType::Count), gFilterEnumStrings));
    outProps.push_back(Property(DatumType::Integer, "Wrap Mode", this, &mWrapMode, 1, Texture::HandlePropChange, NULL_DATUM, int32_t(WrapMode::Count), gWrapEnumStrings));
    outProps.push_back(Property(DatumType::Bool, "Force High Quality", this, &mForceHighQuality, 1, HandlePropChange));
    outProps.push_back(Property(DatumType::Byte, "LQ Downsample Factor", this, &mLowQualityDownsampleFactor, 1, HandlePropChange));
}

glm::vec4 Texture::GetTypeColor()
{
    return glm::vec4(1.0f, 0.3f, 0.3f, 1.0f);
}

const char* Texture::GetTypeName()
{
    return "Texture";
}

const char* Texture::GetTypeImportExt()
{
    return ".png";
}

void Texture::Init(uint32_t width, uint32_t height, uint8_t* data)
{
    OCT_ASSERT(width > 0);
    OCT_ASSERT(height > 0);
    OCT_ASSERT(data != nullptr);

    mWidth = width;
    mHeight = height;
    
    uint32_t imageSize = width * height * 4;
    mPixels.resize(imageSize);
    memcpy(mPixels.data(), data, imageSize);
}

void Texture::InitDynamic(uint32_t width, uint32_t height)
{
    OCT_ASSERT(width > 0);
    OCT_ASSERT(height > 0);
    OCT_ASSERT(!IsLoaded());

    mWidth = width;
    mHeight = height;
    mFormat = PixelFormat::RGBA8;
    mFilterType = FilterType::Linear;
    mWrapMode = WrapMode::Clamp;
    mMipmapped = false;
    mMipLevels = 1;
    mDynamic = true;

    // Start opaque black.
    mPixels.assign(width * height * RGBA8_SIZE, 0);
    for (uint32_t i = 3; i < mPixels.size(); i += RGBA8_SIZE)
    {
        mPixels[i] = 0xff;
    }
}

bool Texture::IsDynamic() const
{
    return mDynamic;
}

void Texture::InitDynamicYuv(uint32_t width, uint32_t height)
{
    OCT_ASSERT((width % 2) == 0 && (height % 2) == 0);

    InitDynamic(width, height);
    mYuv = true;
}

bool Texture::IsYuv() const
{
    return mYuv;
}

void Texture::UpdatePixels(const uint8_t* rgba8)
{
    if (mDynamic && !mYuv && IsLoaded() && rgba8 != nullptr)
    {
        GFX_UpdateTextureResourcePixels(this, rgba8);
    }
}

void Texture::SetMipmapped(bool mipmapped)
{
    mMipmapped = mipmapped;
    mMipLevels = mMipmapped ? static_cast<int32_t>(floor(log2(std::max(mWidth, mHeight))) + 1) : 1;
}

bool Texture::IsMipmapped() const
{
    return mMipmapped;
}

bool Texture::IsRenderTarget() const
{
    return mRenderTarget;
}

bool Texture::IsSrgb() const
{
    return mSrgb;
}

bool Texture::IsForcedHighQuality() const
{
    return mForceHighQuality;
}

uint32_t Texture::GetWidth() const
{
    return mWidth;
}

uint32_t Texture::GetHeight() const
{
    return mHeight;
}

uint32_t Texture::GetMipLevels() const
{
    return mMipLevels;
}

uint32_t Texture::GetLayers() const
{
    return mLayers;
}

PixelFormat Texture::GetFormat() const
{
    return mFormat;
}

FilterType Texture::GetFilterType() const
{
    return mFilterType;
}

WrapMode Texture::GetWrapMode() const
{
    return mWrapMode;
}

int32_t Texture::GetLowQualityDownsampleFactor() const
{
    return mLowQualityDownsampleFactor;
}

// These Set***() calls need to be called before Create().
void Texture::SetFormat(PixelFormat format)
{
    mFormat = format;
}

void Texture::SetFilterType(FilterType filterType)
{
    mFilterType = filterType;
}

void Texture::SetWrapMode(WrapMode wrapMode)
{
    mWrapMode = wrapMode;
}

void Texture::SetForceHighQuality(bool forceHq)
{
    mForceHighQuality = forceHq;
}
