# usb2most main-v24.bin: AUD3 amplifier decode (from the disassembly only)

Marks: `(label)` = meaning taken from a log string; `(inference)` = my deduction, not a direct read of the code; `(spec)` = name taken from the MOST or OS8104 documents; `UNKNOWN` = not determinable from the code. All other statements are direct reads of the code.

Notation: `S` = src of the request (msg+2). `N` = DS+2 (own node address). `T` = amp type returned by 0x0803709C. Bytes in hex. OP codes: 0 Set/Start, 1 Get/Abort, 2 SetGet/StartResult, 0xC Status/Result, 0xF Error (spec). "Reply" means a message through 0x08031504. Unless stated, a reply copies FBlock, InstID and FktID from the request, with dst=S, src=N.

---

## 0. Common machinery

### 0.1 Receive path
- Parser 0x08031380 (frame = OS8104 receive buffer): frame[2..3] BE to msg+2; frame[4] to +4; frame[5] to +5; frame[6..7]: upper 12 bits to +6, lower 4 to +8; frame[8]: high nibble to +0xC (TelID), low nibble to +0xD; data at frame[9] (TelID 0). TelID 1/2/3 are assembled in a slot (10 slots of 0x118 bytes, data at slot+0x18); segment data starts at frame[10], length = low nibble - 1. msg+0 (dst) is not filled by the parser.
- Router 0x080249E0, by `mode` = RAM 0x20003F50 (same value as cfg+0xB): 0 -> 0x0803CCC4 (role UNKNOWN); 1 -> master 0x08030E10; 2 and 4 -> amp 0x0803200C; 3 -> tuner 0x08038E84; >4 -> raw frame to host (0x08023614).
- 0x0803200C: FBlock 01 -> NetBlock table 0x080458C4, any InstID. Any other FBlock -> driver method +38 (AUD3: dispatcher 0x08038620).
- Lookup 0x0802E6FC: key = FktID<<4 | (OPType & 0xF), sorted table, returns 0 when found, 3 when not. Not found = log only. **No handler table path ever sends an Error telegram for an unknown function or OPType.**

### 0.2 Transmit path, 0x08031504
- len <= 12: frame `01 00 dstH dstL FBlock InstID (Fkt<<4|OP)H (Fkt<<4|OP)L len data...`.
- len > 12: segments of 11 data bytes; byte 8 = (TelID<<4)|(n+1) with TelID 1 first, 2 middle, 3 last; byte 9 = segment counter from 0; data from byte 10.
- Frame byte 0 = priority, byte 1 = message type (OS8104 mXCMB C0h, C1h) (spec).
- msg+2 (src) is passed as second argument to the TX hook, but the hook 0x08024862 -> 0x08023A4C copies only the 21-byte frame. So the firmware does not put src on the wire; the chip's node address does (inference).

### 0.3 Configuration and state
- cfg = RAM 0x20005700; default image at flash 0x0804E8C8. Used bytes (default): +0xB mode (01), +0xC amp type (01), +0xD (04), +0x12 surround (00), +0x13 source (00), +0x17 volume index (05), +0x18..+0x1E tone bytes (00). A stored configuration may replace the defaults at boot (inference).
- Driver choice at 0x08031CB0 by cfg+0xC: 1,2 -> other driver; **3,4,5 -> AUD3 table (RAM 0x200059DC, flash 0x0804EBA4)**; 6,7 -> other; else stub.
- Init calls: +00(cfg+0xC); +10(0x186); mode 4 only: +10(0x187); +08(1); +18(0x322); +28(0,0,0,0) (no message); +30(cfg+0x12); +20(0x40); DS+0x1A=0; obj(0x200183BC)+0xB=0.
- `T` = 0x0803709C: mode 4 -> DS+0xA, otherwise DS+9.
- DS = 0x20018560 (bss, zero at boot; reset code 0x0802C6B4):
  - +0 u8: node position (+08; set from the OS8104 npr callback 0x08024978 in modes 2,4,7).
  - +2 u16: node address N (+0C/+10). 0x0186, or 0x0187 in mode 4. Programmed into OS8104 0x8A/0x8B at 0x080241CE..0x080241E0.
  - +4 u16: group address 0x0322 (+14/+18). Only reported by 01.xx 004 Get. The chip's bGA 0x89 is written 0x50 at 0x080241E8 (so hardware group 0x0350 by the datasheet formula) (spec).
  - +6 u8: allocated first channel; 0x40 = none (+1C/+20).
  - +7 u8: last value given to +28 (source code).
  - +8 u8: surround (+2C/+30).
  - +9 u8: cfg+0xC.
  - +0xA u8: read as T in mode 4. **No writer exists: literal scan of 0x20018560 and computed-address scan of every store found none** (inference that it stays 0).
  - +0xB..+0x15: 11-byte volume snapshot written by +4C; +0xC also written by 46D Set. No reader found.
  - +0x18..+0x2F: saved request (24 bytes); +0x1A saved src, 0 = nothing pending (+34 returns DS+0x18).
