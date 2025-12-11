#include "gcheap.h"
#include "handles.h"
#include "heap.h"

#include <pthread.h>
#include <sys/sysctl.h>
#include <time.h>

namespace
{
    // The heap has no generations. Three are reported because code asks for generation 2 by
    // number; every object is in the oldest one, so nothing is ever ephemeral.
    constexpr unsigned MaxGeneration = 2;

    bool s_gcInProgress;
    uint32_t s_suspensions;  // reported at shutdown; the VM suspends for more than collections

    // Threads returning to managed code during any EE suspension block on this event, so it
    // works from the start even though nothing collects yet. It begins signalled.
    pthread_mutex_t s_eventLock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t s_eventCond = PTHREAD_COND_INITIALIZER;
    bool s_eventSignalled = true;

    // Values from GC.CoreCLR.cs / gcpriv.h pause modes: 0 batch (blocking), 4 no-GC region.
    int s_latencyMode = 0;
    int s_lohCompactionMode = 1;  // GCLargeObjectHeapCompactionMode.Default
    bool s_noGCRegion;

    struct FrozenSegment
    {
        uint8_t* first;
        uint8_t* allocated;
        uint8_t* committed;
        uint8_t* reserved;
        FrozenSegment* next;
    };
    pthread_mutex_t s_frozenLock = PTHREAD_MUTEX_INITIALIZER;
    FrozenSegment* s_frozen;

    uint64_t PhysicalMemory()
    {
        uint64_t bytes = 0;
        size_t length = sizeof(bytes);
        sysctlbyname("hw.memsize", &bytes, &length, nullptr, 0);
        return bytes;
    }

    size_t ObjectSize(Object* obj)
    {
        MethodTable* mt = (MethodTable*)(*(uintptr_t*)obj & ~(uintptr_t)7);
        size_t size = mt->GetBaseSize();
        if (mt->HasComponentSize())
            size += (size_t)mt->RawGetComponentSize() * *(uint32_t*)((uint8_t*)obj + sizeof(void*));
        return (size + 7) & ~(size_t)7;
    }

    void LogStatistics()
    {
        LogLine("exit, used %zu MB, committed %zu MB, live handles %zu, EE suspensions %u",
                Heap::UsedBytes() >> 20, Heap::CommittedBytes() >> 20, Handles::LiveCount(),
                __atomic_load_n(&s_suspensions, __ATOMIC_RELAXED));
    }
}

// ---- Hosting ----
bool ManualGCHeap::IsValidSegmentSize(size_t) { return true; }
bool ManualGCHeap::IsValidGen0MaxSize(size_t) { return true; }
size_t ManualGCHeap::GetValidSegmentSize(bool) { return Heap::ReservedBytes(); }
void ManualGCHeap::SetReservedVMLimit(size_t) {}

// ---- Concurrent GC: there is none ----
void ManualGCHeap::WaitUntilConcurrentGCComplete() {}
bool ManualGCHeap::IsConcurrentGCInProgress() { return false; }
void ManualGCHeap::TemporaryEnableConcurrentGC() {}
void ManualGCHeap::TemporaryDisableConcurrentGC() {}
bool ManualGCHeap::IsConcurrentGCEnabled() { return false; }
HRESULT ManualGCHeap::WaitUntilConcurrentGCCompleteAsync(int) { return S_OK; }

// ---- Finalization: accepted, never run ----
size_t ManualGCHeap::GetNumberOfFinalizable() { return 0; }
Object* ManualGCHeap::GetNextFinalizable() { return nullptr; }
bool ManualGCHeap::RegisterForFinalization(int, Object*) { return true; }
FinalizerWorkItem* ManualGCHeap::GetExtraWorkForFinalization() { return nullptr; }

void ManualGCHeap::SetFinalizationRun(Object* obj)
{
    ((ObjHeader*)obj - 1)->SetBit(BIT_SBLK_FINALIZER_RUN);
}

