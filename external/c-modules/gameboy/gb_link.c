//
// gb_link.c - a make-believe Game Boy on the link cable, for trading one
// Pokemon with Red, Blue or Yellow. See gb_link.h.
//
// The game always clocks the link here: it calls with 0x01 and we answer
// 0x02, so it uses its internal clock and we only ever answer bytes. A
// real Game Boy answers each byte with what it loaded before the
// transfer, and so does the game; the patterns below allow for that.
//
// What the game sends, and what we answer:
//
//   Cable Club desk     0x01 -> 0x02 to link; 0x6n (sync) -> 0x60
//   Cable Club menu     0xDn -> 0xD4: we picked TRADE CENTER (with A)
//   Sitting down        0x6n -> 0x60, then three blocks, each led by
//                       0xFD bytes: 10 random numbers (unused when the
//                       game clocks), the trainer name and party, and a
//                       patch list. We send ours from the first 0xFD.
//   Trade screen        0x6n: the Pokemon picked, or 0x6F for CANCEL.
//                       We pick our one Pokemon (0x60), or cancel too.
//   TRADE / CANCEL      0x62 / 0x61. We say 0x62 only for the promised
//                       Pokemon, before the first trade.
//
// Each nybble is sent several times, then zero bytes: a run of 0x6n is
// one exchange.
//
#include <string.h>

#include "gb_link.h"

#define PREAMBLE            0xfd    // SERIAL_PREAMBLE_BYTE
#define NO_DATA             0xfe    // SERIAL_NO_DATA_BYTE
#define PATCH_END           0xff    // SERIAL_PATCH_LIST_PART_TERMINATOR
#define NAME_END            0x50
#define SPECIES_END         0xff

#define LINK_INTERNAL       0x01    // ESTABLISH_CONNECTION_WITH_INTERNAL_CLOCK
#define LINK_EXTERNAL       0x02    // ... WITH_EXTERNAL_CLOCK: what we are

#define MENU_TRADE_CENTER   0xd4    // item 0, A pressed
#define NYBBLE              0x60
#define NYB_CANCEL          0x0f
#define NYB_NO              0x01
#define NYB_YES             0x02

// The blocks we send, back to back. The game takes in a block from the
// byte after the first 0xFD it sees, and a fixed number of bytes:
// 17, 424 and 200. It drops leading 0x00/0xFD/0xFE when reading them.
#define RN_LEN              (7 + 10)
#define PARTY_LEN           (6 + GB_LINK_NAME + 1 + (GB_LINK_PARTY + 1) \
                             + GB_LINK_PARTY * GB_LINK_MON_BYTES + 3)
#define PATCH_LEN           200
#define OUT_LEN             (RN_LEN + PARTY_LEN + PATCH_LEN)

// Where things are in the stream of bytes the game sent during the blocks,
// counting from the transfer of our first 0xFD. The game stores what we
// send at 1..17 as random numbers; 18 is its first party block transfer,
// which takes our 0xFD; it stores 19..442 as our party; 443 takes 0xFD
// again, and it stores 444..643 as our patch list. What it sends lags by
// one transfer: its party block arrives at 20..443, its patch list at
// 445..644.
#define IN_PARTY            19
#define IN_PATCH            444

#if GB_LINK_OUT != OUT_LEN || GB_LINK_CAPTURE != IN_PATCH + PATCH_LEN
# error "block sizes"
#endif

static bool name_eq(const uint8_t *a, const uint8_t *b)
{
    for(int i = 0; i < GB_LINK_NAME; i++) {
        if(a[i] != b[i]) return false;
        if(a[i] == NAME_END) break;
    }
    return true;
}

    bool
gb_link_same_mon(const gb_link_mon_t *a, const gb_link_mon_t *b)
{
    return a->mon[0] == b->mon[0]                       // species
        && !memcmp(a->mon + 12, b->mon + 12, 2)         // OT ID
        && !memcmp(a->mon + 27, b->mon + 27, 2)         // DVs
        && name_eq(a->ot, b->ot)
        && name_eq(a->nick, b->nick);
}

