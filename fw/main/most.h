#pragma once
/* MOST25 control messages as the OS8104A buffers hold them (DS8104AFP4 13.2.7, 13.2.8): 17 data bytes =
   FBlockID, InstID, FktID (12 bits) | OPType (4), TelID (4) | TelLen (4), 12 data bytes. Field packing, the OPType
   and error numbers and the segment counter are as JLR-HU and SocketMost use them in JLR cars (stock_amps.md: HU, SM).
   No ESP-IDF here: the selftest runs the same code */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MOST_DATA       12      /* data bytes of one telegram */
#define MOST_LONG       255     /* reassembled segmented message: the most an 8-bit length holds */
#define MOST_RCMB_LEN   20      /* mRCMB A0h..B3h: bRTYP, bRSAH, bRSAL, bRCD0..16 */
#define MOST_XCMB_LEN   21      /* mXCMB C0h..D4h: bXPRI, bXTYP, bXTAH, bXTAL, bXCD0..16 */

/* bRTYP of a received message (13.5.1) */
enum { MOST_RX_LOGICAL = 0, MOST_RX_PHYSICAL = 1, MOST_RX_BROADCAST = 2, MOST_RX_GROUPCAST = 3 };
#define MOST_BROADCAST  0x03C8  /* 13.1.4 */

/* OPTypes: properties and methods share the numbers (JLR-HU Common/enums.ts) */
enum {
    MOST_OP_SET = 0x0, MOST_OP_GET = 0x1, MOST_OP_SETGET = 0x2, MOST_OP_INC = 0x3, MOST_OP_DEC = 0x4,
    MOST_OP_GETINTERFACE = 0x5, MOST_OP_STATUS = 0xC, MOST_OP_INTERFACE = 0xE, MOST_OP_ERROR = 0xF,
};
enum {      /* methods */
    MOST_OP_START = 0x0, MOST_OP_ABORT = 0x1, MOST_OP_STARTRESULT = 0x2, MOST_OP_STARTRESULTACK = 0x6,
    MOST_OP_PROCESSING = 0xB, MOST_OP_RESULT = 0xC,
};
/* the first data byte of an Error reply */
enum {
    MOST_ERR_FBLOCK = 0x01, MOST_ERR_INST = 0x02, MOST_ERR_FKT = 0x03, MOST_ERR_OPTYPE = 0x04, MOST_ERR_LENGTH = 0x05,
    MOST_ERR_PARAM = 0x06, MOST_ERR_NOTAVAIL = 0x07, MOST_ERR_SEGMENT = 0x0C, MOST_ERR_FUNC = 0x20,
    MOST_ERR_UNAVAIL = 0x41,
};
/* TelID: 0 a single telegram; 1 first, 2 next, 3 last segment, each starting with a counter byte */
enum { MOST_TEL_SINGLE = 0, MOST_TEL_FIRST = 1, MOST_TEL_NEXT = 2, MOST_TEL_LAST = 3 };

typedef struct {
    uint8_t  type;          /* RX: bRTYP; TX: bXTYP, 0 = normal message */
    uint16_t src;           /* RX: logical address of the sender */
    uint16_t dst;           /* TX: target address */
    uint8_t  fblock, inst;
    uint16_t fkt;           /* 12 bits */
    uint8_t  op;            /* 4 bits */
    uint8_t  tel_id;        /* 4 bits */
    uint8_t  len;           /* TelLen 0..12 */
    uint8_t  data[MOST_DATA];
} most_msg_t;

/* a whole message after segment reassembly, or before splitting */
typedef struct {
    uint8_t  type;
    uint16_t src, dst;
    uint8_t  fblock, inst;
    uint16_t fkt;
    uint8_t  op;
    uint8_t  len;
    uint8_t  data[MOST_LONG];
} most_long_t;

void most_decode(const uint8_t raw[MOST_RCMB_LEN], most_msg_t *m);
void most_encode(const most_msg_t *m, uint8_t prio, uint8_t raw[MOST_XCMB_LEN]);
/* one log line: "rx 0161 > phys 01.04 000 Get [] " style; returns the length written */
int most_format(const most_msg_t *m, bool rx, char *buf, size_t n);
const char *most_op_name(uint16_t fkt, uint8_t op);

/* segments: feed every received telegram; true when a whole message is in *out (single telegrams pass straight).
   Each sender has its own slot, so another node's telegrams never break a message under way (MOST 2.5, table 2-5:
   one unfinished segmented message per sender). After a failure err holds the Segmentation Error's ErrorInfo for the
   message that failed, whose sender and function are in err_msg (MOST 2.5, 2.3.11.1.1, point 8) */
#define MOST_SEG_SLOTS  4
typedef struct {
    struct {
        bool busy;
        uint16_t src, fkt;
        uint8_t fblock, inst, op, next;
        most_long_t msg;
    } slot[MOST_SEG_SLOTS];
    uint8_t err;            /* 01h first segment missing, 02h too long, 03h unexpected segment, 04h too many senders,
                               07h a new message before the last one was finished; 0 none */
    most_long_t err_msg;    /* the failed message's type, sender, FBlock, InstID, FktID and OPType, no data */
} most_rx_seg_t;
bool most_reassemble(most_rx_seg_t *s, const most_msg_t *m, most_long_t *out);
/* splits a long message into telegrams; returns how many (0 if it does not fit) */
int most_split(const most_long_t *in, most_msg_t *out, int max);
/* FktIDs Status of an FBlock with the functions f[0..n) (ascending, from 000h); returns the bytes written, 0 if more
   than max */
int most_fktids(const uint16_t *f, int n, uint8_t *out, int max);
/* the other way: the FktIDs in a Status of n bytes into out (at most max); returns how many */
int most_fktids_decode(const uint8_t *b, int n, uint16_t *out, int max);
