#include "Stream.h"
#include "Asset.h"
#include "AssetRef.h"
#include "AssetManager.h"
#include "Log.h"
#include "System/System.h"
#include "Maths.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <unordered_set>

#define ACQUIRE_FILE_DIRECTLY 1

// Track UUIDs we've already warned about to reduce log spam
static std::unordered_set<uint64_t> sWarnedUuids;

#define MAX_FILE_SIZE (1024 * 1024 * 1024)
#define MAX_STRING_SIZE (1024 * 16)

Stream::Stream() :
    mData(nullptr),
    mSize(0),
    mCapacity(0),
    mPos(0),
    mAssetVersion(ASSET_VERSION_CURRENT),
    mAsyncRequest(nullptr),
    mExternal(false)
{

}

// TODO: Avoid const cast?
Stream::Stream(const char* externalData, uint32_t externalSize) :
    mData(const_cast<char*>(externalData)),
    mSize(externalSize),
    mCapacity(externalSize),
    mPos(0),
    mAssetVersion(ASSET_VERSION_CURRENT),
    mAsyncRequest(nullptr),
    mExternal(true)
{

}

Stream::~Stream()
{
    Reset();
}

char* Stream::GetData()
{
    return mData;
}

uint32_t Stream::GetSize() const
{
    return mSize;
}

uint32_t Stream::GetPos()
{
    return mPos;
}

void Stream::SetPos(uint32_t pos)
{
    mPos = pos;

    if (pos > mSize)
    {
        mPos = mSize;
    }
}

void Stream::Reset()
{
    if (!mExternal && mData != nullptr)
    {
        // We need to use free() here because SYS_AcquireFileData() calls malloc().
        // And we use that allocated data directly when ACQUIRE_FILE_DIRECTLY is enabled.
        free(mData);
        mData = nullptr;
    }

    mData = nullptr;
    mSize = 0;
    mCapacity = 0;
    mPos = 0;
    mAssetVersion = ASSET_VERSION_CURRENT;
    mAsyncRequest = nullptr;
    mExternal = false;
    mWindowed = false;
    mReadFailed = false;
    mWindowStart = 0;
    mWindowLen = 0;
    mWindowPath.clear();
}

void Stream::Resize(uint32_t size)
{
    OCT_ASSERT(!mExternal);
    Grow(size);
}

void Stream::SetExternalData(const char* externalData, uint32_t externalSize)
{
    mData = const_cast<char*>(externalData);
    mSize = externalSize;
    mCapacity = externalSize;
    mPos = 0;
    mExternal = true;
}

void Stream::SetAssetVersion(uint32_t version)
{
    mAssetVersion = version;
}

uint32_t Stream::GetAssetVersion() const
{
    return mAssetVersion;
}

// WINDOWED READING. A GameCube streaming its stages in and out loaded each asset by reading the
// WHOLE FILE into one buffer and parsing it: a 576 KB pipe piece needed a free block of 576 KB
// before it needed anything for itself, and a few stages in there was no such block left with
// megabytes free. Here the file is read a window at a time as the parser moves through it; a read
// bigger than the window (a texture's texels) goes straight from the file into its destination.
// The largest block a load needs is then the asset's own. Only for readers that go front to back.
static const uint32_t kStreamWindow = 32 * 1024;

bool Stream::ReadFileWindowed(const char* path, bool isAsset, uint32_t fileSize)
{
    OCT_ASSERT(!mExternal);
    Reset();

    mData = (char*)malloc(kStreamWindow);
    if (mData == nullptr)
    {
        LogError("Stream: no room for a %u byte window reading %s.", kStreamWindow, path);
        return false;
    }

    mCapacity = kStreamWindow;
    mSize = fileSize;
    mPos = 0;
    mWindowed = true;
    mWindowAsset = isAsset;
    mWindowPath = path;
    return FillWindow(0);
}

