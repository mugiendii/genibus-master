# PC tools

All three talk to the board's console (USB-serial adapter on PA9/PA10,
default `/dev/ttyUSB0`, 115200). **Only one program can hold the port**: close
the VS Code serial monitor and the other apps first. They need `pyserial` and
`tkinter`; both are in `~/.platformio/penv/bin/python`, and in the system
Python on this laptop.

| App | Use it for |
|---|---|
| `geni_master.py` | **Any Grundfos GENIbus product**: scan, pick the profile, live readings, CSV logging |
| `geni_console.py` | MP 204 console: raw log, command box, MP 204 panel |
| `bench_log.py` | MP 204 bench test to spreadsheets (needs `print on`) |

## geni_master.py: master console

```bash
python3 tools/geni_master.py [port]
```

1. **Connect** (automatic). The board's own polling is switched off so the app
   decides what goes on the bus; it is switched back on when the app
   disconnects (MP 204 profile only).
2. **Scan bus.** Each unit is listed with address, family and type, and a
   profile from `profiles/` is suggested.
3. **Device family / profile.** Confirm or override. A profile that does not
   match the unit's family/type asks first.
4. **Use this unit.** Scaling is read from the unit (INFO); the profile's items
   are polled every 2 s, status fields decoded, changes logged.
5. **Commands** come from the profile and ask before acting.

Read-only tools: Identify (family/type), Probe IDs (class 2 or 4), Bus counters.

Products: [`profiles/`](profiles/README.md). Today: `mp204.json` (complete),
`cue202_draft.json` (read-only, names unknown), `_generic.json` (anything else).

### Logging

Each "Use this unit" starts `logs/<date-time>_unit<addr>/` (not in git),
written row by row so nothing is lost if the app or PC stops. The app must be
running to log.

- `readings.csv`: every good poll, scaled values, second row = units.
- `events.csv`:

| Event | When |
|---|---|
| FIRST READING | first good poll, with a snapshot |
| CHANGE | a watched item changed (mode, relays, alarm, warnings) |
| ALERT / ALERT CLEARED | the profile's alert condition (MP 204: tripped) |
| PHASE VOLTAGE LOW / ALL PHASES LOW / PHASE VOLTAGES NORMAL | a phase below 85 % of the highest or below 50 V |
| LINK DOWN | 2 polls in a row unanswered; includes the last readings |
| LINK UP | answering again: outage length, and "WITHOUT POWER for about N min" or "STAYED POWERED" from the unit's power-on counter |
| STATUS AFTER RECOVERY | the profile's status command after LINK UP (MP 204: alarm log) |
| COMMAND | a command sent from the app |

Phase checks and the power verdict need `phases` and `power_on_counter` in the
profile (MP 204 has both; the CUE draft has neither). Pop-ups never block
polling, so an unattended outage is still logged.

## geni_console.py: MP 204 console

```bash
python3 tools/geni_console.py [port]
```

Colour-coded log, command box (Up/Down = history), buttons for the read-only
commands, TRIP / STOP / RESET with confirmation, and an MP 204 panel (mode,
alarm text, relays, readings) refreshed every 5 s without filling the log.
Shows the firmware's `event` lines and pops up when the MP 204 trips. Uses the
firmware's own polling, so it is MP 204 only.

## bench_log.py: bench test to spreadsheets

```bash
python3 tools/bench_log.py [port]
```

Send `print on` first (one line per poll). Takes the same commands as the
console. Writes `logs/<date-time>/readings.csv` (one row per poll),
`events.csv` (mode, relay, alarm and warning changes, lost bus, commands) and
`raw.txt`. A change that reverses between two polls is not seen; check the
alarm log with `mp204`.
