#pragma once
/* Stage 7: the serial number, and with it the stock amplifier's whole face on MOST (docs/firmware.md, "Серійний номер
   і навчання"). With SW1's "read serial number" key closed the device sits in the ring next to the stock amplifier
   (concept.md, "Серійний номер"): a plain node with no FBlocks on a dynamic address. It finds the amplifier, asks for
   its FBlocks, their functions and its Diagnosis answers - the serial number and the part numbers - and keeps them;
   from then on the emulation answers as that amplifier */
#include <stdbool.h>
#include <stdint.h>
#include "amp.h"
#include "most.h"

#define LEARN_FB    6       /* FBlocks kept */
#define LEARN_DID   5       /* Diagnosis answers kept */
#define LEARN_PART  "amp"   /* the NVS partition it is kept in (partitions.csv), apart from the settings: an erase of
                               the main NVS must not take the car's serial number with it */

typedef struct {
    uint8_t ver;
    uint16_t node;                                                          /* the amplifier's logical address */
    uint8_t nfb;
    struct { uint8_t fblock, inst, len; uint8_t fkts[96]; } fb[LEARN_FB];   /* FktIDs.Status as it came */
    uint8_t ndid;
    struct { uint8_t len; uint8_t data[2 + 32]; } did[LEARN_DID];          /* D22h Status [DID, value] as it came */
} learn_id_t;

typedef enum { LEARN_OFF, LEARN_RUNNING, LEARN_DONE, LEARN_FAILED } learn_state_t;

/* the read mode: from now on every received telegram goes to learn_on_msg(), and the answers out through send;
   set_node gives this node its dynamic address once the ring is known (NULL: none to set, as in QEMU) */
void learn_start(amp_send_t send, void (*set_node)(uint16_t node));
void learn_on_msg(const most_msg_t *m);
void learn_ring(uint8_t pos, uint8_t count);    /* the ring locked: this node's position, the number of nodes */
learn_state_t learn_state(void);
bool learn_active(void);
void learn_stop(void);                          /* QEMU: the telegrams go back to the emulation */
/* the emulation's profile: base with the learned node, FBlocks, functions and Diagnosis answers; NULL if none kept */
const amp_profile_t *learn_profile(const amp_profile_t *base);
void learn_forget(void);
void learn_show(void);