bool Stream::FillWindow(uint32_t pos)
{
    uint32_t n = (mSize - pos < kStreamWindow) ? (mSize - pos) : kStreamWindow;
    mWindowStart = pos;
    mWindowLen = 0;

    if (n == 0 || !SYS_ReadFileRange(mWindowPath.c_str(), mWindowAsset, pos, n, mData))
    {
        LogError("Stream: reading %s at %u failed.", mWindowPath.c_str(), pos);
        mReadFailed = true;
        return false;
    }

    mWindowLen = n;
    return true;
}

void Stream::ReadWindowed(void* dst, uint32_t length)
{
    uint8_t* out = (uint8_t*)dst;

    while (length > 0)
    {
        if (mPos >= mWindowStart && mPos < mWindowStart + mWindowLen)
        {
            // from the window
            uint32_t n = mWindowStart + mWindowLen - mPos;
            n = (length < n) ? length : n;
            memcpy(out, &mData[mPos - mWindowStart], n);
            out += n;
            mPos += n;
            length -= n;
        }
        else if (length >= kStreamWindow)
        {
            // a big read: straight from the file into its destination, a window's worth at a time
            if (mReadFailed || !SYS_ReadFileRange(mWindowPath.c_str(), mWindowAsset, mPos, kStreamWindow, (char*)out))
            {
                mReadFailed = true;
                break;
            }
            out += kStreamWindow;
            mPos += kStreamWindow;
            length -= kStreamWindow;
        }
        else if (mReadFailed || mPos >= mSize || !FillWindow(mPos))
        {
            break;
        }
    }

    if (length > 0)
    {
        // Past the end or a failed read: zeros, and the stream says so (HasReadFailed).
        memset(out, 0, length);
        mPos += length;
        mReadFailed = true;
    }
}

bool Stream::ReadFile(const char* path, bool isAsset, int32_t maxSize)
{
    OCT_ASSERT(!mExternal);
    if (mExternal)
    {
        LogError("Cannot ReadFile() using an external Stream");
        return false;
    }

#if ACQUIRE_FILE_DIRECTLY
    // This method avoids the extra allocation and copy.
    if (mData != nullptr)
    {
        free(mData);
        mData = nullptr;
        mSize = 0;
        mCapacity = 0;
    }
    SYS_AcquireFileData(path, isAsset, maxSize, mData, mSize);
    mCapacity = mSize;
    mPos = 0;

    if (mData == nullptr)
    {
        LogError("Stream failed to read file: %s", path);
        return false;
    }
#else
    char* fileData = nullptr;
    uint32_t fileSize = 0;
    SYS_AcquireFileData(path, isAsset, maxSize, fileData, fileSize);

    if (fileData != nullptr)
    {
        OCT_ASSERT(fileSize <= MAX_FILE_SIZE);
        Reserve(fileSize);

        // Reserve leaves the stream alone if it could not allocate, so the room for this copy has
        // to be confirmed rather than assumed.
        if (mData == nullptr || mCapacity < fileSize)
        {
            LogError("Stream: no room for %u bytes reading %s.", fileSize, path);
            SYS_ReleaseFileData(fileData);
            return false;
        }

        memcpy(mData, fileData, fileSize);

        SYS_ReleaseFileData(fileData);
        fileData = nullptr;

        mSize = fileSize;
        mPos = 0;
    }
    else
    {
        LogError("Stream failed to read file: %s", path);
        return false;
    }
#endif

    return true;
}

bool Stream::WriteFile(const char* path)
{
    bool success = false;
    FILE* file = fopen(path, "wb");

    if (file != nullptr)
    {
        fwrite(mData, mSize, 1, file);

        fclose(file);
        file = nullptr;
        success = true;
    }
    else
    {
        LogWarning("Failed to write file. fopen failed");
    }

    return success;
}

void Stream::SetAsyncRequest(AsyncLoadRequest* request)
{
    mAsyncRequest = request;
}

