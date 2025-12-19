# manual-gc

An experimental standalone GC for .NET 10.0.14 on macOS arm64, meant to be collected by hand.
Stage 3 of 5: GC.Collect() shows the heap in a web page, and frees nothing.

    make                 # builds artifacts/libmanualgc.dylib (Command Line Tools only)
    tests/stage1.sh      # each stage has a script that builds, runs the samples and checks the result

To run another app, copy the dylib next to the app's dll and set `DOTNET_GCName=libmanualgc.dylib`.
`DOTNET_GCName` must be a bare file name; with an absolute path the runtime itself crashes. Use
`DOTNET_GCPath` for a path. `MANUALGC_LOG=1` prints statistics. Server GC is refused; some apps
need `DOTNET_gcServer=0`.

To see a heap:

    dotnet run --project viewer                  # then open http://localhost:5711
    DOTNET_GCName=libmanualgc.dylib dotnet samples/orchard/bin/Release/net10.0/orchard.dll

Do not set `DOTNET_GCName` for the viewer itself.

[Protocol](docs/protocol.md) · [Design](docs/design.md) · [what is stubbed](docs/interface.md)
