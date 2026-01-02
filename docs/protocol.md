# GC to viewer protocol, version 3

The GC connects to `127.0.0.1:5710` (`MANUALGC_PORT`; 0 turns the viewer off) each time it
collects, and closes the connection when the app resumes. The app stays suspended from before Hello
until the viewer sends Done or closes the connection. If nothing is listening, the collection
is a no-op.

An app also keeps one control connection open, on which it sends only Attach. The viewer may answer
with Collect at any time, which makes the app collect as if it had called `GC.Collect()`. A
collection uses a connection of its own, so a listener must accept several at once; the first
message says which kind it is.

Every message is a little-endian `u32` length, then that many bytes: a `u8` type and the payload.
Integers are little-endian. Strings are UTF-8.

## GC to viewer

| Type | Name | Payload |
|---|---|---|
| 1 | Hello | `u32` protocol version, `u32` process id, `u32` collection number, `u64` address of the first object, `u64` end of the used heap, `u8` flags (bit 0: hardcore allowed, bit 1: the app has run out of memory) |
| 2 | Module | `u32` module id, then the path of its file (rest of message; empty if it has none) |
| 3 | Type | `u64` type id, `u8` kind, then by kind (below) |
| 4 | Objects | repeated: `u64` address, `u64` size with flags in its low 3 bits (bit 0 live, bit 1 free space), `u64` type id (0 for free space) |
| 5 | SnapshotEnd | `u64` number of entries sent |
| 6 | EraseResults | repeated: `u64` address, `u8` status (0 freed, 1 refused because it is alive, 2 not an object, 3 freed although alive). Answers one Erase, in order |
| 7 | References | repeated: `u64` address of an object, `u64` address of an object it keeps alive |
| 8 | Roots | repeated: `u64` address, `u8` kind (0 stack or thread static, 1 handle, 2 pinned handle, 3 ref-counted handle) |
| 9 | Attach | `u32` process id, then the app's name (rest of message). First on a control connection |

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
order and cover the whole heap. References and Roots follow them, before SnapshotEnd. They include
dependent handles and collectible types, so following References from Roots reaches exactly the
objects flagged live. References out of dead objects may be stale: they can name whatever now lives
where an erased object used to be.

## Viewer to GC

| Type | Name | Payload |
|---|---|---|
| 64 | Erase | `u8` flags (bit 0: free live objects too), `u32` count, that many `u64` object addresses |
| 65 | Done | empty |
| 66 | Collect | empty. On a control connection |

Messages to the GC are at most 64 KB, so at most 8,000 addresses per Erase. The GC decides for
itself what is alive; the flags in Objects are for display. It ignores bit 0 of Erase unless the
app was started with `MANUALGC_HARDCORE=1`. A malformed message ends the collection
and the app resumes.
