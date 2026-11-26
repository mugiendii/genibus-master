# GENIbus product profiles

`tools/geni_master.py` loads every `*.json` here. After a scan, each unit's
`family`/`type` (class 2 IDs 148/149, answered by every GENIbus unit) picks the
profile; you can override it. Units with no matching profile get
`_generic.json`: read-only, no commands.

**Item IDs, sizes and commands are per product.** Take them from that
product's *GENIbus functional profile* (Grundfos). Never copy them from another
product: an ID that exists on both can mean something else.

## Adding a product

1. Connect it, **give it its own address** (Grundfos GO), scan, note family/type.
2. Optional: "Probe IDs" lists the IDs it answers to. Useful as a check, not a
   substitute for the functional profile.
3. Copy `mp204.json` to `<product>.json` and edit it as below.
4. Restart the app. Use the unit, check every value against the product's own
   display before trusting it.

## Format

```jsonc
{
  "name": "Display name",
  "family": 7, "type": 1,          // null = any type of that family
  "firmware_poll_ok": false,       // true only for the MP 204 (the board's own table)
  "source": "which document the IDs come from",
  "notes": "shown in the app under the profile",

  "items": [
    // cls: 2 measured, 4 config, 5 reference. id: the HIGH byte's ID.
    // size: 1-4 bytes; bytes are id, id+1, ... high byte first.
    // Scaling (unit, factor, zero, range) is read from the unit (INFO): do not add it here.
    {"name": "v_line", "label": "Line voltage", "cls": 2, "id": 35, "size": 2},
    {"name": "act_mode1", "label": "Mode", "cls": 2, "id": 95, "size": 1, "decode": "act_mode1"},
    {"name": "warnings_1", "label": "Warnings 1", "cls": 2, "id": 145, "size": 1, "watch": true}
  ],

  "decoders": {
    // bit fields of one byte ...
    "act_mode1": {"watch": true, "fields": [
      {"name": "mode", "mask": 7, "shift": 0, "values": {"0": "Started", "1": "TRIPPED"}},
      {"name": "motor", "mask": 1, "shift": 3, "values": {"0": "running", "1": "off"}}
    ]},
    // ... or a lookup table for the whole byte
    "alarm": {"watch": true, "values": {"0": "none", "48": "OVERLOAD"}}
  },

  // pop-up when this decoded field leaves its normal value
  "alert": {"item": "act_mode1", "field": "mode", "normal": "Started", "cause_item": "alarm_code"},

  "commands": [
    // send: any board console command. kind: read | warn | danger (button colour)
    {"label": "Status", "send": "mp204", "kind": "read"},
    {"label": "Start", "send": "cmd 6", "kind": "danger",
     "confirm": "Start the pump? It will run."},
    // two-step: send, wait for a reply containing `expect`, then send `then`
    {"label": "Reset", "send": "mp204 reset", "kind": "danger",
     "arm": {"expect": "armed_s", "then": "mp204 reset yes"}, "confirm": "..."}
  ]
}
```

`watch: true` on an item or decoder logs an EVENT line whenever that byte
changes. Comments (`//`) above are for explanation only: JSON files must not
contain them.

## Command safety

* `cmd <id>` (class 3) and `set`/`setv` (class 4/5) go to the unit **unchecked**.
  Give every such command a `confirm` text that says what will physically happen.
* The MP 204's `mp204 trip|stop|reset` are checked in firmware (family/type,
  state before, read-back after). Other products have no such checks yet.
