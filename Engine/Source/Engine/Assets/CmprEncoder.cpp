#include "CmprEncoder.h"

#include <string.h>

// GX CMPR, which is DXT1 laid out the way Flipper wants it.
//
// The image is cut into 8x8 tiles, and each tile holds four 4x4 DXT1 blocks in
// reading order: top-left, top-right, bottom-left, bottom-right. A block is
// eight bytes -- two big-endian RGB565 endpoints, then one byte per row of
// four 2-bit indices with the leftmost texel in the high bits.
//
// The endpoint order is what carries alpha. With the first endpoint greater
// than the second the block has four opaque colours; with it less than or
// equal the block has three colours and index 3 means transparent. That is the
// whole of DXT1's alpha, and it is exactly enough for a mask.

namespace
{
    // A texel at or below this is treated as absent, above it as present.
    // Matching what the fixed-function hardware does with a 1-bit mask: there
    // is no partial coverage to represent, so the only question is which side
    // of the line a texel falls.
    const uint8_t kAlphaCutoff = 128;

    struct Colour
    {
        int32_t r, g, b;
    };

    uint16_t ToRgb565(const Colour& c)
    {
        return uint16_t(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
    }

    Colour FromRgb565(uint16_t v)
    {
        const int32_t r = (v >> 11) & 0x1F;
        const int32_t g = (v >> 5) & 0x3F;
        const int32_t b = v & 0x1F;

        // Replicate the high bits into the low ones so that full-scale values
        // come back as 255 rather than 248.
        Colour c;
        c.r = (r << 3) | (r >> 2);
        c.g = (g << 2) | (g >> 4);
        c.b = (b << 3) | (b >> 2);
        return c;
    }

    int32_t Distance(const Colour& a, const Colour& b)
    {
        const int32_t dr = a.r - b.r;
        const int32_t dg = a.g - b.g;
        const int32_t db = a.b - b.b;

        // Weighted toward green, which is where the eye has most of its
        // resolution. Cheap, and noticeably better than treating the channels
        // alike on skies and skin.
        return (dr * dr * 3) + (dg * dg * 6) + (db * db);
    }

    // Picks the two colours that sit furthest apart along the block's own
    // dominant direction.
    //
    // The obvious alternative -- the per-channel bounding box -- gives corners
    // that no texel actually occupies, which wastes both endpoints on a block
    // whose colours lie along a line. Projecting onto the box's diagonal and
    // taking the extremes keeps the endpoints on real colours.
    void ChooseEndpoints(const Colour* texels, int32_t count, Colour& outA, Colour& outB)
    {
        Colour lo = texels[0];
        Colour hi = texels[0];

        for (int32_t i = 1; i < count; ++i)
        {
            lo.r = texels[i].r < lo.r ? texels[i].r : lo.r;
            lo.g = texels[i].g < lo.g ? texels[i].g : lo.g;
            lo.b = texels[i].b < lo.b ? texels[i].b : lo.b;
            hi.r = texels[i].r > hi.r ? texels[i].r : hi.r;
            hi.g = texels[i].g > hi.g ? texels[i].g : hi.g;
            hi.b = texels[i].b > hi.b ? texels[i].b : hi.b;
        }

        const Colour axis = { hi.r - lo.r, hi.g - lo.g, hi.b - lo.b };

        int32_t minDot = 0x7FFFFFFF;
        int32_t maxDot = -0x7FFFFFFF;
        outA = texels[0];
        outB = texels[0];

        for (int32_t i = 0; i < count; ++i)
        {
            const int32_t dot = texels[i].r * axis.r + texels[i].g * axis.g + texels[i].b * axis.b;

            if (dot < minDot) { minDot = dot; outA = texels[i]; }
            if (dot > maxDot) { maxDot = dot; outB = texels[i]; }
        }
    }

    // Encodes one 4x4 block into eight bytes.
    void EncodeBlock(const uint8_t* rgba,
                     uint32_t width,
                     uint32_t height,
                     uint32_t blockX,
                     uint32_t blockY,
                     uint8_t* out)
    {
        Colour texels[16];
        bool present[16];
        Colour opaqueTexels[16];
        int32_t numOpaque = 0;
        bool anyAbsent = false;

        for (int32_t i = 0; i < 16; ++i)
        {
            // Clamp rather than wrap, so a block running past the edge of a
            // non-multiple-of-8 image repeats its edge instead of folding the
            // far side of the image into it.
            uint32_t x = blockX + (i & 3);
            uint32_t y = blockY + (i >> 2);
            x = x < width ? x : width - 1;
            y = y < height ? y : height - 1;

            const uint8_t* p = rgba + ((y * width) + x) * 4;

            texels[i].r = p[0];
            texels[i].g = p[1];
            texels[i].b = p[2];
            present[i] = (p[3] >= kAlphaCutoff);

            if (present[i])
            {
                opaqueTexels[numOpaque++] = texels[i];
            }
            else
            {
                anyAbsent = true;
            }
        }

        // Nothing visible: one flat transparent block. The endpoints must still
        // satisfy first <= second so that index 3 keeps its meaning.
        if (numOpaque == 0)
        {
            out[0] = 0; out[1] = 0;
            out[2] = 0; out[3] = 0;
            out[4] = 0xFF; out[5] = 0xFF; out[6] = 0xFF; out[7] = 0xFF;
            return;
        }

        Colour a, b;
        ChooseEndpoints(opaqueTexels, numOpaque, a, b);

        uint16_t c0 = ToRgb565(a);
        uint16_t c1 = ToRgb565(b);

        // Put the endpoints in the order that selects the mode this block
        // needs. A block with absent texels has to be in the three-colour mode
        // whether or not its colours would have preferred four.
        if (anyAbsent)
        {
            if (c0 > c1)
            {
                const uint16_t t = c0; c0 = c1; c1 = t;
            }
        }
        else if (c0 <= c1)
        {
            if (c0 == c1)
            {
                // A flat block. Either mode reproduces it, and the three-colour
                // one is safe because no texel will be given index 3.
            }
            else
            {
                const uint16_t t = c0; c0 = c1; c1 = t;
            }
        }

        const bool threeColour = (c0 <= c1);

        Colour palette[4];
        palette[0] = FromRgb565(c0);
        palette[1] = FromRgb565(c1);

        if (threeColour)
        {
            palette[2].r = (palette[0].r + palette[1].r) / 2;
            palette[2].g = (palette[0].g + palette[1].g) / 2;
            palette[2].b = (palette[0].b + palette[1].b) / 2;
            palette[3] = palette[2];            // never chosen; index 3 is transparent
        }
        else
        {
            palette[2].r = (2 * palette[0].r + palette[1].r) / 3;
            palette[2].g = (2 * palette[0].g + palette[1].g) / 3;
            palette[2].b = (2 * palette[0].b + palette[1].b) / 3;
            palette[3].r = (palette[0].r + 2 * palette[1].r) / 3;
            palette[3].g = (palette[0].g + 2 * palette[1].g) / 3;
            palette[3].b = (palette[0].b + 2 * palette[1].b) / 3;
        }

        const int32_t usable = threeColour ? 3 : 4;

        out[0] = uint8_t(c0 >> 8); out[1] = uint8_t(c0 & 0xFF);
        out[2] = uint8_t(c1 >> 8); out[3] = uint8_t(c1 & 0xFF);

        for (int32_t row = 0; row < 4; ++row)
        {
            uint8_t packed = 0;

            for (int32_t col = 0; col < 4; ++col)
            {
                const int32_t i = (row * 4) + col;
                int32_t index = 3;

                if (present[i])
                {
                    int32_t best = 0;
                    int32_t bestDistance = Distance(texels[i], palette[0]);

                    for (int32_t p = 1; p < usable; ++p)
                    {
                        const int32_t d = Distance(texels[i], palette[p]);

                        if (d < bestDistance)
                        {
                            bestDistance = d;
                            best = p;
                        }
                    }

                    index = best;
                }

                // Leftmost texel in the high bits.
                packed |= uint8_t(index << (6 - (2 * col)));
            }

            out[4 + row] = packed;
        }
    }

    void WriteBigUint32(std::vector<uint8_t>& out, uint32_t offset, uint32_t value)
    {
        out[offset + 0] = uint8_t(value >> 24);
        out[offset + 1] = uint8_t(value >> 16);
        out[offset + 2] = uint8_t(value >> 8);
        out[offset + 3] = uint8_t(value);
    }

    void WriteBigUint16(std::vector<uint8_t>& out, uint32_t offset, uint16_t value)
    {
        out[offset + 0] = uint8_t(value >> 8);
        out[offset + 1] = uint8_t(value);
    }
}

bool EncodeCmprTpl(const uint8_t* rgba,
                   uint32_t width,
                   uint32_t height,
                   std::vector<uint8_t>& outTpl)
{
    if (rgba == nullptr || width == 0 || height == 0)
        return false;

    // DXT1 works in 4x4 blocks and GX groups those into 8x8 tiles. Anything
    // else would need padding the image out, which the cook does not do.
    if ((width % 4) != 0 || (height % 4) != 0)
        return false;

    // A TPL header is 64 bytes: magic, one image table entry, and one texture
    // header, with the pixels starting immediately after.
    const uint32_t kHeaderSize = 64;
    const uint32_t pixelBytes = (width * height) / 2;      // 4 bits a texel

    outTpl.assign(kHeaderSize + pixelBytes, 0);

    WriteBigUint32(outTpl, 0x00, 0x0020AF30);   // magic
    WriteBigUint32(outTpl, 0x04, 1);            // one texture
    WriteBigUint32(outTpl, 0x08, 0x0C);         // image table follows

    WriteBigUint32(outTpl, 0x0C, 0x14);         // texture header offset
    WriteBigUint32(outTpl, 0x10, 0);            // no palette

    WriteBigUint16(outTpl, 0x14, uint16_t(height));
    WriteBigUint16(outTpl, 0x16, uint16_t(width));
    WriteBigUint32(outTpl, 0x18, 14);           // CMPR
    WriteBigUint32(outTpl, 0x1C, kHeaderSize);  // where the pixels start
    WriteBigUint32(outTpl, 0x20, 1);            // wrap s: repeat
    WriteBigUint32(outTpl, 0x24, 1);            // wrap t: repeat
    WriteBigUint32(outTpl, 0x28, 1);            // min filter: linear
    WriteBigUint32(outTpl, 0x2C, 1);            // mag filter: linear
    WriteBigUint32(outTpl, 0x30, 0);            // lod bias
    // The four bytes after that -- edge lod, min lod, max lod, unpacked -- stay
    // zero. The engine sets wrap and filter on the texture object at load time
    // from the asset's own properties anyway; these are here so the file is
    // well formed, not because anything reads them.

    uint8_t* out = outTpl.data() + kHeaderSize;

    // 8x8 tiles, four 4x4 blocks each, in reading order.
    for (uint32_t tileY = 0; tileY < height; tileY += 8)
    {
        for (uint32_t tileX = 0; tileX < width; tileX += 8)
        {
            for (uint32_t sub = 0; sub < 4; ++sub)
            {
                const uint32_t blockX = tileX + ((sub & 1) * 4);
                const uint32_t blockY = tileY + ((sub >> 1) * 4);

                EncodeBlock(rgba, width, height, blockX, blockY, out);
                out += 8;
            }
        }
    }

    return true;
}
