"""Parse `fs read /lfs/settings` output into settings records and count them.

usage: parse_settings_dump.py <uart output file>

Record format (subsys/settings/src/settings_line.c with SETTINGS_ENCODE_LEN):
  uint16 little-endian length, then `name=value` of that many bytes.
A record whose value is empty is a delete. The newest record of a name wins.
"""
import collections, re, sys

text = open(sys.argv[1], errors="replace").read()
data = {}
size = None
for line in text.splitlines():
    m = re.search(r"File size: (\d+)", line)
    if m:
        size = int(m.group(1))
    m = re.match(r"^\s*([0-9A-F]{8})  ((?:[0-9A-F]{2} ){1,16})", line)
    if m:
        off = int(m.group(1), 16)
        for i, h in enumerate(m.group(2).split()):
            data[off + i] = int(h, 16)
n = max(data) + 1 if data else 0
missing = [i for i in range(n) if i not in data]
blob = bytes(data.get(i, 0) for i in range(n))
print(f"file size {size}, dump bytes {n}, missing bytes {len(missing)}")

records = []
pos = 0
while pos + 2 <= len(blob):
    length = blob[pos] | (blob[pos + 1] << 8)
    if length == 0 or length == 0xFFFF or pos + 2 + length > len(blob):
        break
    body = blob[pos + 2 : pos + 2 + length]
    eq = body.find(b"=")
    if eq < 0:
        print(f"unparsable record at {pos}")
        break
    records.append((pos, body[:eq].decode(errors="replace"), length - eq - 1))
    pos += 2 + length
print(f"records (lines) {len(records)}, parsed up to byte {pos}")

latest = {}
for off, name, vlen in records:
    latest[name] = vlen
live = {k: v for k, v in latest.items() if v > 0}
deleted = [k for k, v in latest.items() if v == 0]
stale = len(records) - len(latest)
print(f"unique names {len(latest)}, live {len(live)}, deleted {len(deleted)}, superseded lines {stale}")

def group(name):
    parts = name.split("/")
    if parts[0] == "mt" and len(parts) > 2 and parts[1] in ("f", "g", "acl", "ac"):
        return "/".join(parts[:3])
    return "/".join(parts[:2])

by_group = collections.Counter(group(k) for k in live)
for g, c in sorted(by_group.items()):
    print(f"  {g:24s} {c}")
print("live keys:")
for k in sorted(live):
    print(f"  {k} ({live[k]} bytes)")
