# Design

Pinned to Microsoft.NETCore.App 10.0.14 on osx-arm64, GC interface 5.5, EE interface 4. Line
references are to dotnet/runtime tag v10.0.14. Current as of Stage 4.

## Heap layout

- One contiguous reservation, 32 GB by default or `DOTNET_GCHeapHardLimit` (hexadecimal bytes),
  aligned to 16 MB. Address space only; pages are made accessible in 16 MB steps.
- A bump pointer hands out fresh space and never goes back. Erased objects become free objects,
  which the allocator reuses.
- Objects of every kind (small, large, pinned) share the range. Frozen objects belong to the
  VM and live outside it; their segments are recorded in a list.

## Object header and mark bit

- An object pointer points at the MethodTable slot. The 8-byte `ObjHeader` sits before it, so an
  object of size n at p owns `[p - 8, p + n - 8)`. Sizes round up to 8; the minimum is 24.
- No header or MethodTable bits are used. Two side bitmaps, one bit per 8 bytes of heap each, hold
  "an object starts here" and "marked". They are valid during a collection only.

## Allocation path

1. JIT helpers bump `alloc_ptr` up to `alloc_limit` in the thread's context without calling us.
2. `IGCHeap::Alloc` (gcinterface.h:923) takes the slow path. Large- and pinned-heap requests, and
   objects of 8 KB and up, get a hole that fits, or fresh space.
3. Otherwise the context is refilled from a hole of 4 KB or more; or the object goes into a smaller
   hole on its own; or the context gets a fresh 64 KB chunk; or, when the heap is nearly full, the
   object is placed on its own at the end.

`alloc_limit` stops 24 bytes before the end of a chunk. A retired context's tail is covered with a
free object, so the tail is always at least one minimum object. Fresh memory is zero from `mmap`;
holes are zeroed when taken. Null means out of memory, and the VM throws.

Holes are kept in lists by power-of-two size, linked through the free object's `ObjHeader` slot, the
only spare word a 24-byte block has. A split must leave nothing or at least 24 bytes.

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
handles directly. One mutex and one free list. Every slot records its `HandleType`. The collector
walks slots without the lock, so EE callbacks made during the walk cannot deadlock.

## Suspension

The VM suspends threads for more than collections, and uses our `SetGCInProgress`,
`ResetWaitForGCEvent`, `SetWaitForGCEvent` and `WaitUntilGCComplete` every time
(vm/threadsuspend.cpp:2224, 3316, 3641, 5452, 5594). They are real.

## Finalization

Registration is accepted and forgotten. No finalizer runs. An erased object that has one is simply
freed, so whatever it would have released leaks. That is safe, but a program that creates many
`WeakReference`s or undisposed `SafeHandle`s leaks GC handles.

## Collection sequence

`GC.Collect()` and other blocking requests, on the calling thread:

1. Leave cooperative mode, take the collection lock, connect to the viewer. If there is none (and
   no logging), stop here.
2. `SuspendEE`, go back to cooperative mode.
3. Retire every allocation context, so the heap is objects and free objects end to end.
4. Walk the heap and rebuild the start bitmap; clear the mark bitmap.
5. Mark from stacks and thread statics (`GcScanRoots`), strong, pinned and ref-counted handles,
   then dependent handles until nothing changes. Marking uses an explicit stack. Objects of
   collectible types also mark their LoaderAllocator.
6. Sever the dead: clear weak, ref-counted and dependent handles and sync block entries whose
   object is unmarked. From here a dead object can never become reachable again, so it does not
   matter that unerased dead objects may refer to erased ones: nothing ever follows them.
7. Send the snapshot and serve Erase until Done or the connection closes. Erasing checks that the
   address is an object start and unmarked, then makes it a free object.
8. Sweep: merge neighbouring free objects and rebuild the free lists. Resume.

Without a viewer, steps 6 to 8 do not happen: a census for the log changes nothing.

An interior pointer is resolved by searching the start bitmap downwards. Any other reference that
is not the start of an object ends the process: it means the heap is corrupt. Frozen objects count
as live and are not scanned, as in the stock GC; they hold no references into this heap.

`MANUALGC_POISON=1` fills erased objects with 0xDD, to expose use after free.

## Viewer protocol

See [protocol.md](protocol.md). The GC connects before suspending the runtime. With no viewer
listening and no log wanted, a collection returns without doing anything. Socket I/O happens on the
collecting thread while the app is suspended.

## Type names

`IGCToCLR` has no way to name a type. coreclr exports `DotNetRuntimeContractDescriptor`, which gives
the field offsets diagnostic tools use. The GC reads those offsets at run time and sends each type
as a module path and TypeDef token, with generic arguments and array elements resolved to other
types. The viewer looks the names up in the assemblies' metadata. If the descriptor is missing or
incomplete, types appear by address. Types from in-memory assemblies have no file, so they do too.
