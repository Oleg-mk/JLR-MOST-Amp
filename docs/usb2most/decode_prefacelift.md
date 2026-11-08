# usb2most main-v24: pre-facelift amplifier driver (amp_lr2), wire reference from disassembly

Source: `main-v24.bin`, base 0x08020000. Everything comes from the code and the flash image. Names in headings (Bass, Limiter, ...) are the author's log strings, so they are all (label). Bytes and behaviour are from the code. The OS8104A data sheet in `most-spec/txt` was used only for register names.

Notation:
- `d[i]` = request data byte i; `len` = msg+0x0D; `n` = len-2 where stated.
- **Rep[...]** = reply on the ring: dst = request's src, FBlock/InstID/FktID copied from the request, OPType 0xC, data as listed. Object src field = DS[2] (see 1.1: it never reaches the wire).
- **Err06** = same header, OPType 0xF, len 1, data `06`.
- "No reply" = nothing goes on the ring: no Status, no Error, no Ack.
- T = amp type used by gating: mode==4 ? DS[0x0A] : DS[9].

---

## 1. Transport

### 1.1 Send: 0x08031504 (message object in r0)
Object: +0 dst u16, +2 src u16, +4 FBlock, +5 InstID, +6 FktID u16, +8 OPType, +0D len, +10 data ptr.
The result is a 21-byte record written to the OS8104 buffer mXCMB C0h..D4h (0x08023EB4..0x08023F1A sends start register C0h + 21 bytes):

| byte | reg | content |
|---|---|---|
| 0 | C0h bXPRI | 01 |
| 1 | C1h bXTYP | 00 (03 = Resource Allocate, 04 = De-Allocate, built elsewhere) |
| 2,3 | C2h,C3h | dst high, dst low |
| 4 | | FBlockID |
| 5 | | InstID |
| 6 | | FktID bits 11..4 |
| 7 | | FktID bits 3..0 << 4 \| OPType |
| 8 | | len <= 12: len. len > 12: TelID<<4 \| (m+1) |
| 9.. | | len <= 12: data. len > 12: [9] = segment counter from 0, [10..] = m data bytes, m <= 11 |

- Segmented: count = ceil(len/11); TelID 1 first, 2 middle, 3 last.
- msg+2 (src) goes in r1 to callback 0x08024862 (`b.w 0x08023A4C`), which ignores r1. So src is not on the wire; the chip's source address is bNAH/bNAL (inference from data sheet).
- TX ring: 0x46 slots of 21 bytes at 0x2000CE58, one slot kept empty. When full: log, OS8104 reset state (0x08023A08), message lost.
- No delays between messages at this layer; actual gaps are UNKNOWN.

### 1.2 Receive: 0x080249E0 then 0x08031380
- RX record: [2,3] source address, [4] FBlock, [5] InstID, [6,7] FktID/OP, [8] TelID<<4|len, data from [9].
- TelID 0: single. TelID 1: new slot, data from [10], len-1 bytes. TelID 2/3: appended to a slot found by **FBlockID only** (0x08031308); 3 completes. 10 slots, 255 bytes max.
- Object dst (+0) is not filled on RX; handlers never read it.
- Routing by mode (byte 0x20003F50[0], TBB at 0x080249FE): 0 = log only, 1 = 0x08030E10 (NM layer), **2 and 4 = 0x0803200C (amp layer)**, 3 = 0x08038E84, >4 = forwarded to the host.
- 0x0803200C: FBlock==01 goes to NetBlock table 0x080458C4 (any InstID); anything else goes to driver +38 (dispatcher).

### 1.3 Config and state
Config 0x20005700 (.data, flash 0x0804E8C8; can be replaced at boot from a saved copy, content UNKNOWN). Default bytes:
`+0B mode=01, +0C amp type=01, +0D second type=04, +12 surround=00, +17 volume index=05, +18..+1E all 00` (signed accessors 0x0802C52C/544/55C/574/5A4/58C/5BC for +18/+19/+1A/+1B/+1C/+1D/+1E).

DS 0x200184E0 (.bss, zero at reset; startup zeroes 0x20006524..0x200186B8):

| off | shape | meaning from code |
|---|---|---|
| +00 | u8 | init 1, then OS8104 register 87h (bNPR) from callback 0x08024978 in modes 2,4,7 |
| +02 | u16 | node address |
| +04 | u16 | 0x0322, only read by GroupAddress_Get |
| +06 | u8 | first channel, 0x40 = none |
| +07 | u8 | source selector, set by +28 (NM layer only) |
| +08 | u8 | surround (label), from cfg+12 |
| +09 | u8 | amp type 1 or 2, from cfg+0C |
| +0A | u8 | **no writer found** in the binary (literal scan, base-register scan), stays 0 (inference) |
| +0B..+15 | 11 bytes | copy of level struct from +4C, not read back. +0C also written by 46D Set |
| +18..+2F | msg object | pending Allocate/DeAllocate request. +1A = pending src, 0 = none |

Other RAM:
- .bss: BASS 0x200183F8 [1]; EQ 0x200183F9 [45 rows x 4]; MUTE 0x200184AD [4 rows x 6]; TREB 0x200184C5 [1]; XO 0x200184C6 [16]; SL 0x200184D6 [8].
- .data: E04T 0x20005800 [4], E05T 0x20005804 [7], E07T 0x2000580B [6], E08T 0x20005811 [14], LVL 0x20005821 [20], SUB 0x20005835 [1], FBL_H 0x20005836 [8], FBL_P 0x2000583E [8], DLY 0x200058D0 [18].
- Initial values are in section 9. BASS, TREB and MUTE have no reader: stored and never used.

### 1.4 Which mode is which (read from gating and routing, the firmware does not name them)
- Mode 2: FBlock 22 handlers reachable (T = DS[9]). Node 0x0186 unless changed. This is the "acts as amplifier" role.
- Mode 4: T = DS[0x0A] = 0, so every FBlock 22 message is dropped silently; 23.x, 06.x, 01.x and 02.x are still handled. Node 0x0187. On network config Ok, sends +40 to 0x0186 (section 8.3). On NotOk, switches itself to mode 2. 06.xx FktIDs always gets the T!=2 list here.
- Mode 1: NM layer (0x08030E10), not this driver's dispatcher. It calls driver methods with dst/inst from its device registry (section 10).

**Alloc callback routing** 0x08024918: only mode 4 calls 0x08031E28. Mode 2 logs only. Consequence in 3.3.

---

## 2. Dispatcher 0x08034E9C (driver +38; r0 = message)

