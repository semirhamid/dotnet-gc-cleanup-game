#include "heap.h"

#include <pthread.h>
#include <sys/mman.h>

uint8_t* Heap::s_base;
uint8_t* Heap::s_end;
uint8_t* Heap::s_committed;
uint8_t* Heap::s_next;
MethodTable* Heap::s_freeObjectMT;

// Threads take this in cooperative mode. Nothing inside the lock waits for the runtime, so a
// suspension request is delayed by at most one short critical section.
static pthread_mutex_t s_heapLock = PTHREAD_MUTEX_INITIALIZER;

// Byte-per-2KB card and byte-per-2MB bundle shifts (vm/gchelpers.inl card_byte_shift and
// card_bundle_byte_shift; the same shifts are hard coded in vm/arm64/patchedcode.S).
static constexpr int CardByteShift = 11;
static constexpr int BundleByteShift = 21;

static constexpr size_t AlignUpTo(size_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

static uint8_t* Reserve(size_t bytes, int protection)
{
    void* p = mmap(nullptr, bytes, protection, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : (uint8_t*)p;
}

bool Heap::Initialize(size_t reserveBytes, MethodTable* freeObjectMT)
{
    s_freeObjectMT = freeObjectMT;

    // Aligning the base to CommitStep also aligns it to a card bundle (2 MB).
    reserveBytes = AlignUpTo(reserveBytes, CommitStep);
    uint8_t* raw = Reserve(reserveBytes + CommitStep, PROT_NONE);
    if (raw == nullptr)
        return false;
    s_base = (uint8_t*)AlignUpTo((size_t)raw, CommitStep);
    s_end = s_base + reserveBytes;
    s_committed = s_base;
    s_next = FirstObject();

    // Real tables covering the whole reservation. JIT write barriers skip them (see below), but
    // bulk reference copies into the heap always write them (vm/gchelpers.inl). Pages that are
    // never written are never backed by memory. The runtime indexes them with addr >> shift, so
    // the pointers passed to it are biased.
    size_t cardBytes = reserveBytes >> CardByteShift;
    size_t bundleBytes = AlignUpTo(reserveBytes, (size_t)1 << BundleByteShift) >> BundleByteShift;
    uint8_t* cards = Reserve(cardBytes, PROT_READ | PROT_WRITE);
    uint8_t* bundles = Reserve(bundleBytes, PROT_READ | PROT_WRITE);
    if (cards == nullptr || bundles == nullptr)
        return false;

    WriteBarrierParameters args = {};  // gcinterface.h:59-116
    args.operation = WriteBarrierOp::Initialize;
    args.is_runtime_suspended = true;
    args.requires_upper_bounds_check = false;
    args.card_table = (uint32_t*)((uintptr_t)cards - ((uintptr_t)s_base >> CardByteShift));
    args.card_bundle_table = (uint32_t*)((uintptr_t)bundles - ((uintptr_t)s_base >> BundleByteShift));
    // True bounds, because the arm64 checked barrier drops its release ordering for
    // destinations it thinks are outside the heap.
    args.lowest_address = s_base;
    args.highest_address = s_end;
    // Nothing is ephemeral: every reference compares below ephemeral_low, so JIT barriers
    // return before touching cards. Frozen objects may live above the heap, hence the maximum
    // address rather than s_end.
    args.ephemeral_low = (uint8_t*)~(uintptr_t)0;
    args.ephemeral_high = (uint8_t*)~(uintptr_t)0;
    g_ee->StompWriteBarrier(&args);
    return true;
}

// Returns zeroed space for an object range of the given size, or null when out of memory.
uint8_t* Heap::BumpGlobal(size_t size)
{
    uint8_t* result = nullptr;
    pthread_mutex_lock(&s_heapLock);
    if (size <= (size_t)(s_end - s_next))
    {
        uint8_t* newNext = s_next + size;
        bool committed = true;
        if (newNext > s_committed)
        {
            // s_end is CommitStep aligned, so this never passes the reservation.
            uint8_t* newCommitted = (uint8_t*)AlignUpTo((size_t)newNext, CommitStep);
            committed = mprotect(s_committed, (size_t)(newCommitted - s_committed), PROT_READ | PROT_WRITE) == 0;
            if (committed)
                __atomic_store_n(&s_committed, newCommitted, __ATOMIC_RELAXED);
        }
        if (committed)
        {
            result = s_next;
            __atomic_store_n(&s_next, newNext, __ATOMIC_RELAXED);
        }
    }
    pthread_mutex_unlock(&s_heapLock);
    return result;
}

void Heap::MakeFreeObject(uint8_t* p, size_t size)
{
    // Free object: MethodTable, then a component count of one-byte components
    // (gcinterface.ee.h:363-371). Header and base size take min_obj_size.
    assert(size >= min_obj_size && size - min_obj_size <= UINT32_MAX);
    *(MethodTable**)p = s_freeObjectMT;
    *(uint32_t*)(p + sizeof(void*)) = (uint32_t)(size - min_obj_size);
}

void Heap::RetireContext(gc_alloc_context* ctx)
{
    if (ctx->alloc_ptr == nullptr)
        return;
    // alloc_limit always sits min_obj_size before the end of the chunk, so this fits.
    size_t unused = (size_t)(ctx->alloc_limit - ctx->alloc_ptr);
    MakeFreeObject(ctx->alloc_ptr, unused + min_obj_size);
    ctx->alloc_bytes -= (int64_t)unused;
    ctx->alloc_ptr = nullptr;
    ctx->alloc_limit = nullptr;
}

Object* Heap::Alloc(gc_alloc_context* ctx, size_t size, uint32_t flags)
{
    if (size > ReservedBytes())
        return nullptr;
    size = AlignUpTo(size < min_obj_size ? min_obj_size : size, sizeof(void*));

    if (flags & GC_ALLOC_USER_OLD_HEAP)  // large or pinned object heap (gcinterface.h:1093-1095)
    {
        uint8_t* p = BumpGlobal(size);
        if (p != nullptr)
            ctx->alloc_bytes_uoh += (int64_t)size;
        return (Object*)p;
    }

    if (size <= (size_t)(ctx->alloc_limit - ctx->alloc_ptr))
    {
        uint8_t* p = ctx->alloc_ptr;
        ctx->alloc_ptr += size;
        return (Object*)p;
    }

    if (size >= DirectThreshold)
    {
        uint8_t* p = BumpGlobal(size);
        if (p != nullptr)
            ctx->alloc_bytes += (int64_t)size;
        return (Object*)p;
    }

    uint8_t* chunk = BumpGlobal(ChunkSize);
    if (chunk == nullptr)
        return nullptr;
    RetireContext(ctx);
    ctx->alloc_ptr = chunk + size;
    ctx->alloc_limit = chunk + ChunkSize - min_obj_size;
    // The VM reports per-thread totals as alloc_bytes - (alloc_limit - alloc_ptr).
    ctx->alloc_bytes += (int64_t)(ctx->alloc_limit - chunk);
    ctx->alloc_count++;
    return (Object*)chunk;
}
