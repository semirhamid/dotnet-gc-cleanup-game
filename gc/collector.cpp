#include "collector.h"
#include "handles.h"
#include "heap.h"
#include "link.h"
#include "typeinfo.h"

#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

namespace
{
    constexpr int MaxGeneration = 2;

    pthread_mutex_t s_collectLock = PTHREAD_MUTEX_INITIALIZER;
    bool s_marking;  // start and mark bits are valid
    unsigned s_collections;

    // Objects marked but not yet scanned. Explicit, so deep object graphs cannot overflow the
    // thread's stack.
    struct MarkStack
    {
        uint8_t** items;
        size_t count;
        size_t capacity;

        void Push(uint8_t* obj)
        {
            if (count == capacity)
            {
                capacity = capacity == 0 ? 64 * 1024 : capacity * 2;
                items = (uint8_t**)realloc(items, capacity * sizeof(uint8_t*));
                if (items == nullptr)
                    Fatal("out of memory growing the mark stack to %zu entries", capacity);
            }
            items[count++] = obj;
        }
    };
    MarkStack s_markStack;

    void Mark(Object* obj, const char* source)
    {
        // Null, frozen objects, and pointers to things that are not in this heap. Frozen objects
        // hold no references into it.
        if (!Heap::Contains(obj))
            return;
        if (!Heap::IsObjectStart(obj))
            Fatal("%s refers to %p, which is not an object", source, (void*)obj);
        if (!Heap::TestAndMark(obj))
            s_markStack.Push((uint8_t*)obj);
    }

    void Drain()
    {
        while (s_markStack.count > 0)
        {
            uint8_t* obj = s_markStack.items[--s_markStack.count];
            ForEachReference(obj, ObjectSize(obj), [](Object** slot) { Mark(*slot, "an object field"); });
            // An object keeps its collectible assembly alive (gcinterface.ee.h:290-292).
            if (MethodTableOf(obj)->Collectible())
                Mark((Object*)g_ee->GetLoaderAllocatorObjectForGC((Object*)obj), "a collectible type");
        }
    }

    // promote_func (gcinterface.h:25).
    void Promote(Object** slot, ScanContext*, uint32_t flags)
    {
        Object* obj = *slot;
        if (!Heap::Contains(obj))
            return;
        if (flags & GC_CALL_INTERIOR)
        {
            obj = (Object*)Heap::FindObject(obj);
            if (obj == nullptr)
                return;
        }
        Mark(obj, "a root");
        Drain();
    }

    void MarkHandles(ScanContext* sc)
    {
        Handles::ForEach([=](HandleSlot* slot, uint32_t type) {
            switch (type)
            {
            case HNDTYPE_STRONG:
            case HNDTYPE_PINNED:
            case HNDTYPE_ASYNCPINNED:     // not used by this VM (gcinterface.h:499-502)
            case HNDTYPE_SIZEDREF:        // not used by this VM (gcinterface.h:512-515)
            case HNDTYPE_CROSSREFERENCE:  // treated as strong: no bridge support
                Promote(&slot->object, sc, 0);
                break;
            case HNDTYPE_REFCOUNTED:
                if (!Collector::IsPromoted(slot->object) && g_ee->RefCountedHandleCallbacks(slot->object))
                    Promote(&slot->object, sc, 0);
                break;
            default:  // weak kinds do not keep anything alive; dependent handles come later
                break;
            }
        });
    }

    // A dependent handle keeps its secondary alive while its primary is. Marking one secondary
    // can make another primary reachable, so repeat until nothing changes.
    void MarkDependentHandles()
    {
        bool changed = true;
        while (changed)
        {
            changed = false;
            Handles::ForEach([&](HandleSlot* slot, uint32_t type) {
                if (type == HNDTYPE_DEPENDENT && slot->secondary != nullptr &&
                    Collector::IsPromoted(slot->object) && !Collector::IsPromoted(slot->secondary))
                {
                    Mark(slot->secondary, "a dependent handle");
                    Drain();
                    changed = true;
                }
            });
        }
    }

    ScanContext MakeScanContext()
    {
        ScanContext sc;  // gcinterface.h:1121
        sc.promotion = true;
        sc.thread_number = 0;
        sc.thread_count = 1;
        return sc;
    }

    void MarkFromRoots(ScanContext& sc)
    {
        g_ee->GcScanRoots(Promote, MaxGeneration, MaxGeneration, &sc);
        MarkHandles(&sc);
        MarkDependentHandles();
    }

    void ClearIfDead(Object** slot)
    {
        if (!Collector::IsPromoted(*slot))
            __atomic_store_n(slot, (Object*)nullptr, __ATOMIC_RELEASE);
    }

