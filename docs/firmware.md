# Firmware: GENIbus master

GENIbus master per the Grundfos *GENIbus Protocol Specification* (Aug 2005):
CRC-16/CCITT, telegram builder/parser, Connection Request discovery, INFO
scaling (standard and extended), batched polling, class 3 commands and class
4/5 writes. Portable C99 protocol core in `lib/genibus/`, STM32 HAL glue in
`src/`.

## Code layout

```
include/geni_config.h     pins, UARTs, timing, addresses            <- knobs
src/main.c                state machine, JSON output, console commands (HAL)
src/geni_port_f103.c      RS-485 transport: DE, timing, RX interrupt   (HAL)
src/poll_table.c          items the firmware polls (MP 204)             <- per device
lib/genibus/genibus.c     CRC, telegram builder/parser, INFO, scaling   (portable)
lib/genibus/geni_units.c  unit table (indices 1-95)
lib/genibus/geni_master.c transfers, discovery, INFO cache, batching, writes
lib/genibus/mp204.c       checked MP 204 stop / trip / reset
test/host/                host unit tests (spec vectors, simulated slaves)
```

`src/main.cpp` is the old Arduino template; the build excludes it.

## Build and test

```bash
pio run                     # build (framework = stm32cube, -Wall -Wextra -Werror)
pio run -t upload           # flash with an ST-Link
make -C test/host           # 527 host checks
make -C test/host asan      # the same under ASan/UBSan
```

## Behaviour

```
BOOT -> DISCOVER -> READ_INFO -> POLL (every 1 s)
          ^-- 5 timeouts in a row: offline, discover again (or re-read INFO if the target is fixed)
```

- `GENI_TARGET_ADDR = 0` polls the first unit discovered; set an address to
  skip discovery. `target <addr>` does the same at run time.
- Same-class items go out in one GET per run of consecutive IDs. If a batch is
  rejected for an unknown ID, the items are retried one by one and the unknown
  one is skipped from then on.
- Requests are capped at a 64-byte PDU; `df_buf_len` is logged, not enforced.
- Discovery repeats every 60 s to find units added or powered on.
- Not implemented: group/multicast addressing, Data Message (0x26) helpers,
  multi-master / RFS.

Timing (`geni_config.h`): reply timeout 60 ms, gap inside a reply 4 ms,
bus idle 3 ms (50 ms after an error), DE set-up 20 us, receive discarded for
2.5 ms after transmitting (turnaround glitch).

## Console (USART1, 115200, numbers accept `0x..`)

| Command | Does |
|---|---|
| `help` | list commands |
| `scan`, `units` | Connection Request discovery; list known units |
| `target <addr>` | fix the unit (0 = discover), re-read its INFO |
| `get <cls> <id> [n]` | raw GET of n consecutive IDs |
| `info <cls> <id>` | INFO of one item, decoded |
| `probe <cls> <from> <to>` | GET each ID, list the ones that answer |
| `str <id>` | class 7 string |
| `mp204 [stop\|trip\|reset]` | checked MP 204 commands, see [MP 204](products/mp204.md) |
| `cmd <id>` | class 3 command, **unchecked** |
| `set <cls> <id> <byte>`, `setv <name> <value>` | class 4/5 write, **unchecked** |
| `reinfo`, `poll on\|off`, `stats` | re-read INFO, start/stop polling, error counters |
| `print on\|off`, `show` | a JSON line per poll (default off); print the latest one |

`cmd`, `set` and `setv` act on real equipment and have no safety checks. Use
them only with the product's functional profile and the unit in a safe state.

## Output (one JSON object per line)

The console is quiet by default: replies to commands, plus these lines on their own:

```
{"boot":{"fw":"genibus-f103-master","master":1,"target":0,"items":27,"sysclk":72000000}}
{"unit":{"addr":231,"group":247,"family":7,"type":1,"df_buf_len":77,"bus_mode":14}}
{"link":{"addr":231,"up":false,"err":"truncated"}}
{"event":{"addr":231,"t":114559,"mode":"tripped2_test","relay1_closed":0,...,"alarm":18,"alarm_text":"commanded_trip","warnings":[0,0,0],"was":{...}}}
{"offline":{"addr":231}}
```

- `link`: printed when polling starts or stops working.
- `event`: MP 204 mode, relays, motor, alarm or warnings changed since the last
  poll (within about a second). The first good poll prints the state with
  `"first":true`.

With `print on`, or once with `show`:

```
{"t":42023,"addr":231,"ok":true,"items":{"v12":{"v":414.6,"u":"V"},...,"alarm_code":{"raw":0}}}
```

Each item is `{"v","u"}` (scaled), `{"raw"}` (no scaling), `{"ack":n}` (rejected)
or `null` (no data this poll). Stale values are never printed.

Error names in replies and `stats`: `ok`, `timeout`, `truncated`, `uart`, `crc`,
`bad_length`, `bad_sd`, `bad_addr`, `bad_apdu`, `nack`, `overflow`, `bad_arg`.