// ---- BCL ----
void ManualGCHeap::GetMemoryInfo(uint64_t* highMemLoadThresholdBytes, uint64_t* totalAvailableMemoryBytes,
                                 uint64_t* lastRecordedMemLoadBytes, uint64_t* lastRecordedHeapSizeBytes,
                                 uint64_t* lastRecordedFragmentationBytes, uint64_t* totalCommittedBytes,
                                 uint64_t* promotedBytes, uint64_t* pinnedObjectCount,
                                 uint64_t* finalizationPendingCount, uint64_t* index, uint32_t* generation,
                                 uint32_t* pauseTimePct, bool* isCompaction, bool* isConcurrent,
                                 uint64_t*, uint64_t*, int)
{
    // genInfoRaw and pauseInfoRaw keep the zeros the caller put there.
    uint64_t physical = PhysicalMemory();
    *highMemLoadThresholdBytes = physical / 10 * 9;
    *totalAvailableMemoryBytes = Heap::ReservedBytes() < physical ? Heap::ReservedBytes() : physical;
    *lastRecordedMemLoadBytes = 0;
    *lastRecordedHeapSizeBytes = 0;
    *lastRecordedFragmentationBytes = 0;
    *totalCommittedBytes = Heap::CommittedBytes();
    *promotedBytes = 0;
    *pinnedObjectCount = 0;
    *finalizationPendingCount = 0;
    *index = 0;
    *generation = 0;
    *pauseTimePct = 0;
    *isCompaction = false;
    *isConcurrent = false;
}

uint32_t ManualGCHeap::GetMemoryLoad() { return 0; }

int ManualGCHeap::GetGcLatencyMode() { return s_noGCRegion ? 4 : s_latencyMode; }

int ManualGCHeap::SetGcLatencyMode(int newLatencyMode)
{
    if (s_noGCRegion)
        return 1;  // set_pause_mode_no_gc
    s_latencyMode = newLatencyMode;
    return 0;  // set_pause_mode_success
}

int ManualGCHeap::GetLOHCompactionMode() { return s_lohCompactionMode; }
void ManualGCHeap::SetLOHCompactionMode(int mode) { s_lohCompactionMode = mode; }
bool ManualGCHeap::RegisterForFullGCNotification(uint32_t, uint32_t) { return false; }
bool ManualGCHeap::CancelFullGCNotification() { return false; }
int ManualGCHeap::WaitForFullGCApproach(int) { return wait_full_gc_na; }
int ManualGCHeap::WaitForFullGCComplete(int) { return wait_full_gc_na; }

unsigned ManualGCHeap::WhichGeneration(Object* obj)
{
    return Heap::Contains(obj) ? MaxGeneration : (unsigned)INT32_MAX;
}

int ManualGCHeap::CollectionCount(int, int) { return 0; }

int ManualGCHeap::StartNoGCRegion(uint64_t totalSize, bool, uint64_t, bool)
{
    if (s_noGCRegion)
        return start_no_gc_in_progress;
    if (totalSize > Heap::ReservedBytes() - Heap::UsedBytes())
        return start_no_gc_no_memory;
    s_noGCRegion = true;
    return start_no_gc_success;
}

int ManualGCHeap::EndNoGCRegion()
{
    if (!s_noGCRegion)
        return end_no_gc_not_in_progress;
    s_noGCRegion = false;
    return end_no_gc_success;
}

size_t ManualGCHeap::GetTotalBytesInUse() { return Heap::UsedBytes(); }
uint64_t ManualGCHeap::GetTotalAllocatedBytes() { return Heap::UsedBytes(); }

HRESULT ManualGCHeap::GarbageCollect(int, bool, int) { return S_OK; }

unsigned ManualGCHeap::GetMaxGeneration() { return MaxGeneration; }
int ManualGCHeap::GetLastGCPercentTimeInGC() { return 0; }
size_t ManualGCHeap::GetLastGCGenerationSize(int) { return 0; }

// ---- Used throughout the VM ----
HRESULT ManualGCHeap::Initialize()
{
    size_t reserve = Heap::DefaultReserve;
    int64_t limit = 0;
    if (g_ee->GetIntConfigValue("GCHeapHardLimit", "System.GC.HeapHardLimit", &limit) && limit > 0)
        reserve = (size_t)limit;

    if (!Heap::Initialize(reserve, g_ee->GetFreeObjectMethodTable()))
    {
        g_ee->LogErrorToHost("manualgc: could not reserve the heap");
        return (HRESULT)E_OUTOFMEMORY;
    }
    LogLine("initialized, reserved %zu MB", Heap::ReservedBytes() >> 20);
    if (LogEnabled())
        atexit(LogStatistics);
    return S_OK;
}

