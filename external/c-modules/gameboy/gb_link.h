//
// gb_link.h - a make-believe Game Boy on the link cable, for trading one
// Pokemon with Red, Blue or Yellow (not the Japanese versions).
//
// The other player's Pokemon arrives some other way (a QR code); this
// plays their Game Boy's side of the Cable Club's Trade Center, offering
// that one Pokemon. The trade only goes through if the player gives the
// Pokemon they promised; anything else is cancelled. After one trade,
// every further trade is cancelled.
//
// The game's side of this protocol is in pret/pokered:
// engine/link/cable_club.asm, cable_club_npc.asm, home/serial.asm.
//
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define GB_LINK_NAME        11      // trainer name or nickname, 0x50 ends it
#define GB_LINK_MON         44      // party struct
#define GB_LINK_PARTY       6

// One Pokemon as the trade moves it: party struct, OT name, nickname
typedef struct {
    uint8_t mon[GB_LINK_MON];
    uint8_t ot[GB_LINK_NAME];
    uint8_t nick[GB_LINK_NAME];
} gb_link_mon_t;

#define GB_LINK_MON_BYTES   (GB_LINK_MON + 2 * GB_LINK_NAME)

// gb_link_t.state
enum {
    GB_LINK_OFF = 0,        // no cable
    GB_LINK_WAITING,        // cable in, nobody has called yet
    GB_LINK_READY,          // the game linked up (Cable Club)
    GB_LINK_BLOCKS,         // party data going back and forth
    GB_LINK_SELECT,         // at the trade screen
    GB_LINK_CONFIRM,        // both picked; waiting for TRADE or CANCEL
};

// The game's party, as it sent it over the cable
typedef struct {
    uint8_t name[GB_LINK_NAME];
    uint8_t count;
    gb_link_mon_t mons[GB_LINK_PARTY];
} gb_link_party_t;

// The blocks we send, and the bytes the game sends meanwhile; see gb_link.c
#define GB_LINK_OUT         641
#define GB_LINK_CAPTURE     644

typedef struct {
    int state;

    // the other player: name and their Pokemon on offer
    uint8_t name[GB_LINK_NAME];
    gb_link_mon_t theirs;

    // the Pokemon the player promised to give
    gb_link_mon_t mine;

    int pos;                    // in the stream of blocks we send
    uint8_t out[GB_LINK_OUT];
    uint8_t capture[GB_LINK_CAPTURE];
    gb_link_party_t party;      // the game's, parsed from capture
    bool party_ok;

    // nybble exchanges at the trade screen
    bool in_sync;
    uint8_t nybble;             // last one the game sent
    int chosen;                 // index of the game's Pokemon picked
    bool accept;                // is that the promised one?

    bool traded;                // done: theirs went in, mine came out
    int refused;                // trades cancelled because of a wrong pick
} gb_link_t;

// Plug the cable in, for one trade: their name and Pokemon, and the
// Pokemon the player gives in return.
void gb_link_setup(gb_link_t *l, const uint8_t name[GB_LINK_NAME],
                   const gb_link_mon_t *theirs, const gb_link_mon_t *mine);

// Unplug.
void gb_link_off(gb_link_t *l);

// One byte each way: the game is clocking the transfer and sends tx;
// returns our byte.
uint8_t gb_link_exchange(gb_link_t *l, uint8_t tx);

// Do two Pokemon look like the same one? (Species, trainer ID, DVs, OT
// name and nickname; HP, PP and status may have changed since.)
bool gb_link_same_mon(const gb_link_mon_t *a, const gb_link_mon_t *b);
