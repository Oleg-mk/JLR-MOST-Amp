#pragma once
/* OS8104A MOST transceiver (DS8104AFP4) on I2C0: control port I2C, source ports serial, Enhanced mode (LEG low).
   Wiring: SR1 = SPDIF_SRC from the ESP32, SX0 = S/PDIF out, SX1 and SX3 = ring receivers to the ESP32 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "most.h"

/* page 0 registers (DS8104AFP4 appendix B); MRT0x00..MRT0x7F are memory locations 00h..7Fh */
#define OS_bXCR     0x80    /* transceiver control */
#define OS_bXSR     0x81    /* transceiver status */
#define OS_bSDC1    0x82    /* source data control 1 */
#define OS_bCM1     0x83    /* clock manager 1 */
#define OS_bMSGC    0x85    /* message control, write-only */
#define OS_bMSGS    0x86    /* message status, read-only */
#define OS_bNPR     0x87    /* node position */
#define OS_bIE      0x88    /* interrupt enable */
#define OS_bGA      0x89    /* group address */
#define OS_bNAH     0x8A    /* node address high */
#define OS_bNAL     0x8B    /* node address low */
#define OS_bSDC2    0x8C    /* source data control 2 */
#define OS_bSDC3    0x8D    /* source data control 3 */
#define OS_bCM2     0x8E    /* clock manager 2 */
#define OS_bNDR     0x8F    /* node delay */
#define OS_bMPR     0x90    /* maximum position: nodes in the ring */
#define OS_bMDR     0x91    /* maximum delay */
#define OS_bCM3     0x92    /* clock manager 3 */
#define OS_bSBC     0x96    /* synchronous bandwidth: quadlets */
#define OS_bXSR2    0x97    /* transceiver status 2 */
#define OS_mRCMB    0xA0    /* receive message buffer A0h..B3h (13.2.7) */
#define OS_mXCMB    0xC0    /* transmit message buffer C0h..D4h (13.2.8) */
#define OS_VERSION  0xC4    /* C4h..C6h: version, month, year; overwritten by the first mXCMB write */
#define OS_bXTS     0xD5    /* transmit result: 10h ok, 00h no answer, 20h CRC, 21h receiver full (13.2.3) */
#define OS_PAGE     0xFF    /* writing 0..3 selects the memory page */

#define XCR_MTR     0x80    /* timing master */
#define XCR_OE      0x40    /* transmitter on */
#define XCR_LB      0x20    /* Legacy bypass architecture */
#define XCR_ABY     0x02    /* set: node active, clear: All-Bypass */
#define XSR_MSL     0x40    /* masks: S/PDIF lock, transceiver lock, coding errors */
#define XSR_MXL     0x20
#define XSR_ME      0x10
#define XSR_ERR     0x08    /* sticky captures, cleared by writing 0 */
#define XSR_ESL     0x02
#define XSR_EXL     0x01
#define SDC1_EDG    0x80    /* SR sampled on the rising SCK edge, SX changes on the falling one */
#define SDC1_DEL    0x40    /* first bit one SCK after the FSY edge */
#define SDC1_IO     0x10    /* FSY and SCK are outputs */
#define SDC1_SPD    0x04    /* SR0/SX0 in S/PDIF format */
#define SDC1_MT     0x02    /* SX outputs on (clear: forced low) */
#define SDC1_TCE    0x01    /* SR1/SX1 are normal ports (clear: transparent) */
#define SDC2_64FS   0x60    /* SPR = 011: SCK 64Fs */
#define CM1_RD256   0x10    /* RMCK 256Fs = 12.288 MHz */
#define CM1_XTL256  0x00    /* the crystal is 256Fs */
#define CM1_MXXTAL  0x02    /* PLL source (or Frequency Regulator reference): crystal */
#define CM2_LOK     0x80    /* PLL unlocked */
#define CM2_NAC     0x40    /* activity on RX */
#define CM3_FREN    0x10    /* Frequency Regulator */
#define CM3_ASR     0x08
#define CM3_ACD     0x04
#define XSR2_DFE    0x08    /* deep RX FIFO, for the timing master */
#define IE_IALC     0x08
#define IE_IERR     0x04
#define IE_IMTX     0x02
#define IE_IMRX     0x01
#define MSGC_STX    0x80    /* send what mXCMB holds */
#define MSGC_RBE    0x40    /* release the receive message buffer */
#define MSGC_RALC   0x08    /* resets of the interrupt events */
#define MSGC_RERR   0x04
#define MSGC_RMTX   0x02
#define MSGC_RMRX   0x01
#define MSGC_RALL   0x0F
#define MSGS_RBS    0x80    /* mRCMB holds a message */
#define MSGS_TXR    0x40    /* the last transmission succeeded; valid with MTX */
#define MSGS_ALC    0x08
#define MSGS_ERR    0x04
#define MSGS_MTX    0x02
#define MSGS_MRX    0x01

