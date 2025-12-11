// The managed heap: one contiguous reservation with a bump pointer. See docs/design.md.
#pragma once

#include "platform.h"

// Addresses in this class are "object pointers": they point at the MethodTable slot. An object
// of size n at p owns [p - 8, p + n - 8), because its ObjHeader sits 8 bytes before p.
class Heap
{
public:
    static constexpr size_t CommitStep = 16 * 1024 * 1024;
    static constexpr size_t DefaultReserve = 32ull * 1024 * 1024 * 1024;
    static constexpr size_t ChunkSize = 64 * 1024;       // one allocation context refill
    static constexpr size_t DirectThreshold = 8 * 1024;  // this size and up bypass contexts

    // Reserves the heap and card tables and tells the runtime's write barrier about them.
    static bool Initialize(size_t reserveBytes, MethodTable* freeObjectMT);

    static Object* Alloc(gc_alloc_context* ctx, size_t size, uint32_t flags);

    // Plugs the unused tail of a context with a free object and empties the context.
    static void RetireContext(gc_alloc_context* ctx);

    static bool Contains(const void* p)
    {
        return (const uint8_t*)p >= s_base && (const uint8_t*)p < s_end;
    }

    static size_t ReservedBytes() { return (size_t)(s_end - s_base); }
    static size_t CommittedBytes() { return (size_t)(__atomic_load_n(&s_committed, __ATOMIC_RELAXED) - s_base); }
    // Bytes handed out so far, including the unused tails of allocation contexts.
    static size_t UsedBytes() { return (size_t)(__atomic_load_n(&s_next, __ATOMIC_RELAXED) - FirstObject()); }
    static uint8_t* FirstObject() { return s_base + sizeof(ObjHeader); }

    static void MakeFreeObject(uint8_t* p, size_t size);

private:
    static uint8_t* BumpGlobal(size_t size);

    static uint8_t* s_base;
    static uint8_t* s_end;
    static uint8_t* s_committed;  // guarded by the heap lock
    static uint8_t* s_next;       // guarded by the heap lock
    static MethodTable* s_freeObjectMT;
};
