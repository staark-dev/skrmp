"""OP215: stage the custom vehicle entries into an SA-MP CUSTOM.IMG.

Keeps every existing entry byte-for-byte at its sector, appends the 612/613
DFF/TXD sector spans from assets/op215/skr_vehicles.img (txd then dff, as in
the certified SKR-EXP-001 D5 archive), writes <target>.staging, verifies it,
backs up the original and atomically replaces the target.

Refuses to run while gta_sa.exe is running. Nothing is deleted.

usage: python -I tools/op215_stage_custom_img.py <target custom.img> <source skr_vehicles.img> [expected-sha256]
"""
import datetime
import hashlib
import os
import struct
import subprocess
import sys

SECTOR = 2048
ASSETS = ["skr_xc60.txd", "skr_xc60.dff", "skr_m4gts.txd", "skr_m4gts.dff"]
# SHA-256 of each full sector span (custom_img_registration.cpp pins).
PINS = {
    "skr_xc60.dff": (4208, "4BF4C6395F506A97CD09381E5CA39EF633F1CF07BA913324EEAECF8F72D08E14"),
    "skr_xc60.txd": (773, "190B88DE53D85481E6ECF16F85824450BEDAA2FE32C563C2341E281FF3C79DE9"),
    "skr_m4gts.dff": (3728, "82E6811E3CD500D800E9C97C53F365602D4880A1AA41AD6B479B8F646116D466"),
    "skr_m4gts.txd": (1332, "2245ECA09E806B32EB1FA63D961CE53FCAFC1B5CE12A466267F23F31E7E425BC"),
}


def fail(message):
    print("FAIL:", message)
    sys.exit(1)


def parse(data, label):
    if len(data) < 8 or data[:4] != b"VER2" or len(data) % SECTOR:
        fail(f"{label}: not a sector-aligned VER2 archive")
    count, = struct.unpack_from("<I", data, 4)
    if not 0 < count <= 4096 or 8 + 32 * count > len(data):
        fail(f"{label}: bad entry count {count}")
    entries = []
    for i in range(count):
        sector, sectors, _ = struct.unpack_from("<IHH", data, 8 + 32 * i)
        name = data[8 + 32 * i + 8: 8 + 32 * i + 32].split(b"\0")[0].decode("ascii")
        if not sectors or (sector + sectors) * SECTOR > len(data):
            fail(f"{label}: entry {name} out of bounds")
        entries.append((name, sector, sectors))
    spans = sorted((s, s + n) for _, s, n in entries)
    for a, b in zip(spans, spans[1:]):
        if a[1] > b[0]:
            fail(f"{label}: overlapping entries")
    return entries


def span(data, sector, sectors):
    return data[sector * SECTOR:(sector + sectors) * SECTOR]


def sha(data):
    return hashlib.sha256(data).hexdigest().upper()


def gta_running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq gta_sa.exe", "/NH"],
                         capture_output=True, text=True).stdout
    return "gta_sa.exe" in out.lower()


def main():
    if len(sys.argv) not in (3, 4):
        fail(__doc__)
    target, source = sys.argv[1], sys.argv[2]
    expected = sys.argv[3].upper() if len(sys.argv) == 4 else None
    if gta_running():
        fail("gta_sa.exe is running; the archive must not change while GTA may read it")

    original = open(target, "rb").read()
    existing = parse(original, "target")
    names = {n.lower() for n, _, _ in existing}
    print(f"target {target}: {len(original)} bytes sha={sha(original)} entries={[n for n, _, _ in existing]}")
    if any(a in names for a in ASSETS):
        if expected and sha(original) == expected:
            print("already staged, hash matches")
            return
        fail("target already contains custom entries; refusing to stage twice")

    src = open(source, "rb").read()
    src_entries = {n.lower(): (s, c) for n, s, c in parse(src, "source")}
    payload = []
    for name in ASSETS:
        if name not in src_entries:
            fail(f"source lacks {name}")
        s, c = src_entries[name]
        data = span(src, s, c)
        if (c, sha(data)) != PINS[name]:
            fail(f"source {name} does not match its pin")
        payload.append((name, c, data))

    count = len(existing) + len(payload)
    first_data = min(s for _, s, _ in existing)
    if 8 + 32 * count > first_data * SECTOR:
        fail("directory would overlap existing data; relocation is not supported")
    end = max(s + c for _, s, c in existing)
    out = bytearray(original[:end * SECTOR])
    struct.pack_into("<I", out, 4, count)
    for i, (name, c, data) in enumerate(payload):
        entry = 8 + 32 * (len(existing) + i)
        struct.pack_into("<IHH24s", out, entry, end, c, 0, name.encode("ascii"))
        out += data
        end += c
    out = bytes(out)

    # Verify: existing spans unchanged, new spans match pins, directory valid.
    staged = parse(out, "staged")
    for name, s, c in existing:
        if span(out, s, c) != span(original, s, c):
            fail(f"existing entry {name} changed")
    for name, s, c in staged:
        if name.lower() in PINS and (c, sha(span(out, s, c))) != PINS[name.lower()]:
            fail(f"staged {name} mismatch")
    digest = sha(out)
    if expected and digest != expected:
        fail(f"staged sha {digest} != expected {expected}")

    staging = target + ".staging"
    if os.path.exists(staging):
        fail(f"{staging} exists; inspect it first")
    with open(staging, "xb") as f:
        f.write(out)
        f.flush()
        os.fsync(f.fileno())
    if sha(open(staging, "rb").read()) != digest:
        fail("staging readback mismatch")
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    backup = f"{target}.pre-op215-{sha(original)[:8]}-{stamp}.bak"
    with open(backup, "xb") as f:
        f.write(original)
    if sha(open(backup, "rb").read()) != sha(original):
        fail("backup readback mismatch")
    if gta_running():
        fail("gta_sa.exe started during staging; target untouched, staging file left for inspection")
    os.replace(staging, target)
    final = open(target, "rb").read()
    print(f"backup {backup}")
    print(f"staged {target}: {len(final)} bytes sha={sha(final)}")
    for name, s, c in parse(final, "final"):
        print(f"  {name:24s} sector={s:6d} sectors={c:5d} sha={sha(span(final, s, c))[:16]}")
    if sha(final) != digest:
        fail("final readback mismatch")


if __name__ == "__main__":
    main()
