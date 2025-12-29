# manual-gc

An experimental garbage collector for .NET where the collecting is done by a person. When an app
calls `GC.Collect()`, the app freezes and a web page shows its heap. You drag an eraser over what
you think is garbage. Garbage is freed and its memory reused; touching something alive costs a life.
Press Done and the app goes on.

It is a standalone GC library for the stock .NET runtime: no fork, no patches. It is pinned to .NET
10.0.14 on macOS arm64, and it is a toy: one heap, no concurrency, no compaction, no generations,
and finalizers never run.

## Play

Needs the .NET 10.0.14 runtime and the Xcode Command Line Tools.

    make
    dotnet run --project viewer          # then open http://localhost:5711

In another terminal, an app that asks for five collections:

    dotnet build -c Release samples/orchard
    cp artifacts/libmanualgc.dylib samples/orchard/bin/Release/net10.0/
    DOTNET_GCName=libmanualgc.dylib dotnet samples/orchard/bin/Release/net10.0/orchard.dll 5

Any other app works the same way. `DOTNET_GCName` must be a bare file name, found next to the app's
dll; with an absolute path the runtime itself crashes. Use `DOTNET_GCPath` for a path. Do not set
either for the viewer. Without a viewer, `GC.Collect()` does nothing and the heap grows.

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

`tests/stage1.sh` to `tests/stage5.sh` build everything, run the samples under this GC and check the
results. The page checks need Node 22 and Chrome, and are skipped without them.

[Design](docs/design.md) · [Protocol](docs/protocol.md) · [What is stubbed](docs/interface.md)
