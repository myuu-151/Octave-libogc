#pragma once

#include <cstdint>

// Destination planes for JpegYuvDecoder. Cb and Cr are half size in each dimension.
struct JpegYuvPlanes
{
    uint8_t* mY = nullptr;
    uint8_t* mCb = nullptr;
    uint8_t* mCr = nullptr;

    // false: rows top to bottom. true: GX_TF_I8 texel layout (8 wide x 4 tall
    // blocks), so each plane can be used directly as a GameCube texture.
    bool mGxLayout = false;
};

// A small, fast decoder for the JPEG frames the video cook produces: baseline
// Huffman, 8-bit, three components with 4:2:0 subsampling, and dimensions that are
// multiples of 16. Rather than converting to RGB it writes the Y, Cb and Cr planes
// out directly, leaving color conversion to the GPU. Allocates nothing per frame.
class JpegYuvDecoder
{
public:

    JpegYuvDecoder();

    bool Decode(const uint8_t* data, uint32_t size, uint32_t width, uint32_t height, const JpegYuvPlanes& out);

private:

    struct Huffman
    {
        uint16_t mFast[512];        // 9-bit prefix -> symbol index, 0xffff = longer code
        uint16_t mCode[256];
        uint8_t mValues[256];
        uint8_t mSize[257];
        uint32_t mMaxCode[18];
        int32_t mDelta[17];
        bool mValid = false;
    };

    struct Component
    {
        uint8_t mId = 0;
        uint8_t mQuant = 0;
        uint8_t mDcTable = 0;
        uint8_t mAcTable = 0;
        int32_t mPrediction = 0;
    };

    class BitReader;

    bool ParseHeaders(const uint8_t* data, uint32_t size, const uint8_t*& outScan);
    bool DecodeBlock(BitReader& reader, Component& comp, uint8_t* outPixels);

    static bool BuildHuffman(Huffman& huffman, const uint8_t* counts);

    uint16_t mQuant[4][64];
    bool mQuantValid[4];
    Huffman mDc[4];
    Huffman mAc[4];
    Component mComps[3];
    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
    uint32_t mRestartInterval = 0;
};