bool ManualGCHeap::IsPromoted(Object*) { return true; }

bool ManualGCHeap::IsHeapPointer(void* object, bool) { return Heap::Contains(object); }
unsigned ManualGCHeap::GetCondemnedGeneration() { return MaxGeneration; }

bool ManualGCHeap::IsGCInProgressHelper(bool)
{
    return __atomic_load_n(&s_gcInProgress, __ATOMIC_ACQUIRE);
}

unsigned ManualGCHeap::GetGcCount() { return 0; }
bool ManualGCHeap::IsThreadUsingAllocationContextHeap(gc_alloc_context*, int) { return true; }
bool ManualGCHeap::IsEphemeral(Object*) { return false; }

uint32_t ManualGCHeap::WaitUntilGCComplete(bool)
{
    pthread_mutex_lock(&s_eventLock);
    while (!s_eventSignalled)
        pthread_cond_wait(&s_eventCond, &s_eventLock);
    pthread_mutex_unlock(&s_eventLock);
    return S_OK;
}

void ManualGCHeap::FixAllocContext(gc_alloc_context* acontext, void*, void*)
{
    Heap::RetireContext(acontext);
}

size_t ManualGCHeap::GetCurrentObjSize() { return Heap::UsedBytes(); }

void ManualGCHeap::SetGCInProgress(bool fInProgress)
{
    if (fInProgress)
        __atomic_add_fetch(&s_suspensions, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&s_gcInProgress, fInProgress, __ATOMIC_RELEASE);
}

bool ManualGCHeap::RuntimeStructuresValid() { return true; }
void ManualGCHeap::SetSuspensionPending(bool) {}
void ManualGCHeap::SetYieldProcessorScalingFactor(float) {}

// Not called on a normal exit, so the statistics are logged from atexit instead.
void ManualGCHeap::Shutdown() {}

// ---- Memory pressure timing ----
size_t ManualGCHeap::GetLastGCStartTime(int) { return 0; }
size_t ManualGCHeap::GetLastGCDuration(int) { return 0; }
size_t ManualGCHeap::GetNow() { return (size_t)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000); }

// ---- Allocation ----
Object* ManualGCHeap::Alloc(gc_alloc_context* acontext, size_t size, uint32_t flags)
{
    return Heap::Alloc(acontext, size, flags);
}

void ManualGCHeap::PublishObject(uint8_t*) {}

void ManualGCHeap::SetWaitForGCEvent()
{
    pthread_mutex_lock(&s_eventLock);
    s_eventSignalled = true;
    pthread_cond_broadcast(&s_eventCond);
    pthread_mutex_unlock(&s_eventLock);
}

void ManualGCHeap::ResetWaitForGCEvent()
{
    pthread_mutex_lock(&s_eventLock);
    s_eventSignalled = false;
    pthread_mutex_unlock(&s_eventLock);
}

// ---- Heap verification ----
bool ManualGCHeap::IsLargeObject(Object* pObj)
{
    return Heap::Contains(pObj) && ObjectSize(pObj) >= LARGE_OBJECT_SIZE;
}

void ManualGCHeap::ValidateObjectMember(Object*) {}
Object* ManualGCHeap::NextObj(Object*) { return nullptr; }
Object* ManualGCHeap::GetContainingObject(void*, bool) { return nullptr; }

// ---- Profiling and tracing ----
void ManualGCHeap::DiagWalkObject(Object*, walk_fn, void*) {}
void ManualGCHeap::DiagWalkObject2(Object*, walk_fn2, void*) {}
void ManualGCHeap::DiagWalkHeap(walk_fn, void*, int, bool) {}
void ManualGCHeap::DiagWalkSurvivorsWithType(void*, record_surv_fn, void*, walk_surv_type, int) {}
void ManualGCHeap::DiagWalkFinalizeQueue(void*, fq_walk_fn) {}
void ManualGCHeap::DiagScanFinalizeQueue(fq_scan_fn, ScanContext*) {}
void ManualGCHeap::DiagScanHandles(handle_scan_fn, int, ScanContext*) {}
void ManualGCHeap::DiagScanDependentHandles(handle_scan_fn, int, ScanContext*) {}
void ManualGCHeap::DiagDescrGenerations(gen_walk_fn, void*) {}
void ManualGCHeap::DiagTraceGCSegments() {}