| FBlock | InstID | gate | action |
|---|---|---|---|
| 22 | 02 | T==1 | table 0x08046650, else silent drop |
| 22 | 05 | T==2 | table 0x08046650, else silent drop |
| 22 | 20 | T in {1,2} | table 0x080468C0, else silent drop |
| 22 | other | | silent drop, no log |
| 23 | any (not checked) | none | table 0x08046900 |
| 06 | any (not checked) | none | table 0x080468E0 |
| 02 | any | OP==C, FktID==A00 | see below |
| 02 | any | OP==C, other FktID | log, no reply |
| 02 | any | OP!=C | log + data dump, no reply |
| other | | | log + data dump (0x08033FA0 only logs), no reply |

Table lookup 0x0802E6FC: key = FktID<<4 | (OP&0xF), linear over ascending keys. Not found: log "cant process", no reply, no Error. Handler call: r0 = DS, r1 = message.

**02.xx A00 Status** (no InstID check). len==0: nothing. d[0]:
- 00 (label NotOk): DS[2] = 0x0100 + DS[0]; mode := 2 via 0x08024C5E(2), which clears the RX pool, stores mode in 0x20003F50 and cfg+0B, reads the node from +0C into the OS8104 driver struct, and restarts the OS8104 state machine. FBlock list not updated. No ring message from the amp layer.
- 01 (label Ok): if mode==4: +40(dst=0x0186, inst = DS[9]==1 ? 02 : 05, src=DS[2]). Otherwise nothing.
- 02, 03 (labels invalid, new): log only. Other values: nothing.

---

## 3. Table 22.02 / 22.05: 0x08046650 (26 entries, both instances share it)

### 3.1 000 Get FktIDs, 0x080350C8
len and data not read. T==2: Rep[0x20 bytes from 0x08045AC7]. Otherwise Rep[0x21 bytes from 0x08045AA6]. Lists in section 5.

### 3.2 001 Set Notification, 0x080323B0
len<=4: log, no reply. d[0]!=1: log, no reply. Then logs d[1..2] (u16 BE) and each pair from d[3] (count (len-3)>>1). **No state stored. No reply. No notification mechanism exists in this driver.**

### 3.3 101 StartResult Allocate, 0x08034E20
len!=1: log, no reply. d[0] is used only as an echo.
- DS[6]!=0x40: Rep[d[0], DS[0], DS[6], DS[6]+1] (via 0x08031D5C, FktID 101 forced, OP C, len 4).
- DS[6]==0x40: copy request object to DS+18 (pending); send OS8104 system message `bXTYP=03, target 0400, byte 5 (C5h) = 02` (0x08024C56, 0x08023D90). No reply now.
- Later alloc_cb(mrt,num), **mode 4 only**, 0x08031E28: DS[6]=mrt; if pending: Status 101 to pending src, FBlock/Inst of pending, data [01, DS[0], mrt, mrt+1]. If mrt==0x40 (not granted): Status 102 [01] instead. Pending cleared.
- **In mode 2 the callback is not routed**, so DS[6] stays 0x40 and no 101 reply is ever sent. In mode 4 the handler is unreachable (T=0). The deferred path is dead in both (inference, depends on no writer of DS[0x0A]). An immediate reply in mode 2 is possible only if DS[6] was set earlier in mode 4.

### 3.4 102 StartResult DeAllocate, 0x08034D80
len!=1: log, no reply.
- DS[6]==0x40: Rep[d[0]] (via 0x08031DD0, FktID 102 forced).
- Else: copy request to pending; 0x08024C5A(DS[6]) sends `bXTYP=04, target 0400, byte 5 = DS[6]`. alloc_cb(0x40,mrt) runs synchronously inside it: in mode 4 it gives Status 102 [01]; in mode 2, no reply. Then DS[6]=0x40.

### 3.5 111 StartResult Connect, 0x08033ED4
len must be 4, 6 or 8, else log, no reply. Rep[d[0]], len 1.
Then only if d[0]==01 and len==6: 0x08024E98(first=d[2], count=4) writes OS8104 routing registers: 46h/56h/66h/76h = d[2]..d[2]+3; 47h/5Fh/67h/7Fh = F8; 4Fh = d[2]+1; 57h = d[2]; 6Fh = d[2]+3; 77h = d[2]+2. No ring message.
d[1] is logged as "delay" (label), otherwise unused. d[3..] not read. len 4 and 8: reply only, no routing.

### 3.6 112 StartResult DisConnect, 0x08034D00
len!=1: log, no reply. Rep[d[0]], len 1. No routing change.

### 3.7 202 Set Bass, 0x08032378 / 203 Set Treble, 0x08032340
len==1: d[0] stored in BASS / TREB. Other len: not stored. Log. No reply.

### 3.8 427 Set Limiter, 0x08032334
Log only. No length check, no state, no reply.

### 3.9 430 Set Crossover, 0x08033CD4. R=d[0], P=d[1], n=len-2
len<=1: log, no reply.

| condition | store | reply |
|---|---|---|
| R==0,P==0,n==16 | 16 bytes to XO | none |
| R<=4,P==0,n==4 | 4 bytes to XO+4*(R-1) (R==0: XO-4, hits MUTE tail and TREB) | none |
| R<=4,P<=2,n==2 | u16 d[2..3] to XO+4*(R-1)+8*P (as coded; R=1,P=2 lands at XO+0x10 = SL) | none |
| else | | Err06 |

### 3.10 431 Set CrossoverSlope, 0x08033BC4. R=d[0], P=d[1], n=len-2
len<=1: log, no reply.

| condition | store | reply |
|---|---|---|
| R==0,P==0,n==8 | 8 bytes to SL | none |
| R<=4,P==0,n==2 | u16 to SL+2*(R-1) (R==0: SL-2) | none |
| R<=4,P<=2,n==1 | d[2] to SL+2*(R-1)+2*P (as coded) | none |
| else | | Err06 |

### 3.11 441 Set SpeakerDelay, 0x08034200
len<=18: memcpy(DLY, d, len). len>18: not stored. Log, dump. No reply, never Error.

### 3.12 461 Set Equalizer, 0x08033AF0. R=d[0], P=d[1], n=len-2
len<=1: silent, no reply.

| condition | store | reply |
|---|---|---|
| R==0,P==0,n==0xB4 | 180 bytes to EQ | none |
| R<=0x2D,P==0,n==4 | 4 bytes to EQ+4*(R-1) (R==0: EQ-4, hits BASS) | none |
| R in F0,F1,F2, P==0, n==0x3C | 60 bytes to EQ+60*(R&0xF) | none |
| else | | Err06 |

