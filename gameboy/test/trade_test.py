#
# trade_test.py - two games of Pokemon Red trade through QR codes.
#
#   python3 -I trade_test.py RED.gb [OUTDIR]
#
# Needs Pokemon Red (USA, Europe), which can't be in the repo, and
# build/libgbpy.so (make build/libgbpy.so). Plays a new game to the
# bedroom, gives two players a party and the Pokedex, flies them to the
# Viridian City Pokemon Center, and has them trade: RED's Kadabra for
# BLUE's Machoke, which both evolve. On the way, RED offers the wrong
# Pokemon once and the trade is cancelled. Screenshots of each step go
# to OUTDIR (default build/trade).
#
import hashlib, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(HERE, '../../shared')]

import gbpy, gbtrade

RED_SHA1 = 'ea9bcae617fdf159b045185467ae58b2e4a48b9a'

# Red WRAM (pret/pokered symbols)
wPlayerID = 0xd359
wCurMap = 0xd35e
wYCoord = 0xd361
wDestinationMap = 0xd71a
wEnemyMons = 0xd8a4
wTopMenuItemY = 0xcc24
wMaxMenuItem = 0xcc28
wStatusFlags6 = 0xd732
wEventFlags = 0xd747
hSerialConnectionStatus = 0xffaa

EVENT_GOT_POKEDEX = 37
BIT_GAME_TIMER_COUNTING = 0
BIT_FLY_WARP = 3
VIRIDIAN_CITY = 0x01
REDS_HOUSE_2F = 0x26
VIRIDIAN_POKECENTER = 0x29
TRADE_CENTER = 0xef
MACHOKE, MACHAMP, KADABRA, ALAKAZAM = 0x29, 0x7e, 0x26, 0x95

# Party structs, made from pokered's base stats
KADABRA_30 = bytes.fromhex('2600431e001818645d64323c303900550000000000000000000000'
                           'a987191414141e00430020001c00480051')
PIDGEY_12 = bytes.fromhex('2400200c000002ff101c000030390003cd00000000000000000000'
                          'a987230f00000c0020001200100014000f')
RATTATA_9 = bytes.fromhex('a5001909000000ff21270000d4310002d900000000000000000000'
                          'a987231e00000900190010000c0013000a')
MACHOKE_32 = bytes.fromhex('290060200001015a02432b45d43100698d00000000000000000000'
                           'a98719141e14200060004b003700260029')

def with_stat_exp(mon, exp):
    # 0xFE bytes in a party struct go over the link as a patch list
    return mon[:17] + exp + mon[27:]

KADABRA_30 = with_stat_exp(KADABRA_30, bytes.fromhex('00fefe0100fe12340056'))
MACHOKE_32 = with_stat_exp(MACHOKE_32, bytes.fromhex('fe00abcdfefe00000001'))

def name(s):
    # the game's characters: capitals only here
    b = bytes(0x80 + ord(c) - ord('A') for c in s) + b'\x50'
    return (b + b'\x50' * 11)[:11]

def fail(msg):
    print('FAIL: ' + msg)
    sys.exit(1)

