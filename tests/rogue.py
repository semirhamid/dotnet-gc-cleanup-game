#!/usr/bin/env python3
"""A viewer that tries to make the GC misbehave. Usage: rogue.py <port> <attack>

force     ask for live objects to be freed, without the app having allowed it
bogus     erase addresses that are not objects, and the same object twice
garbled   send a message that makes no sense
huge      announce a message larger than the GC accepts
silent    close the connection in the middle of a message
"""
import socket
import struct
import sys

# Message types, from docs/protocol.md.
HELLO, OBJECTS, SNAPSHOT_END, ERASE_RESULTS = 1, 4, 5, 6
ERASE, DONE = 64, 65
LIVE, FREE = 1, 2
FREED, ALIVE, NOT_AN_OBJECT = 0, 1, 2

port, attack = int(sys.argv[1]), sys.argv[2]
server = socket.socket()
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(1)
print("listening", flush=True)

def take(n):
    data = b""
    while len(data) < n:
        piece = gc.recv(n - len(data))
        if not piece:
            raise EOFError
        data += piece
    return data

def message():
    data = take(struct.unpack("<I", take(4))[0])
    return data[0], data[1:]

def erase(addresses, flags=0):
    body = struct.pack("<BBI", ERASE, flags, len(addresses)) + b"".join(struct.pack("<Q", a) for a in addresses)
    gc.sendall(struct.pack("<I", len(body)) + body)
    kind, data = message()
    assert kind == ERASE_RESULTS
    return [data[i + 8] for i in range(0, len(data), 9)]

# The app also opens a control connection, which says Attach. Keep it open and wait for a collection.
control, objects, hello = [], [], None
while hello is None:
    gc, _ = server.accept()
    gc.settimeout(20)
    kind, data = message()
    if kind == HELLO:
        hello = struct.unpack("<IIIQQB", data)
    else:
        control.append(gc)
while True:
    kind, data = message()
    if kind == OBJECTS:
        objects += [struct.unpack_from("<QQQ", data, i) for i in range(0, len(data), 24)]
    if kind == SNAPSHOT_END:
        break
live = [o[0] for o in objects if o[1] & LIVE]
dead = [o for o in objects if not o[1] & (LIVE | FREE)]
free = [o[0] for o in objects if o[1] & FREE]
base, end, hello_flags = hello[3:]

ok = True
if attack == "force":
    ok = hello_flags == 0 and set(erase(live[:200], flags=1)) == {ALIVE}
elif attack == "bogus":
    first = dead[0][0]
    bad = [0, 1, 8, base - 8, end, end + 8, base + 1, first + 8, live[0] + 4, live[0] + 8, 2**63, 2**64 - 8, free[0]]
    ok = (erase(bad) == [NOT_AN_OBJECT] * len(bad)
          and erase([first, first]) == [FREED, NOT_AN_OBJECT]
          and erase([]) == [])
elif attack == "garbled":
    gc.sendall(struct.pack("<IBBI", 6, ERASE, 0, 0xFFFFFFFF))  # a count with nothing after it
elif attack == "huge":
    gc.sendall(struct.pack("<I", 0xFFFFFFFF))
elif attack == "silent":
    gc.sendall(struct.pack("<IB", 100, ERASE))
if attack in ("force", "bogus"):
    gc.sendall(struct.pack("<IB", 1, DONE))
gc.close()
print("ok" if ok else "FAIL")
sys.exit(not ok)
