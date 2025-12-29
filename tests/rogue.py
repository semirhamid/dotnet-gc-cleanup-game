#!/usr/bin/env python3
"""A viewer that tries to make the GC do wrong. Usage: rogue.py <port> <attack>

force     ask for live objects to be freed, without the app having allowed it
bogus     erase addresses that are not objects, and the same object twice
garbled   send a message that makes no sense
huge      announce a message larger than the GC accepts
silent    close the connection in the middle of a message
"""
import socket, struct, sys
port, attack = int(sys.argv[1]), sys.argv[2]
server = socket.socket()
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(1)
print("listening", flush=True)
gc, _ = server.accept()
gc.settimeout(20)

def take(n):
    data = b""
    while len(data) < n:
        piece = gc.recv(n - len(data))
        if not piece: raise EOFError
        data += piece
    return data

def message():
    data = take(struct.unpack("<I", take(4))[0])
    return data[0], data[1:]

def erase(addresses, flags=0):
    body = struct.pack("<BBI", 64, flags, len(addresses)) + b"".join(struct.pack("<Q", a) for a in addresses)
    gc.sendall(struct.pack("<I", len(body)) + body)
    kind, data = message()
    assert kind == 6
    return [data[i + 8] for i in range(0, len(data), 9)]

objects, hello = [], None
while True:
    kind, data = message()
    if kind == 1: hello = struct.unpack("<IIIQQB", data)
    if kind == 4: objects += [struct.unpack_from("<QQQ", data, i) for i in range(0, len(data), 24)]
    if kind == 5: break
live = [o[0] for o in objects if o[1] & 1]
dead = [o for o in objects if not o[1] & 3]
free = [o[0] for o in objects if o[1] & 2]
base, end = hello[3], hello[4]
ok = True
if attack == "force":
    ok = hello[5] == 0 and set(erase(live[:200], flags=1)) == {1}
elif attack == "bogus":
    bad = [0, 1, 8, base - 8, end, end + 8, base + 1, dead[0][0] + 8, live[0] + 4, live[0] + 8, 2**63, 2**64 - 8, free[0]]
    first = dead[0][0]
    ok = erase(bad) == [2] * len(bad) and erase([first, first]) == [0, 2] and erase([]) == []
elif attack == "garbled":
    gc.sendall(struct.pack("<I", 6) + b"\x40\x00\xff\xff\xff\xff")
elif attack == "huge":
    gc.sendall(struct.pack("<I", 0xFFFFFFFF))
elif attack == "silent":
    gc.sendall(struct.pack("<I", 100) + b"\x40")
if attack in ("force", "bogus"):
    gc.sendall(struct.pack("<IB", 1, 65))
gc.close()
print("ok" if ok else "FAIL")
sys.exit(not ok)
