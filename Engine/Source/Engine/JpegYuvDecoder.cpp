#include "JpegYuvDecoder.h"

#include <cstring>

// Coefficient order within an 8x8 block.
static const uint8_t kZigZag[64] =
{
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

static inline uint32_t ReadU16BE(const uint8_t* p)
{
    return (uint32_t(p[0]) << 8) | uint32_t(p[1]);
}

static inline uint8_t Clamp8(int32_t x)
{
    if (uint32_t(x) > 255)
    {
        return (x < 0) ? 0 : 255;
    }
    return uint8_t(x);
}

// Reads entropy-coded bits MSB first, undoing 0xFF00 byte stuffing and stopping
// (feeding zeros) at a marker.
class JpegYuvDecoder::BitReader
{
public:

    BitReader(const uint8_t* data, const uint8_t* end) :
        mData(data),
        mEnd(end)
    {
    }

    void Fill()
    {
        while (mBits <= 24)
        {
            uint32_t byte = 0;

            if (!mMarker && mData < mEnd)
            {
                byte = *mData++;

                if (byte == 0xff)
                {
                    const uint8_t next = (mData < mEnd) ? *mData : 0;
                    if (next == 0x00)
                    {
                        ++mData;
                    }
                    else
                    {
                        // Leave the marker for Restart().
                        mMarker = true;
                        --mData;
                        byte = 0;
                    }
                }
            }

            mBuffer |= byte << (24 - mBits);
            mBits += 8;
        }
    }

    // Skips to just past the next RSTn marker and resets the bit buffer.
    bool Restart()
    {
        mBuffer = 0;
        mBits = 0;
        mMarker = false;

        while (mData + 1 < mEnd)
        {
            if (mData[0] == 0xff && mData[1] >= 0xd0 && mData[1] <= 0xd7)
            {
                mData += 2;
                return true;
            }
            ++mData;
        }

        return false;
    }

    int32_t DecodeHuffman(const Huffman& huffman)
    {
        if (mBits < 16)
        {
            Fill();
        }

        uint32_t k = huffman.mFast[mBuffer >> 23];
        if (k != 0xffff)
        {
            const int32_t size = huffman.mSize[k];
            mBuffer <<= size;
            mBits -= size;
            return huffman.mValues[k];
        }

        // Codes longer than 9 bits.
        const uint32_t top = mBuffer >> 16;
        for (k = 10; k < 17; ++k)
        {
            if (top < huffman.mMaxCode[k])
            {
                break;
            }
        }

        if (k == 17)
        {
            return -1;
        }

        const int32_t index = int32_t(mBuffer >> (32 - k)) + huffman.mDelta[k];
        if (index < 0 || index >= 256)
        {
            return -1;
        }

        mBuffer <<= k;
        mBits -= int32_t(k);
        return huffman.mValues[index];
    }

    // Reads an n-bit coefficient and sign-extends it (JPEG's "EXTEND").
    int32_t ReceiveExtend(int32_t n)
    {
        if (mBits < n)
        {
            Fill();
        }

        int32_t value = int32_t(mBuffer >> (32 - n));
        mBuffer <<= n;
        mBits -= n;

        if (value < (1 << (n - 1)))
        {
            value -= (1 << n) - 1;
        }

        return value;
    }

private:

    const uint8_t* mData;
    const uint8_t* mEnd;
    uint32_t mBuffer = 0;
    int32_t mBits = 0;
    bool mMarker = false;
};

// Integer inverse DCT (the IJG "islow" algorithm, as used by stb_image). Input is
// dequantized coefficients in natural order; output is 8x8 samples, 0-255.
#define IDCT_F2F(x) int32_t((x) * 4096 + 0.5f)
#define IDCT_FSH(x) ((x) * 4096)
#define IDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7)       \
    int32_t t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3; \
    p2 = s2;                                           \
    p3 = s6;                                           \
    p1 = (p2 + p3) * IDCT_F2F(0.5411961f);             \
    t2 = p1 + p2 * IDCT_F2F(0.765366865f);             \
    t3 = p1 + p3 * IDCT_F2F(-1.847759065f);            \
    p2 = s0;                                           \
    p3 = s4;                                           \
    t0 = IDCT_FSH(p2 + p3);                            \
    t1 = IDCT_FSH(p2 - p3);                            \
    x0 = t0 + t3;                                      \
    x3 = t0 - t3;                                      \
    x1 = t1 + t2;                                      \
    x2 = t1 - t2;                                      \
    t0 = s7;                                           \
    t1 = s5;                                           \
    t2 = s3;                                           \
    t3 = s1;                                           \
    p3 = t0 + t2;                                      \
    p4 = t1 + t3;                                      \
    p1 = t0 + t3;                                      \
    p2 = t1 + t2;                                      \
    p5 = (p3 + p4) * IDCT_F2F(1.175875602f);           \
    t0 = t0 * IDCT_F2F(0.298631336f);                  \
    t1 = t1 * IDCT_F2F(2.053119869f);                  \
    t2 = t2 * IDCT_F2F(3.072711026f);                  \
    t3 = t3 * IDCT_F2F(1.501321110f);                  \
    p1 = p5 + p1 * IDCT_F2F(-0.899976223f);            \
    p2 = p5 + p2 * IDCT_F2F(-2.562915447f);            \
    p3 = p3 * IDCT_F2F(-1.961570560f);                 \
    p4 = p4 * IDCT_F2F(-0.390180644f);                 \
    t3 += p1 + p4;                                     \
    t2 += p2 + p3;                                     \
    t1 += p2 + p4;                                     \
    t0 += p1 + p3;

static void IdctBlock(uint8_t* out, const int16_t* data)
{
    int32_t values[64];

    // Columns
    for (int32_t i = 0; i < 8; ++i)
    {
        const int16_t* d = data + i;
        int32_t* v = values + i;

        if (d[8] == 0 && d[16] == 0 && d[24] == 0 && d[32] == 0 &&
            d[40] == 0 && d[48] == 0 && d[56] == 0)
        {
            const int32_t dc = d[0] * 4;
            v[0] = v[8] = v[16] = v[24] = v[32] = v[40] = v[48] = v[56] = dc;
        }
        else
        {
            IDCT_1D(d[0], d[8], d[16], d[24], d[32], d[40], d[48], d[56])
            // Scaled up by 1<<12; bring down, keeping 2 extra bits of precision.
            x0 += 512; x1 += 512; x2 += 512; x3 += 512;
            v[0]  = (x0 + t3) >> 10;
            v[56] = (x0 - t3) >> 10;
            v[8]  = (x1 + t2) >> 10;
            v[48] = (x1 - t2) >> 10;
            v[16] = (x2 + t1) >> 10;
            v[40] = (x2 - t1) >> 10;
            v[24] = (x3 + t0) >> 10;
            v[32] = (x3 - t0) >> 10;
        }
    }

    // Rows
    for (int32_t i = 0; i < 8; ++i)
    {
        const int32_t* v = values + i * 8;
        uint8_t* o = out + i * 8;

        IDCT_1D(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7])
        // Remove the remaining 1<<17 scale with rounding, and add the 128 level shift.
        x0 += 65536 + (128 << 17);
        x1 += 65536 + (128 << 17);
        x2 += 65536 + (128 << 17);
        x3 += 65536 + (128 << 17);
        o[0] = Clamp8((x0 + t3) >> 17);
        o[7] = Clamp8((x0 - t3) >> 17);
        o[1] = Clamp8((x1 + t2) >> 17);
        o[6] = Clamp8((x1 - t2) >> 17);
        o[2] = Clamp8((x2 + t1) >> 17);
        o[5] = Clamp8((x2 - t1) >> 17);
        o[3] = Clamp8((x3 + t0) >> 17);
        o[4] = Clamp8((x3 - t0) >> 17);
    }
}

