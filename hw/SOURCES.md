# Звідки посадкові місця

Решта посадкових місць — зі стандартних бібліотек KiCad 10.

| Файл у `jlr_most_amp.pretty` | Деталь | Джерело | Ліцензія |
|---|---|---|---|
| `TE_MOST_Pigtail_FOT_2+0` + 3D | J2, TE 1-1394640-1 (креслення TE C-2-2141577-1) | [JLR-MOST-Reader](https://github.com/Oleg-mk/JLR-MOST-Reader), `reader_hw2`; змінено: чотири ніжки екрана в металізованих отворах на GND | CERN-OHL-S-2.0 |
| `Everlight_TOSLINK_T10` + 3D | U5, PLT133/T10W | [JLR-MOST-Reader](https://github.com/Oleg-mk/JLR-MOST-Reader), `reader_hw2` | CERN-OHL-S-2.0 |
| `BKL_0101153_RCA_RA` + 3D | J4–J6, BKL 0101153 | [MOST25-to-AUDIO](https://github.com/Oleg-mk/MOST25-to-AUDIO), `listener_hw2` | CERN-OHL-S-2.0 |
| `ESP32-WROVER-E` | основа для `ESP32-WROVER-IE` | [espressif/kicad-libraries](https://github.com/espressif/kicad-libraries), `Espressif.pretty` | CC-BY-SA 4.0 з винятком для плат, Espressif Systems |
| `ESP32-WROVER-IE` | U2, ESP32-WROVER-IE: площадки й контур `ESP32-WROVER-E` без зони заборони під антену; за [даташитом Espressif](https://documentation.espressif.com/esp32-wrover-e_esp32-wrover-ie_datasheet_en.pdf) розкладка виводів IE та сама, зони заборони в нього немає | [espressif/kicad-libraries](https://github.com/espressif/kicad-libraries), `Espressif.pretty` | CC-BY-SA 4.0 з винятком для плат, Espressif Systems |
| `TI_RZT0020A` | U8, LM65625-Q1 | креслення TI RZT0020A (4228518/C) у [даташиті LM656x5-Q1](https://www.ti.com/lit/ds/symlink/lm65625-q1.pdf), SNVSCH2A | — |
| `USB_C_Receptacle_14P_SideInsert_THT` + 3D | J10, USB-C на 14 виводів, бокова вставка, DIP | креслення продавця, [AliExpress 1005006026498474](https://aliexpress.ru/item/1005006026498474.html); посадкове місце й спрощена модель з `gen/usbc_side.py` | — |

Третій вивід RCA BKL 0101153 у кресленні виробника не описаний, на схемі він вільний.