class Player:
    def __init__(self, label, out):
        self.gb = gbpy.GB()
        self.label = label
        self.out = out
        self.n = 0

    def shot(self, what):
        self.n += 1
        self.gb.shot(os.path.join(self.out, '%s-%02d-%s.png' % (self.label, self.n, what)))

    def run(self, frames, *buttons):
        self.gb.run(frames, *buttons)

    def press(self, *seq, after=40):
        for b in seq:
            self.gb.press(b, after=after)

    def peek(self, addr, n=1):
        return self.gb.peek(addr, n)

    def poke(self, addr, data):
        self.gb.poke(addr, data)

    def status(self):
        return self.gb.link_status()

    def wait(self, cond, what, frames=3000, press=None, after=40):
        # run (pressing a button now and then) until cond() holds
        for _ in range(frames // 60):
            if cond():
                return
            if press:
                self.press(press, after=after)
            else:
                self.run(60)
        self.shot('stuck')
        fail('%s: never got to %s (link %s)' % (self.label, what, self.status()))

def new_game(p):
    # title screen, NEW GAME, and through Oak's speech taking the preset
    # names (RED, BLUE) until the game starts in the bedroom
    p.run(1500)
    p.press('START', 'A', after=120)
    for who in ('player', 'rival'):
        p.wait(lambda: name_menu(p), 'the %s name menu' % who, press='A')
        p.press('DOWN', 'A', after=60)
        p.poke(wMaxMenuItem, b'\0')        # so the next menu can be seen
    p.wait(lambda: p.peek(wStatusFlags6)[0] & 1 << BIT_GAME_TIMER_COUNTING,
           'the bedroom', frames=6000, press='A')
    p.run(200)
    if p.peek(wCurMap)[0] != REDS_HOUSE_2F:
        p.shot('intro')
        fail('new game: not in the bedroom (map %02x)' % p.peek(wCurMap)[0])

def name_menu(p):
    # NEW NAME / RED / ASH / JACK: a 4-item menu, at the top left
    return p.peek(wMaxMenuItem)[0] == 3 and p.peek(wTopMenuItemY, 2) == b'\x02\x01'

def main_menu(p):
    # CONTINUE / NEW GAME / OPTION
    return p.peek(wMaxMenuItem)[0] == 2 and p.peek(wTopMenuItemY, 2) == b'\x02\x01'

def walk(p, direction, until, what):
    # step in a direction until until() holds; NPCs may be in the way
    for _ in range(30):
        if until():
            return
        p.run(16, direction)
        p.run(8)
    p.shot('walk')
    fail('%s: walking %s never got to %s' % (p.label, direction, what))

def pos(p):
    y, x = p.peek(wYCoord, 2)
    return x, y

def give_party(p, player, pid, mons):
    gb = p.gb
    gb.poke(0xd158, name(player))                       # wPlayerName
    gb.poke(wPlayerID, pid.to_bytes(2, 'big'))
    gb.poke(0xd163, bytes([len(mons)] + [m[0] for m, _ in mons] + [0xff]))
    for i, (m, nick) in enumerate(mons):
        gb.poke(0xd16b + 44 * i, m)                     # wPartyMons
        gb.poke(0xd273 + 11 * i, name(player))          # wPartyMonOT
        gb.poke(0xd2b5 + 11 * i, name(nick))            # wPartyMonNicks

    ev = wEventFlags + EVENT_GOT_POKEDEX // 8
    gb.poke(ev, bytes([gb.peek(ev)[0] | 1 << (EVENT_GOT_POKEDEX % 8)]))

    # fly to Viridian City, then walk in to the Cable Club desk
    gb.poke(wDestinationMap, bytes([VIRIDIAN_CITY]))
    gb.poke(wStatusFlags6, bytes([gb.peek(wStatusFlags6)[0] | 1 << BIT_FLY_WARP]))
    p.wait(lambda: p.peek(wCurMap)[0] == VIRIDIAN_CITY, 'Viridian City')
    p.run(120)
    walk(p, 'UP', lambda: p.peek(wCurMap)[0] == VIRIDIAN_POKECENTER, 'the Pokemon Center')
    p.run(60)
    walk(p, 'UP', lambda: pos(p)[1] == 4, 'the counter')
    walk(p, 'RIGHT', lambda: pos(p)[0] == 11, 'the Cable Club desk')
    walk(p, 'UP', lambda: pos(p)[1] == 3, 'the Cable Club desk')
    if pos(p) != (11, 3):
        p.shot('desk')
        fail('%s: not at the Cable Club desk' % p.label)

def cable_club(p):
    # talk to the desk, save, and on into the Trade Center; sit down
    p.wait(lambda: p.peek(wCurMap)[0] == TRADE_CENTER, 'the Trade Center', press='A')
    p.shot('trade-center')
    p.run(200)
    if p.peek(hSerialConnectionStatus)[0] != 2:
        fail('%s: game is not clocking the link' % p.label)
    p.run(4, 'RIGHT')
    p.run(20)
    p.press('A')
    p.wait(lambda: p.status()['state'] == 'select', 'the trade screen')
    p.run(300)
    if not p.status()['party_ok']:
        fail("%s: couldn't read the party the game sent" % p.label)
    p.shot('trade-screen')

def offer(p, index):
    # pick the index'th Pokemon, TRADE, then TRADE again to confirm
    p.press(*['DOWN'] * index)
    p.press('A', after=30)
    p.press('RIGHT', 'A', after=10)
    p.wait(lambda: p.status()['state'] == 'confirm', 'the TRADE question')
    p.run(300)
    p.shot('confirm')
    p.wait(lambda: p.status()['state'] != 'confirm', 'an answer', press='A')

def party(p):
    return gbtrade.read_party(p.peek(0xc000, 0x2000))

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    rom = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, 'build', 'trade')
    os.makedirs(out, exist_ok=True)

    if hashlib.sha1(open(rom, 'rb').read()).hexdigest() != RED_SHA1:
        fail('%s is not Pokemon Red (USA, Europe)' % rom)
    gbpy.load_rom(rom)

    red = Player('red', out)
    new_game(red)
    blue = Player('blue', out)
    blue.gb.set_state(red.gb.state())

    give_party(red, 'RED', 12345, [(KADABRA_30, 'KADABRA'), (PIDGEY_12, 'PIDGEY')])
    give_party(blue, 'BLUE', 54321, [(RATTATA_9, 'RATTATA'), (MACHOKE_32, 'MUSCLES')])

    # each shows a QR code of what they give; the other scans it
    red_name, red_party = party(red)
    blue_name, blue_party = party(blue)
    red_qr = gbtrade.encode(red_name, red_party[0])
    blue_qr = gbtrade.encode(blue_name, blue_party[1])
    print('RED shows  %s (%d characters)' % (gbtrade.label(red_party[0]), len(red_qr)))
    print('BLUE shows %s' % gbtrade.label(blue_party[1]))

    got_name, got = gbtrade.decode(blue_qr)
    red.gb.link_setup(got_name, got, red_party[0])
    got_name, got = gbtrade.decode(red_qr)
    blue.gb.link_setup(got_name, got, blue_party[1])

    for p in (red, blue):
        cable_club(p)

    # RED gives the Pidgey instead: refused
    offer(red, 1)
    red.wait(lambda: red.status()['state'] == 'select', 'the trade screen again')
    red.run(300)
    if red.status()['refused'] != 1 or red.status()['traded']:
        fail('RED: the wrong Pokemon was not refused: %s' % red.status())
    print('RED offered the Pidgey: refused')

    # then the Kadabra; BLUE the Machoke
    red.press('UP')
    for p, index in ((red, 0), (blue, 1)):
        offer(p, index)
        p.wait(lambda: p.status()['traded'], 'the trade')
        p.wait(lambda: p.status()['state'] == 'select', 'the trade screen after the trade',
               frames=6000, press='A')
        p.run(300)
        p.shot('after-trade')

    for p, gave, got, ot, ot_id, sent in ((red, KADABRA, MACHAMP, 'BLUE', 54321, MACHOKE_32),
                                          (blue, MACHOKE, ALAKAZAM, 'RED', 12345, KADABRA_30)):
        _, now = party(p)
        species = [m[0] for m in now]
        if gave in species or got not in species:
            fail('%s: party after the trade is %s' % (p.label, [gbtrade.label(m) for m in now]))
        m = now[species.index(got)]
        if gbtrade.text(m[gbtrade.MON:gbtrade.MON + gbtrade.NAME]) != ot \
                or int.from_bytes(m[12:14], 'big') != ot_id:
            fail('%s: the new Pokemon has the wrong trainer' % p.label)
        if m[17:29] != sent[17:29]:
            fail('%s: stat exp or DVs changed on the way: %s' % (p.label, m[17:29].hex()))

        # the game saved the party to cart RAM ("this allows reset into
        # Pokecenter"): count and species list at 0x2f2c
        sram = p.gb.cart_ram()
        wram_party = p.peek(0xd163, 8)
        if sram[0x2f2c:0x2f2c + 8] != wram_party:
            fail('%s: party not saved' % p.label)
        print('%s now has %s' % (p.label.upper(), [gbtrade.label(m) for m in now]))

        # and the other side now offers what it got, as it was sent
        gave_mon = KADABRA_30 if p is red else MACHOKE_32
        if p.peek(wEnemyMons, 44)[12:29] != gave_mon[12:29]:
            fail('%s: the Pokemon given came back changed' % p.label)

    # a second trade is refused; CANCEL leaves the table
    offer(red, 0)
    red.wait(lambda: red.status()['state'] == 'select', 'the trade screen after refusing')
    red.run(300)
    if red.status()['refused'] != 1:
        fail('RED: refused count changed after the trade')
    red.press('DOWN', 'DOWN', 'DOWN', 'A')
    red.wait(lambda: red.status()['state'] == 'ready', 'the room after CANCEL')
    red.run(200)
    red.shot('left-table')

    # the way out is off and on again: CONTINUE is at the Pokemon Center
    red.gb.reset()
    red.gb.link_off()
    if red.status() != dict(state='off', traded=False, refused=0, party_ok=False):
        fail('RED: unplugging kept the old trade: %r' % red.status())
    red.run(1500)
    red.wait(lambda: main_menu(red), 'the main menu', press='START', after=120)
    red.press('A', 'A', after=120)          # CONTINUE, and past the save info
    red.run(300)
    red.shot('continue')
    if red.peek(wCurMap)[0] != VIRIDIAN_POKECENTER:
        fail('RED: not at the Pokemon Center after the reset')
    if MACHAMP not in [m[0] for m in party(red)[1]]:
        fail('RED: Machamp is gone after the reset')
    print('RED reset: back at the Pokemon Center with the Machamp')

    print('TRADE TEST PASSED')

if __name__ == '__main__':
    main()