**F3 is rejected here, while driver +40 sends F3 with n=64 (8.3).**

### 3.13 461 Get, 0x0803351C
len!=2 or d[0]>0x2C: Err06.
- d[1]!=0: Rep[] with len 0.
- d[0]==0: Rep[00 00, 180 bytes of EQ], len 0xB6.
- d[0] in 1..2C: Rep[d[0], 00, 4 bytes of row d[0]], len 6. Row 45 is reachable only via the all-rows form.

### 3.14 466 Set Mute, 0x08033AAC
If d[0]<=4 and len==7: 6 bytes d[1..6] to MUTE+6*(d[0]-1) (d[0]==0: 0x200184A7, overwrites last 6 EQ bytes). Otherwise not stored. Log. No reply. d[0] is read before any length check.

### 3.15 46D Set SpeakerLevel, 0x08034140. R=d[0], P=d[1], n=len-2
len<=1: silent, no reply. P!=0: nothing stored. With P==0:
- R==0,n<=0x16: memcpy(LVL, d+2, n) (n=22 also overwrites SUB and FBL_H[0]).
- R==0,n>0x16: memcpy(0x2000581C, d+2, n). No upper limit: long messages run over the FBlock lists and the driver method table at 0x20005848.
- R in 1..4: memcpy(0x2000581C+5*R, d+2, n). n not checked against 5.
- R>4: not stored.

Then if R==1 and P==0: DS[0x0C] = +44(signed d[2]) (no reader found). Log, dump. No reply.

### 3.16 46D Get, 0x08033DE0
len!=2: Err06. d[1]!=0: Rep[] len 0. d[0]==0: Rep[00 00, 20 bytes LVL], len 0x16. d[0] in 1..4: Rep[00 00, 5 bytes at 0x2000581C+5*d[0]], len 7 (first byte is 00, not d[0]). d[0]>4: Rep[] len 0.

### 3.17 E04, E05, E07

| Fkt | Set handler | Set rule | Get handler | table |
|---|---|---|---|---|
| E04 | 0x08034110 | len==4: store whole | 0x08034C4C | E04T [4] |
| E05 | 0x08035074 | len==7: store whole | 0x08034B98 | E05T [7] |
| E07 | 0x080340E0 | len==6: store whole | 0x08034AE4 | E07T [6] |

Set: log, dump, no reply; wrong length is silently not stored.
Get (same logic for all, E08 too): len!=2: Err06. d[0]==0 and d[1]==0: **the first two table bytes are written to 00 in RAM**, then Rep[table] at full length. Otherwise Rep[] len 0.

### 3.18 E08 Set, 0x08034008. R=d[0], P=d[1], n=len-2
len<=1: log, then always ends in Err06.

| condition | store | reply |
|---|---|---|
| R==0,P==0,n==14 | 14 bytes to E08T | none |
| R in 1,2, P==0, n==7 | 7 bytes to E08T+7*(R-1) | none |
| R==0,P==0,n==7 | computed address 0xA000580A (14*-1 >> 1 unsigned): wild write | none if it survives |
| else | | Err06 |

### 3.19 E08 Get, 0x08034A30
As 3.17. Reply len 14 = E08T with bytes 0,1 zeroed. So a Set of `00 00 + 14 bytes` reads back as 14 bytes: the payload's first two bytes are lost and no `00 00` header is added.

### 3.20 E09 Set, 0x08034338 (also in 22.20 table)
len!=1: log, no reply. len==1: d[0] to SUB. No reply.

---

## 4. Other tables

### 22.20: 0x080468C0 (3 entries)
- 000 Get 0x080334B8: Rep[11 bytes from 0x080459CB]. Not type dependent.
- 466 Set 0x08032328: log only, no reply.
- E09 Set 0x08034338: see 3.20.

### 23.xx: 0x08046900 (3 entries)
- 000 Get 0x08033454: Rep[11 bytes from 0x080459C0].
- 466 Set 0x0803231C: log only, no reply.
- E00 Set 0x08033FEC: log and dump only, no state, no reply.

### 06.xx: 0x080468E0 (3 entries)
- 000 Get 0x0803516C: T==2: Rep[0x26 bytes from 0x08045B0D]. Otherwise Rep[0x26 bytes from 0x08045AE7].
- D19 StartResult 0x08033260. len must be 2 or 5, else log, no reply. Replies OP C, FktID D19 copied. d[4] is never read. Same bytes for Hi-Line and Premium.

| request | reply bytes |
|---|---|
| len 2: `02 AF` | 0x26: `02 7F 9D 84 13 69 F0 00 87 28 9A 03 13 28 9A 04 13 28 9A 05 13 2D 9A 06 13 2D 9A 10 13 2D 9A 11 13 2D 9A 03 11 68` |
| len 5: `04 9D 84 13 xx` | 0x1C: `04 9D 84 13 69 10 05 DD 00 5E 3C 4F DD DD 01 00 02 7A BD DD 02 38 DD 04 3C DD 06 C7` |
| len 5: `06 9D 84 13 xx` | 0x15: `06 9D 84 13 2C 01 DD 02 81 03 FF 04 8E 05 00 10 80 11 7F 12 00` |
| len 5: `04 F0 00 87 xx` | 0x1C: `04 F0 00 87 28 10 05 DD 00 FF FF FF FF DD 01 FF FF FF FF DD 02 FF DD 04 FF DD 06 FF` |
| len 5: `06 F0 00 87 xx` | 0x15: `06 F0 00 87 28 01 FE 02 FD 03 FF 04 80 05 00 10 80 11 7F 12 00` |
| anything else | log, no reply |

- D22 StartResult 0x0803485C. len!=2: log, no reply. d[0]!=F1: log, no reply. d[0]==F1 as below. Same bytes for Hi-Line and Premium.

| d[1] | Rep, bytes | ASCII part (label in log) |
|---|---|---|
| 8C | 0x12: `F1 8C 36 39 39 31 32 39 39 31 37 39 33 32 00 00 00 00` | 699129917932 (ecu_sn) |
| 12 | 0x1A: `F1 12` + ASCII + 10x00 | 6H52-18C808-CD (ecu_assembly_pn) |
| 91 | 0x1A: `F1 91` + ASCII + 10x00 | 6H52-14C524-CB (vm_ecu_hw_pn) |
| 88 | 0x1A: `F1 88` + ASCII + 10x00 | 6H52-14C526-CD (vm_ecu_sw_pn) |
| 24 | 0x1A: `F1 24` + ASCII + 10x00 | 6H52-14C527-CB (ecu_cal_01_pn) |
| other | OP F, len 4: `06 01 F1 d[1]` | |

