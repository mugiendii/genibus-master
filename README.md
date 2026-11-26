# GENIbus master on STM32F103 (Blue Pill)

The Blue Pill acts as a GENIbus master on RS-485 for Grundfos pumps and controllers (MGE, CU, MP 204, …). It discovers units, caches INFO scaling, polls a table of items and prints one JSON line per poll. It also has a bring-up CLI.
Protocol reference: `GENIbus_F103_Master.md` (Grundfos GENIbus Protocol Specification, Aug 2005).

## Layout
```
include/geni_config.h          pins, UARTs, timing, addresses   <- knobs
src/main.c                     state machine, JSON output, CLI   (HAL)
src/geni_port_f103.c           RS-485 transport: DE, timing, RX IRQ (HAL)
src/poll_table.c               items to poll                     <- EDIT per device
lib/genibus/genibus.c          CRC, telegram builder/parser, INFO, scaling (portable C99)
lib/genibus/geni_units.c       unit table (indices 1..95)
lib/genibus/geni_master.c      transfers, discovery, INFO cache, batching, writes
lib/genibus/mp204.c            checked MP 204 stop / trip / reset
test/host/                     host unit tests (spec vectors + simulated slaves)
```

## Build / test
```
pio run                 # firmware (framework = stm32cube)
pio run -t upload       # ST-Link
pio device monitor      # 115200, JSON + CLI
make -C test/host       # host tests;  make -C test/host asan  for ASan/UBSan
```

## Wiring
| Signal | F103 | Transceiver (MAX3485/SP3485, 3.3 V) |
|---|---|---|
| USART3 TX | PB10 | DI |
| USART3 RX | PB11 (pull-up) | RO |
| DE + /RE tied | PB1 | DE, /RE (high = transmit) |
| USART1 TX/RX | PA9 / PA10 | USB-UART for JSON + CLI |
| LED | PC13 | toggles on each good poll |

Wire bus A to A, B to B, and Y to the transceiver GND. If nothing else biases the bus, add about 680 Ω pull-up on A and 680 Ω pull-down on B. A 5 V MAX485 needs a level shifter on RO.

## Bring-up
1. Get the device's GENIbus **functional profile**. Data item IDs are per product, and the IDs in `src/poll_table.c` are CU3 *examples*.
2. Set the unit's GENIbus address with Grundfos GO or R100 (R100 "No. 1" = 32 = 0x20), or rely on discovery.
3. Flash, open the monitor and expect `{"boot":...}`, then `{"unit":{"addr":32,...}}`.
   - **Nothing found?** Swap A and B, check the bias resistors, and scope the line. Request bytes should be no more than 1.2 ms apart, and the reply should start 3–50 ms after the request.
   - **You know the address?** Run `target 0x20`, or set `GENI_TARGET_ADDR` to skip discovery.
4. Explore the unit:
   - `info 2 <id>` and `get 2 <id> [n]` read single items;
   - `probe 2 1 255` lists every class 2 ID the unit knows, with its raw value;
   - `str <id>` reads class 7 strings.
5. Put the IDs from the profile into `src/poll_table.c` and rebuild. List multi-byte values once, with `size` = 2..4 and the high-byte ID.
6. `cmd` and `setv`/`set` can start, stop or reconfigure real equipment. Use them only with the unit in a safe state.
7. `stats` counts each kind of error:
   - `crc` points to noise, bias, ground or reflections;
   - `timeout` points to the address, wiring, DE timing or a unit that is off.

### MP 204 notes
- `src/poll_table.c` is set up for an MP 204 on a **3-phase motor**. The IDs come from the MP 204 functional profile, *Operating the MP 204 … via GENIbus or G100*, Ed. 2.28, May 2011.
- Discovery should report `family 7, type 1`.
- With a 1-phase motor, the line voltages, `i_asym` and the `cos_phi1..3` items read `null` (NA). Use `v_phase` (class 2, ID 27, 2 bytes) instead.
- Some items are only measured in one motor state; otherwise they read `null`:
  - `r_insulation` is measured only while the motor is **off**. The value 101 means more than 1000 kΩ.
  - `t_mo1`, `i_asym` and `cos_phi` are measured only while the motor is **on**.