void Stream::ReadAsset(AssetRef& asset)
{
    // Check asset version to determine format
    // Version < 12: Old format (just string, no format byte)
    // Version >= 12: New format (format byte + data)

    if (mAssetVersion < ASSET_VERSION_UUID_SUPPORT)
    {
        // Legacy format: name-based reference (no format byte)
        std::string assetName;
        ReadString(assetName);

        if (assetName == "")
        {
            asset = nullptr;
        }
        else if (mAsyncRequest != nullptr)
        {
            Asset* reqAsset = FetchAsset(assetName);
            if (reqAsset != nullptr)
            {
                asset = reqAsset;
            }
            else
            {
                AssetStub* stub = AssetManager::Get()->GetAssetStub(assetName);
                if (stub != nullptr)
                {
                    AsyncLoadAsset(assetName, &asset);
                    bool hasDependency = false;
                    for (uint32_t i = 0; i < mAsyncRequest->mDependentAssets.size(); ++i)
                    {
                        if (mAsyncRequest->mDependentAssets[i] == stub)
                        {
                            hasDependency = true;
                            break;
                        }
                    }
                    if (!hasDependency)
                    {
                        mAsyncRequest->mDependentAssets.push_back(stub);
                    }
                }
                else
                {
                    LogWarning("Could not find asset %s", assetName.c_str());
                }
            }
        }
        else
        {
            asset = LoadAsset(assetName);
        }
    }
    else
    {
        // New format: format byte + data
        uint8_t format = ReadUint8();

        if (format == 0)
        {
            // Name-based reference (legacy format in new container)
            std::string assetName;
            ReadString(assetName);

            if (assetName == "")
            {
                asset = nullptr;
            }
            else if (mAsyncRequest != nullptr)
            {
                Asset* reqAsset = FetchAsset(assetName);
                if (reqAsset != nullptr)
                {
                    asset = reqAsset;
                }
                else
                {
                    AssetStub* stub = AssetManager::Get()->GetAssetStub(assetName);
                    if (stub != nullptr)
                    {
                        AsyncLoadAsset(assetName, &asset);
                        bool hasDependency = false;
                        for (uint32_t i = 0; i < mAsyncRequest->mDependentAssets.size(); ++i)
                        {
                            if (mAsyncRequest->mDependentAssets[i] == stub)
                            {
                                hasDependency = true;
                                break;
                            }
                        }
                        if (!hasDependency)
                        {
                            mAsyncRequest->mDependentAssets.push_back(stub);
                        }
                    }
                    else
                    {
                        LogWarning("Could not find asset %s", assetName.c_str());
                    }
                }
            }
            else
            {
                asset = LoadAsset(assetName);
            }
        }
        else if (format == 1)
        {
            // UUID-based reference
            uint64_t uuid = ReadUint64();

            // Version 13+ includes name for fallback lookup
            std::string assetName;
            if (mAssetVersion >= ASSET_VERSION_UUID_WITH_NAME_FALLBACK)
            {
                ReadString(assetName);
            }

            if (uuid == 0)
            {
                asset = nullptr;
            }
            else if (mAsyncRequest != nullptr)
            {
                Asset* reqAsset = FetchAssetByUuid(uuid);
                if (reqAsset != nullptr)
                {
                    asset = reqAsset;
                }
                else
                {
                    AssetStub* stub = AssetManager::Get()->GetAssetStubByUuid(uuid);
                    if (stub == nullptr && !assetName.empty())
                    {
                        // Fallback to name-based lookup if UUID not found (version 13+)
                        stub = AssetManager::Get()->GetAssetStub(assetName);
                        if (stub != nullptr && sWarnedUuids.find(uuid) == sWarnedUuids.end())
                        {
                            sWarnedUuids.insert(uuid);
                            LogWarning("Asset UUID 0x%llx not found, falling back to name: %s", (unsigned long long)uuid, assetName.c_str());
                        }
                    }
                    if (stub != nullptr)
                    {
                        if (!assetName.empty())
                        {
                            AsyncLoadAsset(assetName, &asset);
                        }
                        else
                        {
                            AsyncLoadAssetByUuid(uuid, &asset);
                        }
                        bool hasDependency = false;
                        for (uint32_t i = 0; i < mAsyncRequest->mDependentAssets.size(); ++i)
                        {
                            if (mAsyncRequest->mDependentAssets[i] == stub)
                            {
                                hasDependency = true;
                                break;
                            }
                        }
                        if (!hasDependency)
                        {
                            mAsyncRequest->mDependentAssets.push_back(stub);
                        }
                    }
                    else if (sWarnedUuids.find(uuid) == sWarnedUuids.end())
                    {
                        sWarnedUuids.insert(uuid);
                        LogWarning("Could not find asset with UUID 0x%llx%s", (unsigned long long)uuid,
                            assetName.empty() ? "" : (std::string(" or name '") + assetName + "'").c_str());
                    }
                }
            }
            else
            {
                asset = LoadAssetByUuid(uuid);
                if (asset == nullptr && !assetName.empty())
                {
                    // Fallback to name-based lookup if UUID not found (version 13+)
                    asset = LoadAsset(assetName);

                    // Only warn once per UUID to reduce log spam
                    if (sWarnedUuids.find(uuid) == sWarnedUuids.end())
                    {
                        sWarnedUuids.insert(uuid);
                        LogWarning("Asset UUID 0x%llx not found, falling back to name: %s", (unsigned long long)uuid, assetName.c_str());
                    }
                }
                else if (asset == nullptr)
                {
                    // Only warn once per UUID
                    if (sWarnedUuids.find(uuid) == sWarnedUuids.end())
                    {
                        sWarnedUuids.insert(uuid);
                        LogWarning("Could not find asset with UUID 0x%llx", (unsigned long long)uuid);
                    }
                }
            }
        }
        else
        {
            LogError("Unknown asset reference format: %d", format);
            asset = nullptr;
        }
    }
}