---

## 5. FktID lists (MOST 2.3.9: toggle positions, state starts on at 000)

| list | raw | toggles | available |
|---|---|---|---|
| amp1 T!=2, 0x08045AA6, 0x21 | `00 31 11 11 42 02 20 44 27 42 84 30 43 24 41 44 24 61 46 24 66 46 74 6D 46 EE 08 E0 90 00 00 00 00` | 003 111 114 202 204 427 428 430 432 441 442 461 462 466 467 46D 46E E08 E09 000 000 000 | **000 001 002 111 112 113 202 203 427 430 431 441 461 466 46D E08** |
| amp1 T==2, 0x08045AC7, 0x20 | `00 31 11 11 42 02 20 44 27 42 84 30 43 24 41 44 24 61 46 24 66 46 74 6D 46 EE 04 E0 6E 07 E0 90` | same up to 46E, then E04 E06 E07 E09, stuffing 0 | **000 001 002 111 112 113 202 203 427 430 431 441 461 466 46D E04 E05 E07 E08** |
| 22.20, 0x080459CB, 0x0B | `00 34 27 42 84 66 46 7E 09 E0 A0` | 003 427 428 466 467 E09 E0A, 0 | 000 001 002 427 466 E09 |
| 23.xx, 0x080459C0, 0x0B | `00 31 11 11 44 66 46 7E 00 E0 10` | 003 111 114 466 467 E00 E01, 0 | 000 001 002 111 112 113 466 E00 |
| 06.xx T!=2, 0x08045AE7, 0x26 | `00 3D 10 D1 2D 14 D1 5D 19 D1 AD 22 D2 3D 27 D2 8D 2E D2 FD 31 D3 2D 34 D3 5D 36 D3 5D 36 D3 FD 85 D8 6E 10 E1 10` | 003 D10 D12 D14 D15 D19 D1A D22 D23 D27 D28 D2E D2F D31 D32 D34 D35 D36 **D35 D36** D3F D85 D86 E10 E11, 0 | 000 001 002 D10 D11 D14 D19 D22 D27 D2E D31 D34 D36..D3E D85 E10 (non-monotonic list; range read literally) |
| 06.xx T==2, 0x08045B0D, 0x26 | `00 3D 10 D1 2D 14 D1 5D 19 D1 AD 22 D2 3D 27 D2 8D 2E D2 FD 31 D3 2D 34 D3 5D 36 D3 8D 3E D3 FD 85 D8 6E 10 E1 10` | same up to D36, then D38 D3E D3F D85 D86 E10 E11, 0 | 000 001 002 D10 D11 D14 D19 D22 D27 D2E D31 D34 D36 D37 D3E D85 E10 |

- Who gets what: the amp1 table runs only with T==1 (22.02, Hi-Line list) or T==2 (22.05, Premium list).
- Trailing `000 000 000` in the Hi-Line list: padding (inference); a literal RLE decoder would toggle on them.

---

## 6. NetBlock: table 0x080458C4 (8 entries; FBlock 01, any InstID), r0 = 0x200183BC

Replies: dst = req.src, FBlock/InstID/FktID copied, OP C. Request data is not checked unless stated. Unknown key: log and dump, no reply.

| key | handler | request | reply |
|---|---|---|---|
| 000 Get FBlockIDs | 0x08031840 | none | DS[9]==1: `22 02 22 20 23 01 06 XX`; otherwise `22 05 22 20 23 01 06 XX`. Default XX = low byte of node at init (86, or 87 in mode 4). Uses DS[9], not T. |
| 000 SetGet | 0x0803191C | len!=3: log, no reply. d[0]=FBlock, d[1] logged only, d[2]=new inst | d[0]==06: byte 7 of both lists = d[2]. d[0]==23: byte 5 of both lists = d[2]. Other: no change. Then same reply as 000 Get. |
| 001 Get DeviceInfo | 0x080318B4 | none | `FF FF 00` |
| 002 Get NodePositionAddress | 0x080317E0 | none | `40 DS[0]` (byte 0 is 0x40) |
| 003 Get NodeAddress | 0x0803177C | none | DS[2] high, low |
| 004 Get GroupAddress | 0x08031718 | none | `03 22` (DS[4]). OS8104 bGA is written 0x50 at 0x080241EA, the only immediate write found, so the chip's group is 0x0350 (inference from data sheet). |
| 006 Set ShutDown | 0x080316A8 | len!=1: nothing. d[0]==02: log, 0x0802405C (chip registers 88h=00, 85h=0F, 8Eh=20, state 3), no reply | d[0]!=02: Rep[d[0]] |
| 401 Set Notification | 0x08031630 | len<=4 or d[0]!=1: log | no state, no reply |

---

## 7. Init and node address

0x08031CB0, called once from 0x08024B90 before OS8104 setup.
- ampstate 0x200183BC: +0 driver table, +4 = 0x080458C4, +8 = cfg+0C, +9 = cfg+0D (no reader in the amp layer), +0B = 0 (current volume index).
- Driver by type: 1,2 = LR2 0x20005848; 3,4,5 = 0x200059DC; 6,7 = 0x20005920; other = stub.

Calls, in order:
1. +00(type): DS[6]=40, DS[9]=type, DS[1A]=0. Type 2: EQ from 0x080467DC, XO from 0x080468A0, SL from 0x080468B8. Type 1: EQ from 0x08046728, XO from 0x08046890, SL from 0x080468B0.
2. +10(0x0186): DS[2]=0x0186; FBL_H[7]=FBL_P[7]=86.
3. If mode==4: +10(0x0187): DS[2]=0x0187; both [7]=87.
4. +08(1): DS[0]=1.
5. +18(0x0322): DS[4].
6. +28(..,0): DS[7]=0.
7. +30(cfg+12): DS[8].
8. +20(0x40): DS[6].
9. Pending src=0.

Then 0x08024B9A: 0x08024AE4(mode): modes 2,4,7 read the node from +0C; the OS8104 state machine later writes bNAH/bNAL from it, and bGA=50.

Node address and Diagnosis InstID afterwards:

