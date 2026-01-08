#include "typeinfo.h"
#include "protocol.h"

#include <dlfcn.h>
#include <stdlib.h>

namespace
{
    // Exported by coreclr as DotNetRuntimeContractDescriptor
    // (docs/design/datacontracts/contract-descriptor.md in dotnet/runtime).
    struct ContractDescriptor
    {
        uint64_t magic;
        uint32_t flags;
        uint32_t jsonSize;
        const char* json;
        uint32_t pointerCount;
        uint32_t pad;
        uintptr_t* pointers;
    };

    struct Offsets
    {
        bool valid;
        size_t flags, flags2, eeClass, module, perInstInfo;
        size_t numDicts, numTypeArgs, dictInfoSize;
        size_t arrayRank, modulePath, typeDescFlags, typeDescArg;
    };

    // Finds "type":{..."field":N...} in the descriptor's compact JSON. Nested values are only
    // [offset,"type"] pairs, so a type's object ends at the first closing brace.
    bool FindOffset(const char* json, const char* type, const char* field, size_t* offset)
    {
        char key[96];
        snprintf(key, sizeof(key), "\"%s\":{", type);
        const char* start = strstr(json, key);
        if (start == nullptr)
            return false;
        start += strlen(key);
        const char* end = strchr(start, '}');
        snprintf(key, sizeof(key), "\"%s\":", field);
        const char* at = strstr(start, key);
        if (at == nullptr || end == nullptr || at > end)
            return false;
        at += strlen(key);
        if (*at == '[')
            at++;
        char* stop;
        *offset = (size_t)strtoul(at, &stop, 10);
        return stop != at;
    }

    const Offsets& GetOffsets()
    {
        static const Offsets offsets = [] {
            Offsets o = {};
            // Any address inside coreclr leads to its handle; the EE object's vtable is one.
            Dl_info info;
            if (dladdr(*(void**)g_ee, &info) == 0)
                return o;
            void* coreclr = dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD);
            const ContractDescriptor* descriptor =
                coreclr == nullptr ? nullptr : (const ContractDescriptor*)dlsym(coreclr, "DotNetRuntimeContractDescriptor");
            const uint64_t magic = 0x0043414443434e44;  // "DNCCDAC\0"
            if (descriptor == nullptr || descriptor->magic != magic)
                return o;
            const char* json = descriptor->json;
            o.dictInfoSize = sizeof(void*);  // GenericsDictInfo sits just before PerInstInfo (vm/methodtable.h:282-296)
            o.valid = FindOffset(json, "MethodTable", "MTFlags", &o.flags) &&
                      FindOffset(json, "MethodTable", "MTFlags2", &o.flags2) &&
                      FindOffset(json, "MethodTable", "EEClassOrCanonMT", &o.eeClass) &&
                      FindOffset(json, "MethodTable", "Module", &o.module) &&
                      FindOffset(json, "MethodTable", "PerInstInfo", &o.perInstInfo) &&
                      FindOffset(json, "GenericsDictInfo", "NumDicts", &o.numDicts) &&
                      FindOffset(json, "GenericsDictInfo", "NumTypeArgs", &o.numTypeArgs) &&
                      FindOffset(json, "ArrayClass", "Rank", &o.arrayRank) &&
                      FindOffset(json, "Module", "Path", &o.modulePath) &&
                      FindOffset(json, "TypeDesc", "TypeAndFlags", &o.typeDescFlags) &&
                      FindOffset(json, "ParamTypeDesc", "TypeArg", &o.typeDescArg);
            if (!o.valid)
                LogLine("the runtime's data descriptor lacks fields; type names are off");
            return o;
        }();
        return offsets;
    }

    template <typename T>
    T Read(uintptr_t address) { return *(T*)address; }

    // MethodTable flags (vm/methodtable.h:3666-3744).
    constexpr uint32_t HasComponentSize = 0x80000000;
    constexpr uint32_t CategoryArrayMask = 0x000C0000, CategoryArray = 0x00080000, SzArray = 0x00020000;
    constexpr uint32_t GenericsMask = 0x30;

    constexpr uint8_t ElementPtr = 0x0f, ElementByRef = 0x10;

    constexpr int MaxTypeDepth = 32;

    size_t ToUtf8(const char16_t* text, char* out, size_t capacity)
    {
        size_t n = 0;
        while (*text != 0 && n + 4 <= capacity)
        {
            uint32_t c = *text++;
            if (c >= 0xD800 && c < 0xDC00 && *text >= 0xDC00 && *text < 0xE000)
                c = 0x10000 + ((c - 0xD800) << 10) + (*text++ - 0xDC00u);
            if (c < 0x80)
                out[n++] = (char)c;
            else if (c < 0x800)
            {
                out[n++] = (char)(0xC0 | c >> 6);
                out[n++] = (char)(0x80 | (c & 0x3F));
            }
            else if (c < 0x10000)
            {
                out[n++] = (char)(0xE0 | c >> 12);
                out[n++] = (char)(0x80 | (c >> 6 & 0x3F));
                out[n++] = (char)(0x80 | (c & 0x3F));
            }
            else
            {
                out[n++] = (char)(0xF0 | c >> 18);
                out[n++] = (char)(0x80 | (c >> 12 & 0x3F));
                out[n++] = (char)(0x80 | (c >> 6 & 0x3F));
                out[n++] = (char)(0x80 | (c & 0x3F));
            }
        }
        return n;
    }
}

void TypeDescriber::Reset()
{
    Free();
    m_capacity = 4096;
    m_entries = (Entry*)calloc(m_capacity, sizeof(Entry));
    if (m_entries == nullptr)
        Fatal("out of memory for the type table");
}

