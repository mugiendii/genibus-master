# GENIbus master (FlowHub F103)

Talk to Grundfos pumps, drives and controllers over **GENIbus** (Grundfos'
RS-485 fieldbus) from an STM32F103 board, and watch, log and control them from
a PC.

- **Firmware** (`src/`, `lib/`): a GENIbus master on the FlowHub F103 board.
  Discovers units, reads their scaling, polls data items, prints JSON on a
  serial console, and has checked commands for the MP 204.
- **PC tools** (`tools/`): a master console that scans the bus, matches each
  unit to a **product profile** and shows, decodes and logs its values to CSV,
  including power failures and phase loss.
- **Product profiles** (`tools/profiles/`): one JSON file per product. Adding a
  product needs no firmware change.

## Supported products

| Product | Family / type | Status | Notes |
|---|---|---|---|
| **MP 204** motor protector | 7 / 1 | reading, events, trip and reset work on hardware | [docs/products/mp204.md](docs/products/mp204.md) |
| **CUE202** pump drive | 2 / 5 | read-only draft: values and units, **names unknown** | [docs/products/cue202.md](docs/products/cue202.md) |
| anything else | any | read-only: identify, probe, raw values | add a profile: [tools/profiles/README.md](tools/profiles/README.md) |

Data item IDs are **per product**. A profile must come from that product's
GENIbus functional profile (Grundfos), never from another product.

## Quick start

1. Wire the board to the bus: [docs/hardware.md](docs/hardware.md).
2. Build and flash:
   ```bash
   pio run -t upload
   ```
3. Start the master console, scan, pick the unit, "Use this unit":
   ```bash
   python3 tools/geni_master.py
   ```
   Close any other serial monitor first: only one program can hold the port.

## Repository layout

```
README.md                  this overview
platformio.ini             PlatformIO project (STM32F103C8, stm32cube)
include/geni_config.h      pins, UARTs, timing, addresses
src/                       board-specific firmware: main loop, console, RS-485 port, poll table
lib/genibus/               portable GENIbus core: protocol, master, units, MP 204 commands
test/host/                 host unit tests (make -C test/host)
tools/                     PC apps (master console, MP 204 console, bench logger)
tools/profiles/            product profiles (JSON) and how to write one
docs/hardware.md           board pins, bus wiring, troubleshooting, flashing
docs/firmware.md           firmware behaviour, console commands, JSON output
docs/products/             one page per product (MP 204, CUE202)
logs/                      CSV logs from the PC tools (not in git)
```

## Documentation

| Page | Covers |
|---|---|
| [docs/hardware.md](docs/hardware.md) | FlowHub F103 pins, transceiver and bus wiring, troubleshooting, flashing |
| [docs/firmware.md](docs/firmware.md) | state machine, timing, console commands, JSON output, error names |
| [tools/README.md](tools/README.md) | master console, MP 204 console, bench logger, CSV events |
| [tools/profiles/README.md](tools/profiles/README.md) | profile format, adding a product, command safety |
| [docs/products/mp204.md](docs/products/mp204.md) | MP 204 items, status bits, stop/trip/reset, bench test |
| [docs/products/cue202.md](docs/products/cue202.md) | CUE202 identification, item map, finishing its profile |

## Status

- Host tests: 527 checks pass, also under ASan/UBSan. Firmware builds with
  `-Wall -Wextra -Werror`.
- On hardware (FlowHub F103, 2026-11-25/26): MP 204 reading, events, trip and
  reset; CUE202 identification and reading with the draft profile.
- Not yet run: `mp204 stop`, the full MP 204 bench test, any CUE command.

## Safety

Commands and writes act on real motors and pumps. `mp204 stop|trip|reset` are
checked in firmware (identity, state before, read-back after); the raw `cmd`,
`set` and `setv` are not. Test with motor power isolated, and make sure an
emergency stop works without this board.
