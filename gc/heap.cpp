#include "heap.h"

#include <pthread.h>
#include <stdlib.h>
#include <sys/mman.h>

uint8_t* Heap::s_base;
uint8_t* Heap::s_end;
uint8_t* Heap::s_committed;
uint8_t* Heap::s_next;
MethodTable* Heap::s_freeObjectMT;
uint64_t* Heap::s_startBits;
uint64_t* Heap::s_markBits;
uint64_t Heap::s_totalAllocated;
uint8_t* Heap::s_bins[BinCount];
size_t Heap::s_freeListBytes;

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

    // Nothing here reads the card tables, and JIT write barriers skip them (see below), but bulk
    // reference copies always write to them (vm/gchelpers.inl), so they have to exist and cover the
    // whole reservation. They only cost memory where they are written. The runtime indexes them
    // with addr >> shift, so the pointers it gets are biased.
    size_t cardBytes = reserveBytes >> CardByteShift;
    size_t bundleBytes = AlignUpTo(reserveBytes, (size_t)1 << BundleByteShift) >> BundleByteShift;
    uint8_t* cards = Reserve(cardBytes, PROT_READ | PROT_WRITE);
    uint8_t* bundles = Reserve(bundleBytes, PROT_READ | PROT_WRITE);
    if (cards == nullptr || bundles == nullptr)
        return false;

    s_startBits = (uint64_t*)Reserve(reserveBytes / 64, PROT_READ | PROT_WRITE);
    s_markBits = (uint64_t*)Reserve(reserveBytes / 64, PROT_READ | PROT_WRITE);
    if (s_startBits == nullptr || s_markBits == nullptr)
        return false;

    WriteBarrierParameters args = {};
    args.operation = WriteBarrierOp::Initialize;
    args.is_runtime_suspended = true;
    args.requires_upper_bounds_check = false;
    args.card_table = (uint32_t*)((uintptr_t)cards - ((uintptr_t)s_base >> CardByteShift));
    args.card_bundle_table = (uint32_t*)((uintptr_t)bundles - ((uintptr_t)s_base >> BundleByteShift));
    // These have to be the real bounds: the arm64 checked barrier drops its release ordering for
    // destinations it thinks are outside the heap.
    args.lowest_address = s_base;
    args.highest_address = s_end;
    // Every reference compares below ephemeral_low, so JIT barriers return before touching cards.
    // s_end would not be high enough, because frozen objects may live above the heap.
    args.ephemeral_low = (uint8_t*)~(uintptr_t)0;
    args.ephemeral_high = (uint8_t*)~(uintptr_t)0;
    g_ee->StompWriteBarrier(&args);
    return true;
}

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

void Heap::RetireContext(gc_alloc_context* ctx, bool discountTail)
{
    if (ctx->alloc_ptr == nullptr)
        return;
    // alloc_limit always sits min_obj_size before the end of the chunk, so this fits.
    size_t unused = (size_t)(ctx->alloc_limit - ctx->alloc_ptr);
    MakeFreeObject(ctx->alloc_ptr, unused + min_obj_size);
    ctx->alloc_bytes -= (int64_t)unused;
    if (discountTail)
        __atomic_sub_fetch(&s_totalAllocated, unused, __ATOMIC_RELAXED);
    ctx->alloc_ptr = nullptr;
    ctx->alloc_limit = nullptr;
}

void Heap::PushFree(uint8_t* block, size_t size)
{
    uint8_t*& bin = s_bins[BinOf(size)];
    NextFree(block) = bin;
    bin = block;
    __atomic_store_n(&s_freeListBytes, s_freeListBytes + size, __ATOMIC_RELAXED);
}

uint8_t* Heap::TakeFree(size_t minimum, size_t maximum, size_t* taken)
{
    uint8_t* result = nullptr;
    pthread_mutex_lock(&s_heapLock);
    for (int bin = BinOf(minimum); bin < BinCount && result == nullptr; bin++)
    {
        uint8_t** link = &s_bins[bin];
        for (int probes = 0; *link != nullptr && probes < MaxProbes; probes++, link = &NextFree(*link))
        {
            uint8_t* block = *link;
            size_t size = ObjectSize(block);
            if (size < minimum)
                continue;
            // What is left over must be nothing, or big enough to be an object.
            size_t use = size;
            if (size >= maximum + min_obj_size)
                use = maximum;
            else if (size > maximum && minimum == maximum)
                continue;
            *link = NextFree(block);
            __atomic_store_n(&s_freeListBytes, s_freeListBytes - size, __ATOMIC_RELAXED);
            if (use < size)
            {
                MakeFreeObject(block + use, size - use);
                PushFree(block + use, size - use);
            }
            result = block;
            *taken = use;
            break;
        }
    }
    pthread_mutex_unlock(&s_heapLock);
    return result;
}

uint8_t* Heap::TakeHole(size_t size)
{
    size_t taken;
    uint8_t* p = FreeListBytes() >= size ? TakeFree(size, size, &taken) : nullptr;
    if (p != nullptr)
        Zero(p, size);
    return p;
}