- Statics: 0x20018544 Balance, 0x20018545 Bass, 0x20018546 Fader, 0x2001855F Treble; 0x20018547..0x2001855E MuteParameters rows (labels). No reader except the logging in their own handlers.
- .data tables (RAM, initial bytes from flash):
  - 0x200059A8 E05 table, 7 bytes: `00 00 06 00 14 03 00`.
  - 0x200059AF E08 table, 14 bytes: `00 50 00 00 07 01 01 00 00 13 88 05 01 01`.
  - 0x200059BD..BE: `00 00` (gap).
  - 0x200059BF SpeakerLevel rows 1..4, 4 x 7 = 28 bytes, all `9D`. Row address = 0x200059B8 + 7*row. (Row "0" at 0x200059B8 overlaps the last 5 bytes of the E08 table and the gap.)
  - 0x20005A64 FBlock list: `22 0A 06 86 23 01`.

### 0.4 Reachability that changes how to read the rest
- **Proxy branches (mode 4 and DS+0xA in {4,5}) are unreachable in this binary** because DS+0xA has no writer (inference). In mode 4 T=0, so the dispatcher drops every 22.0A message. The proxy bytes below are still what the code would send.
- Normal = mode 2 with cfg+0xC 4 or 5: T=DS+9. This is the only mode in which 22.0A handlers run (inference from the two facts above and the router).
- The allocation callback 0x08024918 forwards to the amp layer only in **mode 4** (compare at 0x08024960). So in mode 2 no asynchronous Allocate result is ever produced and DS+6 stays 0x40.

---

## 1. FBlock 22.0A, table 0x080486D4 (20 handlers)
Runs only if InstID == 0x0A and T in {4,5}.

**000 Get, 0x08037410 FktIDs.** No length check, request bytes ignored. T 3,4: reply OP C, len 0x1A, list from 0x0804790F. T 5: reply OP C, len 0x1D, list from 0x08047929. Decode in section 5. Nothing stored.

**001 Set, 0x080388F4 Notification.**
- len must be > 4 else log, no reply. data[0] must be 1 else log, no reply (so SetAll, ClearAll, ClearFunction are ignored).
- data[1..2] BE = DeviceID D. From data[3], (len-3)/2 FktIDs, 16-bit BE.
- No notification matrix: nothing is stored. Each FktID is logged. If FktID == 0xD00 and T in 3..5: call 0x08038878(D, N, 1), one D00 Status to **D** (the parameter, not S). Other FktIDs: no reply.

**D00 report, 0x08038878(dst, src, mode)** (no table entry). Hard-coded header: dst, 22.0A, 0xD00, OP C, len 7, data `mode 00 02 00 06 00 01`. Meaning of the six fixed bytes UNKNOWN. Called only from 001 Set (mode 1) and E05 Set.

**101 StartResult, 0x080385A0 Allocate.**
- len must be 1 else log, no reply.
- DS+6 != 0x40: immediate reply 0x101 OP C, len 4: `data[0], DS+0, DS+6, DS+6 + 1`.
- DS+6 == 0x40: save request to DS+0x18; 0x08024C56(2) -> 0x08023D90 queues OS8104 system frame `01 03 04 00 00 02 00...` (type 03 Resource Allocate to 0x0400, 2 channels) (spec). No reply now.
- Later, OS8104 answer at 0x08024680: Answer1==1 and count 2 or 4 -> callback(P0, count); else callback(0x40, 0). The callback reaches 0x08031E28 only in mode 4: DS+6 = first arg; if DS+0x1A != 0, reply to saved src with saved FBlock.InstID: granted: 0x101 OP C, len 4 `01, DS+0, P0, P0+1`; not granted: **0x102** OP C, len 1 `01`. Then DS+0x1A=0.
- Mode 2: callback dropped, no result ever (see 0.4).
- DS+0 in the reply: node position (label `npr`).

**102 StartResult, 0x08038500 DeAllocate.**
- len must be 1 else log, no reply.
- DS+6 == 0x40: immediate reply 0x102 OP C, len 1 `data[0]`.
- Else: save request; 0x08024C5A(DS+6) -> 0x08023D30 queues `01 04 04 00 00 <mrt> 00...` (type 04 Resource De-Allocate) (spec) and calls the callback with (0x40, mrt); DS+6=0x40. In mode 4 the callback replies 0x102 OP C, len 1 `01` to the saved src.

**111 StartResult, 0x0803785C Connect.**
- len must be 4, 6 or 8 else log, no reply.
- data[0]=SinkNr, data[1] used only in logs and proxy forward (label delay), data[2]=first channel c. Other bytes ignored. No sink state stored.
- **Reply first, for every SinkNr and every accepted length:** 0x111 OP C, len 1, `data[0]`.
- Then the action (only when the length matches the row):

| SinkNr | label | required len | Normal: 0x08024E98(c, n) | Proxy forward to dst 0x0186, FBlock 22, InstID P, 0x111, OP 2 (P = 2 if DS+9==1 else 5) |
|---|---|---|---|---|
| 1 | main | 6 | c, 4 | len 6: `01 d c c+1 c+2 c+3` |
| 2 | video | 8 | c, 4 | len 6: `01 d c+1 c c+5 c+4` |
| 4 | call | 4 | c, 2 | len 4: `02 d c c+1` |
| 6 | radar | 4 | c, 2 | len 4: `04 d c c+1` |
| 7 | navi | 4 | c, 2 | len 4: `03 d c c+1` |
| others | | | nothing | nothing |

- 0x08024E98 -> 0x08023F88 writes the OS8104 routing table (MRT addresses, upper half = source port) (spec):
  - n=4: 46=c, 56=c+1, 66=c+2, 76=c+3, 47=F8, 4F=c+1, 57=c, 5F=F8, 67=F8, 6F=c+3, 77=c+2, 7F=F8.
  - n=2: 46=c, 56=c+1, 47=F8, 4F=c+1, 57=c, 5F=F8.
  - 0xF8 = zero output (spec). The last Connect wins; there is no sink arbitration.

