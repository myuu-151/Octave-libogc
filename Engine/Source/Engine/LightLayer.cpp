#include "LightLayer.h"

#include "Assets/Texture.h"
#include "AssetManager.h"
#include "Log.h"
#include "Graphics/GraphicsTypes.h"

#include <math.h>
#include <string.h>

#if API_GX
#include <gccore.h>
#include <malloc.h>
#include "Graphics/GX/GxUtils.h"
#endif

// The light is summed in 0-255; a light's radius is at most this many pixels (the square-root table
// covers distances up to it).
static const float kMaxRadius = 255.0f;
static const uint32_t kRootSize = 16384;        // squared distances / 4, up to 255^2

// The 4x4 ordered dither: where a pixel falls between two bands, its cell decides which it takes.
static const uint8_t kBayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

static LightLayer* sLightLayer = nullptr;

LightLayer* LightLayer::Get()
{
    // Made once and kept: no destructor runs at exit, after the asset manager has gone.
    if (sLightLayer == nullptr)
    {
        sLightLayer = new LightLayer();
    }
    return sLightLayer;
}

bool LightLayer::Init(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
    {
        return false;
    }

#if API_GX
    // IA4 is 8 x 4 texels a 32-byte tile: round up to whole tiles.
    width = (width + 7) & ~7u;
    height = (height + 3) & ~3u;
#endif

    if (mTexture.Get() != nullptr && width == mWidth && height == mHeight)
    {
        return true;
    }

#if API_GX
    // The GPU may still be drawing from the old buffers.
    for (uint8_t*& texels : mTexels)
    {
        if (texels != nullptr) GxDeferFree(texels);
        texels = nullptr;
    }
#endif
    mTexture = nullptr;
    mWidth = width;
    mHeight = height;
    mSum.assign(width * height, 0);
    MakeTables();

    Texture* texture = NewTransientAsset<Texture>();
    texture->SetName("LightLayer");

#if API_GX
    // The engine's own dynamic texture is the smallest it makes (8 x 4); its texture object is then
    // pointed at the layer's IA4 buffers, which are what the GPU reads.
    texture->InitDynamic(8, 4);
    texture->SetFilterType(FilterType::Nearest);
    texture->Create();
    mTexelSize = width * height;
    for (uint8_t*& texels : mTexels)
    {
        texels = (uint8_t*)memalign(32, mTexelSize);
        if (texels == nullptr)
        {
            LogError("LightLayer: no memory for %ux%u (%u bytes)", width, height, mTexelSize);
            return false;
        }
        memset(texels, 0, mTexelSize);       // no darkness until the first Commit
        DCFlushRange(texels, mTexelSize);
    }
    mNext = 0;
    GXTexObj* obj = &texture->GetResource()->mGxTexObj;
    GX_InitTexObj(obj, mTexels[0], (uint16_t)width, (uint16_t)height, GX_TF_IA4, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(obj, GX_NEAR, GX_NEAR);
    GX_InvalidateTexAll();
#else
    texture->InitDynamic(width, height);
    texture->SetFilterType(FilterType::Nearest);
    texture->Create();
    mRgba.assign(width * height * 4, 0);
#endif

    mTexture = texture;
    return true;
}

Texture* LightLayer::GetTexture() const
{
    return mTexture.Get<Texture>();
}

void LightLayer::SetBands(uint32_t bands)
{
    mBands = (bands < 1) ? 1 : (bands > 15 ? 15 : bands);
    MakeTables();
}

void LightLayer::SetDarkness(float darkest)
{
    mDarkest = (darkest < 0.0f) ? 0.0f : (darkest > 1.0f ? 1.0f : darkest);
    MakeTables();
}

// The square roots, and the darkness for each dither cell and light level (as the texture wants it:
// an IA4 byte on the GameCube, an alpha elsewhere).
void LightLayer::MakeTables()
{
    if (mRoot.empty())
    {
        mRoot.resize(kRootSize);
        for (uint32_t i = 0; i < kRootSize; ++i)
        {
            mRoot[i] = (uint16_t)(sqrtf((float)(i * 4)) * 16.0f + 0.5f);
        }
    }

    mDark.resize(16 * 256);
    for (uint32_t cell = 0; cell < 16; ++cell)
    {
        const float dither = (kBayer[cell] / 16.0f - 0.5f) * 0.9f;
        for (uint32_t level = 0; level < 256; ++level)
        {
            float band = floorf((level / 255.0f) * mBands + dither + 0.5f) / mBands;
            band = (band < 0.0f) ? 0.0f : (band > 1.0f ? 1.0f : band);
            float dark = mDarkest - band;
            dark = (dark < 0.0f) ? 0.0f : dark;
#if API_GX
            mDark[cell * 256 + level] = (uint8_t)((uint32_t)(dark * 15.0f + 0.5f) << 4);    // alpha high, black
#else
            mDark[cell * 256 + level] = (uint8_t)(dark * 255.0f + 0.5f);
#endif
        }
    }
}

void LightLayer::Clear()
{
    mLights.clear();
}

void LightLayer::AddLight(float x, float y, float radius, float strength)
{
    if (radius <= 0.0f || strength <= 0.0f)
    {
        return;
    }
    mLights.push_back({ x, y, radius > kMaxRadius ? kMaxRadius : radius, strength });
}

void LightLayer::Commit()
{
    if (mTexture.Get() == nullptr)
    {
        return;
    }

    const int32_t width = (int32_t)mWidth;
    const int32_t height = (int32_t)mHeight;
    uint8_t* sum = mSum.data();
    memset(sum, 0, mSum.size());

    // Each light, over the pixels in its square only: (1 - d / r) * 1.7 * strength, capped at 1,
    // added to what's there, the sum capped at 1.
    for (const Light& light : mLights)
    {
        const float r = light.mRadius;
        const float r2 = r * r;
        const float scale = 1.7f * light.mStrength * 255.0f;
        const float perRoot = 1.0f / (16.0f * r);
        int32_t x0 = (int32_t)floorf(light.mX - r), x1 = (int32_t)ceilf(light.mX + r);
        int32_t y0 = (int32_t)floorf(light.mY - r), y1 = (int32_t)ceilf(light.mY + r);
        x0 = x0 < 0 ? 0 : x0;
        y0 = y0 < 0 ? 0 : y0;
        x1 = x1 > width - 1 ? width - 1 : x1;
        y1 = y1 > height - 1 ? height - 1 : y1;

        for (int32_t y = y0; y <= y1; ++y)
        {
            const float dy = (float)y + 0.5f - light.mY;
            const float dy2 = dy * dy;
            if (dy2 >= r2)
            {
                continue;
            }
            uint8_t* row = sum + y * width;
            float dx = (float)x0 + 0.5f - light.mX;
            for (int32_t x = x0; x <= x1; ++x, dx += 1.0f)
            {
                const float d2 = dx * dx + dy2;
                if (d2 >= r2)
                {
                    continue;
                }
                const float v = (1.0f - mRoot[(uint32_t)(d2 * 0.25f)] * perRoot) * scale;
                if (v <= 0.0f)
                {
                    continue;
                }
                const uint32_t total = row[x] + (v >= 255.0f ? 255u : (uint32_t)v);
                row[x] = (uint8_t)(total > 255u ? 255u : total);
            }
        }
    }

    const uint8_t* dark = mDark.data();

#if API_GX
    // Into the buffer the GPU isn't reading, in IA4's 8 x 4 tiles; then the texture object moves to it.
    uint8_t* texels = mTexels[mNext];
    mNext ^= 1;
    uint8_t* out = texels;
    for (int32_t by = 0; by < height; by += 4)
    {
        for (int32_t bx = 0; bx < width; bx += 8)
        {
            for (int32_t py = 0; py < 4; ++py)
            {
                const int32_t y = by + py;
                const uint8_t* row = sum + y * width + bx;
                const uint8_t* cells = dark + (((y & 3) << 2) << 8);
                for (int32_t px = 0; px < 8; ++px)
                {
                    *out++ = cells[((px & 3) << 8) + row[px]];      // bx is a multiple of 8: x & 3 == px & 3
                }
            }
        }
    }
    DCFlushRange(texels, mTexelSize);
    GXTexObj* obj = &mTexture.Get<Texture>()->GetResource()->mGxTexObj;
    GX_InitTexObj(obj, texels, (uint16_t)width, (uint16_t)height, GX_TF_IA4, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(obj, GX_NEAR, GX_NEAR);
    GX_InvalidateTexAll();
#else
    // Black (a breath of blue, as the mockup's darkest stone), its alpha the darkness.
    uint8_t* out = mRgba.data();
    for (int32_t y = 0; y < height; ++y)
    {
        const uint8_t* row = sum + y * width;
        const uint8_t* cells = dark + (((y & 3) << 2) << 8);
        for (int32_t x = 0; x < width; ++x, out += 4)
        {
            out[0] = 0;
            out[1] = 1;
            out[2] = 4;
            out[3] = cells[((x & 3) << 8) + row[x]];
        }
    }
    mTexture.Get<Texture>()->UpdatePixels(mRgba.data());
#endif
}