## CLI (USART1, numbers accept `0x..`)
`help`, `scan`, `units`, `target <addr>`, `get <cls> <id> [n]`, `info <cls> <id>`, `probe <cls> <from> <to>`,
`cmd <id>`, `set <cls> <id> <byte>`, `setv <name> <value>`, `mp204 [stop|trip|reset]`, `str <id>`, `reinfo`, `poll on|off`, `stats`.

## MP 204 stop / trip / reset
| Command | Sends | Effect |
|---|---|---|
| `mp204` | reads only | mode, relays, alarm, last five alarms |
| `mp204 stop` | `TRIP_A` (3, 95) | motor relay opens, no alarm (Tripped 3) |
| `mp204 trip` | `TRIP` (3, 81) | like the Test button, alarm 18 (Tripped 2) |
| `mp204 reset`, then `mp204 reset yes` within 15 s | `RESET_ALARM` (3, 2) | clears the trip **and starts the motor** |

- Before sending, the unit must identify as an MP 204 and be operating. Stop and trip need mode `started`; reset is refused while started or with protection disabled.
- The command is sent once and never retried. `result:"ok"` means the mode and motor relay were read back as requested.
- `still_tripped`: the MP 204 is in Tripped 1 after the reset. `unconfirmed`: **treat the motor state as unknown** and check it at the panel.
- `relay1_closed:0` is the relay's reported state. It does not prove that the contactor opened.
- The raw `cmd`, `set` and `setv` have none of these checks. `cmd 3` is a factory reset and `cmd 12` disables motor protection.

### Bench test (not yet run on hardware)
Motor power isolated: control voltage only on the contactor, or a lamp/meter on 95-96. An emergency stop must work without the board. The MP 204 may restart on its own (`auto_restart:1`) and starts at power-on unless `power_on_stop:1`. Write down `t_auto_restart` before step 6.

1. Start the logger (below), then `target 231`, `mp204`. Leave polling on. Expect `started`, `relay1_closed:1`, `protection:1`. **Write down `alarm` with no trip** (0 is assumed). If it is already tripped, stop and find out why.
2. `mp204 stop`. Expect `ok`, `tripped3_stop`, `relay1_closed:0`, no red Trip light, 95-96 open on the meter.
3. `mp204 stop` again: `not_started`, `sent:false`.
4. `mp204 reset`, wait over 15 s, `mp204 reset yes`: `not_armed`, 95-96 still open. Then `mp204 reset`, `mp204 reset yes`: `ok`, 95-96 closed.
5. `mp204 trip`: `ok`, `tripped2_test`, alarm 18, red light on, 95-96 open and 97-98 closed. Wait past `t_auto_restart`: it must stay tripped. Reset as in 4.
6. Simulated protection trip: `set 5 15 2` (undervoltage bit, assumed to match `alarms1_enable`), wait for the trip delay, then `mp204`: expect `tripped1_protection`, alarm 40, newest log entry 40. It should stay tripped while simulated. Reset as in 4 should clear the simulation; confirm with `get 5 15`, which must read 0 (otherwise `set 5 15 0`).
7. Stop once more, pull bus wire A, wait 10 s: 95-96 must stay open (the bus watchdog is off by default). `mp204 reset` must fail to read, and `mp204 reset yes` must report `bus_error`, `sent:false`. Reconnect.
8. Power-cycle the MP 204 from `tripped3_stop` and note whether the relay closes. The profile says it does unless `power_on_stop:1`.
9. Finish with `get 5 15` = 0, `stats`, `quit`.

### Master console (any Grundfos GENIbus product)
`python3 tools/geni_master.py [port]`: Scan the bus, select a unit, confirm its device family/profile, then "Use this unit". The app reads the unit's scaling (INFO), polls the profile's items every 2 s, decodes status, logs changes as EVENT lines and pops up when the profile's alert fires. Commands come from the profile and ask before acting. Products are described in `tools/profiles/*.json` (see `tools/profiles/README.md`); unknown products get a read-only generic profile with "Probe IDs". It switches the board's own polling off while connected and back on when it disconnects (MP 204 profile only).

