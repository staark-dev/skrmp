# Generates vehicle_names.js (model id -> in-game name) from the local GTA SA install:
#   python tools/gen_vehicle_names.py <GTA>/text/american.gxt <GTA>/data/vehicles.ide > vehicle_names.js
# GXT v4 (SA): MAIN table keys are JAMCRC32 of the uppercase key; vehicles.ide column 6 is the game-name key.
# Output is derived from game files and must not be committed (deploy into the CEF resource only).
import struct, zlib, re, json, sys
gxt = open(sys.argv[1], 'rb').read()
ide = open(sys.argv[2], encoding='latin-1').read()
# TABL: (name[8], offset) entries; MAIN table starts at its offset with TKEY directly
assert gxt[4:8] == b'TABL'
tabl_size = struct.unpack_from('<I', gxt, 8)[0]
main_off = None
for i in range(0, tabl_size, 12):
    name = gxt[12+i:20+i].rstrip(b'\0')
    off = struct.unpack_from('<I', gxt, 20+i)[0]
    if name == b'MAIN': main_off = off
p = main_off
assert gxt[p:p+4] == b'TKEY', gxt[p:p+8]
tkey_size = struct.unpack_from('<I', gxt, p+4)[0]
entries = [struct.unpack_from('<II', gxt, p+8+i) for i in range(0, tkey_size, 8)]
tdat = p + 8 + tkey_size
assert gxt[tdat:tdat+4] == b'TDAT'
data = tdat + 8
table = {}
for off, h in entries:
    s = gxt[data+off: gxt.index(b'\0', data+off)].decode('latin-1')
    table[h] = s
def variants(k):
    b = k.encode()
    c = zlib.crc32(b) & 0xFFFFFFFF
    return {'crc32': c, 'jamcrc': c ^ 0xFFFFFFFF}
probe = variants('LANDSTK')
mode = next((m for m, h in probe.items() if h in table), None)
print('hash mode:', mode, '->', table.get(probe.get(mode)) if mode else None, file=sys.stderr)
names = {}
in_cars = False
for line in ide.splitlines():
    t = line.strip()
    if t == 'cars': in_cars = True; continue
    if t == 'end': in_cars = False; continue
    if not in_cars or not t or t.startswith('#'): continue
    parts = [x for x in re.split(r'[,\s]+', t) if x]
    mid, key = int(parts[0]), parts[5]
    name = table.get(variants(key.upper())[mode]) if mode else None
    if not name: print('fallback', mid, parts[1], key, file=sys.stderr)
    names[mid] = name or parts[1].capitalize()
print('window.SKR_VEHICLE_NAMES=' + json.dumps(names, separators=(',', ':'), ensure_ascii=False) + ';')
