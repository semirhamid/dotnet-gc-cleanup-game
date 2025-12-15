// Handle storage. An OBJECTHANDLE is the address of a HandleSlot; the VM reads the object by
// dereferencing the handle directly (vm/gchandleutilities.h:37-54), so `object` must come first
// and slots never move.
#pragma once

#include "platform.h"

struct HandleSlot
{
    Object* object;
    Object* secondary;   // HNDTYPE_DEPENDENT only
    uintptr_t extraInfo; // free-list link while the slot is free
    uint32_t type;       // HandleType, or FreeType
    uint32_t unused;

    static constexpr uint32_t FreeType = 0xFFFFFFFF;
};
static_assert(sizeof(HandleSlot) == 32 && offsetof(HandleSlot, object) == 0, "handle slot layout");

class HandleStore final : public IGCHandleStore  // gcinterface.h:578-594
{
public:
    void Uproot() override;
    bool ContainsHandle(OBJECTHANDLE handle) override;
    OBJECTHANDLE CreateHandleOfType(Object* object, HandleType type) override;
    OBJECTHANDLE CreateHandleOfType(Object* object, HandleType type, int heapToAffinitizeTo) override;
    OBJECTHANDLE CreateHandleWithExtraInfo(Object* object, HandleType type, void* pExtraInfo) override;
    OBJECTHANDLE CreateDependentHandle(Object* primary, Object* secondary) override;
};

class HandleManager final : public IGCHandleManager  // gcinterface.h:596-634
{
public:
    bool Initialize() override;
    void Shutdown() override;
    IGCHandleStore* GetGlobalHandleStore() override;
    IGCHandleStore* CreateHandleStore() override;
    void DestroyHandleStore(IGCHandleStore* store) override;
    OBJECTHANDLE CreateGlobalHandleOfType(Object* object, HandleType type) override;
    OBJECTHANDLE CreateDuplicateHandle(OBJECTHANDLE handle) override;
    void DestroyHandleOfType(OBJECTHANDLE handle, HandleType type) override;
    void DestroyHandleOfUnknownType(OBJECTHANDLE handle) override;
    void SetExtraInfoForHandle(OBJECTHANDLE handle, HandleType type, void* pExtraInfo) override;
    void* GetExtraInfoFromHandle(OBJECTHANDLE handle) override;
    void StoreObjectInHandle(OBJECTHANDLE handle, Object* object) override;
    bool StoreObjectInHandleIfNull(OBJECTHANDLE handle, Object* object) override;
    void SetDependentHandleSecondary(OBJECTHANDLE handle, Object* object) override;
    Object* GetDependentHandleSecondary(OBJECTHANDLE handle) override;
    Object* InterlockedCompareExchangeObjectInHandle(OBJECTHANDLE handle, Object* object, Object* comparandObject) override;
    HandleType HandleFetchType(OBJECTHANDLE handle) override;
    void TraceRefCountedHandles(HANDLESCANPROC callback, uintptr_t param1, uintptr_t param2) override;
};

namespace Handles
{
    static constexpr size_t ChunkBytes = 64 * 1024;

    struct Chunk
    {
        Chunk* next;
        uint8_t pad[sizeof(HandleSlot) - sizeof(Chunk*)];
        HandleSlot slots[(ChunkBytes / sizeof(HandleSlot)) - 1];
    };
    static_assert(sizeof(Chunk) == ChunkBytes, "handle chunk layout");
    static constexpr size_t SlotsPerChunk = sizeof(Chunk::slots) / sizeof(HandleSlot);

    Chunk* FirstChunk();

    // Calls fn(HandleSlot*, uint32_t type) for every slot holding an object. Takes no lock, so fn
    // may call into the EE. Chunks are never freed. Threads in preemptive mode can still create
    // and destroy handles during a collection, but only with null objects, which are skipped.
    template <typename Fn>
    void ForEach(Fn fn)
    {
        for (Chunk* chunk = FirstChunk(); chunk != nullptr; chunk = chunk->next)
        {
            for (HandleSlot& slot : chunk->slots)
            {
                uint32_t type = __atomic_load_n(&slot.type, __ATOMIC_ACQUIRE);
                if (type != HandleSlot::FreeType && __atomic_load_n(&slot.object, __ATOMIC_ACQUIRE) != nullptr)
                    fn(&slot, type);
            }
        }
    }

    size_t LiveCount();
    HandleManager* CreateManager();
}
