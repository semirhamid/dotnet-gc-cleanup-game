// The managed heap: one contiguous reservation with a bump pointer. See docs/design.md.
#pragma once

#include "object.h"

// Addresses in this class are "object pointers": they point at the MethodTable slot. An object
// of size n at p owns [p - 8, p + n - 8), because its ObjHeader sits 8 bytes before p.
class Heap
{
public:
    static constexpr size_t CommitStep = 16 * 1024 * 1024;
    static constexpr size_t DefaultReserve = 32ull * 1024 * 1024 * 1024;
    static constexpr size_t ChunkSize = 64 * 1024;
    static constexpr size_t DirectThreshold = 8 * 1024;

    static constexpr size_t MinHoleForContext = 4 * 1024; // smaller holes get single objects
    static constexpr size_t MaxFreeObject = 1ull << 31;

    static bool Initialize(size_t reserveBytes, MethodTable* freeObjectMT);

    static Object* Alloc(gc_alloc_context* ctx, size_t size, uint32_t flags);

    // Plugs the unused tail of a context with a free object and empties the context. discountTail
    // takes the tail off the allocation total; the VM does that itself when a thread dies
    // (vm/threads.cpp:2783).
    static void RetireContext(gc_alloc_context* ctx, bool discountTail);

    // The following may only be called while the EE is suspended.

    static void RetireAllContexts();

    template <typename Fn>
    static void Walk(Fn fn)
    {
        for (uint8_t* p = FirstObject(); p < s_next;)
        {
            MethodTable* mt = MethodTableOf(p);
            if (mt == nullptr)
                Fatal("heap walk reached %p with no MethodTable", (void*)p);
            size_t size = ObjectSize(p);
            fn(p, size, mt == s_freeObjectMT);
            p += size;
        }
    }

    // Two bitmaps with one bit per 8 bytes of heap. A start bit says that an object, live or dead,
    // starts there; a mark bit says that the object is alive. Rebuilding clears the mark bits.
    static void RebuildStartBits();
    static bool IsObjectStart(const void* p) { return TestBit(s_startBits, p); }
    static bool IsMarked(const void* p) { return TestBit(s_markBits, p); }
    static bool TestAndMark(const void* p)
    {
        size_t bit = BitIndex(p);
        uint64_t mask = 1ull << (bit & 63);
        bool was = (s_markBits[bit >> 6] & mask) != 0;
        s_markBits[bit >> 6] |= mask;
        return was;
    }
    // The object whose [obj, obj + size) contains p, which must be inside the heap.
    static uint8_t* FindObject(const void* p);

    static void Erase(uint8_t* obj);

    static void Sweep();

    // The rest may be called at any time.

    static bool Contains(const void* p)
    {
        return (const uint8_t*)p >= s_base && (const uint8_t*)p < s_end;
    }

    static size_t ReservedBytes() { return (size_t)(s_end - s_base); }
    static size_t CommittedBytes() { return (size_t)(__atomic_load_n(&s_committed, __ATOMIC_RELAXED) - s_base); }
    // Bytes handed out so far, including the unused tails of allocation contexts.
    static size_t UsedBytes() { return (size_t)(__atomic_load_n(&s_next, __ATOMIC_RELAXED) - FirstObject()); }
    // Every byte ever handed to an allocation, as GC.GetTotalAllocatedBytes counts it.
    static uint64_t TotalAllocated() { return __atomic_load_n(&s_totalAllocated, __ATOMIC_RELAXED); }
    static uint8_t* FirstObject() { return s_base + sizeof(ObjHeader); }

    static void MakeFreeObject(uint8_t* p, size_t size);

    static size_t FreeListBytes() { return __atomic_load_n(&s_freeListBytes, __ATOMIC_RELAXED); }

private:
    static uint8_t* BumpGlobal(size_t size);
    static uint8_t* TakeHole(size_t size);

    // Free blocks are free objects. The list link lives in the block's ObjHeader slot, which even a
    // minimum-size block has. Bin k holds sizes in [2^k, 2^(k+1)).
    static constexpr int BinCount = 48;
    static constexpr int MaxProbes = 32;
    static uint8_t*& NextFree(uint8_t* block) { return ((uint8_t**)block)[-1]; }
    static int BinOf(size_t size) { return 63 - __builtin_clzll(size); }
    static void PushFree(uint8_t* block, size_t size);
    // Finds a block of at least minimum bytes and takes up to maximum of it. A remainder goes back
    // on the lists. The result is not zeroed.
    static uint8_t* TakeFree(size_t minimum, size_t maximum, size_t* taken);
    static void Zero(uint8_t* p, size_t size) { memset(p - sizeof(ObjHeader), 0, size); }

    static size_t BitIndex(const void* p) { return (size_t)((const uint8_t*)p - s_base) >> 3; }
    static bool TestBit(const uint64_t* bits, const void* p)
    {
        size_t bit = BitIndex(p);
        return ((uintptr_t)p & 7) == 0 && (bits[bit >> 6] >> (bit & 63) & 1) != 0;
    }

    static uint8_t* s_base;
    static uint8_t* s_end;
    static uint8_t* s_committed;  // guarded by the heap lock
    static uint8_t* s_next;       // guarded by the heap lock
    static MethodTable* s_freeObjectMT;
    static uint64_t* s_startBits;
    static uint64_t* s_markBits;
    static uint64_t s_totalAllocated;
    static uint8_t* s_bins[BinCount];  // guarded by the heap lock
    static size_t s_freeListBytes;     // written under the heap lock
};