#undef IDCT_1D
#undef IDCT_FSH
#undef IDCT_F2F

// Writes an 8x8 block of samples at block coordinates (bx, by) of a plane.
static inline void PutBlock(const uint8_t* pixels, uint8_t* plane, uint32_t planeWidth, uint32_t bx, uint32_t by, bool gxLayout)
{
    if (gxLayout)
    {
        // GX_TF_I8 groups texels into 8x4 blocks, so one JPEG block spans two block
        // rows, each 32 contiguous bytes.
        const uint32_t blocksWide = planeWidth / 8;
        memcpy(plane + ((by * 2) * blocksWide + bx) * 32, pixels, 32);
        memcpy(plane + ((by * 2 + 1) * blocksWide + bx) * 32, pixels + 32, 32);
    }
    else
    {
        uint8_t* row = plane + (by * 8) * planeWidth + bx * 8;
        for (int32_t y = 0; y < 8; ++y)
        {
            memcpy(row, pixels + y * 8, 8);
            row += planeWidth;
        }
    }
}

JpegYuvDecoder::JpegYuvDecoder()
{
    memset(mQuant, 0, sizeof(mQuant));
    memset(mQuantValid, 0, sizeof(mQuantValid));
}

bool JpegYuvDecoder::BuildHuffman(Huffman& huffman, const uint8_t* counts)
{
    int32_t k = 0;
    for (int32_t i = 0; i < 16; ++i)
    {
        for (int32_t j = 0; j < counts[i]; ++j)
        {
            huffman.mSize[k++] = uint8_t(i + 1);
        }
    }
    huffman.mSize[k] = 0;

    int32_t code = 0;
    k = 0;
    for (int32_t j = 1; j <= 16; ++j)
    {
        huffman.mDelta[j] = k - code;

        if (huffman.mSize[k] == j)
        {
            while (huffman.mSize[k] == j)
            {
                huffman.mCode[k++] = uint16_t(code++);
            }

            if (code - 1 >= (1 << j))
            {
                return false;
            }
        }

        huffman.mMaxCode[j] = uint32_t(code) << (16 - j);
        code <<= 1;
    }
    huffman.mMaxCode[17] = 0xffffffff;

    for (int32_t i = 0; i < 512; ++i)
    {
        huffman.mFast[i] = 0xffff;
    }

    for (int32_t i = 0; i < k; ++i)
    {
        const int32_t size = huffman.mSize[i];
        if (size <= 9)
        {
            const int32_t first = huffman.mCode[i] << (9 - size);
            const int32_t count = 1 << (9 - size);
            for (int32_t j = 0; j < count; ++j)
            {
                huffman.mFast[first + j] = uint16_t(i);
            }
        }
    }

    return true;
}

