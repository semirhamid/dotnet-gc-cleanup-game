#include "handles.h"

#include <pthread.h>
#include <sys/mman.h>

namespace
{
    constexpr size_t ChunkBytes = 64 * 1024;

    struct HandleChunk
    {
        HandleChunk* next;
        uint8_t pad[sizeof(HandleSlot) - sizeof(HandleChunk*)];
        HandleSlot slots[(ChunkBytes / sizeof(HandleSlot)) - 1];
    };
    static_assert(sizeof(HandleChunk) == ChunkBytes, "handle chunk layout");
    constexpr size_t SlotsPerChunk = sizeof(HandleChunk::slots) / sizeof(HandleSlot);

    pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
    HandleChunk* s_chunks;
    HandleSlot* s_free;
    size_t s_live;

    HandleSlot* Slot(OBJECTHANDLE handle) { return (HandleSlot*)handle; }

    bool AddChunk()  // s_lock held
    {
        void* p = mmap(nullptr, ChunkBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED)
            return false;
        HandleChunk* chunk = (HandleChunk*)p;
        chunk->next = s_chunks;
        s_chunks = chunk;
        for (size_t i = SlotsPerChunk; i-- > 0;)
        {
            chunk->slots[i].type = HandleSlot::FreeType;
            chunk->slots[i].extraInfo = (uintptr_t)s_free;
            s_free = &chunk->slots[i];
        }
        return true;
    }

    OBJECTHANDLE Create(Object* object, HandleType type, Object* secondary, void* extraInfo)
    {
        pthread_mutex_lock(&s_lock);
        if (s_free == nullptr && !AddChunk())
        {
            pthread_mutex_unlock(&s_lock);
            return nullptr;
        }
        HandleSlot* slot = s_free;
        s_free = (HandleSlot*)slot->extraInfo;
        s_live++;
        slot->secondary = secondary;
        slot->extraInfo = (uintptr_t)extraInfo;
        slot->type = (uint32_t)type;
        __atomic_store_n(&slot->object, object, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&s_lock);
        return (OBJECTHANDLE)slot;
    }

    void Destroy(OBJECTHANDLE handle)
    {
        if (handle == nullptr)
            return;
        HandleSlot* slot = Slot(handle);
        pthread_mutex_lock(&s_lock);
        // A double free would put the slot on the list twice and corrupt later handles.
        if (slot->type == HandleSlot::FreeType)
        {
            pthread_mutex_unlock(&s_lock);
            LogLine("double free of handle %p ignored", (void*)handle);
            return;
        }
        __atomic_store_n(&slot->object, (Object*)nullptr, __ATOMIC_RELEASE);
        slot->secondary = nullptr;
        slot->type = HandleSlot::FreeType;
        slot->extraInfo = (uintptr_t)s_free;
        s_free = slot;
        s_live--;
        pthread_mutex_unlock(&s_lock);
    }

    // Constructed with placement new and never destroyed, so no destructor runs at process exit
    // while runtime threads may still be using handles.
    alignas(HandleStore) uint8_t s_storeMemory[sizeof(HandleStore)];
    alignas(HandleManager) uint8_t s_managerMemory[sizeof(HandleManager)];
    HandleStore* s_store;
}

void Handles::ForEach(SlotFn fn, void* context)
{
    pthread_mutex_lock(&s_lock);
    for (HandleChunk* chunk = s_chunks; chunk != nullptr; chunk = chunk->next)
        for (size_t i = 0; i < SlotsPerChunk; i++)
            if (chunk->slots[i].type != HandleSlot::FreeType)
                fn(&chunk->slots[i], context);
    pthread_mutex_unlock(&s_lock);
}

size_t Handles::LiveCount()
{
    pthread_mutex_lock(&s_lock);
    size_t live = s_live;
    pthread_mutex_unlock(&s_lock);
    return live;
}

HandleManager* Handles::CreateManager()
{
    return new (s_managerMemory) HandleManager();
}

void HandleStore::Uproot() {}

bool HandleStore::ContainsHandle(OBJECTHANDLE handle)
{
    bool found = false;
    pthread_mutex_lock(&s_lock);
    for (HandleChunk* chunk = s_chunks; chunk != nullptr && !found; chunk = chunk->next)
        found = Slot(handle) >= chunk->slots && Slot(handle) < chunk->slots + SlotsPerChunk;
    pthread_mutex_unlock(&s_lock);
    return found;
}