/* MRT at SCK 64Fs (DS8104AFP4 12.2, 12.3): bytes 0..7 of an FSY frame on a pin, 0..3 left and 4..7 right, MSB first */
static inline uint8_t os8104_sr(int port, int byte) { return 0x40 + 8 * byte + port; }  /* MRA of an SRn byte */
static inline uint8_t os8104_sx(int port, int byte) { return 0x47 + 8 * byte - port; }  /* MRT location, SXn byte */
#define OS_MRA_ZERO 0xF8    /* MRA that sends zeros */

esp_err_t os8104_init(void);                    /* I2C0, the INT interrupt and its task; does not touch the chip */
esp_err_t os8104_reset(void);                   /* RS pulse, power-on interrupt, SCK out; stays in All-Bypass */
/* both starts reset the chip first, so a role can be changed at any time */
esp_err_t os8104_start_master(void);            /* bench: timing master on the 12.288 MHz crystal (DS8104AFP4 16.2) */
esp_err_t os8104_start_slave(uint16_t node, uint8_t group);  /* car: timing slave on RX (DS8104AFP4 16.3) */
esp_err_t os8104_set_node(uint16_t node, uint8_t group);     /* the logical and group address, the ring running */
/* cb runs in the INT task at every lock with this node's position and the number of nodes; it must not call the
   driver (the INT task holds it) */
void os8104_on_ring(void (*cb)(uint8_t pos, uint8_t count));
/* the same, on ALC (on: the ring's size or delay changed, DS8104AFP4 6.2.8 - not at every relock) and on a lock error
   with RX dark (off); os8104_poll() is the steady view */
void os8104_on_light(void (*cb)(bool on));
/* bench routes for the bit-exact check, SR1 bytes 0..7 back to SX1 and SX3: through the ring (light from TX back to
   RX) or straight inside the chip */
esp_err_t os8104_loop_routes(bool ring);
esp_err_t os8104_loop_off(void);                /* the bench routes undone: frame bytes 0..7 pass RX, SX1/SX3 zeros */
esp_err_t os8104_spdif(bool on);                /* SX0 from SR1, or zeros */
/* stage 4: the head unit's sinks from the ring to the ESP32, as usb2most routes them (main-v24.bin 8023F88): a
   16-bit sample is two ring channels, MSB first, left before right. SX1 carries the stereo sink, each sample in the
   top half of its 32-bit slot; SX3 the mono sinks as 16-bit halves: slot L mono 0 and 1, slot R mono 2. NULL routes
   zeros; both starts route zeros until the head unit connects */
esp_err_t os8104_route_stereo(const uint8_t ch[4]);
esp_err_t os8104_route_mono(int k, const uint8_t ch[2]);    /* k 0..2 */
/* 13.5.2.4, 14.2: n (1..8) channels from the timing master (physical address 0400h), the master asked again while
   busy; ch gets them, ch[0] being the Connection Label, *delay this node's delay (bNDR). Their MRT locations keep
   passing the ring through. Returns how many, 0 if none */
int os8104_alloc(int n, uint8_t ch[8], uint8_t *delay);
/* 13.5.2.5, 14.3: the channels of the label ch[0] back to the master */
esp_err_t os8104_dealloc(const uint8_t *ch, int n);
/* 14.1.1: this node's allocation table still has the channels under the label ch[0] - false after the master started
   afresh */
bool os8104_alloc_valid(const uint8_t *ch, int n);
/* bCM2 now: light = activity on RX, locked = the PLL locked; a lock loss masked after the last one is armed again
   once locked. The INT events alone see the light only on edges */
esp_err_t os8104_poll(bool *light, bool *locked);
/* page 0 only: the driver never switches pages */
esp_err_t os8104_read(uint8_t reg, uint8_t *buf, size_t n);
esp_err_t os8104_write(uint8_t reg, const uint8_t *buf, size_t n);   /* n <= 64 */
const uint8_t *os8104_version(void);            /* C4h..C6h as read at the last reset */
/* control messages (chapter 13): every received telegram lands in this queue (most_msg_t); sending waits for the
   chip's result, *xts gets bXTS */
QueueHandle_t os8104_rx_queue(void);
esp_err_t os8104_send(const most_msg_t *m, uint8_t *xts);