bool JpegYuvDecoder::ParseHeaders(const uint8_t* data, uint32_t size, const uint8_t*& outScan)
{
    if (size < 4 || data[0] != 0xff || data[1] != 0xd8)
    {
        return false;
    }

    const uint8_t* p = data + 2;
    const uint8_t* end = data + size;
    bool haveFrame = false;
    mRestartInterval = 0;

    while (p < end)
    {
        if (*p != 0xff)
        {
            return false;
        }

        while (p < end && *p == 0xff)
        {
            ++p;
        }

        if (p >= end)
        {
            return false;
        }

        const uint8_t marker = *p++;

        if (marker == 0xd8 || marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7))
        {
            continue;
        }

        if (marker == 0xd9)
        {
            return false;   // End of image before any scan.
        }

        // Only baseline / extended sequential Huffman frames are supported.
        if (marker >= 0xc2 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc)
        {
            return false;
        }

        if (p + 2 > end)
        {
            return false;
        }

        const uint32_t length = ReadU16BE(p);
        if (length < 2 || p + length > end)
        {
            return false;
        }

        const uint8_t* seg = p + 2;
        const uint8_t* segEnd = p + length;

        switch (marker)
        {
        case 0xdb: // DQT
        {
            while (seg < segEnd)
            {
                const uint8_t precision = seg[0] >> 4;
                const uint8_t table = seg[0] & 15;
                ++seg;

                const uint32_t bytes = precision ? 128 : 64;
                if (table > 3 || seg + bytes > segEnd)
                {
                    return false;
                }

                for (uint32_t i = 0; i < 64; ++i)
                {
                    mQuant[table][i] = uint16_t(precision ? ReadU16BE(seg + i * 2) : seg[i]);
                }

                mQuantValid[table] = true;
                seg += bytes;
            }
            break;
        }

        case 0xc4: // DHT
        {
            while (seg < segEnd)
            {
                if (seg + 17 > segEnd)
                {
                    return false;
                }

                const uint8_t tableClass = seg[0] >> 4;
                const uint8_t table = seg[0] & 15;
                if (tableClass > 1 || table > 3)
                {
                    return false;
                }

                uint32_t total = 0;
                for (uint32_t i = 0; i < 16; ++i)
                {
                    total += seg[1 + i];
                }

                if (total > 256 || seg + 17 + total > segEnd)
                {
                    return false;
                }

                Huffman& huffman = tableClass ? mAc[table] : mDc[table];
                huffman.mValid = BuildHuffman(huffman, seg + 1);
                if (!huffman.mValid)
                {
                    return false;
                }

                memcpy(huffman.mValues, seg + 17, total);
                seg += 17 + total;
            }
            break;
        }

        case 0xc0: // SOF0 baseline
        case 0xc1: // SOF1 extended sequential
        {
            if (length < 17 || seg[0] != 8 || seg[5] != 3)
            {
                return false;
            }

            mHeight = ReadU16BE(seg + 1);
            mWidth = ReadU16BE(seg + 3);

            for (uint32_t c = 0; c < 3; ++c)
            {
                const uint8_t sampling = seg[7 + c * 3];
                const uint8_t quant = seg[8 + c * 3];

                // 4:2:0 only: luma 2x2, chroma 1x1.
                if (sampling != ((c == 0) ? 0x22 : 0x11) || quant > 3)
                {
                    return false;
                }

                mComps[c].mId = seg[6 + c * 3];
                mComps[c].mQuant = quant;
            }

            haveFrame = true;
            break;
        }

        case 0xdd: // DRI
        {
            if (length != 4)
            {
                return false;
            }

            mRestartInterval = ReadU16BE(seg);
            break;
        }

        case 0xda: // SOS
        {
            if (!haveFrame || seg[0] != 3 || length != 12)
            {
                return false;
            }

            // A single interleaved scan with the components in frame order.
            for (uint32_t c = 0; c < 3; ++c)
            {
                const uint8_t id = seg[1 + c * 2];
                const uint8_t tables = seg[2 + c * 2];
                Component& comp = mComps[c];

                comp.mDcTable = tables >> 4;
                comp.mAcTable = tables & 15;

                if (id != comp.mId ||
                    comp.mDcTable > 3 ||
                    comp.mAcTable > 3 ||
                    !mDc[comp.mDcTable].mValid ||
                    !mAc[comp.mAcTable].mValid ||
                    !mQuantValid[comp.mQuant])
                {
                    return false;
                }
            }

            outScan = segEnd;
            return true;
        }

        default:
            break;  // APPn, COM, ...
        }

        p = segEnd;
    }

    return false;
}