// Our blocks, offering one Pokemon
static void build_out(gb_link_t *l, const gb_link_mon_t *offer)
{
    uint8_t *p = l->out;

    memset(l->out, 0, sizeof(l->out));

    memset(p, PREAMBLE, 7);
    p += 7;
    for(int i = 0; i < 10; i++) {
        *p++ = 0x10 + 0x17 * i;             // anything below 0xFD
    }

    memset(p, PREAMBLE, 6);
    p += 6;
    memcpy(p, l->name, GB_LINK_NAME);
    p += GB_LINK_NAME;
    *p++ = 1;
    *p++ = offer->mon[0];
    memset(p, SPECIES_END, GB_LINK_PARTY);
    p += GB_LINK_PARTY;

    uint8_t *mons = p;
    memcpy(p, offer->mon, GB_LINK_MON);
    p += GB_LINK_PARTY * GB_LINK_MON;
    memcpy(p, offer->ot, GB_LINK_NAME);
    p += GB_LINK_PARTY * GB_LINK_NAME;
    memcpy(p, offer->nick, GB_LINK_NAME);
    p += GB_LINK_PARTY * GB_LINK_NAME + 3;

    // 0xFE would read as "no data": send 0xFF and list where they were,
    // 1-based, in two parts (the second from party byte 0xFC on)
    memset(p, PREAMBLE, 3);
    p += 10;
    for(int part = 0; part < 2; part++) {
        int from = part ? PREAMBLE - 1 : 0;
        int to = part ? GB_LINK_PARTY * GB_LINK_MON : PREAMBLE - 1;
        for(int i = from; i < to; i++) {
            if(mons[i] == NO_DATA) {
                mons[i] = 0xff;
                *p++ = i - from + 1;
            }
        }
        *p++ = PATCH_END;
    }
}

// The game's party out of what it sent, the way the game reads ours
static bool parse_party(gb_link_t *l)
{
    const uint8_t *in = l->capture;
    gb_link_party_t *party = &l->party;
    uint8_t raw[1 + (GB_LINK_PARTY + 1) + GB_LINK_PARTY * GB_LINK_MON_BYTES];
    int i = IN_PARTY;

    memset(party, 0, sizeof(*party));

    while(i < IN_PATCH && (in[i] == 0 || in[i] == PREAMBLE || in[i] == NO_DATA)) i++;

    for(int n = 0; n < GB_LINK_NAME + (int)sizeof(raw); i++) {
        if(i >= IN_PATCH) return false;
        if(in[i] == NO_DATA) continue;
        if(n < GB_LINK_NAME) party->name[n] = in[i];
        else raw[n - GB_LINK_NAME] = in[i];
        n++;
    }

    const uint8_t *species = raw + 1;
    uint8_t *mons = raw + 1 + GB_LINK_PARTY + 1;
    const uint8_t *ots = mons + GB_LINK_PARTY * GB_LINK_MON;
    const uint8_t *nicks = ots + GB_LINK_PARTY * GB_LINK_NAME;

    // put back the 0xFE bytes the patch list names
    i = IN_PATCH;
    for(int part = 0; part < 2; part++) {
        int base = part ? PREAMBLE - 1 : 0;
        for(;; i++) {
            if(i >= GB_LINK_CAPTURE) return false;
            uint8_t b = in[i];
            if(b == 0 || b == PREAMBLE || b == NO_DATA) continue;
            if(b == PATCH_END) {
                i++;
                break;
            }
            int at = base + b - 1;
            if(at < GB_LINK_PARTY * GB_LINK_MON) mons[at] = NO_DATA;
        }
    }

    int count = raw[0];
    if(count < 1 || count > GB_LINK_PARTY || species[count] != SPECIES_END) return false;

    party->count = count;
    for(int k = 0; k < count; k++) {
        gb_link_mon_t *m = &party->mons[k];
        memcpy(m->mon, mons + k * GB_LINK_MON, GB_LINK_MON);
        memcpy(m->ot, ots + k * GB_LINK_NAME, GB_LINK_NAME);
        memcpy(m->nick, nicks + k * GB_LINK_NAME, GB_LINK_NAME);
        if(m->mon[0] != species[k]) return false;
    }

    return true;
}

    void
