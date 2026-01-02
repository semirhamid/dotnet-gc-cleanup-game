# manual-gc

An experimental garbage collector for .NET where the collecting is done by a person. When an app
calls `GC.Collect()`, the app freezes and a web page shows its heap. You drag an eraser over what
you think is garbage. Garbage is freed and its memory reused; touching something alive costs a life.
Press Done and the app goes on.

It is a standalone GC library for the stock .NET runtime: no fork, no patches. It is pinned to .NET
10.0.14 on macOS arm64, and it is a toy: one heap, no concurrency, no compaction, no generations,
and finalizers never run.

There are two ways to play: the heap map, with an eraser, and **Loose Balloons**, where every object is
a balloon, every reference a rope, and you shoot down what no chain of ropes holds to the root.

## Play

Needs the .NET 10.0.14 runtime and the Xcode Command Line Tools. Point it at any .NET app, yours or
this sample:

    ./play samples/airfield
    ./play ~/code/MyWebApp                     # a project folder, a .csproj or a built .dll
    ./play --limit 256 ~/code/MyWebApp         # freezes and asks for room at 256 MB

Open http://localhost:5711/flight.html (Loose Balloons) or http://localhost:5711 (the heap map). Press
**Freeze** to start a round whenever you like; apps that call `GC.Collect()` also start their own.

Tested: framework-dependent web apps built for .NET 8, 9 and 10, on Apple silicon. Older ones
run on .NET 10, so they must tolerate that. What does not: .NET Framework, Native AOT, single-file and
self-contained apps, x64 builds, and anything that is not .NET.

While a round lasts, the app is frozen: its requests wait, and clients or health checks may time out.
Finalizers never run, so long sessions leak. Use it on a development machine only.

`./play` sets these for the app. To do it by hand, set `DOTNET_GCPath` to the library's full path and
`DOTNET_gcServer=0`. (`DOTNET_GCName` takes only a bare file name next to the app's dll; with a path the
runtime crashes.) Do not set either for the viewer. Without a viewer, nothing is collected.

| On the app | |
|---|---|
| `DOTNET_GCHeapHardLimit` | heap limit in bytes, hexadecimal (default 32 GB of address space) |
| `DOTNET_gcServer=0` | needed for apps that ask for server GC, which is refused |
| `MANUALGC_LOG` | 1 for summaries, 2 for counts per type |
| `MANUALGC_PORT` | where the viewer listens for the GC (default 5710, 0 for no viewer) |
| `MANUALGC_POISON=1` | overwrite erased memory, to expose use after free |
| `MANUALGC_HARDCORE=1` | allow hardcore mode, where erasing live objects **crashes the app** |

Viewer options: `--WebPort`, `--GcPort`, `--Lives`, `--Seconds`.

## Tests

`tests/stage1.sh` to `tests/stage5.sh` `tests/flight.sh` and `tests/anyapp.sh` build everything, run the samples under this GC and check the
results. The page checks need Node 22 and Chrome, and are skipped without them.

[Design](docs/design.md) · [Protocol](docs/protocol.md) · [What is stubbed](docs/interface.md)