| event | node (DS[2]) | 06 InstID in FBlock list | DS[0] |
|---|---|---|---|
| boot, mode!=4 | 0x0186 | 86 | 1 |
| boot, mode 4 | 0x0187 | 87 | 1 |
| 0x08024978 callback (modes 2,4,7, tick from 0x0802456E) | no change | no change | register 87h |
| 02.xx A00 Status d[0]=00 | 0x0100+DS[0], mode:=2 | **no change** | no change |
| 02.xx A00 Status d[0]=01 | no change | no change | no change |
| 01.xx 000 SetGet [06,x,y] | no change | y | no change |

+10 has no other callers. Lock/unlock callbacks do nothing in modes 2 and 4. The dispatcher does not compare the InstID of 06 or 23 with the list.

---

## 8. Driver methods, flash 0x0804EA10. Args: r0=dst, r1=inst, r2=src, r3=fourth

All ring messages: bXPRI 01, OPType 0 unless stated. All FBlock 22 with InstID=r1 unless stated.

### 8.1 Non-sending
- +00 init; +04/+08 DS[0]; +0C/+10 DS[2] (+10 also list byte 7); +14/+18 DS[4]; +1C/+20 DS[6]; +24 (fblock,inst) list update.
- **+28: DS[7]=r3, no message.** +2C/+30 DS[8]; +34 returns DS+18; +38 dispatcher.
- +3C (r0=&size): size=8, returns FBL_H if DS[9]==1, else FBL_P.
- +44(level): level=min(level,0), clamped to >=-99; returns the index of the last row in VOL with row[1]<=level, 0x24 if all pass.
- **+48: no message**, see below.
- +6C (0x08035070) and +70 (0x08032238): `bx lr`.

**+48 0x08033604(out, idx)**: idx clamped to <=0x23, row=VOL[idx], returns idx.
- `base = row[1] + (DS[7]==4 ? row[5] : DS[7] in 1..3 ? row[4] : 0)`, int8.
- `F=cfg+1A, B=cfg+19` (signed). `A1=F>0?T1[F]:0, A2=F<0?T1[-F]:0, B1=B>0?T1[B]:0, B2=B<0?T1[-B]:0`.
- `out[0]=0, out[1]=idx, out[2]=0x23, out[3]=base-A1-B1, out[4]=row[0]+base+cfg18, out[5]=base-A1-B2, out[6]=row[2]+base-A2-B1, out[7]=row[2]+base-A2-B2, out[8]=out[9]=9D, out[10]=min(row[3]+base+cfg1E,0)`.
- out[10],[3],[5],[6],[7] below -99 become 9D (out[4] not clamped).
- DS[8]==0: out[4]=9D. DS[8]==2: v=int8(row[6]+base), out[3..7]=out[10]=v.

### 8.2 Sending

| method | addr | FktID | data | notes |
|---|---|---|---|---|
| +4C | 0x080330E8 | 46D | `01 00 p[3] p[4] p[5] p[6] p[7]` (r3=p, 11 bytes, stored to DS+0B) | followed by +64(p[10]) |
| +50 | 0x08033088 | 46D | `r3 00 a 9D a b b`, a=stack0, b=stack1 | wrapper 0x080319EC passes r3=04 for types 1,2 |
| +54 | 0x08032FE8 | E08 | `00 00 00 50 00 1E row[7] 01 01 07 D0 13 88 row[9] 01 01`, row=VOL[r3] | len 16, no clamp |
| +58 | 0x08033778 | 46D | `00 00 out[3] 9D out[5] out[6] out[7]` + 15x`9D`, out=+48(r3) | len 22 |
| +5C | 0x08032FAA | 202 | `r3` | |
| +60 | 0x08032F6C | 203 | `r3` | |
| +64 | 0x08032F2C | E09 | `r3` | **FBlock 22 InstID 20 constant**, r1 ignored |
| +68 | 0x080344DC | E04 | `01 00 r3` | DS[9]==1: not sent |
| +74 | 0x08032EA8 | E05 | 1: `01 00 X`; 2: `03 00 14` | DS[9]==1: neither sent. X = r3>=0 ? 6+T2[r3] : 6-T2[-r3] |
| +78 | 0x08034530 | E05, E04, 441 | E05 `02 00 DS[8]` (always); then +68(cfg18 + (DS[8]==2)); then 0x08032E1C | r3 ignored |
| +7C | 0x08032CD0 | 441 | r3=cmd. cmd[0]!=0: host report only, no ring message. cmd[0]==0: each LE word in cmd is stored big-endian: DLY+02=cmd[1..2], +04=cmd[3..4], +06=cmd[5..6], +08=cmd[7..8], +0A=cmd[9..A], +0C=cmd[F..10], +0E=cmd[B..C], +10=cmd[D..E]; then 0x08032C7C | |
| +80 | 0x08032B68 | 461 | r3=cmd. `00 00`: host report only. `00 k` with k in 1..2D: EQ row k = cmd[2..5], send single. `01`: load Hi table, send all. `02`: load Premium table, send all. Other: nothing | send all: DS[9]==1 singles, else 3 blocks |
| +84 | 0x08034380 | 430, 431 | r3=cmd. cmd[0]==0: host report only. cmd[0] in 1..4: XO row = cmd[1..4], SL row = cmd[5..6]; send 430 `cmd[0] 00 cmd[1] cmd[2] cmd[3] cmd[4]`, then 431 `cmd[0] 00 cmd[5] cmd[6]`. >4: nothing | |
| +40 | 0x080345A8 | see 8.3 | | |

Sub-routines:
- **0x08032C7C**: DS[9]==1 first zeroes DLY+0E..+11 in RAM. Then 441, len 18, data DLY.
- **0x08032E1C**: calls +7C with cmd words at cmd[1],[3],[5],[7],[9],[B],[D],[F] = 07D0, W, 07D0, 15E0, 15E0, 1900, 1900, 0000 (W: DS[8]==1: 0320; ==2: 01F4; else 0000). Resulting 441 data: `DLY[0..1] 07 D0 W(BE) 07 D0 15 E0 15 E0 00 00 19 00 19 00` (Hi-Line: last 4 bytes zero).
- **0x08032428 single EQ** (idx=row-1): 461, len 6: `row 00 EQ[4idx] EQ[4idx+1] EQ[4idx+2] EQ[4idx+3]`.
- **0x08032494 Hi-Line EQ**: single for rows 06-0A, 0D-11, 14-18, 1B-1F, 2C, 2D (22 messages).
- **0x080325A4 Premium EQ**: three 461 messages: `F0 00` + EQ[0..0x3B] (len 62); `F1 00` + EQ[0x3C..0x77] (len 62); **`F3 00`** + EQ[0x78..0xB3] + `00 00 00 00` (len 66).
- **0x0803317C crossover**: 430 `01 02 XO[2] XO[3]`; 431 `01 02 SL[1]`; 430 `02 01 XO[4] XO[5]`; 431 `02 01 SL[2]`.
- **0x08033824 common**: section 8.3 rows 1-13. Reads cfg+1C, cfg+1D. Data of rows 12, 13 use src=r2.

