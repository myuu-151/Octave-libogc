#pragma once

#include <stdint.h>
#include <vector>

#include "AssetRef.h"

class Texture;

// A layer of light for a 2D game: the lights added each frame are summed per pixel, the sum is cut
// into a few hard bands with a 4x4 ordered dither where they meet, and the result is written to a
// texture as darkness (black, its alpha how dark). Draw that texture on a Quad over the scene,
// stretched to it, with nearest filtering: what's lit shows through, what isn't goes dark. Draw
// anything with light of its own (flames, eyes) after it.
//
// Per pixel, for each light at distance d (within its radius r): (1 - d / r) * 1.7 * strength,
// capped at 1; the lights' sum capped at 1; then cut into bands, and the darkness is
// max(0, darkest - band). Pixels are the layer's own (a 320 x 240 layer over a 640 x 480 screen
// lights 2 x 2 blocks), and so is the dither, which stays put on them.
//
// GameCube/Wii: the texture is GX_TF_IA4 (16 levels of darkness, a byte a pixel), two buffers taken
// in turn so the GPU never reads one being written (Octave draws one frame while the next is
// made). Elsewhere: RGBA8, through Texture::UpdatePixels.
class LightLayer
{
public:
    static LightLayer* Get();

    // The layer's size in pixels (a multiple of 8 wide and 4 high on the GameCube). Makes its
    // texture; again with another size, makes it anew. False if there isn't the memory.
    bool Init(uint32_t width, uint32_t height);

    Texture* GetTexture() const;

    // The bands the light is cut into (1-15, 5 by default) and the darkness where there is none
    // (0-1, 0.97 by default).
    void SetBands(uint32_t bands);
    void SetDarkness(float darkest);

    // Each frame: Clear, a light at a time, then Commit to put it in the texture.
    void Clear();
    void AddLight(float x, float y, float radius, float strength);
    void Commit();

private:
    struct Light
    {
        float mX;
        float mY;
        float mRadius;
        float mStrength;
    };

    void MakeTables();

    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
    uint32_t mBands = 5;
    float mDarkest = 0.97f;
    std::vector<Light> mLights;
    std::vector<uint8_t> mSum;          // the light at each pixel, 0-255
    std::vector<uint16_t> mRoot;        // sqrt(i * 4) * 16: a distance from its square, without sqrt
    std::vector<uint8_t> mDark;         // [dither cell][light] -> darkness 0-255
    TextureRef mTexture;
    uint8_t* mTexels[2] = { nullptr, nullptr };    // GameCube/Wii: the IA4 buffers
    uint32_t mTexelSize = 0;
    uint32_t mNext = 0;
    std::vector<uint8_t> mRgba;         // elsewhere: what goes to UpdatePixels
};
