# manual-gc

An experimental standalone GC for .NET 10.0.14 on macOS arm64, meant to be collected by hand.
Stage 1 of 5: it allocates and never collects.

    make                 # builds artifacts/libmanualgc.dylib (Command Line Tools only)
    tests/stage1.sh      # builds, runs the samples, checks the result

To run another app, copy the dylib next to the app's dll and set `DOTNET_GCName=libmanualgc.dylib`.
`DOTNET_GCName` must be a bare file name; with an absolute path the runtime itself crashes. Use
`DOTNET_GCPath` for a path. `MANUALGC_LOG=1` prints statistics. Server GC is refused; some apps
need `DOTNET_gcServer=0`.

[Design](docs/design.md) · [what is stubbed](docs/interface.md)