Host report (0x08023614, framing 7E/7D) is a serial path, not the ring (inference).

### 8.3 +40 0x080345A8 start-up, all to dst=r0

- From dispatcher: dst=0186, inst=02 (Hi) or 05 (Prem), src=DS[2] (0187 in mode 4). From NM layer: registry values.
- Listing is for mode 4 defaults: cfg+18..+1E = 0, DS[8]=0, tables as loaded by +00. Variables are noted after the lists.
- No level message follows in mode 4. Other DS[9]: nothing sent.

**Common, both types (I = inst):**

| # | FB.Inst | Fkt | OP | data |
|---|---|---|---|---|
| 1 | 22.20 | 466 | 0 | 01 00 03 00 03 01 01 |
| 2 | 22.I | 466 | 0 | 02 00 01 00 01 01 01 |
| 3 | 22.I | 466 | 0 | 03 00 01 00 01 01 01 |
| 4 | 22.I | 466 | 0 | 04 00 01 00 01 01 01 |
| 5 | 22.I | 46D | 0 | 00 00 + 20x9D (len 22) |
| 6 | 22.I | 427 | 0 | 01 00 00 20 03 E8 |
| 7 | 22.I | 202 | 0 | cfg+1C (00) |
| 8 | 22.I | 203 | 0 | cfg+1D (00) |
| 9 | 22.I | E08 | 0 | 00 00 00 50 00 1E 09 01 01 07 D0 13 88 06 01 01 |
| 10 | 23.01 | 466 | 0 | 01 00 03 00 03 01 01 |
| 11 | 23.01 | E00 | 0 | 00 00 C1 C1 C1 C1 |
| 12 | 01.02 | 401 | 0 | 01 srcH srcL 0C 01 (01 01 87 0C 01) |
| 13 | 06.(dst&FF) | 001 | 0 | 01 srcH srcL 0E 10 (06.86: 01 01 87 0E 10) |

**Hi-Line continues (41 messages, 44 telegrams), all 22.02 unless shown:**

| # | Fkt | OP | data |
|---|---|---|---|
| 14 | 430 | 0 | 01 02 00 5A |
| 15 | 431 | 0 | 01 02 04 |
| 16 | 430 | 0 | 02 01 00 70 |
| 17 | 431 | 0 | 02 01 02 |
| 18 | 441 | 0 | 00 00 07 D0 00 00 07 D0 15 E0 15 E0 00 00 00 00 00 00 |
| 19 | 461 | 0 | 06 00 FA 00 FA 14 |
| 20 | 461 | 0 | 07 00 F8 03 E8 14 |
| 21 | 461 | 0 | 08 00 FE 07 08 07 |
| 22 | 461 | 0 | 09 00 F9 18 9C 14 |
| 23 | 461 | 0 | 0A 00 FC 1F 40 07 |
| 24 | 461 | 0 | 0D 00 FA 00 FA 14 |
| 25 | 461 | 0 | 0E 00 F8 03 E8 14 |
| 26 | 461 | 0 | 0F 00 FE 07 08 07 |
| 27 | 461 | 0 | 10 00 F9 18 9C 14 |
| 28 | 461 | 0 | 11 00 FC 1F 40 07 |
| 29 | 461 | 0 | 14 00 FE 00 B4 0A |
| 30 | 461 | 0 | 15 00 F8 03 E8 0A |
| 31 | 461 | 0 | 16 00 FC 13 88 1E |
| 32 | 461 | 0 | 17 00 00 0C 4E 07 |
| 33 | 461 | 0 | 18 00 FC 0F A0 03 |
| 34 | 461 | 0 | 1B 00 FE 00 B4 0A |
| 35 | 461 | 0 | 1C 00 F8 03 E8 0A |
| 36 | 461 | 0 | 1D 00 FC 13 88 1E |
| 37 | 461 | 0 | 1E 00 00 0C 4E 07 |
| 38 | 461 | 0 | 1F 00 FC 0F A0 03 |
| 39 | 461 | 0 | 2C 00 FB 00 2D 28 |
| 40 | 461 | 0 | 2D 00 05 00 3F 28 |
| 41 | D22 on **06.86** (InstID constant) | **2** | F1 8C |

**Premium continues (32 messages, 52 telegrams), all 22.05:**

| # | Fkt | OP | data |
|---|---|---|---|
| 14 | 430 | 0 | 01 02 00 5A |
| 15 | 431 | 0 | 01 02 04 |
| 16 | 430 | 0 | 02 01 00 70 |
| 17 | 431 | 0 | 02 01 02 |
| 18 | 430 | 0 | 03 02 **7D 00** (XO[0B], XO[0A]: reversed compared with row 14) |
| 19 | 431 | 0 | 03 02 02 (SL[5]) |
| 20 | 430 | 0 | 04 01 **3B 01** (XO[0D], XO[0C]: reversed) |
| 21 | 431 | 0 | 04 01 03 (SL[6]) |
| 22 | 441 | 0 | 00 00 07 D0 00 00 07 D0 15 E0 15 E0 00 00 19 00 19 00 |
| 23 | 461 | 0 | 62 bytes: `F0 00 00 00 FA 14 FD 02 C6 28 F6 03 84 1E F8 0A F0 0A FA 1F 40 1E FA 00 FA 1E FA 03 84 1E FA 07 08 1E F9 18 9C 14 FC 1F 40 03 00 00 14 03 00 00 14 03 FA 00 FA 1E FA 03 84 1E FA 07 08 1E` |
| 24 | 461 | 0 | 62 bytes: `F1 00 F9 18 9C 14 FC 1F 40 03 00 00 14 03 00 00 14 03 FE 00 B4 0A F8 03 E8 0A FC 13 88 1E 00 03 E8 1E FC 0F A0 03 00 00 14 03 00 00 14 03 FE 00 B4 0A F8 03 E8 0A FC 13 88 1E 00 03 E8 1E` |
| 25 | 461 | 0 | 66 bytes: `F3 00 FC 0F A0 03 00 00 14 03 00 00 14 03 F7 00 7D 1E FD 01 63 1E FB 01 F4 1E F9 03 E8 0A FD 0A F0 1E F7 00 7D 1E FD 01 63 1E FB 01 F4 1E F9 03 E8 0A FD 0A F0 1E F9 00 2D 28 05 00 3F 28 00 00 00 00` |
| 26 | E05 | 0 | 00 00 X DS[8] 14 03 00 (default 00 00 06 00 14 03 00) |
| 27 | E04 | 0 | 00 00 (DS[8]==2) (cfg18+(DS[8]==2)) (default 00 00 00 00) |
| 28 | E07 | 0 | 00 00 FD FD F6 F6 |
| 29 | E05 | 0 | 02 00 DS[8] (+78) |
| 30 | E04 | 0 | 01 00 (cfg18+(DS[8]==2)) (+78 via +68) |
| 31 | 441 | 0 | 00 00 07 D0 W 07 D0 15 E0 15 E0 00 00 19 00 19 00 (+78) |
| 32 | 441 | 0 | same as 31 (second 0x08032E1C) |