void Stream::WriteAsset(const AssetRef& asset)
{
    Asset* assetPtr = asset.Get();

    if (assetPtr != nullptr && !assetPtr->IsTransient() && assetPtr->GetUuid() != 0)
    {
        // New format: UUID-based with name fallback
        WriteUint8(1);  // Format indicator
        WriteUint64(assetPtr->GetUuid());
        WriteString(assetPtr->GetName());  // Store name for fallback lookup
    }
    else if (assetPtr != nullptr && !assetPtr->IsTransient())
    {
        // Legacy format fallback (asset has no UUID yet)
        WriteUint8(0);
        WriteString(assetPtr->GetName());
    }
    else
    {
        // Null reference - use UUID format with 0
        WriteUint8(1);
        WriteUint64(0);
        WriteString("");  // Empty name for null reference
    }
}

void Stream::ReadString(std::string& dst)
{
    // If updating the integer size for string length, please update STREAM_STRING_LEN_BYTES
    OCT_ASSERT(mPos + sizeof(uint32_t) <= mSize);
    uint32_t stringSize = ReadUint32();
    OCT_ASSERT(stringSize <= MAX_STRING_SIZE);
    dst.resize(stringSize);

    if (stringSize > 0)
    {
        OCT_ASSERT(mPos + stringSize <= mSize);
        if (mWindowed)
        {
            ReadWindowed(&dst[0], stringSize);
        }
        else
        {
            dst.assign(&mData[mPos], stringSize);
            mPos += stringSize;
        }
    }
    else
    {
        dst = "";
    }
}

void Stream::WriteString(const std::string& src)
{
    OCT_ASSERT(src.size() <= MAX_STRING_SIZE);
    uint32_t deltaSize = uint32_t(sizeof(uint32_t) + src.size());

    if (mPos + deltaSize > mSize)
    {
        Grow(mPos + deltaSize);
    }

    // If updating the integer size for string length, please update STREAM_STRING_LEN_BYTES
    WriteUint32(uint32_t(src.size()));

    if (src.size() > 0)
    {
        memcpy(&mData[mPos], src.data(), src.size());
        mPos += uint32_t(src.size());
    }
}

void Stream::ReadBytes(uint8_t* dst, uint32_t length)
{
    // If updating the integer size for string length, please update STREAM_STRING_LEN_BYTES
    OCT_ASSERT(mPos + length <= mSize);

    if (length > 0)
    {
        if (mWindowed)
        {
            ReadWindowed(dst, length);
            return;
        }

        memcpy(dst, &mData[mPos], length);
        mPos += length;
    }
}

