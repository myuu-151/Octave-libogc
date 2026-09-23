// BigBlockCache_Dolphin.cpp -- GameCube only.
//
// WHY. A GameCube has 24 MB, and a game that streams assets in and out (a stage's pipe, a sky's
// star frames, the menus) frees and allocates blocks of hundreds of kilobytes all session long.
// With newlib's malloc that cuts the heap to pieces: small allocations made in between (Lua
// tables, nodes, strings) land in the holes the big blocks leave, and a few stages later a
// 512 KB star frame has nowhere to go with 9.7 MB free -- measured in Sonic2Special3D-GC: the
// largest block fell from 6.5 MB at boot to 544 KB by the third stage, and loads failed.
//
// WHAT. Freed blocks of BIG_BLOCK bytes or more are kept here instead of going back to the heap,
// and an allocation of (about) the same size takes one back. The big allocations a game repeats
// are the same sizes every time -- every sky's star frames are one size, a drop piece is the same
// size in every stage's palette, the file buffers match the files -- so they go on reusing the
// same blocks, and the small allocations never get into them.
//
// NOTHING STARVES. Whenever any allocation fails, big or small, the cache gives its blocks back
// to the heap one at a time (the biggest first) and the allocation is tried again after each.
// The cache can only ever hold memory nothing else could get at that moment.
//
// HOW. The link wraps malloc, free, realloc, calloc and memalign (Standalone/Makefile_GCN:
// -Wl,--wrap=...): every call in the game, the engine, Lua and libstdc++ (operator new) comes
// here. newlib's own internal allocations (_malloc_r) do not, which is fine: their blocks are
// small, and freeing one through free() still arrives here.

#if PLATFORM_GAMECUBE

#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/reent.h>

extern "C"
{
void* __real_malloc(size_t size);
void __real_free(void* ptr);
void* __real_realloc(void* ptr, size_t size);
void* __real_calloc(size_t count, size_t size);
void* __real_memalign(size_t align, size_t size);

void __malloc_lock(struct _reent* r);
void __malloc_unlock(struct _reent* r);
}

namespace
{
    const size_t BIG_BLOCK = 32 * 1024;     // blocks this size and up are kept
    const uint32_t MAX_KEPT = 96;           // at most this many at once

    struct Kept
    {
        void* mPtr;
        size_t mSize;                       // malloc_usable_size
    };

    Kept sKept[MAX_KEPT];
    uint32_t sNumKept = 0;

    struct Lock
    {
        Lock() { __malloc_lock(_REENT); }
        ~Lock() { __malloc_unlock(_REENT); }
    };

    // A kept block for a request of `size` aligned to `align` (1 for plain malloc): the
    // smallest that fits and wastes no more than an eighth. nullptr if there is none.
    void* Take(size_t size, size_t align)
    {
        if (size < BIG_BLOCK)
        {
            return nullptr;
        }

        Lock lock;
        int32_t best = -1;
        for (uint32_t i = 0; i < sNumKept; ++i)
        {
            const Kept& k = sKept[i];
            if (k.mSize >= size && k.mSize <= size + size / 8 &&
                ((uintptr_t)k.mPtr & (align - 1)) == 0 &&
                (best < 0 || k.mSize < sKept[best].mSize))
            {
                best = int32_t(i);
            }
        }

        if (best < 0)
        {
            return nullptr;
        }

        void* ptr = sKept[best].mPtr;
        sKept[best] = sKept[--sNumKept];
        return ptr;
    }

    // Give the biggest kept block back to the heap. False when there is none left.
    bool GiveBackOne()
    {
        void* ptr = nullptr;
        {
            Lock lock;
            if (sNumKept == 0)
            {
                return false;
            }

            uint32_t biggest = 0;
            for (uint32_t i = 1; i < sNumKept; ++i)
            {
                if (sKept[i].mSize > sKept[biggest].mSize)
                {
                    biggest = i;
                }
            }

            ptr = sKept[biggest].mPtr;
            sKept[biggest] = sKept[--sNumKept];
        }

        __real_free(ptr);
        return true;
    }
}

// What the cache holds: free memory as far as the game is concerned (System.GetFreeMemory).
size_t BigBlockCacheBytes()
{
    Lock lock;
    size_t total = 0;
    for (uint32_t i = 0; i < sNumKept; ++i)
    {
        total += sKept[i].mSize;
    }

    return total;
}

extern "C"
{

void* __wrap_malloc(size_t size)
{
    void* ptr = Take(size, 1);
    if (ptr != nullptr)
    {
        return ptr;
    }

    ptr = __real_malloc(size);
    while (ptr == nullptr && GiveBackOne())
    {
        ptr = __real_malloc(size);
    }

    return ptr;
}

void* __wrap_memalign(size_t align, size_t size)
{
    if (align > 0 && (align & (align - 1)) == 0)
    {
        void* ptr = Take(size, align);
        if (ptr != nullptr)
        {
            return ptr;
        }
    }

    void* ptr = __real_memalign(align, size);
    while (ptr == nullptr && GiveBackOne())
    {
        ptr = __real_memalign(align, size);
    }

    return ptr;
}

void __wrap_free(void* ptr)
{
    if (ptr == nullptr)
    {
        return;
    }

    size_t size = malloc_usable_size(ptr);
    if (size >= BIG_BLOCK)
    {
        Lock lock;
        if (sNumKept < MAX_KEPT)
        {
            sKept[sNumKept].mPtr = ptr;
            sKept[sNumKept].mSize = size;
            sNumKept++;
            return;
        }
    }

    __real_free(ptr);
}

void* __wrap_realloc(void* ptr, size_t size)
{
    void* out = __real_realloc(ptr, size);
    while (out == nullptr && size > 0 && GiveBackOne())
    {
        out = __real_realloc(ptr, size);
    }

    return out;
}

void* __wrap_calloc(size_t count, size_t size)
{
    void* ptr = __real_calloc(count, size);
    while (ptr == nullptr && count > 0 && size > 0 && GiveBackOne())
    {
        ptr = __real_calloc(count, size);
    }

    return ptr;
}

}

#endif
