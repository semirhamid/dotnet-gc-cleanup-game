#pragma once

#include "platform.h"
#include "gcdesc.h"

inline MethodTable* MethodTableOf(const void* obj)
{
    // The low bits are reserved for GCs (gcenv.object.h GetGCSafeMethodTable). This one uses none.
    return (MethodTable*)(*(uintptr_t*)obj & ~(uintptr_t)7);
}

inline size_t ObjectSize(const void* obj)
{
    MethodTable* mt = MethodTableOf(obj);
    size_t size = mt->GetBaseSize();
    if (mt->HasComponentSize())
        size += (size_t)mt->RawGetComponentSize() * ((ArrayBase*)obj)->GetNumComponents();
    return (size + 7) & ~(size_t)7;
}

// Calls fn(Object**) for every reference slot in obj. The layout is described in gcdesc.h: runs
// of references for ordinary objects and reference arrays, or a repeating pattern of
// (pointer count, bytes to skip) for arrays of structs. A negative series count means the latter.
template <typename Fn>
inline void ForEachReference(uint8_t* obj, size_t size, Fn fn)
{
    MethodTable* mt = MethodTableOf(obj);
    if (!mt->ContainsGCPointers())
        return;
    CGCDesc* map = CGCDesc::GetCGCDescFromMT(mt);
    CGCDescSeries* series = map->GetHighestSeries();
    ptrdiff_t count = (ptrdiff_t)map->GetNumSeries();

    if (count >= 0)
    {
        for (CGCDescSeries* last = map->GetLowestSeries(); series >= last; series--)
        {
            Object** slot = (Object**)(obj + series->GetSeriesOffset());
            // Series sizes are stored with the base size subtracted.
            Object** end = (Object**)((uint8_t*)slot + series->GetSeriesSize() + size);
            for (; slot < end; slot++)
                fn(slot);
        }
        return;
    }

    Object** slot = (Object**)(obj + series->GetSeriesOffset());
    while ((uint8_t*)slot < obj + size - sizeof(ObjHeader))
    {
        for (ptrdiff_t i = 0; i > count; i--)
        {
            val_serie_item item = series->val_serie[i];
            for (HALF_SIZE_T n = 0; n < item.nptrs; n++)
                fn(slot++);
            slot = (Object**)((uint8_t*)slot + item.skip);
        }
    }
}
