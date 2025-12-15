#!/usr/bin/env python3
"""Compares a sample's EXPECT lines with the GC's log. Usage: compare.py <stdout> <stderr>"""
import re, sys
out, err = (open(p).read().splitlines() for p in sys.argv[1:3])
logged = {line.split("] ", 1)[1] for line in err if line.startswith("[manualgc") and "] " in line}
good = bad = 0
for line in out:
    if not line.startswith("EXPECT"):
        continue
    want, name = re.fullmatch(r"EXPECT (.*?)\s{4}(.*)", line).groups()
    if want in logged:
        good += 1
    else:
        bad += 1
        key = " ".join(want.split()[:4]) + " "
        print(f"mismatch {name}: wanted '{want}', logged {[l for l in logged if l.startswith(key)] or 'nothing'}")
# Every walk must cover the heap exactly.
walks = [re.search(r"walked (\d+) of (\d+)", l).groups() for l in logged if " walked " in l]
short = [w for w in walks if w[0] != w[1]]
requested = next((int(l.split(": ")[1]) for l in out if l.startswith("collections requested")), None)
print(f"{good} of {good + bad} expectations matched; {len(walks)} collections logged, {requested} requested, {len(short)} incomplete walks")
sys.exit(not (good and not bad and walks and not short and len(walks) == requested))
