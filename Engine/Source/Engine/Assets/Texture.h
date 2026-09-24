#pragma once

#include <string>
#include "glm/glm.hpp"
#include "Asset.h"

#include "Graphics/GraphicsTypes.h"

class Texture : public Asset
{

public:

    DECLARE_ASSET(Texture, Asset);

    Texture();
    virtual ~Texture();

    TextureResource* GetResource();

    // Console only (GX): fill THIS texture's existing buffer with another texture asset's texels,
    // read straight off the disc in small pieces, when the two are the same size and format.
    // Nothing big is allocated or freed -- see the .cpp. False (and nothing changed) otherwise.
    bool ReloadFrom(const std::string& assetName);
    // The same, a piece at a time: up to maxBytes from byte `at`. Returns where the next piece
    // starts (outTotal, the texel bytes, when done), or -1.
    int32_t ReloadPart(const std::string& assetName, uint32_t at, uint32_t maxBytes, uint32_t& outTotal);

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

    void Init(uint32_t width, uint32_t height, uint8_t* data);

    // Dynamic textures hold RGBA8 pixels that can be replaced every frame with
    // UpdatePixels() (e.g. video). Call InitDynamic() before Create().
    void InitDynamic(uint32_t width, uint32_t height);
    bool IsDynamic() const;
    void UpdatePixels(const uint8_t* rgba8);

    // A dynamic texture made of Y, Cb and Cr planes (GameCube/Wii), converted to RGB
    // by the GPU when drawn by a widget. Its data is set with GFX_SetTextureResourceData().
    void InitDynamicYuv(uint32_t width, uint32_t height);
    bool IsYuv() const;

    void SetMipmapped(bool mipmapped);
    bool IsMipmapped() const;
    bool IsRenderTarget() const;
    bool IsSrgb() const;
    bool IsForcedHighQuality() const;

    uint32_t GetWidth() const;
    uint32_t GetHeight() const;
    uint32_t GetMipLevels() const;
    uint32_t GetLayers() const;
    PixelFormat GetFormat() const;
    FilterType GetFilterType() const;
    WrapMode GetWrapMode() const;
    int32_t GetLowQualityDownsampleFactor() const;

    void SetFormat(PixelFormat format);
    void SetFilterType(FilterType filterType);
    void SetWrapMode(WrapMode wrapMode);
    void SetForceHighQuality(bool forceHq);

    static bool HandlePropChange(class Datum* datum, uint32_t index, const void* newValue);

protected:

    std::string mReloadSource;          // ReloadPart: the asset being read in, and where its texels start
    uint32_t mReloadOffset = 0;

    uint32_t mWidth;
    uint32_t mHeight;
    uint32_t mMipLevels;
    uint32_t mLayers;
    PixelFormat mFormat;
    FilterType mFilterType;
    WrapMode mWrapMode;
    bool mMipmapped;
    bool mRenderTarget;
    bool mSrgb;
    bool mForceHighQuality;
    uint8_t mLowQualityDownsampleFactor;
    bool mDynamic = false;
    bool mYuv = false;

    // This pixel array is used as an intermediate storage between LoadStream() and Create()
    // It is cleared and shrunk within Create() except when compiled for EDITOR
    std::vector<uint8_t> mPixels;

    // Graphics Resource
    TextureResource mResource;
};