Each "Use this unit" starts a log folder `logs/<date-time>_unit<addr>/`, written row by row (nothing is lost if the PC or app stops; the app must be running to log):
- `readings.csv`: every good poll (every 2 s), scaled values, second row = units.
- `events.csv`: FIRST READING, CHANGE (watched items: mode, relays, alarm, warnings), ALERT / ALERT CLEARED, PHASE VOLTAGE LOW / ALL PHASES LOW / PHASE VOLTAGES NORMAL (a phase below 85 % of the highest or below 50 V), LINK DOWN (2 polls in a row unanswered; includes the last readings), LINK UP with the outage length, STATUS AFTER RECOVERY (`mp204` incl. alarm log), COMMAND.

LINK UP compares the wall-clock gap with the MP 204's power-on counter (`t_on`, minutes, which stops while it is unpowered): "WITHOUT POWER for about N min" (loss of the phases that feed it, or all phases) versus "STAYED POWERED" (bus or wiring fault). Resolution is about a minute. A lost phase that does not feed the MP 204 shows as PHASE VOLTAGE LOW and normally as an MP 204 alarm (2 missing phase / 40 undervoltage) in CHANGE and ALERT. Pop-ups do not block polling.

### Console window
`python3 tools/geni_console.py [port]` (default `/dev/ttyUSB0`, 115200) opens a window with the colour-coded log, a command box (Up/Down = history), buttons for the read-only commands, TRIP/STOP/RESET with confirmation, and an MP 204 panel (mode, alarm text, relays, latest readings) refreshed every 5 s without filling the log. Close the VS Code serial monitor first: only one program can hold the port.

The console is quiet by default: no line per poll, only `{"link":...}` when polling starts or stops working. `print on` restores the line per poll, `show` prints the latest one.

### Record the test to spreadsheets
Needs `print on` first (one line per poll). `~/.platformio/penv/bin/python tools/bench_log.py [port]` replaces the serial monitor (close `pio device monitor` first) and takes the same commands. Files go to `logs/<date-time>/` and open in Excel, LibreOffice or Google Sheets:
- `readings.csv`: one row per poll, about one a second; blank = no data. The first row below the header normally holds the units.
- `events.csv`: mode, relay, alarm and warning changes, lost bus, commands and replies. Times are the PC clock. A change between two polls that also reverses is not seen; check the alarm log with `mp204`.
- `raw.txt`: everything received.

## JSON output
```
{"unit":{"addr":32,"group":247,"family":3,"type":1,"df_buf_len":70,"bus_mode":14}}
{"info":{"name":"p","class":2,"id":26,"head":"0x82","sif":2,"vi":0,"bo":0,"unit_idx":9,"zero_neg":0,"zero":0,"range":250,"unit":"W","factor":100}}
{"t":12345,"addr":32,"ok":true,"items":{"i_rst":{"v":13.689,"u":"A"},...,"unit_family":{"raw":3}}}
{"t":13345,"addr":32,"ok":false,"err":"timeout","items":{"i_rst":null,...}}
```
Each item is printed as one of:
- `{"v","u"}`: scaled value;
- `{"raw"}`: the item has no scaling;
- `{"ack":n}`: the device rejected the item;
- `null`: no data this poll. Stale values are never printed.

After 5 consecutive timeouts the unit is marked offline. The firmware then runs discovery again, or re-reads INFO if the target is fixed.

## Behaviour notes
- Each item list is sent as one GET telegram per run of same-class items. If the device rejects the batch for an unknown ID, the master retries item by item. After that the unknown item is skipped, and reported as `{"ack":2}`.
- Requests are capped at a 64-byte PDU. `df_buf_len` is logged but not enforced.
- Not implemented: group/multicast addressing, Data Message (0x26) helpers, multi-master/RFS.
- Status: host suite passes, also under ASan/UBSan. Firmware builds with `-Wall -Wextra -Werror`. Reading an MP 204 works on the FlowHub board (2026-11-25). **The `mp204` commands are not yet tested on hardware.**