uint32_t Stream::ReadBytesMax(uint8_t* dst, uint32_t length)
{
    if (length > 0 && mPos < mSize)
    {
        if (mPos + length > mSize)
        {
            length = (mSize - mPos);
        }

        ReadBytes(dst, length);
        return length;
    }

    return 0;
}

void Stream::WriteBytes(const uint8_t* src, uint32_t length)
{
    if (mPos + length > mSize)
    {
        Grow(mPos + length);
    }

    if (mData == nullptr || mPos + length > mCapacity)
    {
        // No room, and Grow has already said so.
        return;
    }

    if (length > 0)
    {
        memcpy(&mData[mPos], src, length);
        mPos += length;
    }
}

int32_t Stream::ReadInt32()
{
    int32_t ret = 0;
    Read(ret);
    return ret;
}

uint32_t Stream::ReadUint32()
{
    uint32_t ret = 0;
    Read(ret);
    return ret;
}

int64_t Stream::ReadInt64()
{
    // Read as two 32-bit values to handle endianness properly
    uint32_t low = ReadUint32();
    uint32_t high = ReadUint32();
    return (int64_t)(((uint64_t)high << 32) | low);
}

uint64_t Stream::ReadUint64()
{
    // Read as two 32-bit values to handle endianness properly
    uint32_t low = ReadUint32();
    uint32_t high = ReadUint32();
    return ((uint64_t)high << 32) | low;
}

int16_t Stream::ReadInt16()
{
    int16_t ret = 0;
    Read(ret);
    return ret;
}

uint16_t Stream::ReadUint16()
{
    uint16_t ret = 0;
    Read(ret);
    return ret;
}

int8_t Stream::ReadInt8()
{
    int8_t ret = 0;
    Read(ret);
    return ret;
}

uint8_t Stream::ReadUint8()
{
    uint8_t ret = 0;
    Read(ret);
    return ret;
}

float Stream::ReadFloat()
{
    float ret = 0;
    Read(ret);
    return ret;
}

bool Stream::ReadBool()
{
    uint8_t ret = 0;
    Read(ret);
    return ret != 0;
}

glm::vec2 Stream::ReadVec2()
{
    glm::vec2 ret;
    ret.x = ReadFloat();
    ret.y = ReadFloat();
    return ret;
}

glm::vec3 Stream::ReadVec3()
{
    glm::vec3 ret;
    ret.x = ReadFloat();
    ret.y = ReadFloat();
    ret.z = ReadFloat();
    return ret;
}

glm::vec4 Stream::ReadVec4()
{
    glm::vec4 ret;
    ret.x = ReadFloat();
    ret.y = ReadFloat();
    ret.z = ReadFloat();
    ret.w = ReadFloat();
    return ret;
}

glm::quat Stream::ReadQuat()
{
    glm::quat ret;
    ret.x = ReadFloat();
    ret.y = ReadFloat();
    ret.z = ReadFloat();
    ret.w = ReadFloat();
    return ret;
}

glm::mat4 Stream::ReadMatrix()
{
    glm::mat4 ret;
    float* dst = glm::value_ptr(ret);
    for (uint32_t i = 0; i < 16; ++i)
    {
        dst[i] = ReadFloat();
    }
    return ret;
}

void Stream::WriteInt32(const int32_t& src)
{
    Write(src);
}

void Stream::WriteUint32(const uint32_t& src)
{
    Write(src);
}

void Stream::WriteInt64(const int64_t& src)
{
    // Write as two 32-bit values to handle endianness properly
    WriteUint32((uint32_t)(src & 0xFFFFFFFF));
    WriteUint32((uint32_t)((uint64_t)src >> 32));
}

void Stream::WriteUint64(const uint64_t& src)
{
    // Write as two 32-bit values to handle endianness properly
    WriteUint32((uint32_t)(src & 0xFFFFFFFF));
    WriteUint32((uint32_t)(src >> 32));
}

void Stream::WriteInt16(const int16_t& src)
{
    Write(src);
}