    // Cuts every path by which the app could still get at a dead object. After this a dead object
    // stays dead, so references between dead objects may dangle without harm. The order is the
    // stock GC's; its finalization step, between short and long weak handles, does not exist here.
    void SeverDead(ScanContext& sc)
    {
        Handles::ForEach([](HandleSlot* slot, uint32_t type) {
            switch (type)
            {
            case HNDTYPE_WEAK_SHORT:
            case HNDTYPE_WEAK_NATIVE_COM:
            case HNDTYPE_WEAK_LONG:
            case HNDTYPE_REFCOUNTED:
            case HNDTYPE_WEAK_INTERIOR_POINTER:
                ClearIfDead(&slot->object);
                break;
            case HNDTYPE_DEPENDENT:
                if (!Collector::IsPromoted(slot->object))
                {
                    __atomic_store_n(&slot->object, (Object*)nullptr, __ATOMIC_RELEASE);
                    __atomic_store_n(&slot->secondary, (Object*)nullptr, __ATOMIC_RELEASE);
                }
                break;
            default:
                break;
            }
        });
        g_ee->SyncBlockCacheWeakPtrScan(
            [](PTR_UNCHECKED_OBJECTREF slot, uintptr_t*, uintptr_t, uintptr_t) { ClearIfDead((Object**)slot); },
            (uintptr_t)&sc, 0);
        g_ee->SyncBlockCachePromotionsGranted(MaxGeneration);
    }

    struct Tally
    {
        size_t count;
        size_t bytes;
    };

    // Live and dead totals by MethodTable, for MANUALGC_LOG=2.
    struct TypeTally
    {
        MethodTable* mt;
        Tally live;
        Tally dead;
    };
    constexpr size_t TypeTableSize = 64 * 1024;

    void LogCensus()
    {
        TypeTally* types = LogLevel() >= 2 ? (TypeTally*)calloc(TypeTableSize, sizeof(TypeTally)) : nullptr;
        size_t typeCount = 0;
        Tally live = {}, dead = {}, free = {};
        Heap::Walk([&](uint8_t* obj, size_t size, bool isFree) {
            bool marked = !isFree && Heap::IsMarked(obj);
            Tally& tally = isFree ? free : marked ? live : dead;
            tally.count++;
            tally.bytes += size;
            if (types == nullptr || isFree)
                return;
            MethodTable* mt = MethodTableOf(obj);
            size_t i = ((uintptr_t)mt >> 3) % TypeTableSize;
            while (types[i].mt != nullptr && types[i].mt != mt)
                i = (i + 1) % TypeTableSize;
            if (types[i].mt == nullptr)
            {
                if (typeCount == TypeTableSize - 1)
                    return;
                typeCount++;
                types[i].mt = mt;
            }
            Tally& typeTally = marked ? types[i].live : types[i].dead;
            typeTally.count++;
            typeTally.bytes += size;
        });
        for (size_t i = 0; types != nullptr && i < TypeTableSize; i++)
            if (types[i].mt != nullptr)
                LogLine("gc %u type %p live %zu %zu dead %zu %zu", s_collections, (void*)types[i].mt,
                        types[i].live.count, types[i].live.bytes, types[i].dead.count, types[i].dead.bytes);
        ::free(types);
        LogLine("gc %u live %zu objects %zu bytes, dead %zu objects %zu bytes, free %zu bytes, walked %zu of %zu",
                s_collections, live.count, live.bytes, dead.count, dead.bytes, free.bytes,
                live.bytes + dead.bytes + free.bytes, Heap::UsedBytes());
    }

    ViewerLink s_link;
    TypeDescriber s_types;

    void SendSnapshot()
    {
        s_types.Reset();
        s_link.Begin(1);
        s_link.U32(1);
        s_link.U32((uint32_t)getpid());
        s_link.U32(s_collections);
        s_link.U64((uint64_t)Heap::FirstObject());
        s_link.U64((uint64_t)Heap::FirstObject() + Heap::UsedBytes());
        s_link.U8(0);
        s_link.End();

        // Types are sent before the objects that use them, so each batch needs two passes.
        struct Item
        {
            uint8_t* obj;
            size_t size;
            bool isFree;
        };
        static Item batch[4096];
        size_t count = 0;
        uint64_t total = 0;
        auto sendBatch = [&] {
            for (size_t i = 0; i < count; i++)
                if (!batch[i].isFree)
                    s_types.Send(s_link, MethodTableOf(batch[i].obj));
            s_link.Begin(4);
            for (size_t i = 0; i < count; i++)
            {
                const Item& item = batch[i];
                uint64_t flags = item.isFree ? 2 : Heap::IsMarked(item.obj) ? 1 : 0;
                s_link.U64((uint64_t)item.obj);
                s_link.U64(item.size | flags);
                s_link.U64(item.isFree ? 0 : (uint64_t)MethodTableOf(item.obj));
            }
            s_link.End();
            total += count;
            count = 0;
        };
        Heap::Walk([&](uint8_t* obj, size_t size, bool isFree) {
            batch[count++] = {obj, size, isFree};
            if (count == sizeof(batch) / sizeof(batch[0]))
                sendBatch();
        });
        if (count > 0)
            sendBatch();
        s_link.Begin(5);
        s_link.U64(total);
        s_link.End();
        s_link.Flush();
        s_types.Free();
    }