gb_link_setup(gb_link_t *l, const uint8_t name[GB_LINK_NAME],
              const gb_link_mon_t *theirs, const gb_link_mon_t *mine)
{
    memset(l, 0, sizeof(*l));
    memcpy(l->name, name, GB_LINK_NAME);
    l->theirs = *theirs;
    l->mine = *mine;
    l->chosen = -1;
    l->state = GB_LINK_WAITING;
}

    void
gb_link_off(gb_link_t *l)
{
    // forget the trade too: what's next is a new one
    memset(l, 0, sizeof(*l));
    l->state = GB_LINK_OFF;
}

static uint8_t blocks_byte(gb_link_t *l, uint8_t tx)
{
    int pos = l->pos++;

    if(pos == 0) {
        // after a trade we offer what we got
        build_out(l, l->traded ? &l->mine : &l->theirs);
    }

    l->capture[pos] = tx;

    if(l->pos == GB_LINK_CAPTURE) {
        l->party_ok = parse_party(l);
        l->state = GB_LINK_SELECT;
        l->in_sync = false;
    }

    return pos < OUT_LEN ? l->out[pos] : 0;
}

// A run of nybbles from the game has ended
static void sync_done(gb_link_t *l)
{
    l->in_sync = false;

    if(l->state == GB_LINK_SELECT) {
        if(l->nybble == NYB_CANCEL) {
            // both cancelled: back in the room
            l->state = GB_LINK_READY;
            return;
        }
        l->chosen = l->nybble;
        l->accept = !l->traded && l->party_ok && l->chosen < l->party.count
                    && gb_link_same_mon(&l->party.mons[l->chosen], &l->mine);
        l->state = GB_LINK_CONFIRM;
        return;
    }

    // GB_LINK_CONFIRM
    if(l->nybble == NYB_YES && l->accept) {
        // the game's own copy: the promised one, as it is now
        l->mine = l->party.mons[l->chosen];
        l->traded = true;
        l->state = GB_LINK_READY;      // it syncs, then sends blocks again
        return;
    }
    if(l->nybble == NYB_YES && !l->traded) l->refused++;
    l->state = GB_LINK_SELECT;
}

    uint8_t
gb_link_exchange(gb_link_t *l, uint8_t tx)
{
    if(l->state == GB_LINK_BLOCKS) {
        return blocks_byte(l, tx);
    }

    // the game calling, at the Cable Club desk (or after a reset)
    if(tx == LINK_INTERNAL) {
        l->state = GB_LINK_READY;
        return LINK_EXTERNAL;
    }

    switch(l->state) {
    case GB_LINK_READY:
        if((tx & 0xf0) == NYBBLE) return NYBBLE;
        if((tx & 0xf0) == 0xd0) return MENU_TRADE_CENTER;
        if(tx == PREAMBLE) {
            l->state = GB_LINK_BLOCKS;
            l->pos = 0;
            return blocks_byte(l, tx);
        }
        return 0;

    case GB_LINK_SELECT:
    case GB_LINK_CONFIRM:
        if((tx & 0xf0) == NYBBLE) {
            l->in_sync = true;
            l->nybble = tx & 0x0f;
            if(l->state == GB_LINK_SELECT) {
                return NYBBLE | (l->nybble == NYB_CANCEL ? NYB_CANCEL : 0);
            }
            return NYBBLE | (l->accept ? NYB_YES : NYB_NO);
        }
        if(l->in_sync) sync_done(l);
        return 0;

    default:
        // GB_LINK_WAITING: nobody home yet
        return 0;
    }
}
