// Finding out what is alive. Everything here runs with the EE suspended.
#pragma once

#include "platform.h"

namespace Collector
{
    // Runs one collection on the calling thread.
    void Collect(bool outOfMemory);

    // For an allocation that failed: gives a person the chance to make room. Returns true if it is
    // worth trying again.
    bool CollectForAllocation();

    // True for objects outside the heap, and for every object outside a collection.
    bool IsPromoted(Object* obj);

    // Collections a person has finished, and what the last one left behind.
    struct Totals
    {
        unsigned collections;
        uint64_t heapBytes;   // live and unerased dead objects
        uint64_t freeBytes;
        uint64_t pauseNanoseconds;  // all collections together
    };
    Totals GetTotals();

    // The object containing p, or null. Only answers during a collection.
    Object* FindObject(void* p);
}