    Collector::Totals s_totals;
    struct
    {
        size_t count;
        size_t bytes;
        size_t refused;
    } s_erased;

    enum EraseStatus : uint8_t
    {
        Freed = 0,
        Alive = 1,
        NotAnObject = 2,
    };

    EraseStatus EraseAt(uint64_t address)
    {
        uint8_t* obj = (uint8_t*)address;
        if (!Heap::Contains(obj) || !Heap::IsObjectStart(obj))
            return NotAnObject;
        if (Heap::IsMarked(obj))
        {
            s_erased.refused++;
            return Alive;
        }
        s_erased.count++;
        s_erased.bytes += ObjectSize(obj);
        Heap::Erase(obj);
        return Freed;
    }

    // Serves the player until they are done or the viewer goes away.
    void Play()
    {
        s_erased = {};
        const uint8_t* payload;
        size_t size;
        while (uint8_t type = s_link.Receive(&payload, &size))
        {
            if (type == 65)
                return;
            uint32_t count = 0;
            if (type == 64 && size >= sizeof(count))
                memcpy(&count, payload, sizeof(count));
            if (type != 64 || size != sizeof(count) + count * sizeof(uint64_t))
            {
                LogLine("bad message %u from the viewer; resuming the app", type);
                return;
            }
            s_link.Begin(6);
            for (uint32_t i = 0; i < count; i++)
            {
                uint64_t address;
                memcpy(&address, payload + sizeof(count) + i * sizeof(address), sizeof(address));
                s_link.U64(address);
                s_link.U8(EraseAt(address));
            }
            s_link.End();
            s_link.Flush();
        }
    }

    void CollectSuspended()
    {
        uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        bool playing = s_link.Connected();
        s_collections++;
        Heap::RetireAllContexts();
        Heap::RebuildStartBits();

        ScanContext sc = MakeScanContext();
        if (playing)
        {
            g_ee->GcStartWork(MaxGeneration, MaxGeneration);
            g_ee->BeforeGcScanRoots(MaxGeneration, false, false);
        }
        s_marking = true;
        MarkFromRoots(sc);
        uint64_t marked = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (LogLevel() > 0)
        {
            LogCensus();
            LogLine("gc %u marked in %.1f ms", s_collections, (double)(marked - start) / 1e6);
        }
        // Without a player this was only a census, and the app must not notice it.
        if (playing)
        {
            g_ee->AfterGcScanRoots(MaxGeneration, MaxGeneration, &sc);
            SeverDead(sc);
            SendSnapshot();
            Play();
            Heap::Sweep();
            s_totals.collections++;
            s_totals.freeBytes = Heap::FreeListBytes();
            s_totals.heapBytes = Heap::UsedBytes() - s_totals.freeBytes;
            s_totals.pauseNanoseconds += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start;
            LogLine("gc %u erased %zu objects %zu bytes, refused %zu, reusable %zu bytes", s_collections,
                    s_erased.count, s_erased.bytes, s_erased.refused, Heap::FreeListBytes());
        }
        s_marking = false;
        if (playing)
            g_ee->GcDone(MaxGeneration);
    }
}

Collector::Totals Collector::GetTotals() { return s_totals; }

bool Collector::IsPromoted(Object* obj)
{
    return !s_marking || !Heap::Contains(obj) || Heap::IsMarked(obj);
}

Object* Collector::FindObject(void* p)
{
    return s_marking && Heap::Contains(p) ? (Object*)Heap::FindObject(p) : nullptr;
}

void Collector::Collect()
{
    // The order the stock workstation GC uses. GC.Collect arrives in cooperative mode
    // (vm/comutilnative.cpp:849); waiting for another collection in that mode would deadlock,
    // because that collection could never suspend this thread.
    bool wasCooperative = g_ee->EnablePreemptiveGC();
    pthread_mutex_lock(&s_collectLock);
    // Without a viewer there is nobody to collect, unless the log wants the census.
    if (!s_link.Connect() && LogLevel() == 0)
    {
        pthread_mutex_unlock(&s_collectLock);
        if (wasCooperative)
            g_ee->DisablePreemptiveGC();
        return;
    }
    g_ee->SuspendEE(SUSPEND_FOR_GC);
    if (wasCooperative)
        g_ee->DisablePreemptiveGC();

    CollectSuspended();

    s_link.Close();
    g_ee->RestartEE(true);
    pthread_mutex_unlock(&s_collectLock);
}
