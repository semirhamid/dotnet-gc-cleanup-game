#include "collector.h"
#include "handles.h"
#include "heap.h"
#include "link.h"
#include "protocol.h"
#include "typeinfo.h"

#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

namespace
{
    constexpr int MaxGeneration = 2;

    pthread_mutex_t s_collectLock = PTHREAD_MUTEX_INITIALIZER;
    bool s_marking;
    unsigned s_collections;

    // Set once hardcore mode has freed something alive. From then on, dangling references are
    // expected and must not be treated as corruption.
    bool s_heapIsDamaged;

    // Read from the environment so that only whoever started the app can allow it. Anybody can
    // connect to the socket.
    bool HardcoreAllowed()
    {
        static const bool allowed = [] {
            const char* value = getenv("MANUALGC_HARDCORE");
            return value != nullptr && strcmp(value, "1") == 0;
        }();
        return allowed;
    }

    // Objects marked but not yet scanned. Recursing instead would overflow the thread's stack on a
    // long linked list.
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
        // Skips null, and frozen objects, which live elsewhere and hold no references into this heap.
        if (!Heap::Contains(obj))
            return;
        if (!Heap::IsObjectStart(obj))
        {
            if (s_heapIsDamaged)
                return;
            Fatal("%s refers to %p, which is not an object", source, (void*)obj);
        }
        if (!Heap::TestAndMark(obj))
            s_markStack.Push((uint8_t*)obj);
    }

    enum RootKind : uint8_t
    {
        StackRoot = 0,
        HandleRoot = 1,
        PinnedRoot = 2,
        RefCountedRoot = 3,
    };
    struct Root
    {
        Object* obj;
        RootKind kind;
    };
    struct RootList
    {
        Root* items;
        size_t count;
        size_t capacity;

        void Add(Object* obj, RootKind kind)
        {
            if (count == capacity)
            {
                capacity = capacity == 0 ? 4096 : capacity * 2;
                items = (Root*)realloc(items, capacity * sizeof(Root));
                if (items == nullptr)
                    Fatal("out of memory growing the root list to %zu entries", capacity);
            }
            items[count++] = {obj, kind};
        }
    };
    RootList s_roots;
    RootKind s_rootKind;
    bool s_recordRoots;
    bool s_outOfMemory;

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
        if (s_recordRoots)
            s_roots.Add(obj, s_rootKind);
        Mark(obj, "a root");
        Drain();
    }

    void MarkHandles(ScanContext* sc)
    {
        Handles::ForEach([=](HandleSlot* slot, uint32_t type) {
            s_rootKind = type == HNDTYPE_PINNED ? PinnedRoot : type == HNDTYPE_REFCOUNTED ? RefCountedRoot : HandleRoot;
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
            default:
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
        ScanContext sc;
        sc.promotion = true;
        sc.thread_number = 0;
        sc.thread_count = 1;
        return sc;
    }

    void MarkFromRoots(ScanContext& sc)
    {
        s_rootKind = StackRoot;
        g_ee->GcScanRoots(Promote, MaxGeneration, MaxGeneration, &sc);
        MarkHandles(&sc);
        MarkDependentHandles();
    }

    void ClearIfDead(Object** slot)
    {
        if (!Collector::IsPromoted(*slot))
            __atomic_store_n(slot, (Object*)nullptr, __ATOMIC_RELEASE);
    }

    // Cuts every way the app still has of reaching a dead object. Once this has run, nothing can
    // bring a dead object back, so it does not matter if dead objects point at erased ones.
    // The stock GC handles short and long weak handles separately because finalization happens in
    // between. There is no finalization here, so one pass does both.
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

    constexpr size_t EntriesPerMessage = 4096;

    ViewerLink s_link;
    TypeDescriber s_types;

    struct ReferenceWriter
    {
        size_t inMessage = 0;

        void Add(uint8_t* from, uint8_t* to)
        {
            if (inMessage == EntriesPerMessage)
                Finish();
            if (inMessage++ == 0)
                s_link.Begin(Protocol::References);
            s_link.U64((uint64_t)from);
            s_link.U64((uint64_t)to);
        }

        void Finish()
        {
            if (inMessage > 0)
                s_link.End();
            inMessage = 0;
        }
    };

    void SendGraph()
    {
        ReferenceWriter writer;
        Heap::Walk([&](uint8_t* obj, size_t size, bool isFree) {
            if (isFree)
                return;
            ForEachReference(obj, size, [&](Object** slot) {
                // A dead object may still point at something erased earlier.
                uint8_t* target = (uint8_t*)*slot;
                if (target != obj && Heap::Contains(target) && Heap::IsObjectStart(target))
                    writer.Add(obj, target);
            });
            if (Heap::IsMarked(obj) && MethodTableOf(obj)->Collectible())
            {
                uint8_t* allocator = g_ee->GetLoaderAllocatorObjectForGC((Object*)obj);
                if (allocator != obj && Heap::Contains(allocator))
                    writer.Add(obj, allocator);
            }
        });
        Handles::ForEach([&](HandleSlot* slot, uint32_t type) {
            if (type != HNDTYPE_DEPENDENT || !Heap::Contains(slot->secondary) || !Collector::IsPromoted(slot->object))
                return;
            if (Heap::Contains(slot->object))
                writer.Add((uint8_t*)slot->object, (uint8_t*)slot->secondary);
            else
                s_roots.Add(slot->secondary, HandleRoot);  // a frozen primary never dies
        });
        writer.Finish();

        for (size_t i = 0; i < s_roots.count;)
        {
            s_link.Begin(Protocol::Roots);
            for (size_t end = min(i + EntriesPerMessage, s_roots.count); i < end; i++)
            {
                s_link.U64((uint64_t)s_roots.items[i].obj);
                s_link.U8(s_roots.items[i].kind);
            }
            s_link.End();
        }
    }

    void SendSnapshot()
    {
        s_types.Reset();
        s_link.Begin(Protocol::Hello);
        s_link.U32(Protocol::Version);
        s_link.U32((uint32_t)getpid());
        s_link.U32(s_collections);
        s_link.U64((uint64_t)Heap::FirstObject());
        s_link.U64((uint64_t)Heap::FirstObject() + Heap::UsedBytes());
        s_link.U8((uint8_t)((HardcoreAllowed() ? Protocol::HardcoreAllowed : 0) | (s_outOfMemory ? Protocol::OutOfMemory : 0)));
        s_link.End();

        // The viewer must know a type before it sees an object of it, so each batch is gone
        // through twice: once for the types, once for the objects.
        struct Item
        {
            uint8_t* obj;
            size_t size;
            bool isFree;
        };
        static Item batch[EntriesPerMessage];
        size_t count = 0;
        uint64_t total = 0;
        auto sendBatch = [&] {
            for (size_t i = 0; i < count; i++)
                if (!batch[i].isFree)
                    s_types.Send(s_link, MethodTableOf(batch[i].obj));
            s_link.Begin(Protocol::Objects);
            for (size_t i = 0; i < count; i++)
            {
                const Item& item = batch[i];
                uint64_t flags = item.isFree ? Protocol::FreeSpace : Heap::IsMarked(item.obj) ? Protocol::Live : 0;
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
            if (count == EntriesPerMessage)
                sendBatch();
        });
        if (count > 0)
            sendBatch();
        SendGraph();
        s_link.Begin(Protocol::SnapshotEnd);
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
        size_t destroyed;
    } s_erased;

    enum EraseStatus : uint8_t
    {
        Freed = 0,
        Alive = 1,
        NotAnObject = 2,
        Destroyed = 3,
    };

    EraseStatus EraseAt(uint64_t address, bool force)
    {
        uint8_t* obj = (uint8_t*)address;
        if (!Heap::Contains(obj) || !Heap::IsObjectStart(obj))
            return NotAnObject;
        bool alive = Heap::IsMarked(obj);
        if (alive && !force)
        {
            s_erased.refused++;
            return Alive;
        }
        s_erased.count++;
        s_erased.bytes += ObjectSize(obj);
        Heap::Erase(obj);
        if (alive)
        {
            s_erased.destroyed++;
            s_heapIsDamaged = true;
            return Destroyed;
        }
        return Freed;
    }

    void Play()
    {
        s_erased = {};
        const uint8_t* payload;
        size_t size;
        while (uint8_t type = s_link.Receive(&payload, &size))
        {
            if (type == Protocol::Done)
                return;
            uint8_t flags = 0;
            uint32_t count = 0;
            constexpr size_t header = sizeof(flags) + sizeof(count);
            if (type == Protocol::Erase && size >= header)
            {
                flags = payload[0];
                memcpy(&count, payload + sizeof(flags), sizeof(count));
            }
            if (type != Protocol::Erase || size != header + count * sizeof(uint64_t))
            {
                LogLine("bad message %u from the viewer; resuming the app", type);
                return;
            }
            bool force = (flags & Protocol::EraseLiveToo) != 0 && HardcoreAllowed();
            s_link.Begin(Protocol::EraseResults);
            for (uint32_t i = 0; i < count; i++)
            {
                uint64_t address;
                memcpy(&address, payload + header + i * sizeof(address), sizeof(address));
                s_link.U64(address);
                s_link.U8(EraseAt(address, force));
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
        s_roots.count = 0;
        s_recordRoots = playing;
        MarkFromRoots(sc);
        uint64_t marked = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (LogLevel() > 0)
        {
            LogCensus();
            LogLine("gc %u marked in %.1f ms", s_collections, (double)(marked - start) / 1e6);
        }
        // With nobody playing, this was only a census for the log. Change nothing.
        if (playing)
        {
            g_ee->AfterGcScanRoots(MaxGeneration, MaxGeneration, &sc);
            SeverDead(sc);
            SendSnapshot();
            Play();
            if (!s_link.Connected())
                LogLine("gc %u resuming the app without a Done", s_collections);
            if (s_erased.destroyed > 0)
            {
                SeverDead(sc);  // what was destroyed may still have weak references to it
                LogLine("gc %u destroyed %zu live objects; expect the app to crash", s_collections, s_erased.destroyed);
            }
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

void Collector::Collect(bool outOfMemory)
{
    // Same order as the stock workstation GC. GC.Collect arrives in cooperative mode
    // (vm/comutilnative.cpp:849). Waiting for the lock in that mode would deadlock, because the
    // collection that holds it could never suspend this thread.
    bool wasCooperative = g_ee->EnablePreemptiveGC();
    pthread_mutex_lock(&s_collectLock);
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

    s_outOfMemory = outOfMemory;
    CollectSuspended();

    s_link.Close();
    g_ee->RestartEE(true);
    pthread_mutex_unlock(&s_collectLock);
}

bool Collector::CollectForAllocation()
{
    // Threads tend to run out of memory together, and one collection is enough for all of them.
    static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
    // After a round that made hardly any room, allocations fail until this time, so that the app
    // gets to see the OutOfMemoryException instead of freezing again at once.
    static uint64_t s_uselessUntil;
    constexpr uint64_t UselessNanoseconds = 5000000000;
    unsigned before = s_totals.collections;

    bool wasCooperative = g_ee->EnablePreemptiveGC();
    pthread_mutex_lock(&s_lock);
    bool retry = s_totals.collections != before;
    if (!retry && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) >= s_uselessUntil)
    {
        if (wasCooperative)
            g_ee->DisablePreemptiveGC();
        Collect(true);
        if (wasCooperative)
            g_ee->EnablePreemptiveGC();
        retry = s_totals.collections != before;
        if (!retry || s_erased.bytes < Heap::ReservedBytes() / 64)
            s_uselessUntil = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + UselessNanoseconds;
    }
    pthread_mutex_unlock(&s_lock);
    if (wasCooperative)
        g_ee->DisablePreemptiveGC();
    return retry;
}
