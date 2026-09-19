#pragma once

#include <stdint.h>
#include <vector>

// Encodes 8-bit RGBA pixels as GX CMPR and wraps them in a TPL.
//
// This exists because devkitPro's gxtexconv will not produce CMPR with alpha.
// CMPR is DXT1, which carries one bit of alpha -- a texel is either fully
// there or fully absent -- but gxtexconv always asks its compressor for the
// opaque variant, so a cutout comes back solid and its transparent areas
// arrive as black. There is no flag to change it.
//
// That matters beyond cutouts looking wrong: a material blending layers by
// texture alpha has every layer overwrite the one beneath it, so only the last
// texture is ever seen.
//
// Encoding here keeps masked textures at 4 bits a texel instead of pushing
// them to 16, and removes a tool from the path for the most common console
// format.
//
// Returns false if the size is unusable (zero, or not a multiple of 4).
bool EncodeCmprTpl(const uint8_t* rgba,
                   uint32_t width,
                   uint32_t height,
                   std::vector<uint8_t>& outTpl);
