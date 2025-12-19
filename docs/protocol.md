# GC to viewer protocol, version 1

The GC connects to `127.0.0.1:5710` (`MANUALGC_PORT`; 0 turns the viewer off) each time it
collects, and closes the connection when the app resumes. If nothing is listening, the collection
is a no-op.

Every message is a little-endian `u32` length, then that many bytes: a `u8` type and the payload.
Integers are little-endian. Strings are UTF-8.

## GC to viewer

| Type | Name | Payload |
|---|---|---|
| 1 | Hello | `u32` protocol version, `u32` process id, `u32` collection number, `u64` address of the first object, `u64` end of the used heap, `u8` flags (bit 0: hardcore allowed) |
| 2 | Module | `u32` module id, then the path of its file (rest of message; empty if it has none) |
| 3 | Type | `u64` type id, `u8` kind, then by kind (below) |
| 4 | Objects | repeated: `u64` address, `u64` size with flags in its low 3 bits (bit 0 live, bit 1 free space), `u64` type id (0 for free space) |
| 5 | SnapshotEnd | `u64` number of entries sent |

Type kinds:

| Kind | Meaning | Rest |
|---|---|---|
| 0 | named type | `u32` module id, `u32` TypeDef token, `u16` count, that many `u64` type argument ids |
| 1 | array | `u8` rank, `u8` 1 for a vector (`T[]`), `u64` element type id |
| 2 | pointer | `u64` element type id |
| 3 | byref | `u64` element type id |
| 4 | other | `u8` element type (ECMA-335 II.23.1.16) |

Type ids are addresses in the app, meaningful only within one connection. A module or type is sent
before the first message that refers to it. Names are not sent: the viewer reads them from the
assembly's metadata, so the GC neither runs managed code nor parses metadata. Objects arrive in address
order and cover the whole heap.