Object* Heap::Alloc(gc_alloc_context* ctx, size_t size, uint32_t flags)
{
    if (size > ReservedBytes())
        return nullptr;
    size = AlignUpTo(size < min_obj_size ? min_obj_size : size, sizeof(void*));

    bool oldHeap = (flags & GC_ALLOC_USER_OLD_HEAP) != 0;  // large or pinned (gcinterface.h:1093-1095)
    if (!oldHeap && size <= (size_t)(ctx->alloc_limit - ctx->alloc_ptr))
    {
        uint8_t* p = ctx->alloc_ptr;
        ctx->alloc_ptr += size;
        return (Object*)p;
    }

    auto single = [&](uint8_t* p) {
        if (p != nullptr)
        {
            (oldHeap ? ctx->alloc_bytes_uoh : ctx->alloc_bytes) += (int64_t)size;
            __atomic_add_fetch(&s_totalAllocated, size, __ATOMIC_RELAXED);
        }
        return (Object*)p;
    };
    if (oldHeap || size >= DirectThreshold)
    {
        uint8_t* p = TakeHole(size);
        return single(p != nullptr ? p : BumpGlobal(size));
    }

    // In order of preference: refill the context from a big hole, put just this object in a small
    // hole, refill the context from fresh memory, or, when the heap is nearly full, squeeze in just
    // this object at the end.
    size_t chunkSize = ChunkSize;
    uint8_t* chunk = nullptr;
    size_t wanted = size + min_obj_size > MinHoleForContext ? size + min_obj_size : MinHoleForContext;
    if (FreeListBytes() >= wanted)
        chunk = TakeFree(wanted, ChunkSize, &chunkSize);
    if (chunk != nullptr)
        Zero(chunk, chunkSize);
    else if (uint8_t* hole = TakeHole(size))
        return single(hole);
    else
    {
        chunkSize = ChunkSize;
        chunk = BumpGlobal(ChunkSize);
        if (chunk == nullptr)
            return single(BumpGlobal(size));
    }

    RetireContext(ctx, true);
    ctx->alloc_ptr = chunk + size;
    ctx->alloc_limit = chunk + chunkSize - min_obj_size;
    // The VM reports per-thread totals as alloc_bytes - (alloc_limit - alloc_ptr).
    ctx->alloc_bytes += (int64_t)(ctx->alloc_limit - chunk);
    __atomic_add_fetch(&s_totalAllocated, (uint64_t)(ctx->alloc_limit - chunk), __ATOMIC_RELAXED);
    ctx->alloc_count++;
    return (Object*)chunk;
}

void Heap::RetireAllContexts()
{
    g_ee->GcEnumAllocContexts([](gc_alloc_context* ctx, void*) { RetireContext(ctx, true); }, nullptr);
}

void Heap::RebuildStartBits()
{
    size_t bytes = (BitIndex(s_next) / 64 + 1) * sizeof(uint64_t);
    memset(s_startBits, 0, bytes);
    memset(s_markBits, 0, bytes);
    Walk([](uint8_t* obj, size_t, bool isFree) {
        if (!isFree)
            s_startBits[BitIndex(obj) >> 6] |= 1ull << (BitIndex(obj) & 63);
    });
}

uint8_t* Heap::FindObject(const void* p)
{
    size_t bit = BitIndex(p);
    size_t word = bit >> 6;
    // Keep only bits at or below p, then search downwards for the nearest start.
    uint64_t candidates = s_startBits[word] & (~0ull >> (63 - (bit & 63)));
    while (candidates == 0)
    {
        if (word == 0)
            return nullptr;
        candidates = s_startBits[--word];
    }
    uint8_t* obj = s_base + (((word << 6) + (63 - (size_t)__builtin_clzll(candidates))) << 3);
    return (const uint8_t*)p < obj + ObjectSize(obj) ? obj : nullptr;
}

void Heap::Erase(uint8_t* obj)
{
    static const bool poison = getenv("MANUALGC_POISON") != nullptr;
    size_t size = ObjectSize(obj);
    if (poison)  // so that use after free shows up; the allocator zeroes the memory again
        memset(obj, 0xDD, size - sizeof(ObjHeader));
    s_startBits[BitIndex(obj) >> 6] &= ~(1ull << (BitIndex(obj) & 63));
    s_markBits[BitIndex(obj) >> 6] &= ~(1ull << (BitIndex(obj) & 63));
    NextFree(obj) = nullptr;
    while (size > MaxFreeObject)
    {
        MakeFreeObject(obj, MaxFreeObject);
        obj += MaxFreeObject;
        size -= MaxFreeObject;
    }
    MakeFreeObject(obj, size);
}

void Heap::Sweep()
{
    // A preemptive thread cannot be allocating while the EE is suspended, so no lock is needed.
    memset(s_bins, 0, sizeof(s_bins));
    s_freeListBytes = 0;
    uint8_t* run = nullptr;
    size_t runSize = 0;
    auto finishRun = [&] {
        while (runSize > 0)
        {
            // A free object's size is limited by its 32-bit count.
            size_t piece = runSize > MaxFreeObject ? MaxFreeObject : runSize;
            MakeFreeObject(run, piece);
            PushFree(run, piece);
            run += piece;
            runSize -= piece;
        }
    };
    Walk([&](uint8_t* obj, size_t size, bool isFree) {
        if (!isFree)
        {
            finishRun();
            return;
        }
        if (runSize == 0)
            run = obj;
        runSize += size;
    });
    finishRun();
}