OBJECTHANDLE HandleStore::CreateHandleOfType(Object* object, HandleType type)
{
    return Create(object, type, nullptr, nullptr);
}

OBJECTHANDLE HandleStore::CreateHandleOfType(Object* object, HandleType type, int)
{
    return Create(object, type, nullptr, nullptr);
}

OBJECTHANDLE HandleStore::CreateHandleWithExtraInfo(Object* object, HandleType type, void* pExtraInfo)
{
    return Create(object, type, nullptr, pExtraInfo);
}

OBJECTHANDLE HandleStore::CreateDependentHandle(Object* primary, Object* secondary)
{
    return Create(primary, HNDTYPE_DEPENDENT, secondary, nullptr);
}

bool HandleManager::Initialize()
{
    s_store = new (s_storeMemory) HandleStore();
    return true;
}

void HandleManager::Shutdown() {}

IGCHandleStore* HandleManager::GetGlobalHandleStore() { return s_store; }

// The VM only ever asks for the global store (vm/appdomain.cpp:1697).
IGCHandleStore* HandleManager::CreateHandleStore() { return nullptr; }
void HandleManager::DestroyHandleStore(IGCHandleStore*) {}

OBJECTHANDLE HandleManager::CreateGlobalHandleOfType(Object* object, HandleType type)
{
    return Create(object, type, nullptr, nullptr);
}

// The stock GC also creates the copy as HNDTYPE_DEFAULT, whatever the source type was.
OBJECTHANDLE HandleManager::CreateDuplicateHandle(OBJECTHANDLE handle)
{
    return Create(__atomic_load_n(&Slot(handle)->object, __ATOMIC_ACQUIRE), HNDTYPE_DEFAULT, nullptr, nullptr);
}

void HandleManager::DestroyHandleOfType(OBJECTHANDLE handle, HandleType) { Destroy(handle); }
void HandleManager::DestroyHandleOfUnknownType(OBJECTHANDLE handle) { Destroy(handle); }

void HandleManager::SetExtraInfoForHandle(OBJECTHANDLE handle, HandleType, void* pExtraInfo)
{
    __atomic_store_n(&Slot(handle)->extraInfo, (uintptr_t)pExtraInfo, __ATOMIC_RELEASE);
}

void* HandleManager::GetExtraInfoFromHandle(OBJECTHANDLE handle)
{
    return (void*)__atomic_load_n(&Slot(handle)->extraInfo, __ATOMIC_ACQUIRE);
}

void HandleManager::StoreObjectInHandle(OBJECTHANDLE handle, Object* object)
{
    __atomic_store_n(&Slot(handle)->object, object, __ATOMIC_RELEASE);
}

bool HandleManager::StoreObjectInHandleIfNull(OBJECTHANDLE handle, Object* object)
{
    return InterlockedCompareExchangeObjectInHandle(handle, object, nullptr) == nullptr;
}

void HandleManager::SetDependentHandleSecondary(OBJECTHANDLE handle, Object* object)
{
    __atomic_store_n(&Slot(handle)->secondary, object, __ATOMIC_RELEASE);
}

Object* HandleManager::GetDependentHandleSecondary(OBJECTHANDLE handle)
{
    return __atomic_load_n(&Slot(handle)->secondary, __ATOMIC_ACQUIRE);
}

Object* HandleManager::InterlockedCompareExchangeObjectInHandle(OBJECTHANDLE handle, Object* object, Object* comparandObject)
{
    Object* expected = comparandObject;
    __atomic_compare_exchange_n(&Slot(handle)->object, &expected, object, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}

HandleType HandleManager::HandleFetchType(OBJECTHANDLE handle)
{
    return (HandleType)Slot(handle)->type;
}

namespace
{
    struct RefCountedTrace
    {
        HANDLESCANPROC callback;
        uintptr_t param1;
        uintptr_t param2;
    };
}

void HandleManager::TraceRefCountedHandles(HANDLESCANPROC callback, uintptr_t param1, uintptr_t param2)
{
    RefCountedTrace trace = {callback, param1, param2};
    Handles::ForEach(
        [](HandleSlot* slot, void* context) {
            RefCountedTrace* t = (RefCountedTrace*)context;
            if (slot->type == HNDTYPE_REFCOUNTED)
                t->callback((PTR_UNCHECKED_OBJECTREF)&slot->object, &slot->extraInfo, t->param1, t->param2);
        },
        &trace);
}