bool JpegYuvDecoder::DecodeBlock(BitReader& reader, Component& comp, uint8_t* outPixels)
{
    const uint16_t* quant = mQuant[comp.mQuant];

    const int32_t dcBits = reader.DecodeHuffman(mDc[comp.mDcTable]);
    if (dcBits < 0 || dcBits > 11)
    {
        return false;
    }

    comp.mPrediction += (dcBits != 0) ? reader.ReceiveExtend(dcBits) : 0;

    int16_t coeffs[64];
    memset(coeffs, 0, sizeof(coeffs));
    coeffs[0] = int16_t(comp.mPrediction * quant[0]);

    const Huffman& ac = mAc[comp.mAcTable];
    bool hasAc = false;

    for (int32_t k = 1; k < 64; )
    {
        const int32_t rs = reader.DecodeHuffman(ac);
        if (rs < 0)
        {
            return false;
        }

        const int32_t bits = rs & 15;
        const int32_t run = rs >> 4;

        if (bits == 0)
        {
            if (run != 15)
            {
                break;  // End of block
            }
            k += 16;
            continue;
        }

        k += run;
        if (k > 63)
        {
            return false;
        }

        coeffs[kZigZag[k]] = int16_t(reader.ReceiveExtend(bits) * quant[k]);
        hasAc = true;
        ++k;
    }

    if (hasAc)
    {
        IdctBlock(outPixels, coeffs);
    }
    else
    {
        // DC only (flat block): what the IDCT would produce, without running it.
        memset(outPixels, Clamp8((16384 * coeffs[0] + 65536 + (128 << 17)) >> 17), 64);
    }

    return true;
}

bool JpegYuvDecoder::Decode(const uint8_t* data, uint32_t size, uint32_t width, uint32_t height, const JpegYuvPlanes& out)
{
    const uint8_t* scan = nullptr;
    if (!ParseHeaders(data, size, scan))
    {
        return false;
    }

    if (mWidth != width || mHeight != height ||
        (width % 16) != 0 || (height % 16) != 0 ||
        out.mY == nullptr || out.mCb == nullptr || out.mCr == nullptr)
    {
        return false;
    }

    const uint32_t mcusWide = width / 16;
    const uint32_t mcusHigh = height / 16;
    const uint32_t chromaWidth = width / 2;

    BitReader reader(scan, data + size);
    uint8_t pixels[64];
    uint32_t mcuIndex = 0;

    for (uint32_t i = 0; i < 3; ++i)
    {
        mComps[i].mPrediction = 0;
    }

    for (uint32_t my = 0; my < mcusHigh; ++my)
    {
        for (uint32_t mx = 0; mx < mcusWide; ++mx, ++mcuIndex)
        {
            if (mRestartInterval != 0 && mcuIndex != 0 && (mcuIndex % mRestartInterval) == 0)
            {
                if (!reader.Restart())
                {
                    return false;
                }

                for (uint32_t i = 0; i < 3; ++i)
                {
                    mComps[i].mPrediction = 0;
                }
            }

            for (uint32_t v = 0; v < 2; ++v)
            {
                for (uint32_t h = 0; h < 2; ++h)
                {
                    if (!DecodeBlock(reader, mComps[0], pixels))
                    {
                        return false;
                    }
                    PutBlock(pixels, out.mY, width, mx * 2 + h, my * 2 + v, out.mGxLayout);
                }
            }

            if (!DecodeBlock(reader, mComps[1], pixels))
            {
                return false;
            }
            PutBlock(pixels, out.mCb, chromaWidth, mx, my, out.mGxLayout);

            if (!DecodeBlock(reader, mComps[2], pixels))
            {
                return false;
            }
            PutBlock(pixels, out.mCr, chromaWidth, mx, my, out.mGxLayout);
        }
    }

    return true;
}