- X = cfg+1B>=0 ? 6+T2[cfg+1B] : 6-T2[-cfg+1B]. W from 8.2.
- Crossover, delay and EQ rows use RAM tables, so earlier Set messages or +80/+84 change them.

Example, level path 0x08031A5C(dst,inst,src,5) with cfg zeros, DS[7]=DS[8]=0 (used by the NM layer after +40, not in mode 4): `22.I 46D 01 00 C2 9D C2 BC BC`; `22.20 E09 CA`; `22.I E08 00 00 00 50 00 1E 07 01 01 07 D0 13 88 05 01 01`.

---

## 9. Constant tables

- **T1 0x08046920:** 00 01 02 03 04 06 09 0D 13 1D.
- **T2 0x0804692A:** 00 01 02 03 04 06 08.
- **XO** Hi 0x08046890: `00 50 00 5A 00 70 00 00 00 64 00 64 00 64 00 64`. Prem 0x080468A0: `00 50 00 5A 00 70 00 00 00 64 00 7D 01 3B 00 64`.
- **SL** Hi 0x080468B0: `02 04 02 01 02 02 02 02`. Prem 0x080468B8: `02 04 02 01 02 02 03 02`.
- **.data initial:** E04T `00 00 00 01`; E05T `00 00 06 00 14 03 00`; E07T `00 00 FD FD F6 F6`; E08T `00 50 00 00 07 01 01 00 00 13 88 05 01 01`; LVL 20x`9D`; SUB `D8`; FBL_H `22 02 22 20 23 01 06 86`; FBL_P `22 05 22 20 23 01 06 86`; DLY `00 00 07 D0 00 00 07 D0 15 E0 15 E0 00 00 19 00 19 00`.

**EQ rows** (row numbers in hex). The code only copies these 4 bytes; byte 0 signed, bytes 1-2 big-endian, byte 3 unsigned is (inference) from the values.

Hi-Line 0x08046728:
```
01: 00 00 A0 07  02: 00 01 90 0A  03: 00 03 E8 14  04: 00 0F A0 07  05: 00 27 10 05
06: FA 00 FA 14  07: F8 03 E8 14  08: FE 07 08 07  09: F9 18 9C 14  0A: FC 1F 40 07
0B: FC 03 E8 14  0C: FC 03 E8 14  0D: FA 00 FA 14  0E: F8 03 E8 14  0F: FE 07 08 07
10: F9 18 9C 14  11: FC 1F 40 07  12: FC 03 E8 14  13: FC 03 E8 14  14: FE 00 B4 0A
15: F8 03 E8 0A  16: FC 13 88 1E  17: 00 0C 4E 07  18: FC 0F A0 03  19: FC 03 E8 14
1A: FC 03 E8 14  1B: FE 00 B4 0A  1C: F8 03 E8 0A  1D: FC 13 88 1E  1E: 00 0C 4E 07
1F: FC 0F A0 03  20: FC 03 E8 14  21: FC 03 E8 14  22: 02 00 20 07  23: 01 00 3F 0A
24: F9 00 3F 0A  25: 01 01 90 07  26: FC 03 E8 14  27: 02 00 20 07  28: 01 00 3F 0A
29: F9 00 A0 0A  2A: 01 01 90 07  2B: FC 03 E8 14  2C: FB 00 2D 28  2D: 05 00 3F 28
```
Premium 0x080467DC:
```
01: 00 00 FA 14  02: FD 02 C6 28  03: F6 03 84 1E  04: F8 0A F0 0A  05: FA 1F 40 1E
06: FA 00 FA 1E  07: FA 03 84 1E  08: FA 07 08 1E  09: F9 18 9C 14  0A: FC 1F 40 03
0B: 00 00 14 03  0C: 00 00 14 03  0D: FA 00 FA 1E  0E: FA 03 84 1E  0F: FA 07 08 1E
10: F9 18 9C 14  11: FC 1F 40 03  12: 00 00 14 03  13: 00 00 14 03  14: FE 00 B4 0A
15: F8 03 E8 0A  16: FC 13 88 1E  17: 00 03 E8 1E  18: FC 0F A0 03  19: 00 00 14 03
1A: 00 00 14 03  1B: FE 00 B4 0A  1C: F8 03 E8 0A  1D: FC 13 88 1E  1E: 00 03 E8 1E
1F: FC 0F A0 03  20: 00 00 14 03  21: 00 00 14 03  22: F7 00 7D 1E  23: FD 01 63 1E
24: FB 01 F4 1E  25: F9 03 E8 0A  26: FD 0A F0 1E  27: F7 00 7D 1E  28: FD 01 63 1E
29: FB 01 F4 1E  2A: F9 03 E8 0A  2B: FD 0A F0 1E  2C: F9 00 2D 28  2D: 05 00 3F 28
```