**112 StartResult, 0x080383E4 DisConnect.**
- len must be 1 else log, no reply.
- Reply 0x112 OP C, len 1, `data[0]`, for any value.
- Normal: **nothing else: routing is not cleared.**
- Proxy: sinks 1,2,6,7 only: dst 0x0186, 22.P, 0x112, OP 2, len 1 with mapped sink 1->1, 2->1, 6->4, 7->3. Sink 4 is not forwarded.

**200 Set 0x08037064 Balance (label); 202 Set 0x0803702C Bass (label); 203 Set 0x08036FF4 Treble (label); 204 Set 0x08036FBC Fader (label).** If len==1, data[0] is stored to its static (0.3). Other lengths: not stored. Value logged. No reply ever. No Get handlers exist. The stored value has no reader.

**466 Set, 0x080376FC MuteParameters (label).** data[0] must be 1..4 and len==7, else silently ignored. Stores data[1..6] at 0x20018547 + 6*(data[0]-1); shape 4 rows x 6 bytes. Logged as BE16, BE16, byte, byte. No reply. Field meanings UNKNOWN.

**46D Set, 0x08037D74 SpeakerLevel (label).**
- len <= 1: return, no log.
- data[0] = row selector, data[1] must be 0 for anything to be stored. The log string calls data[0] "param" and data[1] "sink"; that does not match what the code does with them.
- data[1]==0:
  - data[0]==0, len-2 <= 30: memcpy(0x200059BF, data+2, len-2) (all rows; len-2==30 also overwrites the low byte of the method pointer at 0x200059DC).
  - data[0]==0, len-2 > 30: memcpy(0x200059B8, data+2, len-2).
  - data[0]==1..4: memcpy(0x200059B8 + 7*data[0], data+2, len-2). **No length cap.**
  - data[0]>4: no store.
- data[1]!=0: no store, no computation.
- Computation when data[1]==0, with no length check: level = signed data[2]; index = 0x08037D38(level) stored to DS+0xC. The rest is only logged: data[3]-data[2] (label dc), data[4] (label fr), data[5]-data[2] (label drl), data[6]-data[2] (label drr), data[7]-data[2] (label dsl), data[8]-data[2] (label dsr), data[2] (label fl).
- 0x08037D38 (level to index): level < -98 -> 0; level >= 0 -> 60; else highest i with table[i][0] <= level (table in section 7).
- No reply ever. No audio effect in this handler in normal mode.
- Proxy, for any len>=2: vol = 0x08037D38(signed byte at 0x200059BF) (row 1 byte 0); 0x08031A5C(0x186, P, N, vol). Row 4 bytes 0 and 3 (label radar fvol, rvol) and row 2 bytes 0 and 3 (label call fvol, rvol) are only logged.

**46D Get, 0x08037768.**
- len != 2: Error reply OP F, len 1, `06`.
- data[0]==0 and data[1]==0: OP C, len 0x1E: `00 00` + 28 bytes of rows 1..4.
- data[1]==0 and data[0]==1..4: OP C, len 9: `00 00` + 7 bytes of that row. **The row number is not echoed.**
- Other values: OP C, len 0.

**D01 Set, 0x08037D14.** Logged and dumped. Nothing stored. No reply.

**E05 Set, 0x08038994.**
- T==5 and len==3: data[0], data[1] ignored. m = data[2]; 5 or 7 -> 4; 0x0D -> 2; others unchanged. Call 0x08038878(S, N, m): D00 Status `m 00 02 00 06 00 01`. No E05 reply. Proxy (DS+0xA==5): 0x08031AE8(0x186, P, N, {00, min(m,2)}).
- Otherwise, len==7: stores data[0..6] to the E05 table, no reply.
- T==4 and len==3, or any other length: nothing.

**E05 Get, 0x08037FE0.** len != 2: Error OP F, len 1, `06`. data[0]==0 and data[1]==0: table[0] and table[1] are **set to 00 in RAM**, then OP C, len 7, table (from reset: `00 00 06 00 14 03 00`). Other values: OP C, len 0.

**E08 Set, 0x08037C3C.**
- data[0]==0, data[1]==0, len==16: stores data[2..15] to the E08 table.
- data[0]==1 or 2, data[1]==0, len==9: stores data[2..8] at table + 7*(data[0]-1).
- Success: log only, no reply.
- Anything else: Error OP F, len 1, `06`. (len <= 1 logs an error and then falls through to this.)
- Bug path: data[0]==0, data[1]==0, len==9 computes offset 0x7FFFFFF9 and writes 7 bytes at 0xA00059A8.

**E08 Get, 0x08037F2C.** len != 2: Error OP F, len 1, `06`. data[0]==0 and data[1]==0: table[0..1] **set to 00 in RAM**, then OP C, len 0x0E, table (first Get from reset: `00 00 00 00 07 01 01 00 00 13 88 05 01 01`; the initial `50` is lost). Other values: OP C, len 0.

**E09 Set, 0x080383A4 Subwoofer (label).** len must be 1 else log. Value logged. Nothing stored. No reply.

**E53 Set, 0x08036DBC.** Log only. No reply. Not advertised in FktIDs.

---