void ManualGCHeap::DiagGetGCSettings(EtwGCSettingsInfo* settings)
{
    *settings = {};
    settings->heap_hard_limit = Heap::ReservedBytes();
    settings->loh_threshold = LARGE_OBJECT_SIZE;
    settings->use_frozen_segments_p = true;
}

bool ManualGCHeap::StressHeap(gc_alloc_context*) { return false; }

// ---- Frozen segments ----
segment_handle ManualGCHeap::RegisterFrozenSegment(segment_info* info)
{
    FrozenSegment* segment = new (nothrow) FrozenSegment();
    if (segment == nullptr)
        return nullptr;
    uint8_t* base = (uint8_t*)info->pvMem;  // field meanings: gcinterface.h:233-240
    segment->first = base + info->ibFirstObject;
    segment->allocated = base + info->ibAllocated;
    segment->committed = base + info->ibCommit;
    segment->reserved = base + info->ibReserved;
    pthread_mutex_lock(&s_frozenLock);
    segment->next = s_frozen;
    s_frozen = segment;
    pthread_mutex_unlock(&s_frozenLock);
    return (segment_handle)segment;
}

void ManualGCHeap::UnregisterFrozenSegment(segment_handle seg)
{
    pthread_mutex_lock(&s_frozenLock);
    FrozenSegment** link = &s_frozen;
    while (*link != nullptr && *link != (FrozenSegment*)seg)
        link = &(*link)->next;
    if (*link != nullptr)
        *link = (*link)->next;
    pthread_mutex_unlock(&s_frozenLock);
    delete (FrozenSegment*)seg;
}

// The range covers the whole reservation, so objects in a segment count as frozen before the
// VM reports the segment's new allocated limit through UpdateFrozenSegment.
bool ManualGCHeap::IsInFrozenSegment(Object* object)
{
    if (Heap::Contains(object))
        return false;
    bool found = false;
    pthread_mutex_lock(&s_frozenLock);
    for (FrozenSegment* s = s_frozen; s != nullptr && !found; s = s->next)
        found = (uint8_t*)object >= s->first && (uint8_t*)object < s->reserved;
    pthread_mutex_unlock(&s_frozenLock);
    return found;
}

void ManualGCHeap::UpdateFrozenSegment(segment_handle seg, uint8_t* allocated, uint8_t* committed)
{
    pthread_mutex_lock(&s_frozenLock);
    ((FrozenSegment*)seg)->allocated = allocated;
    ((FrozenSegment*)seg)->committed = committed;
    pthread_mutex_unlock(&s_frozenLock);
}

// ---- Everything else ----
void ManualGCHeap::ControlEvents(GCEventKeyword, GCEventLevel) {}
void ManualGCHeap::ControlPrivateEvents(GCEventKeyword, GCEventLevel) {}

unsigned int ManualGCHeap::GetGenerationWithRange(Object* object, uint8_t** ppStart, uint8_t** ppAllocated, uint8_t** ppReserved)
{
    *ppStart = Heap::FirstObject();
    *ppAllocated = Heap::FirstObject() + Heap::UsedBytes();
    *ppReserved = Heap::FirstObject() + Heap::ReservedBytes() - sizeof(ObjHeader);
    return WhichGeneration(object);
}

int64_t ManualGCHeap::GetTotalPauseDuration() { return 0; }
void ManualGCHeap::EnumerateConfigurationValues(void*, ConfigurationValueFunc) {}
int ManualGCHeap::RefreshMemoryLimit() { return 0; }

enable_no_gc_region_callback_status ManualGCHeap::EnableNoGCRegionCallback(NoGCRegionCallbackFinalizerWorkItem*, uint64_t)
{
    return not_started;
}

uint64_t ManualGCHeap::GetGenerationBudget(int) { return 0; }
size_t ManualGCHeap::GetLOHThreshold() { return LARGE_OBJECT_SIZE; }
void ManualGCHeap::DiagWalkHeapWithACHandling(walk_fn, void*, int, bool) {}
void ManualGCHeap::NullBridgeObjectsWeakRefs(size_t, void*) {}
