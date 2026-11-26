# Hardware: FlowHub F103 board and GENIbus wiring

The firmware runs on the **FlowHub F103 prototype** (STM32F103C8, Blue Pill
based, 128 KB part). It is **not** for the STM32F411 FlowHub PCB: that board
has no USART3 and its RS-485 is on USART6 with automatic direction control.

## Board pins

| Signal | F103 pin | Goes to |
|---|---|---|
| GENIbus TX (USART3) | PB10 | transceiver DI |
| GENIbus RX (USART3) | PB11 (pull-up) | transceiver RO |
| Direction, DE + /RE tied | PB1 (high = transmit) | transceiver DE and /RE |
| Console TX / RX (USART1) | PA9 / PA10 | USB-serial adapter RX / TX, 115200 |
| SWD | PA13 / PA14 / GND | ST-Link SWDIO / SWCLK / GND |
| LED | PC13 | toggles on each good poll |
| MOSFET outputs DO1-DO5 | PA7, PB13, PB12, PB8, PB9 | driven **low (off)** at boot |

All pins and timing are in `include/geni_config.h`. Direction polarity:
`GENI_DE_TX_HIGH`.

- PB10/PB11 are 5 V tolerant; **PB1 is not**. A module that pulls DE/RE up to
  5 V needs that pull-up removed or a 3.3 V supply.
- The outputs float from reset until the firmware drives them: each MOSFET gate
  needs its own pull-down. Disconnect loads for first tests.
- Power the board from one supply only. With a separate supply, connect only
  GND, SWDIO and SWCLK from the ST-Link, and GND/TX/RX from the serial adapter.

## Bus wiring

```
 FLOWHUB F103            RS-485 TRANSCEIVER              GRUNDFOS UNIT
 PB10 (TX) ------------> DI
 PB11 (RX) <------------ RO          A ---------------- A
 PB1 --------+---------> DE          B ---------------- B
             +---------> /RE         bus GND ---------- Y (reference / screen)
 3V3 / GND ------------- VCC / GND
                                     screen: clamp to earthed cabinet metal
```

- GENIbus: RS-485 half duplex, 9600 8N1, up to 32 units, 1200 m daisy chain.
  No 120 Ohm termination needed at this speed.
- Use a 3.3 V transceiver (MAX3485 / SP3485), ideally isolated. A 5 V MAX485 is
  marginal at 3.3 V.
- **Connect Y.** Without the reference the master can still transmit, but
  replies arrive garbled (`truncated`, `crc`).
- Keep the bus cable away from motor and supply cables.
- Every unit needs its own address (Grundfos GO). Two units on one address
  answer together and corrupt each other.
- Only one master on the bus: disconnect other controllers or test boards.

## Troubleshooting

Read the counters with `stats` (or "Counters" in the apps).

| Symptom | Means | Check |
|---|---|---|
| only `timeout` | nothing answers | unit powered? its display on? A/B/Y at both ends, address, DE wiring |
| `truncated`, `crc` | replies arrive damaged | Y / ground, loose terminals or jumpers, transceiver supply, cable near motor wiring, a second driver on the bus |
| `bad_apdu` on a non-MP 204 unit | the firmware's MP 204 table does not fit it | use `tools/geni_master.py` with the right profile |
| board restarts (`boot` line) | supply dropout or reset | supply wiring; reset flags below |
| console prints only `00` | console line held low | board unpowered or held in reset; or the adapter is probing a bus pin at the wrong baud |

Lessons from bring-up (2026-11-25/26):

- **Faults were intermittent and physical.** The same firmware went from 75/75
  good polls to almost nothing after the board was moved, and back after the
  wiring was reseated. Suspect connections first, firmware last.
- **Turnaround glitch.** Some transceivers produce a false start bit when DE
  drops; the byte completes about 1 ms later. The firmware discards input for
  2.5 ms after transmitting (`GENI_TURNAROUND_US`) and lets the CRC judge
  noise/framing flags.
- **Probe the bus side with a second USB-serial adapter**: adapter RX on PB10
  (or PB11), GND to GND, TX unconnected, **9600** baud, hex view. A Connection
  Request is `27 0E FE 01 00 02 02 03 04 02 2E 2F 02 02 94 95 A2 AA`; replies
  start with `24`. At the wrong baud everything shows as `00`.
- **Reset flags**: RCC_CSR (0x40021024) read over SWD shows why the chip
  restarted (power-on, pin, software, watchdog). Write 0x01000000 to clear.

## Flashing and debugging

```bash
pio run -t upload          # ST-Link, from platformio.ini
```

If the ST-Link reports a target voltage but "unable to connect", reseat
SWDIO / SWCLK / GND. Back up the flash before replacing other firmware:

```bash
openocd -f interface/stlink.cfg -c "transport select hla_swd" -c "set FLASH_SIZE 0x20000" \
  -f target/stm32f1x.cfg -c "reset_config none" -c "init; dump_image backup.bin 0x08000000 0x20000; shutdown"
```