## 2. FBlock 23, table 0x08048950 (3 handlers). InstID not checked, T not checked.

- **000 Get, 0x08036D4C.** OP C, len 0x0E, list at 0x080477F4 (section 5).
- **466 Set, 0x08036D40.** Log only. Nothing stored. No reply.
- **E00 Set, 0x08037C20.** Log and dump. No reply.

## 3. FBlock 06, table 0x08048928 (4 handlers). InstID not checked, T not checked.

- **000 Get, 0x08037304.** OP C. T==5: 0x080478F5, len 0x1A. T 3,4: 0x080478D8, len 0x1D. T==2: 0x080478B2, len 0x26. Others (T=0 in mode 4): 0x0804788C, len 0x26. Decode in section 5.
- **001 Set, 0x08037224 Notification.** len > 4 and data[0]==1, else log, no reply. Nothing stored. For each 16-bit BE FktID from data[3]: if 0xE10: reply to **S** (DeviceID param only logged), FBlock forced 06, InstID from request, 0xE10, OP C. T != 5: len 2 `01 01`. T == 5: len 0. Others: no reply.
- **D19 StartResult, 0x08036DC8.** len must be 2 or 5 else log, no reply. Unmatched request: log, no reply. Replies OP C with constant data (meaning UNKNOWN):
  - `02 AF` -> len 0x26 (0x08047802): `02 7F 9D 84 13 69 F0 00 87 28 9A 03 13 28 9A 04 13 28 9A 05 13 2D 9A 06 13 2D 9A 10 13 2D 9A 11 13 2D 9A 03 11 68`
  - `04 9D 84 13 xx` -> len 0x1C (0x08047828): `04 9D 84 13 69 10 05 DD 00 5E 3C 4F DD DD 01 00 02 7A BD DD 02 38 DD 04 3C DD 06 C7`
  - `06 9D 84 13 xx` -> len 0x15 (0x08047844): `06 9D 84 13 2C 01 DD 02 81 03 FF 04 8E 05 00 10 80 11 7F 12 00`
  - `04 F0 00 87 xx` -> len 0x1C (0x08047859): `04 F0 00 87 28 10 05 DD 00 FF FF FF FF DD 01 FF FF FF FF DD 02 FF DD 04 FF DD 06 FF`
  - `06 F0 00 87 xx` -> len 0x15 (0x08047875): `06 F0 00 87 28 01 FE 02 FD 03 FF 04 80 05 00 10 80 11 7F 12 00`
  - data[4] is not checked.
- **D22 StartResult, 0x080381D0.** len must be 2 else log, no reply. data[0] must be F1 else log, no reply. Replies OP C (first two bytes echo the request, ASCII padded with 00):
  - `F1 8C` -> len 0x12 (0x08047962): `F1 8C` + "699129917932" + `00 00 00 00` (label ecu_sn)
  - `F1 12` -> len 0x1A (0x08047974): `F1 12` + "6H52-18C808-CD" + 10 x `00` (label ecu_assembly_pn)
  - `F1 91` -> len 0x1A (0x0804798E): `F1 91` + "6H52-14C524-CB" + 10 x `00` (label vm_ecu_hw_pn)
  - `F1 88` -> len 0x1A (0x080479A8): `F1 88` + "6H52-14C526-CD" + 10 x `00` (label vm_ecu_sw_pn)
  - `F1 24` -> len 0x1A (0x080479C2): `F1 24` + "6H52-14C527-CB" + 10 x `00` (label ecu_cal_01_pn)
  - `F1 other`: Error OP F, len 4: `06 01 F1 data[1]`.

---

## 4. Dispatcher 0x08038620
T is computed first (no side effects). Then by msg+4:
- **22:** InstID != 0A: silent return. T not in {4,5}: silent return. Else lookup 0x080486D4; not found: log, no reply.
- **23:** lookup 0x08048950, any InstID; not found: log, no reply.
- **06:** lookup 0x08048928, any InstID; not found: log, no reply.
- **02** (any InstID) with **OP==C**:
  - FktID != A00: log, no reply.
  - FktID == A00, len==0: return. data[0]: 0: log. 2: log. 3: log and dump. Other values: nothing. Nothing else happens on these (no channel release, no state reset).
  - data[0]==1 (label Ok), in this order:
    1. If **DS+9 == 5** (cfg+0xC==5, mode not checked). dst = sender of the A00 message for all three:
       - M1: F5.01, 0x305, OP 1 (Get), len 3: `26 28 24`.
       - M2: F5.01, 0x001, OP 0 (Set), len 5: `01 NH NL 02 00` (Notification SetFunction for 0x200 (spec)).
       - M3: F5.01, 0x001, OP 0 (Set), len 5: `01 NH NL 03 03` (for 0x303).
    2. If **mode 4**: start-up sequence (+40, section 7) with dst 0x0186, InstID 2 if DS+9==1 else 5, src N.
    - Types 3,4: no F5 messages at all.
- **02 with OP != C, F5, any other FBlock:** log "amp cant process" and dump the data. No reply. **Answers from FBlock F5 (Status or Error on 0x305, 0x200, 0x303) are only logged and dumped; nothing is stored or acted on.** Meaning of ids 26, 28, 24 UNKNOWN.
- FBlock 01 never arrives here (0x0803200C takes it).

---

## 5. FktID lists
Encoding per MOST Spec 2.3.9: 12-bit positions where availability flips, initial state 1 (spec). The final `0` nibble is stuffing. Availability below is computed by flipping in list order.

