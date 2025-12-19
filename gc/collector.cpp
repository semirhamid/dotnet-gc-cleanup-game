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

    void MarkFromRoots()
    {
        ScanContext sc;  // gcinterface.h:1121
        sc.promotion = true;
        sc.thread_number = 0;
        sc.thread_count = 1;
        g_ee->GcScanRoots(Promote, MaxGeneration, MaxGeneration, &sc);
        MarkHandles(&sc);
        MarkDependentHandles();
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

    void CollectSuspended()
    {
        s_collections++;
        uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        Heap::RetireAllContexts();
        Heap::RebuildStartBits();
        s_marking = true;
        MarkFromRoots();
        uint64_t marked = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (LogLevel() > 0)
        {
            LogCensus();
            LogLine("gc %u marked in %.1f ms", s_collections, (double)(marked - start) / 1e6);
        }
        if (s_link.Connected())
            SendSnapshot();
        s_marking = false;
    }
}

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
