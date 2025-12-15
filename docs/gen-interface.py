#!/usr/bin/env python3
"""Writes docs/interface.md. Method names and lines come from the pinned header; this file holds
only the status. Fails if any IGCHeap method is missing here or no longer in the header."""
import re, pathlib
root = pathlib.Path(__file__).resolve().parent.parent
lines = (root / "third_party/dotnet-runtime/src/coreclr/gc/gcinterface.h").read_text().splitlines()

REAL = "real"
S = {  # name: (status, note). "real" = does its job; "fixed" = constant that is true for this GC.
"IsValidSegmentSize": ("stub", "Accepts every size. No effect: segment size is not configurable."),
"IsValidGen0MaxSize": ("stub", "As above."),
"GetValidSegmentSize": ("real", "Reservation size. Only used in trace events."),
"SetReservedVMLimit": ("stub", "Ignored. Hosts cannot cap address space this way."),
"WaitUntilConcurrentGCComplete": ("fixed", "No concurrent GC."),
"IsConcurrentGCInProgress": ("fixed", "false"),
"TemporaryEnableConcurrentGC": ("fixed", "No concurrent GC."),
"TemporaryDisableConcurrentGC": ("fixed", "No concurrent GC."),
"IsConcurrentGCEnabled": ("fixed", "false"),
"WaitUntilConcurrentGCCompleteAsync": ("fixed", "S_OK at once."),
"GetNumberOfFinalizable": ("stub", "0. Finalizers never run."),
"GetNextFinalizable": ("stub", "null. Finalizers never run, so unmanaged resources held only by a finalizer stay open until exit, and ArrayPool-style Gen2 callbacks never fire."),
"GetMemoryInfo": ("partial", "Commit and memory totals are real; everything about the last GC is 0."),
"GetMemoryLoad": ("stub", "0. Caches that trim on memory load will not trim."),
"GetGcLatencyMode": ("real", "Stored value; NoGCRegion while one is open."),
"SetGcLatencyMode": ("real", "Stored; has no effect on behaviour."),
"GetLOHCompactionMode": ("real", "Stored value."),
"SetLOHCompactionMode": ("real", "Stored; nothing compacts."),
"RegisterForFullGCNotification": ("stub", "false, so GC.RegisterForFullGCNotification throws."),
"CancelFullGCNotification": ("stub", "false, so the cancel call throws."),
"WaitForFullGCApproach": ("stub", "Not applicable."),
"WaitForFullGCComplete": ("stub", "Not applicable."),
"WhichGeneration": ("fixed", "2 for heap objects, INT32_MAX otherwise. Code expecting new objects in generation 0 is wrong here."),
"CollectionCount": ("stub", "0. Code that waits for the count to rise never finishes."),
"StartNoGCRegion": ("partial", "Succeeds if the size fits what is left of the reservation."),
"EndNoGCRegion": ("partial", "Matches StartNoGCRegion; never reports an exceeded budget."),
"GetTotalBytesInUse": ("real", "Bytes handed out, including unused context tails."),
"GetTotalAllocatedBytes": ("real", "Same number; nothing is ever freed."),
"GarbageCollect": ("partial", "Blocking requests suspend, mark and log. Nothing is freed yet, so memory only grows and weak references never clear. Non-blocking requests (memory pressure) are ignored."),
"GetMaxGeneration": ("fixed", "2, for code that asks for generation 2 by number."),
"SetFinalizationRun": ("real", "Sets the header bit."),
"RegisterForFinalization": ("stub", "Returns true and forgets the object. See GetNextFinalizable."),
"GetLastGCPercentTimeInGC": ("fixed", "0: no GC has run."),
"GetLastGCGenerationSize": ("stub", "0. Runtime counters show empty generations."),
"Initialize": ("real", ""),
"IsPromoted": ("real", "Mark bit during a collection, true otherwise."),
"IsHeapPointer": ("real", "Range check on the reservation."),
"GetCondemnedGeneration": ("fixed", "2"),
"IsGCInProgressHelper": ("real", "Tracks every EE suspension, as the VM expects."),
"GetGcCount": ("fixed", "0: no GC has run."),
"IsThreadUsingAllocationContextHeap": ("fixed", "true: one heap."),
"IsEphemeral": ("fixed", "false"),
"WaitUntilGCComplete": ("real", "Waits on the resume event."),
"FixAllocContext": ("real", "Plugs the tail and empties the context."),
"GetCurrentObjSize": ("real", "Bytes handed out."),
"SetGCInProgress": ("real", ""),
"RuntimeStructuresValid": ("fixed", "true"),
"SetSuspensionPending": ("stub", "Ignored. It only tunes spinning in the stock GC."),
"SetYieldProcessorScalingFactor": ("stub", "Ignored; this GC does not spin."),
"Shutdown": ("stub", "Nothing to release. The VM does not call it on a normal exit."),
"GetLastGCStartTime": ("stub", "0. Not called by the VM at this tag."),
"GetLastGCDuration": ("stub", "0. Not called by the VM at this tag."),
"GetNow": ("real", "Microseconds. Not called by the VM at this tag."),
"Alloc": ("real", ""),
"PublishObject": ("stub", "Only needed for background GC."),
"SetWaitForGCEvent": ("real", ""),
"ResetWaitForGCEvent": ("real", ""),
"IsLargeObject": ("real", "By size, 85,000 bytes."),
"ValidateObjectMember": ("stub", "Heap verification does not check members."),
"NextObj": ("real", ""),
"GetContainingObject": ("partial", "Answers during a collection, null otherwise, so byref validation reports nothing."),
"DiagWalkObject": ("stub", "Profilers and heap-dump events see no references."),
"DiagWalkObject2": ("stub", "As above."),
"DiagWalkHeap": ("stub", "Heap dumps (dotnet-gcdump) are empty."),
"DiagWalkSurvivorsWithType": ("stub", "Profilers see no survivors."),
"DiagWalkFinalizeQueue": ("stub", "Profilers see an empty queue."),
"DiagScanFinalizeQueue": ("stub", "As above."),
"DiagScanHandles": ("stub", "Profilers see no handles."),
"DiagScanDependentHandles": ("stub", "As above."),
"DiagDescrGenerations": ("stub", "Profilers see no generation ranges."),
"DiagTraceGCSegments": ("stub", "No segment events."),
"DiagGetGCSettings": ("real", ""),
"StressHeap": ("stub", "DOTNET_GCStress does nothing."),
"RegisterFrozenSegment": ("real", ""),
"UnregisterFrozenSegment": ("real", ""),
"IsInFrozenSegment": ("real", "Registered segments only."),
"ControlEvents": ("stub", "No GC events, so dotnet-trace and dotnet-counters show no GC data."),
"ControlPrivateEvents": ("stub", "As above."),
"GetGenerationWithRange": ("real", "The whole heap is one range."),
"GetTotalPauseDuration": ("fixed", "0: no pauses."),
"EnumerateConfigurationValues": ("stub", "GC.GetConfigurationVariables() is empty."),
"UpdateFrozenSegment": ("real", ""),
"RefreshMemoryLimit": ("stub", "Reports success; the limit is fixed at startup, so GC.RefreshMemoryLimit has no effect."),
"EnableNoGCRegionCallback": ("stub", "not_started, so GC.RegisterNoGCRegionCallback throws."),
"GetExtraWorkForFinalization": ("fixed", "null: no extra work."),
"GetGenerationBudget": ("stub", "0"),
"GetLOHThreshold": ("fixed", "85,000. DOTNET_GCLOHThreshold is ignored."),
"DiagWalkHeapWithACHandling": ("stub", "Heap dumps are empty."),
"NullBridgeObjectsWeakRefs": ("stub", "Java interop (Android) only."),
}