**22.0A, T 3,4 (of these only 4 is dispatched), len 0x1A, 0x0804790F**
`00 31 01 10 31 11 11 32 00 20 12 02 20 54 66 46 74 6D 46 ED 00 D0 2E 08 E0 A0`
Flips: 003 101 103 111 113 200 201 202 205 466 467 46D 46E D00 D02 E08 E0A.
Available: 000-002, 101-102, 111-112, 200, 202-204, 466, 46D, D00-D01, E08-E09.

**22.0A, T 5, len 0x1D, 0x08047929**
`00 31 01 10 31 11 11 32 00 20 12 02 20 54 66 46 74 6D 46 ED 00 D0 2E 05 E0 6E 08 E0 A0`
Flips: same with E05 E06 added before E08.
Available: same list plus E05.

T comes from cfg+0xC via DS+9: **cfg+0xC==4 gets the list without E05; cfg+0xC==5 gets the list with E05.**

**23.xx, len 0x0E, 0x080477F4**
`00 31 11 11 34 66 46 7E 00 E0 1E 1C E1 F0`
Flips: 003 111 113 466 467 E00 E01 E1C E1F.
Available: 000-002, 111-112, 466, E00, E1C-E1E.

**06.xx, T==5, len 0x1A, 0x080478F5**
`00 3D 10 D1 2D 22 D2 3D 31 D3 2D 3E D3 FD 2E D2 FD 14 D1 5D 19 D1 AD 85 D8 60`
Flips: 003 D10 D12 D22 D23 D31 D32 D3E D3F D2E D2F D14 D15 D19 D1A D85 D86. **Not monotonic.**
Available: 000-002, D10-D11, D22, D31, D3E, D2E, D14, D19, D85.

**06.xx, T 3,4, len 0x1D, 0x080478D8**
`00 10 02 00 3D 10 D1 2D 22 D2 3D 31 D3 2D 3E D3 FD 2E D2 FD 14 D1 5D 19 D1 AD 85 D8 60`
Flips: 001 002 003 then as above. **001 is off.**
Available: 000, 002, D10-D11, D22, D31, D3E, D2E, D14, D19, D85.

**06.xx, T==2, len 0x26, 0x080478B2** (not reachable: AUD3 types are 3..5 and DS+0xA has no writer (inference))
`00 3D 10 D1 2D 14 D1 5D 19 D1 AD 22 D2 3D 27 D2 8D 2E D2 FD 31 D3 2D 34 D3 5D 36 D3 8D 3E D3 FD 85 D8 6E 10 E1 10`
Available: 000-002, D10-D11, D14, D19, D22, D27, D2E, D31, D34, D36-D37, D3E, D85, E10.

**06.xx, other T (T=0 in mode 4), len 0x26, 0x0804788C**
`00 3D 10 D1 2D 14 D1 5D 19 D1 AD 22 D2 3D 27 D2 8D 2E D2 FD 31 D3 2D 34 D3 5D 36 D3 5D 36 D3 FD 85 D8 6E 10 E1 10`
Flips contain `D34 D35 D36 D35 D36 D3F` (repeated pair, not monotonic). Otherwise as the T==2 list.

Handler versus list mismatches: E53 has a handler but is not listed. 002, D00 (Get), E1C-E1E, 111-112 on FBlock 23, and all 06 D1x..D85 except D19 and D22 are listed with no handler. E10 is answered in 06 Notification but is not in the lists used by AUD3 types.

---

## 6. FBlock list, InstIDs, NetBlock (table 0x080458C4, 8 handlers, any InstID)
Table RAM 0x20005A64 (flash 0x0804EC2C): pairs FBlock, InstID: `22 0A | 06 86 | 23 01`.
- **Read:** only by +3C (returns pointer, size 6) for 01.xx 000 Get.
- **Written:** by +10: table[3] = low byte of node address (so **Diagnosis InstID = low byte of N**: 0x86 at 0x186; 0x87 in mode 4, set at init 0x08031CF0). By +24 from 000 SetGet: FBlock 06: table[3]=new; FBlock 23: table[5]=new; FBlock 22 or other: no change.
- The dispatcher never reads this table: it compares 22's InstID with the literal 0A and does not check 06 or 23 at all. D00 hard-codes 22.0A.
- Node address is set only at init (0x186, then 0x187 in mode 4).

