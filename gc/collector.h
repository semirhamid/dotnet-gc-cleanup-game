// Finding out what is alive. Everything here runs with the EE suspended.
#pragma once

#include "platform.h"

namespace Collector
{
    // Runs one collection on the calling thread.
    void Collect();

    // True for objects outside the heap, and for every object outside a collection.
    bool IsPromoted(Object* obj);

    // The object containing p, or null. Only answers during a collection.
    Object* FindObject(void* p);
}