start = next(i for i, l in enumerate(lines) if l.startswith("class IGCHeap {"))
end = next(i for i in range(start, len(lines)) if lines[i].startswith("};"))
found = []
for i in range(start, end):
    m = re.match(r"\s+virtual [^(]*?(\w+)\(", lines[i])
    if m: found.append((m.group(1), i + 1))
names = [n for n, _ in found]
assert len(set(names)) == len(names), "duplicate"
assert set(names) == set(S), set(names) ^ set(S)
count = lambda k: sum(1 for s, _ in S.values() if s == k)
out = ["# IGCHeap coverage", "",
"Generated by `docs/gen-interface.py` from `gcinterface.h` at v10.0.14. Do not edit by hand.", "",
f"{len(found)} methods: {count('real')} real, {count('fixed')} fixed answers that are true for this GC, "
f"{count('partial')} partial, {count('stub')} stubs.", "", "| Line | Method | Status | Notes, and what a stub breaks |", "|---|---|---|---|"]
out += [f"| {ln} | `{n}` | {S[n][0]} | {S[n][1]} |" for n, ln in found]
out += ["", "Every `IGCHandleStore` (578-594) and `IGCHandleManager` (596-634) method is real except `Uproot`,",
"`Shutdown` and `DestroyHandleStore` (nothing to do) and `CreateHandleStore` (null; the VM only uses the",
"global store).", "",
"`GcDacVars` is not filled in, so SOS, `dotnet-dump` and ClrMD cannot read the heap.", ""]
(root / "docs/interface.md").write_text("\n".join(out))
print(out[4])