void TypeDescriber::Free()
{
    ::free(m_entries);
    m_entries = nullptr;
    m_capacity = m_count = 0;
    m_modules = 0;
}

TypeDescriber::Entry* TypeDescriber::Find(uintptr_t key, bool* added)
{
    if (m_count * 2 >= m_capacity)
    {
        size_t oldCapacity = m_capacity;
        Entry* old = m_entries;
        m_capacity *= 2;
        m_entries = (Entry*)calloc(m_capacity, sizeof(Entry));
        if (m_entries == nullptr)
            Fatal("out of memory for the type table");
        for (size_t i = 0; i < oldCapacity; i++)
        {
            if (old[i].key == 0)
                continue;
            size_t j = (old[i].key >> 3) & (m_capacity - 1);
            while (m_entries[j].key != 0)
                j = (j + 1) & (m_capacity - 1);
            m_entries[j] = old[i];
        }
        ::free(old);
    }
    size_t i = (key >> 3) & (m_capacity - 1);
    while (m_entries[i].key != 0 && m_entries[i].key != key)
        i = (i + 1) & (m_capacity - 1);
    *added = m_entries[i].key == 0;
    if (*added)
    {
        m_entries[i].key = key;
        m_count++;
    }
    return &m_entries[i];
}

uint32_t TypeDescriber::SendModule(ViewerLink& link, uintptr_t module)
{
    bool added;
    Entry* entry = Find(module, &added);
    if (added)
    {
        entry->id = ++m_modules;
        char path[4096];
        const char16_t* text = Read<const char16_t*>(module + GetOffsets().modulePath);
        size_t length = text == nullptr ? 0 : ToUtf8(text, path, sizeof(path));
        link.Begin(Protocol::Module);
        link.U32(entry->id);
        link.Append(path, length);
        link.End();
    }
    return entry->id;
}

void TypeDescriber::SendHandle(ViewerLink& link, uintptr_t handle, int depth)
{
    if (handle == 0)
        return;
    bool added;
    Find(handle, &added);
    if (!added)
        return;

    const Offsets& o = GetOffsets();
    if (!o.valid || depth > MaxTypeDepth)
    {
        link.Begin(Protocol::Type);
        link.U64(handle);
        link.U8(Protocol::OtherType);
        link.U8(0);
        link.End();
        return;
    }

    if (handle & 2)  // a TypeDesc, not a MethodTable (vm/typehandle.h:167-170)
    {
        uint8_t element = (uint8_t)Read<uint32_t>(handle - 2 + o.typeDescFlags);
        bool hasElement = element == ElementPtr || element == ElementByRef;
        uintptr_t inner = hasElement ? Read<uintptr_t>(handle - 2 + o.typeDescArg) : 0;
        SendHandle(link, inner, depth + 1);
        link.Begin(Protocol::Type);
        link.U64(handle);
        if (hasElement)
        {
            link.U8(element == ElementPtr ? Protocol::PointerType : Protocol::ByRefType);
            link.U64(inner);
        }
        else
        {
            link.U8(Protocol::OtherType);
            link.U8(element);
        }
        link.End();
        return;
    }

    uint32_t flags = Read<uint32_t>(handle + o.flags);
    if ((flags & CategoryArrayMask) == CategoryArray)
    {
        uintptr_t element = Read<uintptr_t>(handle + o.perInstInfo);  // m_ElementTypeHnd shares the slot (vm/methodtable.h:3947)
        bool isVector = (flags & SzArray) != 0;
        uint8_t rank = 1;
        if (!isVector)
        {
            uintptr_t eeClass = Read<uintptr_t>(handle + o.eeClass);
            if (eeClass & 1)  // points at the canonical MethodTable instead (vm/methodtable.h:3920)
                eeClass = Read<uintptr_t>(eeClass - 1 + o.eeClass);
            rank = Read<uint8_t>(eeClass + o.arrayRank);
        }
        SendHandle(link, element, depth + 1);
        link.Begin(Protocol::Type);
        link.U64(handle);
        link.U8(Protocol::ArrayType);
        link.U8(rank);
        link.U8(isVector);
        link.U64(element);
        link.End();
        return;
    }

    uintptr_t* args = nullptr;
    uint16_t argCount = 0;
    uintptr_t perInstInfo = Read<uintptr_t>(handle + o.perInstInfo);
    if (!(flags & HasComponentSize) && (flags & GenericsMask) != 0 && perInstInfo != 0)
    {
        uintptr_t dictInfo = perInstInfo - o.dictInfoSize;
        uint16_t dicts = Read<uint16_t>(dictInfo + o.numDicts);
        argCount = Read<uint16_t>(dictInfo + o.numTypeArgs);
        // The last dictionary is the type's own; it starts with the type arguments.
        args = Read<uintptr_t*>(perInstInfo + (dicts - 1u) * sizeof(void*));
        for (uint16_t i = 0; i < argCount; i++)
            SendHandle(link, args[i], depth + 1);
    }
    uint32_t module = SendModule(link, Read<uintptr_t>(handle + o.module));
    link.Begin(Protocol::Type);
    link.U64(handle);
    link.U8(Protocol::NamedType);
    link.U32(module);
    link.U32(0x02000000 | Read<uint32_t>(handle + o.flags2) >> 8);  // vm/methodtable.h:2853-2862
    link.U16(argCount);
    for (uint16_t i = 0; i < argCount; i++)
        link.U64(args[i]);
    link.End();
}
