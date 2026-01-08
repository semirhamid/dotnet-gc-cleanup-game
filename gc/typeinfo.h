// Describes MethodTables to the viewer by module and metadata token. Uses the field offsets in
// the data descriptor that coreclr exports for diagnostic tools, so nothing is hard coded.
#pragma once

#include "link.h"

class TypeDescriber
{
public:
    void Reset();
    void Free();

    void Send(ViewerLink& link, MethodTable* mt) { SendHandle(link, (uintptr_t)mt, 0); }

private:
    struct Entry
    {
        uintptr_t key;
        uint32_t id;
    };

    void SendHandle(ViewerLink& link, uintptr_t handle, int depth);
    uint32_t SendModule(ViewerLink& link, uintptr_t module);
    Entry* Find(uintptr_t key, bool* added);

    Entry* m_entries = nullptr;
    size_t m_capacity = 0;
    size_t m_count = 0;
    uint32_t m_modules = 0;
};
