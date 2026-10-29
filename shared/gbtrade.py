# gbtrade.py - Pokemon trades by QR code, for the Game Boy emulator.
#
# Each player picks the Pokemon they will give; their COLDCARD shows it
# as a QR code, and the other scans it. Then in the game, at the Cable
# Club, the emulator plays the other Game Boy, offering the scanned
# Pokemon (see external/c-modules/gameboy/gb_link.c).
#
# Red, Blue and Yellow, not the Japanese versions: those use other
# name lengths. Plain Python, so the host tests can use it too.
#
try:
    from uhashlib import sha256
except ImportError:
    from hashlib import sha256
    const = lambda x: x

NAME = const(11)
MON = const(44)
MON_BYTES = const(MON + 2 * NAME)      # party struct, OT name, nickname
PARTY = const(6)
NAME_END = const(0x50)

# Header byte 0x14a: 0 is Japan
DEST_JAPAN = const(0)

# Where the player's name is in WRAM, then the party right after it:
# Red and Blue, then Yellow.
PARTY_AT = (0xd158, 0xd157)

PREFIX = 'PKMN1:'
VERSION = const(1)

# Real species numbers (the game's own order, 1..190): bit n set for n
REAL_SPECIES = (b'\xfe\xff\xff\x7f\xfe\xff\xeb\x1e\xc7\x7f\x3c\x3f'
                b'\xff\xff\xf7\x79\x3f\xed\xfb\x6f\xf8\x2f\x1f\x7e')

def real_species(n):
    return 0 < n < 8 * len(REAL_SPECIES) and (REAL_SPECIES[n // 8] >> (n % 8)) & 1

def can_trade(title, dest):
    # title from the cartridge header
    return title.startswith('POKEMON') and dest != DEST_JAPAN

def party_ok(wram, at):
    # Does WRAM (from 0xc000) hold a player name and party at `at`?
    o = at - 0xc000
    name = wram[o:o + NAME]
    count = wram[o + NAME]
    if NAME_END not in name or not (1 <= count <= PARTY):
        return False
    species = wram[o + NAME + 1:o + NAME + 1 + count + 1]
    if species[count] != 0xff:
        return False
    mons = o + NAME + 1 + PARTY + 1
    for i in range(count):
        m = wram[mons + i * MON:mons + (i + 1) * MON]
        if m[0] != species[i] or not real_species(m[0]) or not (1 <= m[33] <= 100):
            return False
    return True

def read_party(wram):
    # The player's name, and their party as a list of MON_BYTES each.
    # None if it isn't where Red, Blue or Yellow keep it.
    for at in PARTY_AT:
        if party_ok(wram, at):
            break
    else:
        return None

    o = at - 0xc000
    name = bytes(wram[o:o + NAME])
    count = wram[o + NAME]
    mons = o + NAME + 1 + PARTY + 1
    ots = mons + PARTY * MON
    nicks = ots + PARTY * NAME
    party = []
    for i in range(count):
        party.append(bytes(wram[mons + i * MON:mons + (i + 1) * MON])
                     + bytes(wram[ots + i * NAME:ots + (i + 1) * NAME])
                     + bytes(wram[nicks + i * NAME:nicks + (i + 1) * NAME]))
    return name, party

# Base32 (RFC 4648, no padding): QR alphanumeric mode takes it as is

B32 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567'

def b32encode(data):
    out, acc, bits = [], 0, 0
    for b in data:
        acc = (acc << 8) | b
        bits += 8
        while bits >= 5:
            bits -= 5
            out.append(B32[(acc >> bits) & 31])
    if bits:
        out.append(B32[(acc << (5 - bits)) & 31])
    return ''.join(out)

def b32decode(text):
    out, acc, bits = bytearray(), 0, 0
    for ch in text:
        v = B32.find(ch)
        if v < 0:
            raise ValueError('bad character')
        acc = ((acc << 5) | v) & 0xffff
        bits += 5
        if bits >= 8:
            bits -= 8
            out.append((acc >> bits) & 0xff)
    return bytes(out)

def check(body):
    return sha256(body).digest()[:4]

def encode(name, mon):
    # QR text for one Pokemon (MON_BYTES) from a trainer
    assert len(name) == NAME and len(mon) == MON_BYTES
    body = bytes([VERSION]) + name + mon
    return PREFIX + b32encode(body + check(body))

def name_ok(b):
    return NAME_END in b and 0xfe not in b

def decode(text):
    # Trainer name and Pokemon (MON_BYTES) out of a QR's text; ValueError
    # if it isn't one of ours, or is damaged, or has a made-up Pokemon.
    text = text.strip().upper()
    if not text.startswith(PREFIX):
        raise ValueError("That isn't a Pokemon trade code.")
    raw = b32decode(text[len(PREFIX):])
    body, chk = raw[:-4], raw[-4:]
    if len(body) != 1 + NAME + MON_BYTES or check(body) != chk:
        raise ValueError('That trade code is damaged.')
    if body[0] != VERSION:
        raise ValueError('That trade code is from another version.')

    name = body[1:1 + NAME]
    mon = body[1 + NAME:]
    if not (name_ok(name) and name_ok(mon[MON:MON + NAME]) and name_ok(mon[MON + NAME:])
            and real_species(mon[0]) and 1 <= mon[33] <= 100):
        raise ValueError("That isn't a real Pokemon.")
    return name, mon

# The game's characters, for showing names

CHARS = {0x7f: ' ', 0xe3: '-', 0xe6: '?', 0xe7: '!', 0xe8: '.', 0xf4: ',', 0xe0: "'",
         0xba: 'e', 0xe1: 'PK', 0xe2: 'MN', 0xef: 'M', 0xf5: 'F', 0xf3: '/'}

def text(b):
    out = ''
    for c in b:
        if c == NAME_END:
            break
        if 0x80 <= c <= 0x99:
            out += chr(ord('A') + c - 0x80)
        elif 0xa0 <= c <= 0xb9:
            out += chr(ord('a') + c - 0xa0)
        elif 0xf6 <= c <= 0xff:
            out += chr(ord('0') + c - 0xf6)
        else:
            out += CHARS.get(c, '?')
    return out

def nickname(mon):
    return text(mon[MON + NAME:])

def level(mon):
    return mon[33]

def label(mon):
    # e.g. 'KADABRA  Lv30'
    return '%s  Lv%d' % (nickname(mon), level(mon))

def same(a, b):
    # Same Pokemon? Species, trainer ID, DVs, OT name, nickname (HP, PP,
    # status may have changed since). Matches gb_link_same_mon().
    def nm(x):
        return x[:x.index(NAME_END)] if NAME_END in x else x
    return (a[0] == b[0] and a[12:14] == b[12:14] and a[27:29] == b[27:29]
            and nm(a[MON:MON + NAME]) == nm(b[MON:MON + NAME])
            and nm(a[MON + NAME:]) == nm(b[MON + NAME:]))