void Stream::WriteUint16(const uint16_t& src)
{
    Write(src);
}

void Stream::WriteInt8(const int8_t& src)
{
    Write(src);
}

void Stream::WriteUint8(const uint8_t& src)
{
    Write(src);
}

void Stream::WriteFloat(const float& src)
{
    Write(src);
}

void Stream::WriteBool(const bool& src)
{
    uint8_t uintBool = src ? 1 : 0;
    WriteUint8(uintBool);
}

void Stream::WriteVec2(const glm::vec2& src)
{
    Write(src.x);
    Write(src.y);
}

void Stream::WriteVec3(const glm::vec3& src)
{
    Write(src.x);
    Write(src.y);
    Write(src.z);
}

void Stream::WriteVec4(const glm::vec4& src)
{
    Write(src.x);
    Write(src.y);
    Write(src.z);
    Write(src.w);
}

void Stream::WriteQuat(const glm::quat& src)
{
    Write(src.x);
    Write(src.y);
    Write(src.z);
    Write(src.w);
}

void Stream::WriteMatrix(const glm::mat4& src)
{
    const float* srcArray = glm::value_ptr(src);

    for (uint32_t i = 0; i < 16; ++i)
    {
        Write(srcArray[i]);
    }
}

std::string Stream::GetLine()
{
    std::string line;

    if (mPos < mSize && mSize > 0)
    {
        uint32_t endPos = mPos;

        while (endPos < mSize)
        {
            if (mData[endPos] == '\0' ||
                mData[endPos] == '\n')
            {
                endPos++;
                break;
            }

            endPos++;
        }

        for (uint32_t i = mPos; i < endPos; ++i)
        {
            if (mData[i] != '\0' &&
                mData[i] != '\n' &&
                mData[i] != '\r')
            {
                line.push_back(mData[i]);
            }
        }

        SetPos(endPos);
    }

    return line;
}

int32_t Stream::Scan(const char* format, ...)
{
    int32_t ret = -1;

    if (mPos < mSize)
    {
        va_list argptr;
        va_start(argptr, format);

        std::string line = GetLine();
        if (line != "")
        {
            ret = vsscanf(line.data(), format, argptr);
        }
    }

    return ret;
}

void Stream::Grow(uint32_t newSize)
{
    // Realloc space for new data (will fail and assert if mExternal is true)
    if (!mExternal)
    {
        if (newSize > mCapacity)
        {
            uint32_t newCapacity = glm::max(newSize, mCapacity * 2);
            Reserve(newCapacity);

            // Reserve leaves the stream untouched when it cannot allocate. Record that here, so
            // the write that prompted the growth is dropped instead of running off the end of a
            // buffer that never got any bigger. Silently overrunning the heap is worse than
            // losing the data, and much harder to find afterwards.
            if (mCapacity < newSize)
            {
                mAllocFailed = true;
                return;
            }
        }
    }
    else
    {
        OCT_ASSERT(newSize <= mCapacity);
    }

    mSize = newSize;
}

void Stream::Reserve(uint32_t capacity)
{
    if (mExternal)
    {
        LogError("Cannot Reserve() external stream. Capacity is locked at construction time.");
        OCT_ASSERT(0);
        return;
    }

    if (capacity > mCapacity)
    {
        char* newBuffer = (char*) malloc(capacity);

        // Out of memory. This used to carry on: the copy below went into a null pointer and the
        // game died inside memcpy at address 0, which names neither the file being read nor the
        // fact that memory had run out. Every asset read goes through here, so that was the shape
        // of any out-of-memory during loading.
        //
        // Leave the stream exactly as it was and let the caller find its capacity unchanged. The
        // old buffer is deliberately kept rather than freed -- throwing away good data on the way
        // to reporting a failure only makes the next thing to touch it fail worse.
        if (newBuffer == nullptr)
        {
            LogError("Stream: could not allocate %u bytes; out of memory.", capacity);
            return;
        }

        if (mData != nullptr)
        {
            memcpy(newBuffer, mData, mSize);
            free(mData);
        }

        mData = newBuffer;
        mCapacity = capacity;
    }
}
