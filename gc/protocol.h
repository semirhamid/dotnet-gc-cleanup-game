// The numbers on the wire. docs/protocol.md describes the messages; the viewer's copy of these is
// in viewer/Protocol.cs.
#pragma once

#include <stdint.h>

namespace Protocol
{
    constexpr uint32_t Version = 3;

    enum : uint8_t
    {
        Hello = 1,
        Module = 2,
        Type = 3,
        Objects = 4,
        SnapshotEnd = 5,
        EraseResults = 6,
        References = 7,
        Roots = 8,
        Attach = 9,
    };

    // Viewer to GC. ViewerLink::Receive returns 0 when there is no message.
    enum : uint8_t
    {
        Erase = 64,
        Done = 65,
        Collect = 66,
    };

    enum HelloFlags : uint8_t
    {
        HardcoreAllowed = 1,
        OutOfMemory = 2,
    };

    // In the low bits of an object's size, which are otherwise zero.
    enum ObjectFlags : uint64_t
    {
        Live = 1,
        FreeSpace = 2,
    };

    enum EraseFlags : uint8_t
    {
        EraseLiveToo = 1,
    };

    enum TypeKind : uint8_t
    {
        NamedType = 0,
        ArrayType = 1,
        PointerType = 2,
        ByRefType = 3,
        OtherType = 4,
    };
}