**VOL 0x08046BF7**, 36 rows x 10 bytes, index in decimal 0..35 (max 0x23).
Columns used: [1] base; [0] added for out[4]; [2] for out[6,7]; [3] for out[10]; [4]/[5] source add; [6] DS[8]==2; [7],[9] E08 bytes; [8] no reader found.
```
 0: 00 9D 00 00 02 00 00 09 0B 06
 1: FE AE FA 08 02 05 05 09 0B 06
 2: FE B5 FA 08 02 05 05 09 0B 06
 3: FE BB FA 08 02 05 05 08 09 06
 4: FE BF FA 08 02 05 05 08 09 06
 5: FE C2 FA 08 02 05 05 07 08 05
 6: FE C5 FA 08 02 05 05 07 08 05
 7: FE C8 FA 08 02 05 05 07 08 05
 8: FE CB FA 08 02 05 05 07 08 05
 9: FE CE FA 08 02 05 05 06 07 04
10: FE D1 FA 08 02 05 05 05 06 03
11: FE D4 FA 08 02 05 05 05 06 03
12: FE D7 FA 08 02 05 05 04 05 02
13: FE DA FA 08 02 05 05 04 05 02
14: FE DD FA 08 02 05 05 03 04 01
15: FE E0 FA 08 02 05 05 02 03 00
16: FE E2 FA 08 02 05 05 02 03 00
17: FE E4 FA 08 02 05 05 02 03 00
18: FE E6 FA 08 02 05 05 01 02 FF
19: FE E8 FA 08 02 05 05 01 02 FF
20: FE EA FA 08 02 05 05 00 01 FE
21: FE EC FA 08 02 05 05 00 01 FE
22: FE EE FA 08 02 05 05 00 01 FE
23: FE F0 FA 08 02 05 05 00 01 FE
24: FE F2 FA 08 02 05 05 00 01 FE
25: FE F4 FA 08 02 05 05 00 00 FD
26: FE F6 FA 08 02 05 05 00 00 FD
27: FE F8 FA 08 02 05 05 00 00 FD
28: FE F9 FA 07 02 05 05 00 00 FD
29: FE FA FA 06 02 05 05 00 00 FD
30: FE FB FA 05 02 05 04 00 00 FD
31: FE FC FA 04 02 04 03 00 00 FD
32: FE FD FA 03 02 03 02 00 00 FD
33: FE FE FA 02 02 02 01 00 00 FD
34: FE FF FA 01 01 01 00 00 00 FD
35: FE 00 FA 00 00 00 00 00 00 FD
```

---

## 10. Sent by the NM layer in mode 1 (outside the driver table)

Registry entry: dst = entry+CC, inst = entry+1, src = 0x2000D5C4+28. Found by 0x0802F8E8 (FBlock 22 with FktID 111 in its list, inference).

- Connect 0x0802F95C: 22.inst **111 OP 2**, data `sink delay mrt mrt+1 mrt+2 mrt+3` (len 6) or `sink delay mrt mrt+1` (len 4 when r3==2). Not sent if mrt or delay==40. Main source: sink 01, len 6 (0x080303F4). Second source: sink 04 for types 1,2, len 4 (0x08030AFC).
- Disconnect 0x0802F8F4: 22.inst **112 OP 2**, data `sink`. Sink 01 (0x080302BC, 0x08030A44), or 04 (0x08030B9A).
- After +40: 0x08031A5C with cfg+17 (0x08030F82): +48, +4C (with E09), +54.
- Overlay wrappers, all ending in +54 unless stated:
  - 0x08031EBC: if current index>4: `22.20 E09 C9` and +58(4); then `46D 04 00 9D 9D 9D E2 E2`.
  - 0x08031F2C and 0x08031FC4: `46D 04 00 9D 9D 9D 9D 9D` (0x08031FC4 then runs the level path instead of +54, only if index>4).
  - 0x08031F5C: `46D 04 00 E4 9D E4 9D 9D`.
  - 0x08031F90: `46D 04 00 9D 9D 9D DE DE`.
- Surround 0x08031AE8 (cmd[0]==0): DS[8] = Hi-Line: 0; Premium: cmd[1], with 3 mapped to 2. Then +78, then level path.
- Tone 0x08031B7C (cmd[0]==0): cfg+18..+1E from cmd[1..7], then +5C(cmd[5]), +60(cmd[6]), +68(cmd[1]), +74(cmd[4]); +64 skipped for types 1,2; then level path.

---

## 11. Handler and FktID list mismatches

| FktID | advertised | table entry |
|---|---|---|
| 001 | yes, all lists | Set on 22.02/05 only, no Get anywhere |
| 002 | yes, all lists | none in any table |
| 101, 102 | no | yes on 22.02/05, OP 2 |
| 111, 112 on 23.xx | yes | none |
| **113** | yes on 22.02/05 and 23.xx | **no handler anywhere, no reply** |
| 427 on 22.20 | yes | none |
| 202, 203, 427, 430, 431, 441, 466 Get | FktID advertised | none, only Set |
| E04, E05, E07 on Hi-Line | no | yes (table shared) |
| E09 on 22.02/05 | no | yes |
| D10, D11, D14, D27, D2E, D31, D34, D36.., D85, E10 on 06.xx | yes | none |
| 200, 204, any GetInterface (OP 5) | no | none |

Own start-up against own handlers:
- +40 sends the `F3` EQ block, and 3.12 rejects it with Err06.
- E08 Set with 16 bytes works (R=0, P=0, n=14).
- 430 `01 02 xx xx` lands at XO+0x10 = SL (3.9).
- 431 `01 02 v` lands at SL+4 (3.10).

---

## 12. UNKNOWN and (inference)

UNKNOWN:
- Meaning of every D19 reply byte, and of the E00/E04/E05/E07/E08, 427 and 466 fields. The code treats them as opaque.
- Actual telegram gaps on the ring: there are no delays in this layer.
- Saved configuration in the device flash, which can replace the defaults in 1.3.
- What a real amp answers. Replies from it fall into "cant process" paths or T gating; this firmware has no handler for them.

(inference):
- Source address on the wire comes from bNAH/bNAL, not msg+2 (1.1).
- DS[0x0A] stays 0: it is in .bss and no store was found by literal and base-register scans (1.3). All mode 4 conclusions depend on this: FBlock 22 dropped, deferred Allocate path dead (3.3).
- OS8104 restart at 0x08024C5E rewrites bNAH/bNAL with the new node (state machine write at 0x080241D0, restart at 0x080239EC).
- Chip group is 0x0350, per data sheet "03h is the upper address byte" with bGA=50 (section 6), so messages to 0x0322 would not be received.
- Trailing `000` entries in the Hi-Line list are padding (5).
- Range D36..D3E in the Hi-Line diag list, read literally from a non-monotonic list (5).
- 0x08023614 is the host serial path, not the ring (8.2).
- EQ row field shape (9).
- Mode roles in 1.4 are read from gating and routing.
- 0x0802F8E8 selects the amp by FktID 111 in the registry (10). "Main source", "second source" and "overlay" in section 10 are my wording for call sites.

**Files:** my temporary disassembly dumps in the scratchpad were deleted. Nothing else was written.