NetBlock replies are OP C, FBlock 01, InstID from the request (the firmware's own master uses that InstID as the device index, 0x0802FE70), dst=S.
- **000 Get, 0x08031840:** len 6: `22 0A 06 <NL> 23 01` (current table).
- **000 SetGet, 0x0803191C:** len must be 3 else log, no reply. data[0]=FBlock, data[1]=old InstID (logged only), data[2]=new InstID: +24, then same reply as 000 Get.
- **001 Get, 0x080318B4** (label DeviceInfo): len 3: `FF FF 00`.
- **002 Get, 0x080317E0:** len 2: `40 <DS+0>`.
- **003 Get, 0x0803177C:** len 2: `NH NL`.
- **004 Get, 0x08031718:** len 2: `03 22`.
- **006 Start, 0x080316A8** (label ShutDown): len != 1: nothing. data[0]==2: log, call 0x0802405C (label goto zero-power), no reply. Other: reply len 1 `data[0]`.
- **401 Start, 0x08031630** (label Notification): len > 4, data[0]==1 else log; each FktID logged. Nothing stored. No reply.

---

## 7. Driver methods, flash 0x0804EBA4 (RAM 0x200059DC). Args: r0=dst, r1=InstID I, r2=src, r3=argument

Sending methods all use FBlock 22 and OP 0 (Set), except message 2 of +40.

- **+28, 0x08036CDC, D01.** DS+7=r3. r3==0: nothing sent. r3 1..8: value = table 0x08048676 `08 08 08 0C 01 04 04 07`[r3-1]. r3>8: value 01. Sends 22.I D01 Set, len 2: `01 value`.
- **+40, 0x080374B8, start-up sequence** (r3 unused). sH,sL = src. Order:
  1. 22.I 001 Set, len 5: `01 sH sL 0C 01`
  2. **06.I** 001 Set, len 5: `01 sH sL 0E 10`
  3. 22.I 001 Set, len 5: `01 sH sL 0D 00`
  4. 22.I 001 Set, len 5: `01 sH sL 03 53`
  5. **22.20** 466 Set, len 7: `01 00 03 00 03 01 01` (0x08047946; InstID literal 0x20)
  6. 22.I 466 Set, len 7: `02 00 01 00 01 01 01` (0x0804794D)
  7. 22.I 466 Set, len 7: `03 00 01 00 01 01 01` (0x08047954)
  8. 22.I 466 Set, len 7: `04 00 01 00 01 01 01` (0x0804795B)
  9. 22.I 46D Set, len 9: +48(index 0), then +4C. With DS+7=0, DS+8=0: `01 00 9D 9D 9D 9D 9D 9D 9D`.
  10. 22.I 202 Set, len 1: cfg+0x1C (label Bass)
  11. 22.I 203 Set, len 1: cfg+0x1D (label Treble)
  12. 22.I 204 Set, len 1: cfg+0x1A (label Fader)
  13. 22.I 200 Set, len 1: cfg+0x19 (label Balance)
  14. 22.I E05 Set, len 3: `02 00 DS+8`
  15. 22.I E09 Set, len 1: cfg+0x1E (label Subwoofer)
  Meaning of 0xC01 and 0x353 UNKNOWN. Called from master 0x08030F68 (dst=amp device, I=amp's InstID from its FBlock list, src=0x161) and from proxy paths.
- **+48, 0x080370B0(r0=out, r1=index). Sends nothing.** idx=min(index,60); row=table[idx]; L=signed row[0]. DS+7==4: L+=row[2]. DS+7 in 1..3: L+=row[1]. out[0]=00, out[1]=idx, out[2]=3C, out[3..10]=L. T==4: out[4]=9D. DS+8==0: out[4]=9D. DS+8==2: L2=L+row[3]; out[3..7]=L2, out[8]=out[9]=9D, out[10]=L2. Returns idx.
- **+4C, 0x08036C48(r3=buf).** Copies buf[0..10] to DS+0xB. Sends 22.I 46D Set, len 9: `01 00 buf[3] X buf[5] buf[6] buf[7] buf[8] buf[9]`, X = 9D if DS+8==0 else buf[4]. buf[10] is not sent.
- **+50, 0x08036BDE(r3=sink, stack0=a, stack1=b).** 22.I 46D Set, len 9: `sink 00 a 9D a b b 9D 9D`. Called through 0x080319EC, which forces sink=4 for AUD3 (its r3=fvol and stack=rvol become a,b).
- **+58, 0x08037150(r3=index).** Runs +48, then 22.I 46D Set, **len 0x1E**: `00 00 b3 9D b5 b6 b7 b8 b9` + 21 x `9D`.
- **+5C 0x08036BA0:** 22.I 202 Set, len 1: r3.
- **+60 0x08036B62:** 22.I 203 Set, len 1: r3.
- **+64 0x08036B24:** 22.I E09 Set, len 1: r3.
- **+6C 0x08036AE6:** 22.I 204 Set, len 1: r3.
- **+70 0x08036AA8:** 22.I 200 Set, len 1: r3.
- **+78 0x08036A58:** 22.I E05 Set, len 3: `02 00 DS+8`.
- **+7C 0x08038094(r3=ptr). No MOST message.** ptr[0]==0: nothing. Else log and host frame via 0x08023614: `13 22` + zeros.
- **+80 0x08038170(r3=ptr). No MOST message.** ptr[0]!=0: nothing. ptr[1]!=0: log. Both 0: log and host frame `08 1F 00 00 00 00 00 00`.
- **+84 0x08038100(r3=ptr). No MOST message.** ptr[0]!=0: log only. ptr[0]==0: log and host frame `09 23 01 00 00 00 00 00 00`.
- Empty: +54, +68, +74. +30 0x08037F0C: DS+8 from r0: 1 -> unchanged; 4 -> 1; 3 -> 5; others = r0. +44 = 0x08037D38. +00: DS+9=r0, DS+6=0x40, DS+0x1A=0.

### Table 0x0804877C, 61 rows of 7 bytes (index, hex row, row[0] signed)
Readers: +48 (columns 0,1,2,3) and 0x08037D38 (column 0). Columns 4,5,6 have no reader (inference from the only two literal references).
```
 0 9D 02 00 00 09 0B 06  -99    21 D9 02 05 05 00 01 FE  -39    42 EE 02 03 02 00 00 FD  -18
 1 A5 02 05 05 09 0B 06  -91    22 DA 02 05 05 00 01 FE  -38    43 EF 02 02 01 00 00 FD  -17
 2 AE 02 05 05 09 0B 06  -82    23 DB 02 05 05 00 01 FE  -37    44 F0 01 01 00 00 00 FD  -16
 3 B4 02 05 05 08 09 06  -76    24 DC 02 05 05 00 01 FE  -36    45 F1 00 00 00 00 00 FD  -15
 4 B9 02 05 05 08 09 06  -71    25 DD 02 05 05 00 00 FD  -35    46 F2 02 04 03 00 00 FD  -14
 5 BD 02 05 05 07 08 05  -67    26 DE 02 05 05 00 00 FD  -34    47 F3 02 03 02 00 00 FD  -13
 6 C0 02 05 05 07 08 05  -64    27 DF 02 05 05 00 00 FD  -33    48 F4 02 02 01 00 00 FD  -12
 7 C3 02 05 05 07 08 05  -61    28 E0 02 05 05 00 00 FD  -32    49 F5 01 01 00 00 00 FD  -11
 8 C6 02 05 05 07 08 05  -58    29 E1 02 05 05 00 00 FD  -31    50 F6 00 00 00 00 00 FD  -10
 9 C8 02 05 05 06 07 04  -56    30 E2 02 05 04 00 00 FD  -30    51 F7 02 04 03 00 00 FD   -9
10 CA 02 05 05 05 06 03  -54    31 E3 02 04 03 00 00 FD  -29    52 F8 02 03 02 00 00 FD   -8
11 CC 02 05 05 05 06 03  -52    32 E4 02 03 02 00 00 FD  -28    53 F9 02 02 01 00 00 FD   -7
12 CE 02 05 05 04 05 02  -50    33 E5 02 02 01 00 00 FD  -27    54 FA 01 01 00 00 00 FD   -6
13 D0 02 05 05 04 05 02  -48    34 E6 01 01 00 00 00 FD  -26    55 FB 00 00 00 00 00 FD   -5
14 D1 02 05 05 03 04 01  -47    35 E7 00 00 00 00 00 FD  -25    56 FC 02 04 03 00 00 FD   -4
15 D2 02 05 05 02 03 00  -46    36 E8 02 04 03 00 00 FD  -24    57 FD 02 03 02 00 00 FD   -3
16 D4 02 05 05 02 03 00  -44    37 E9 02 03 02 00 00 FD  -23    58 FE 02 02 01 00 00 FD   -2
17 D5 02 05 05 02 03 00  -43    38 EA 02 02 01 00 00 FD  -22    59 FF 01 01 00 00 00 FD   -1
18 D6 02 05 05 01 02 FF  -42    39 EB 01 01 00 00 00 FD  -21    60 00 00 00 00 00 00 FD    0
19 D7 02 05 05 01 02 FF  -41    40 EC 00 00 00 00 00 FD  -20
20 D8 02 05 05 00 01 FE  -40    41 ED 02 04 03 00 00 FD  -19
```

### Amp-layer wrappers that select these methods for AUD3
- 0x08031A5C(dst, I, src, idx): +48 then 0x08031A14: +4C, +54 (empty), host frame `0D 1B` + 11 bytes. This is the volume message `01 00 ...`.
- 0x08031AE8(dst, I, src, ptr), used for surround: ptr[0]==0: type 1,3,4 -> value 0; type 2 -> ptr[1] (3 becomes 2); others including 5 -> ptr[1]. +30(value), cfg+0x12=value, +78, 0x08031A5C with current index. Then host frame `04 25 x DS+8`.
- 0x08031B7C(dst, I, src, ptr), used for tone: ptr[0]==0: stores cfg+0x18..+0x1E, then +5C(ptr[5]), +60(ptr[6]), +6C(ptr[3]), +70(ptr[2]), +64(ptr[7]), 0x08031A5C with current index. Then host report.
- 46D-row sequences via 0x080319EC (triggered from host commands; purpose UNKNOWN; AUD3 threshold index 8):
  - 0x08031EBC: if current index > 8: +58(index 8). Then `04 00 9D 9D 9D E2 E2 9D 9D`.
  - 0x08031F2C: `04 00 9D 9D 9D 9D 9D 9D 9D`.
  - 0x08031F5C: `04 00 E4 9D E4 9D 9D 9D 9D`.
  - 0x08031F90: `04 00 9D 9D 9D DE DE 9D 9D`.
  - 0x08031FC4(vol): `04 00 9D 9D 9D 9D 9D 9D 9D`; if current index > 8: 0x08031A5C(vol).

---

## 8. Master side (mode 1): the firmware is network master, node 0x0161 (0x080307C6), facing a real amplifier
Mode 6 shares the master's address and callbacks, but its receive path goes raw to the host.

Own FBlock list (RAM 0x2000576C): `F0 01 02 01 06 00 22 A1 50 A1 F5 01`. F5 table at flash 0x08044F68.

All F5 replies copy FBlock and InstID from the request, dst=S, src=0x0161.

- **F5 000 Get, 0x0802E8A0.** OP C, len 0x14: `00 33 00 30 4A 04 A0 5E 05 E0 6E 13 E1 6E 18 E1 AE 1E E1 F0`. Flips 003 300 304 A04 A05 E05 E06 E13 E16 E18 E1A E1E E1F. Available 000-002, 300-303, A04, E05, E13-E15, E18-E19, E1E. (0x305 and 0xE2B have behaviour below but are not listed.)
- **F5 001 Set, 0x0802EDCC Notification.** len > 4 and data[0]==1 else log, no reply. Nothing stored. For each 16-bit BE FktID, reply with FktID replaced:
  - **0x200, 0x303, 0xE14:** OP F (Error), len 2: `41 01` (0x41 = Busy (spec)).
  - **0x301:** OP C, len 1: `18`.
  - **0xE2B:** OP C, len 1: `00`.
  - Others: no reply.
- **F5 300 Get, 0x0802F65C.** len must be 1 else log, no reply. OP C, len 2 `id value`: 6E->01, 77->03, 7B->03, BC->01, 7E->01, 01->08, 7A->03. Other id: no reply.
- **F5 305 Get, 0x0802ECE0.** len > 5: log, no reply. OP C, len 2*len, pairs `id value`: 3A->08, 2A->03, 22->01, others->00. So the AUD3 request `26 28 24` gets `26 00 28 00 24 00`.

A00 announcement, 0x0802F758(ok): dst 0x03C8, 02.00 A00 OP C, len 1: `01` or `00`.

Choice of amplifier, 0x0802F8E8: scan records for FBlock 22 whose stored FktIDs list has any flip value above 0x111 (0x0802F7F8). Record+1 = InstID from the amp's FBlock list; record+0xCC = amp's address (src of its FBlockIDs Status, 0x0802FF7A).

**Connect, 0x0802F95C(delay r0, sink r1, mrt r2, count r3).** Nothing sent if no amp, mrt==0x40 or delay==0x40. dst=amp address, src=0x0161, 22.<amp InstID> 0x111 OP 2. count==2: len 4 `sink delay mrt mrt+1`. Else: len 6 `sink delay mrt mrt+1 mrt+2 mrt+3`.

**DisConnect, 0x0802F8F4(sink r0).** dst=amp address, 22.<amp InstID> 0x112 OP 2, len 1: `sink`.

Callers:
- Source change 0x080302A0: first DisConnect sink 1 (allocation messages toward tuner and aux FBlocks in between are not part of this decode), then +28 (`D01 Set 01 v`: source 1..3 -> 08; 4 -> 0C; 6,7 -> 04; 5 and out-of-range become 6), volume via 0x0802F9FC, then Connect sink 1, count 4. mrt,delay: source 1..3: nm+0x36, nm+0x37; source 4: nm+0x3A, nm+0x3B; source 6: nm+0x34, 00; source 7: nm+0x35, 00. (nm = 0x2000D5C4; nm+0x34 and +0x35 hold the 4-channel own allocation, label uac.)
- 0x08030AC0: Connect sink 6 for types 3,4,5 (else 4), count 2, mrt=nm+0x38, delay=nm+0x39; then 0x08031EBC.
- 0x08030B4C: 0x08031FC4, then DisConnect sink 6 for types 3,4,5 (else 4).
- 0x08030A40: DisConnect sink 1.

After the FktIDs scan, at 0x08030F58: start-up sequence (+40), then volume 0x08031A5C(amp, I, 0x0161, cfg+0x17).

Master handling of amp answers, table 0x08044E68: 0x111 OP C and 0x112 OP C with len 1: log. 0xD00 OP C: log and dump. 06 E10 OP C (table 0x08044EE0): log and dump. Other FBlock 22 answers (0xC01, 0x353, others): "nm amp cant process" log. Nothing is stored.

---

## 9. Absent from the 22.0A table (log only, no Error telegram)
0x113; 0xD00 Get (D00 is only ever sent as a Status by 0x08038878); 0x002; 0x200/0x202/0x203/0x204 Get; 0x466 Get; 0xD01 Get; 0xE09 Get; 0xE53 Get; Notification Get; 0x101/0x102/0x111/0x112 with any OP other than 2. FBlock 23: no 0x111/0x112/0xE1C-E1E handlers despite the list. No SinkInfo, no Version.

---

## 10. Everything marked UNKNOWN or (inference)

UNKNOWN:
- Role of mode 0 (0x0803CCC4); names of modes.
- Meaning of D00 fixed bytes `00 02 00 06 00 01`, and of the `mode` value beyond its mapping.
- Meaning of F5 ids 26 28 24, and of F5 functions 0x200, 0x303, 0x301, 0xE14, 0xE2B, and the 0x300 values.
- Meaning of FktIDs 0xC01, 0x353, 0xE53, 0xE00, and of the E10 reply `01 01`.
- Field meanings of the E05 table, E08 table, MuteParameters rows, D19 payloads.
- Purpose of the 46D-row sequences (0x08031EBC etc.) and their host commands.
- Who sends the A00 message in a car, hence the dst of M1..M3 (the code only copies the sender).

(inference):
- src is not transmitted by the firmware; the chip inserts its node address (0.2).
- A stored configuration may replace the default cfg image at boot (0.3).
- DS+0xA stays 0 because no writer exists; therefore all proxy branches are unreachable and 22.0A is dropped in mode 4 (0.3, 0.4). Basis: literal references to 0x20018560 and a computed-address scan of all stores. A pointer-based write through an unrecognised path cannot be excluded by static reading.
- Mode 2 with cfg+0xC 4 or 5 is the only mode where 22.0A runs (0.4).
- The T==2 Diagnosis list is not reachable (section 5).
- Columns 4,5,6 of table 0x0804877C are unused (section 7).
