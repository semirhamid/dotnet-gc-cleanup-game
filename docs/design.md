# Design

Pinned to Microsoft.NETCore.App 10.0.14 on osx-arm64, GC interface 5.5, EE interface 4. Line
references are to dotnet/runtime tag v10.0.14. Current as of Stage 1.

## Heap layout

- One contiguous reservation, 32 GB by default or `DOTNET_GCHeapHardLimit` (hexadecimal bytes),
  aligned to 16 MB. Address space only; pages are made accessible in 16 MB steps.
- A bump pointer hands out space and never goes back. Nothing is freed yet.
- Objects of every kind (small, large, pinned) share the range. Frozen objects belong to the
  VM and live outside it; their segments are recorded in a list.

## Object header and mark bit

- An object pointer points at the MethodTable slot. The 8-byte `ObjHeader` sits before it, so an
  object of size n at p owns `[p - 8, p + n - 8)`. Sizes round up to 8; the minimum is 24.
- No header or MethodTable bits are used. Stage 2 will keep mark bits in a side bitmap, so that a
  walk never alters live objects.

## Allocation path

1. JIT helpers bump `alloc_ptr` up to `alloc_limit` in the thread's context without calling us.
2. `IGCHeap::Alloc` (gcinterface.h:923) takes the slow path. Large- and pinned-heap requests, and
   objects of 8 KB and up, come straight from the global bump pointer under a mutex.
3. Otherwise the context is retired and gets a new 64 KB chunk.

`alloc_limit` stops 24 bytes before the end of a chunk. A retired context's tail is covered with a
free object, so the tail is always at least one minimum object. Memory is fresh from `mmap`, so it
is already zero. Null means out of memory, and the VM throws.

The mutex is taken in cooperative mode. Nothing inside it waits for the runtime.

## Write barrier

- `lowest_address`/`highest_address` are the true bounds. Inverted bounds would also switch
  off card marking, but the arm64 checked barrier and bulk copies would lose their ordering.
- `ephemeral_low` and `ephemeral_high` are the maximum address, so the JIT barrier exits before
  the card table. Frozen objects may sit above the heap, so `s_end` would not be enough.
- Bulk reference copies mark cards regardless (vm/gchelpers.inl), so there are real card and
  bundle tables for the whole reservation, biased by address: 16 MB and 16 KB at 32 GB, backed
  lazily. Nothing reads them.

## Handles

32-byte slots in 64 KB chunks that never move, with the object first because the VM dereferences
handles directly. One mutex and one free list. Every `HandleType` is stored with its type;
collection-time behaviour arrives in Stage 2.

## Suspension

The VM suspends threads for more than collections, and uses our `SetGCInProgress`,
`ResetWaitForGCEvent`, `SetWaitForGCEvent` and `WaitUntilGCComplete` every time
(vm/threadsuspend.cpp:2224, 3316, 3641, 5452, 5594). They are real.

## Finalization

Registration is accepted and forgotten. No finalizer runs.

## Collection sequence

None yet (Stage 2).

## Viewer protocol

None yet (Stage 3). Open question: `IGCToCLR` has no way to get a type name, and the GC must not
run managed code. Proposal: the sample sends the viewer a MethodTable-to-name map.
