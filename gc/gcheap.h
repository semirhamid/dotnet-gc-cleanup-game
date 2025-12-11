// IGCHeap implementation (gcinterface.h:650-1071), in header order because the order is the ABI.
// docs/interface.md says which methods are stubs and what that breaks.
#pragma once

#include "platform.h"

class ManualGCHeap final : public IGCHeap
{
public:
    bool IsValidSegmentSize(size_t size) override;
    bool IsValidGen0MaxSize(size_t size) override;
    size_t GetValidSegmentSize(bool large_seg) override;
    void SetReservedVMLimit(size_t vmlimit) override;
    void WaitUntilConcurrentGCComplete() override;
    bool IsConcurrentGCInProgress() override;
    void TemporaryEnableConcurrentGC() override;
    void TemporaryDisableConcurrentGC() override;
    bool IsConcurrentGCEnabled() override;
    HRESULT WaitUntilConcurrentGCCompleteAsync(int millisecondsTimeout) override;
    size_t GetNumberOfFinalizable() override;
    Object* GetNextFinalizable() override;
    void GetMemoryInfo(uint64_t* highMemLoadThresholdBytes, uint64_t* totalAvailableMemoryBytes,
                       uint64_t* lastRecordedMemLoadBytes, uint64_t* lastRecordedHeapSizeBytes,
                       uint64_t* lastRecordedFragmentationBytes, uint64_t* totalCommittedBytes,
                       uint64_t* promotedBytes, uint64_t* pinnedObjectCount,
                       uint64_t* finalizationPendingCount, uint64_t* index, uint32_t* generation,
                       uint32_t* pauseTimePct, bool* isCompaction, bool* isConcurrent,
                       uint64_t* genInfoRaw, uint64_t* pauseInfoRaw, int kind) override;
    uint32_t GetMemoryLoad() override;
    int GetGcLatencyMode() override;
    int SetGcLatencyMode(int newLatencyMode) override;
    int GetLOHCompactionMode() override;
    void SetLOHCompactionMode(int newLOHCompactionMode) override;
    bool RegisterForFullGCNotification(uint32_t gen2Percentage, uint32_t lohPercentage) override;
    bool CancelFullGCNotification() override;
    int WaitForFullGCApproach(int millisecondsTimeout) override;
    int WaitForFullGCComplete(int millisecondsTimeout) override;
    unsigned WhichGeneration(Object* obj) override;
    int CollectionCount(int generation, int get_bgc_fgc_count) override;
    int StartNoGCRegion(uint64_t totalSize, bool lohSizeKnown, uint64_t lohSize, bool disallowFullBlockingGC) override;
    int EndNoGCRegion() override;
    size_t GetTotalBytesInUse() override;
    uint64_t GetTotalAllocatedBytes() override;
    HRESULT GarbageCollect(int generation, bool low_memory_p, int mode) override;
    unsigned GetMaxGeneration() override;
    void SetFinalizationRun(Object* obj) override;
    bool RegisterForFinalization(int gen, Object* obj) override;
    int GetLastGCPercentTimeInGC() override;
    size_t GetLastGCGenerationSize(int gen) override;
    HRESULT Initialize() override;
    bool IsPromoted(Object* object) override;
    bool IsHeapPointer(void* object, bool small_heap_only) override;
    unsigned GetCondemnedGeneration() override;
    bool IsGCInProgressHelper(bool bConsiderGCStart) override;
    unsigned GetGcCount() override;
    bool IsThreadUsingAllocationContextHeap(gc_alloc_context* acontext, int thread_number) override;
    bool IsEphemeral(Object* object) override;
    uint32_t WaitUntilGCComplete(bool bConsiderGCStart) override;
    void FixAllocContext(gc_alloc_context* acontext, void* arg, void* heap) override;
    size_t GetCurrentObjSize() override;
    void SetGCInProgress(bool fInProgress) override;
    bool RuntimeStructuresValid() override;
    void SetSuspensionPending(bool fSuspensionPending) override;
    void SetYieldProcessorScalingFactor(float yieldProcessorScalingFactor) override;
    void Shutdown() override;
    size_t GetLastGCStartTime(int generation) override;
    size_t GetLastGCDuration(int generation) override;
    size_t GetNow() override;
    Object* Alloc(gc_alloc_context* acontext, size_t size, uint32_t flags) override;
    void PublishObject(uint8_t* obj) override;
    void SetWaitForGCEvent() override;
    void ResetWaitForGCEvent() override;
    bool IsLargeObject(Object* pObj) override;
    void ValidateObjectMember(Object* obj) override;
    Object* NextObj(Object* object) override;
    Object* GetContainingObject(void* pInteriorPtr, bool fCollectedGenOnly) override;
    void DiagWalkObject(Object* obj, walk_fn fn, void* context) override;
    void DiagWalkObject2(Object* obj, walk_fn2 fn, void* context) override;
    void DiagWalkHeap(walk_fn fn, void* context, int gen_number, bool walk_large_object_heap_p) override;
    void DiagWalkSurvivorsWithType(void* gc_context, record_surv_fn fn, void* diag_context, walk_surv_type type, int gen_number) override;
    void DiagWalkFinalizeQueue(void* gc_context, fq_walk_fn fn) override;
    void DiagScanFinalizeQueue(fq_scan_fn fn, ScanContext* context) override;
    void DiagScanHandles(handle_scan_fn fn, int gen_number, ScanContext* context) override;
    void DiagScanDependentHandles(handle_scan_fn fn, int gen_number, ScanContext* context) override;
    void DiagDescrGenerations(gen_walk_fn fn, void* context) override;
    void DiagTraceGCSegments() override;
    void DiagGetGCSettings(EtwGCSettingsInfo* settings) override;
    bool StressHeap(gc_alloc_context* acontext) override;
    segment_handle RegisterFrozenSegment(segment_info* pseginfo) override;
    void UnregisterFrozenSegment(segment_handle seg) override;
    bool IsInFrozenSegment(Object* object) override;
    void ControlEvents(GCEventKeyword keyword, GCEventLevel level) override;
    void ControlPrivateEvents(GCEventKeyword keyword, GCEventLevel level) override;
    unsigned int GetGenerationWithRange(Object* object, uint8_t** ppStart, uint8_t** ppAllocated, uint8_t** ppReserved) override;
    int64_t GetTotalPauseDuration() override;
    void EnumerateConfigurationValues(void* context, ConfigurationValueFunc configurationValueFunc) override;
    void UpdateFrozenSegment(segment_handle seg, uint8_t* allocated, uint8_t* committed) override;
    int RefreshMemoryLimit() override;
    enable_no_gc_region_callback_status EnableNoGCRegionCallback(NoGCRegionCallbackFinalizerWorkItem* callback, uint64_t callback_threshold) override;
    FinalizerWorkItem* GetExtraWorkForFinalization() override;
    uint64_t GetGenerationBudget(int generation) override;
    size_t GetLOHThreshold() override;
    void DiagWalkHeapWithACHandling(walk_fn fn, void* context, int gen_number, bool walk_large_object_heap_p) override;
    void NullBridgeObjectsWeakRefs(size_t length, void* unreachableObjectHandles) override;
};
