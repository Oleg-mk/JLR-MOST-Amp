#pragma once
/* Stage 2: every control message that reaches the node goes to the log. Stage 3 (skeleton): the node answers as the
   stock amplifier of the profile (docs/firmware.md, stock_amps.md 2, 5; the FBlocks, functions and part numbers as
   usb2most keeps them, ref/usb2most). Stage 4: the sinks the head unit connects play (amp_io_t) */
#include <stdbool.h>
#include "most.h"

typedef struct {
    uint8_t fblock, inst;
    const uint16_t *fkts;           /* its functions */
    int nfkts;
    const uint8_t *raw;             /* its FktIDs Status as it goes on the wire when that is not what most_fktids makes
                                       of fkts: usb2most's Hi-Line and AUD3 lists end in padding or are not ascending,
                                       a learned amplifier's are kept as they came. NULL: made from fkts */
    uint8_t nraw;
} amp_fb_t;

typedef struct {
    uint16_t did;                   /* Diagnosis D22h SetGet [DID] -> Status [DID, the value's field] */
    uint8_t len;                    /* the field, zero-padded after the value */
    const uint8_t *value;
    uint8_t vlen;
} amp_did_t;

/* a sink of the AudioAmplifier: what it feeds in the sound processing and how many ring channels its Connect brings */
typedef struct {
    uint8_t sink;                   /* SinkNr */
    uint8_t dsp;                    /* the processing's sink: 1 the stereo one (music), 2..4 mono; 0 answered, not routed */
    uint8_t nch;                    /* 4 stereo 16-bit, 2 mono 16-bit, 6 AUD3's video (its first 4 the stereo) */
    uint8_t row;                    /* its row in SpeakerLevel and MuteParameters, 1..4; 0 none. On the pre-facelift the
                                       same number as the SinkNr, on AUD3 not (docs/usb2most/decode_aud3.md, sect. 1) */
} amp_sink_t;

typedef struct {
    const char *name;
    uint16_t node;                  /* logical address of the stock amplifier */
    uint8_t group;
    const amp_fb_t *fb;             /* its FBlocks: fb[0] the AudioAmplifier with the sinks; the others answer FktIDs,
                                       Notification and their functions as stored values */
    int nfb;
    uint8_t nlvl;                   /* SpeakerLevel values per row: 5 on the pre-facelift, 7 on AUD3 */
    /* what it asks the network master, at the first Configuration OK: the master is whoever sent that */
    uint8_t gw_fblock, gw_inst;     /* the master's FBlock */
    const uint8_t *gw_get;          /* 305h Get with these bytes, NULL none */
    uint8_t ngw_get;
    const uint16_t *subs;           /* notifications it registers for, a message each */
    int nsubs;
    const amp_did_t *dids;          /* what its Diagnosis FBlock reports: part numbers, the serial number */
    int ndids;
    const amp_sink_t *sinks;        /* its sinks, by SinkNr ascending */
    int nsinks;
} amp_profile_t;

/* the types SW1 sets - key 3 bit 0, key 2 bit 1, key 1 bit 2 (board_switch(), Bitline docs/03_usage.md: Premium is
   key 3 ON): 0 pre-facelift Hi-Line, 1 pre-facelift Premium, 2 AUD3-8/12/16, which look alike on MOST
   (docs/firmware.md, "Профілі") */
extern const amp_profile_t AMP_PRE_HILINE, AMP_PRE_PREMIUM, AMP_AUD3;
const amp_profile_t *amp_profile_of(unsigned type);         /* a spare position: pre-facelift Hi-Line */

typedef bool (*amp_send_t)(const most_msg_t *m);    /* one telegram out; false when it was not delivered */

/* stage 4: what the sinks and the source drive; any of them may be NULL (QEMU, the selftest's captures) */
typedef struct {
    bool (*route)(int dsp, const uint8_t *ch, int n);   /* the processing's sink onto ring channels; ch NULL: off.
                                                           False: not taken (I2C), and Connect answers an error */
    void (*sound)(bool on);                             /* a sink connected or none: REM and the DAC follow */
    int (*alloc)(int n, uint8_t ch[8], uint8_t *delay); /* ring channels for a source: how many, 0 none */
    bool (*dealloc)(const uint8_t *ch, int n);          /* back to the master; false: the master kept them */
    bool (*alloc_ok)(const uint8_t *ch, int n);         /* the ring's allocation table still has them for us */
    int (*position)(void);                              /* this node's position in the ring (bNPR) */
} amp_io_t;

void amp_init(const amp_profile_t *p, amp_send_t send);    /* forgets all state */
const amp_io_t *amp_set_io(const amp_io_t *io);            /* returns the one before */
/* the OS8104A was started afresh (console): its routes are zeros and the head unit connects again, so the sinks,
   the source's channels and both notification matrices are forgotten, REM and the DAC told */
void amp_reset_link(void);
void amp_on_msg(const most_msg_t *m);                       /* a received telegram, segments included */
void amp_show(void);
const amp_profile_t *amp_profile(void);
amp_send_t amp_sender(void);

/* firmware side: the task that takes telegrams from the OS8104A, logs them and, when on, answers */
void amp_start(void);
void amp_set_log(bool on);
void amp_set_active(bool on);
bool amp_log_on(void);
bool amp_active(void);
bool amp_tx(const most_msg_t *m);                           /* send through the OS8104A, logged */

int amp_selftest(void);                                     /* selftest.c: failures, 0 = all passed */
