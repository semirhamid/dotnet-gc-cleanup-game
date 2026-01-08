// Runs collections: finds out what is alive, lets a person play, and frees what they erased.
#pragma once

#include "platform.h"

namespace Collector
{
    void Collect(bool outOfMemory);

    // Called when an allocation fails, to let a person make room. Returns true if it is worth trying
    // the allocation again.
    bool CollectForAllocation();

    // True for objects outside the heap, and for every object outside a collection.
    bool IsPromoted(Object* obj);

    struct Totals
    {
        unsigned collections;
        uint64_t heapBytes;
        uint64_t freeBytes;
        uint64_t pauseNanoseconds;
    };
    Totals GetTotals();

    // The object containing p, or null. Only answers during a collection.
    Object* FindObject(void* p);
}
