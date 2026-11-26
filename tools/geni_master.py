#!/usr/bin/env python3
"""GENIbus master console: scan the bus, pick a unit, load its product profile.

    python3 tools/geni_master.py [port]          (default /dev/ttyUSB0, 115200)

1. Connect. The board's own MP 204 polling is switched off (`poll off`) so this
   app decides what goes on the bus.
2. Scan. Every GENIbus unit that answers is listed with its address, family
   and type. The profile is chosen from family/type; you can override it.
3. Use unit. The app targets that address, reads each item's scaling (INFO)
   from the unit itself, then polls the profile's items, decodes status
   fields, logs changes and raises a pop-up when the alert condition fires.
4. Commands come from the profile. Anything that acts on equipment asks first.

Profiles live in tools/profiles/*.json (see _generic.json and mp204.json).
Item IDs are per product: a profile must come from that product's GENIbus
functional profile. Unknown products get the read-only generic profile, with
"Probe IDs" to list what the unit answers to.

Needs pyserial and tkinter. Only one program can hold the port.
"""
import csv
import datetime
import glob
import json
import os
import queue
import re
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import serial
import serial.tools.list_ports

BAUD = 115200
HERE = os.path.dirname(os.path.abspath(__file__))
PROFILE_DIR = os.path.join(HERE, "profiles")
POLL_S = 2.0
LOG_ROOT = os.path.join(HERE, "..", "logs")
DOWN_AFTER = 2           # consecutive failed polls before the link counts as down


# ----------------------------------------------------------------- profiles
def load_profiles():
    out = []
    for f in sorted(glob.glob(os.path.join(PROFILE_DIR, "*.json"))):
        try:
            p = json.load(open(f))
            p["_file"] = os.path.basename(f)
            out.append(p)
        except Exception as e:
            print(f"profile {f}: {e}", file=sys.stderr)
    return out


def match_profile(profiles, family, typ):
    for p in profiles:
        if p.get("family") == family and p.get("type") in (typ, None):
            return p
    return next((p for p in profiles if p["_file"] == "_generic.json"), None)


def scale(info, raw):
    """Physical value from raw bytes (high first) and a decoded INFO reply.
    Mirrors geni_scale() in lib/genibus/genibus.c. Returns (value, unit) or
    (None, 'n/a') / (int, '') for unscaled items."""
    n = len(raw)
    x = int.from_bytes(bytes(raw), "big")
    if info is None:
        return x, ""
    if not info.get("vi", 0) and raw[0] == 0xFF:
        return None, "n/a"
    sif = info.get("sif", 0)
    factor = info.get("factor")
    if sif < 2 or factor is None:
        return x, ""
    sign = -1 if info.get("zero_neg") else 1
    if sif == 2:
        z = sign * info.get("zero", 0)
        full = 254 * 256 ** (n - 1)
        v = (z + x * info.get("range", 0) / full) * factor
    else:
        z16 = sign * info.get("zero", 0)  # firmware prints ZERO_hi*256+ZERO_lo
        mult = 256 ** (n - 2) if n > 2 else 1
        v = (z16 * mult + x) * factor
    return round(v, 4), info.get("unit") or ""


def ranges(ids):
    """[1,2,3,7,9,10] -> '1-3, 7, 9-10'"""
    out, ids = [], sorted(ids)
    for i in ids:
        if out and out[-1][1] == i - 1:
            out[-1][1] = i
        else:
            out.append([i, i])
    return ", ".join(f"{a}-{b}" if a != b else f"{a}" for a, b in out)


def decode(dec, raw):
    """Decode a byte with a profile decoder -> (summary text, {field: text})."""
    if not dec:
        return None, {}
    if "values" in dec:
        t = dec["values"].get(str(raw), f"unknown ({raw})")
        return f"{raw}: {t}", {"value": t}
    fields = {}
    for f in dec.get("fields", []):
        v = (raw >> f.get("shift", 0)) & f.get("mask", 1)
        fields[f["name"]] = f.get("values", {}).get(str(v), str(v))
    return ", ".join(f"{k} {v}" for k, v in fields.items()), fields


# -------------------------------------------------------------- bus thread
class Bus(threading.Thread):
    """Owns the port. Sends one command at a time and waits for its reply;
    every other line goes to the log. Callbacks run on the GUI thread."""

    def __init__(self, port, ui_q):
        super().__init__(daemon=True)
        self.ser = serial.Serial(port, BAUD, timeout=0.05)
        self.jobs = queue.Queue()
        self.ui_q = ui_q
        self.stop = threading.Event()

    def submit(self, cmd, match, timeout=3.0, cb=None, log=True):
        self.jobs.put((cmd, match, timeout, cb, log))

    def pending(self):
        return self.jobs.qsize()

    def _lines(self, buf):
        try:
            buf += self.ser.read(4096)
        except Exception as e:
            if not self.stop.is_set():
                self.ui_q.put(("error", str(e)))
            self.stop.set()
            return [], buf
        *ls, buf = buf.split(b"\n")
        return [l.strip().decode("ascii", "replace") for l in ls if l.strip()], buf

    def run(self):
        buf = b""
        while not self.stop.is_set():
            try:
                cmd, match, timeout, cb, log = self.jobs.get(timeout=0.05)
            except queue.Empty:
                ls, buf = self._lines(buf)
                for l in ls:
                    self.ui_q.put(("line", l, None))
                continue
            try:
                self.ser.write(cmd.encode() + b"\n")
            except Exception as e:
                self.ui_q.put(("error", str(e)))
                return
            if log:
                self.ui_q.put(("tx", cmd))
            reply, extra, t0 = None, [], time.time()
            while time.time() - t0 < timeout and not self.stop.is_set():
                ls, buf = self._lines(buf)
                for l in ls:
                    if reply is None and match(l):
                        reply = l
                    else:
                        extra.append(l)
                        self.ui_q.put(("line", l, None))
                if reply is not None:
                    break
            if log and reply is not None:
                self.ui_q.put(("line", reply, "reply"))
            if cb:
                self.ui_q.put(("cb", cb, reply, extra))

    def close(self):
        self.stop.set()
        try:
            self.ser.close()
        except Exception:
            pass


def starts(key):
    return lambda l: l.startswith('{"' + key + '"')


# ------------------------------------------------------------------ CSV log
class CsvLog:
    """events.csv and readings.csv for one unit session. Every row is flushed
    as it is written, so an outage while unattended is never lost."""

    def __init__(self, unit, profile, items):
        stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        self.dir = os.path.abspath(os.path.join(LOG_ROOT, f"{stamp}_unit{unit['addr']}"))
        os.makedirs(self.dir, exist_ok=True)
        self.items = items
        self.ef = open(os.path.join(self.dir, "events.csv"), "w", newline="")
        self.ev = csv.writer(self.ef)
        self.ev.writerow(["time", "event", "detail"])
        self.rf = open(os.path.join(self.dir, "readings.csv"), "w", newline="")
        self.rd = csv.writer(self.rf)
        self.rd.writerow(["time"] + [i["name"] for i in items])
        self.units_written = False
        self.event("SESSION START", f"unit {unit['addr']} family {unit.get('family')} type {unit.get('type')}, "
                                    f"profile {profile['name']}")

    @staticmethod
    def now():
        return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    def event(self, kind, detail=""):
        self.ev.writerow([self.now(), kind, detail])
        self.ef.flush()

    def reading(self, vals, units):
        if not self.units_written:
            self.rd.writerow(["(unit)"] + [units.get(i["name"], "") for i in self.items])
            self.units_written = True
        self.rd.writerow([self.now()] + ["" if vals.get(i["name"]) is None else vals[i["name"]]
                                         for i in self.items])
        self.rf.flush()

    def close(self):
        self.event("SESSION END")
        self.ef.close()
        self.rf.close()


# --------------------------------------------------------------------- app
class Master(tk.Tk):
    def __init__(self, port):
        super().__init__()
        self.title("GENIbus master")
        self.geometry("1280x800")
        self.profiles = load_profiles()
        self.ui_q = queue.Queue()
        self.bus = None
        self.units = {}          # addr -> unit dict
        self.unit = None         # selected unit dict
        self.profile = None
        self.infos = {}          # item name -> INFO dict
        self.values = {}         # item name -> raw bytes
        self.last_watch = {}     # item name -> raw byte, for change events
        self.polling = False
        self.poll_busy = False
        self.unknown = set()     # item names the unit rejected
        self.fresh = set()       # items updated by the current poll
        self.csv = None          # CsvLog of the selected unit
        self.link = None         # None (unknown), "up", "down"
        self.fail_streak = 0
        self.last_ok = None      # wall time of the last good poll
        self.last_counter = None # power-on counter value at the last good poll
        self.last_snapshot = ""  # readings text at the last good poll
        self.phase_low = None    # frozenset of phases reported low
        self._alerted = False
        self.session = []
        self.history, self.hpos = [], 0
        self._build(port)
        self.after(50, self._pump)

    # ------------------------------------------------------------- layout
    def _build(self, port):
        top = ttk.Frame(self, padding=6)
        top.pack(fill="x")
        ttk.Label(top, text="Port").pack(side="left")
        ports = [p.device for p in serial.tools.list_ports.comports()] or [port]
        self.port = tk.StringVar(value=port)
        ttk.Combobox(top, textvariable=self.port, values=ports, width=16).pack(side="left", padx=4)
        self.conn_btn = ttk.Button(top, text="Connect", command=self.toggle)
        self.conn_btn.pack(side="left")
        self.conn_lbl = ttk.Label(top, text="disconnected", foreground="grey")
        self.conn_lbl.pack(side="left", padx=8)
        ttk.Button(top, text="Save log", command=self.save).pack(side="right")
        ttk.Button(top, text="Clear log", command=lambda: self.log.delete("1.0", "end")).pack(side="right", padx=4)
        ttk.Button(top, text="Counters", command=lambda: self.raw("stats", starts("stats"))).pack(side="right", padx=4)

        body = ttk.PanedWindow(self, orient="horizontal")
        body.pack(fill="both", expand=True, padx=6)

        # left: units
        left = ttk.Frame(body)
        body.add(left, weight=1)
        uf = ttk.LabelFrame(left, text="1. Units on the bus", padding=6)
        uf.pack(fill="both", expand=True)
        ttk.Button(uf, text="Scan bus", command=self.scan).pack(fill="x")
        self.unit_tree = ttk.Treeview(uf, columns=("addr", "fam", "type", "profile"), show="headings", height=6)
        for c, w, t in [("addr", 50, "Addr"), ("fam", 55, "Family"), ("type", 45, "Type"), ("profile", 170, "Profile")]:
            self.unit_tree.heading(c, text=t)
            self.unit_tree.column(c, width=w, anchor="w")
        self.unit_tree.pack(fill="both", expand=True, pady=4)
        self.unit_tree.bind("<<TreeviewSelect>>", self._unit_selected)
        pf = ttk.LabelFrame(left, text="2. Device family / profile", padding=6)
        pf.pack(fill="x", pady=(6, 0))
        self.prof_var = tk.StringVar()
        self.prof_box = ttk.Combobox(pf, textvariable=self.prof_var, state="readonly",
                                     values=[self._plabel(p) for p in self.profiles])
        self.prof_box.pack(fill="x")
        ttk.Label(pf, text="Manual address (if not found by scan):").pack(anchor="w", pady=(6, 0))
        self.addr_var = tk.StringVar()
        ttk.Entry(pf, textvariable=self.addr_var, width=8).pack(anchor="w")
        ttk.Button(pf, text="3. Use this unit", command=self.use_unit).pack(fill="x", pady=(6, 0))
        self.prof_note = tk.Label(pf, text="", justify="left", wraplength=320, fg="#555")
        self.prof_note.pack(anchor="w", pady=(6, 0))

        # middle: readings
        mid = ttk.Frame(body)
        body.add(mid, weight=3)
        self.unit_hdr = tk.Label(mid, text="No unit selected", anchor="w", font=("TkDefaultFont", 12, "bold"))
        self.unit_hdr.pack(fill="x")
        self.alert_lbl = tk.Label(mid, text="", anchor="w", font=("TkDefaultFont", 11, "bold"))
        self.alert_lbl.pack(fill="x")
        rf = ttk.Frame(mid)
        rf.pack(fill="both", expand=True)
        self.read_tree = ttk.Treeview(rf, columns=("label", "value", "detail", "raw"), show="headings")
        for c, w, t in [("label", 190, "Item"), ("value", 130, "Value"), ("detail", 330, "Decoded"), ("raw", 90, "Raw")]:
            self.read_tree.heading(c, text=t)
            self.read_tree.column(c, width=w, anchor="w")
        self.read_tree.tag_configure("bad", foreground="#c62828")
        self.read_tree.tag_configure("na", foreground="#888")
        ys = ttk.Scrollbar(rf, command=self.read_tree.yview)
        self.read_tree.configure(yscrollcommand=ys.set)
        ys.pack(side="right", fill="y")
        self.read_tree.pack(fill="both", expand=True)
        pr = ttk.Frame(mid)
        pr.pack(fill="x", pady=4)
        self.poll_btn = ttk.Button(pr, text="Start polling", command=self.toggle_poll, state="disabled")
        self.poll_btn.pack(side="left")
        ttk.Button(pr, text="Read once", command=self.poll_once).pack(side="left", padx=4)
        self.age_lbl = ttk.Label(pr, text="", foreground="grey")
        self.age_lbl.pack(side="left", padx=8)
        self.logdir_lbl = ttk.Label(mid, text="", foreground="grey")
        self.logdir_lbl.pack(fill="x")

        # right: commands
        right = ttk.Frame(body)
        body.add(right, weight=1)
        self.cmd_frame = ttk.LabelFrame(right, text="4. Commands (from profile)", padding=6)
        self.cmd_frame.pack(fill="x")
        ttk.Label(self.cmd_frame, text="Select a unit first.").pack()
        gf = ttk.LabelFrame(right, text="Read-only tools", padding=6)
        gf.pack(fill="x", pady=6)
        ttk.Button(gf, text="Identify (family/type)", command=lambda: self.raw("get 2 148 2", starts("get"))).pack(fill="x")
        ttk.Button(gf, text="Probe IDs 1-255 (class 2)", command=lambda: self.probe(2)).pack(fill="x", pady=2)
        ttk.Button(gf, text="Probe IDs 1-255 (class 4)", command=lambda: self.probe(4)).pack(fill="x")
        ttk.Button(gf, text="Bus counters", command=lambda: self.raw("stats", starts("stats"))).pack(fill="x", pady=2)
        self.probe_lbl = tk.Label(gf, text="", justify="left", wraplength=260, fg="#555")
        self.probe_lbl.pack(anchor="w")

        # bottom: log + command line
        logf = ttk.Frame(self, padding=(6, 0))
        logf.pack(fill="both")
        self.log = tk.Text(logf, height=11, wrap="none", font=("Monospace", 9), bg="#1e1e1e", fg="#d4d4d4")
        ys2 = ttk.Scrollbar(logf, command=self.log.yview)
        self.log.configure(yscrollcommand=ys2.set)
        ys2.pack(side="right", fill="y")
        self.log.pack(fill="both", expand=True)
        for tag, col in [("tx", "#569cd6"), ("err", "#f44747"), ("ok", "#6a9955"), ("event", "#ffcc00"),
                         ("info", "#c586c0"), ("dim", "#808080")]:
            self.log.tag_configure(tag, foreground=col)
        bot = ttk.Frame(self, padding=6)
        bot.pack(fill="x")
        ttk.Label(bot, text="Raw command:").pack(side="left")
        self.entry = ttk.Entry(bot)
        self.entry.pack(side="left", fill="x", expand=True, padx=4)
        self.entry.bind("<Return>", lambda e: self._typed())
        self.entry.bind("<Up>", lambda e: self._hist(-1))
        self.entry.bind("<Down>", lambda e: self._hist(1))
        ttk.Button(bot, text="Send", command=self._typed).pack(side="left")
        self.protocol("WM_DELETE_WINDOW", self.close)

    @staticmethod
    def _plabel(p):
        fam = "any" if p.get("family") is None else f"{p['family']}/{p.get('type')}"
        return f"{p['name']}  [family {fam}]"

    # ------------------------------------------------------------- serial
    def toggle(self):
        if self.bus:
            self.disconnect()
            return
        try:
            self.bus = Bus(self.port.get(), self.ui_q)
        except Exception as e:
            messagebox.showerror("Serial", f"Cannot open {self.port.get()}:\n{e}\n\n"
                                 "Close the VS Code serial monitor or the other console app: "
                                 "only one program can hold the port.")
            self.bus = None
            return
        self.bus.start()
        self.conn_btn.config(text="Disconnect")
        self.conn_lbl.config(text=f"connected {self.port.get()} @ {BAUD}", foreground="green")
        self._line(f"--- connected {self.port.get()} ---", "info")
        # this app decides what goes on the bus: stop the firmware's own polling
        self.bus.submit("poll off", starts("poll"), cb=lambda r, x: None)
        self.bus.submit("units", starts("units"), cb=self._units_reply)

    def disconnect(self):
        self.polling = False
        self.poll_btn.config(text="Start polling")
        if self.bus:
            # Hand the bus back to the firmware's own polling (its MP 204 table
            # and trip events) - but never for another product: the firmware
            # would read MP 204 meanings into that unit's items.
            fw_ok = self.profile is None or self.profile.get("firmware_poll_ok", False)
            if fw_ok and not self.bus.stop.is_set():
                try:
                    self.bus.ser.write(b"poll on\n")
                    self.bus.ser.flush()
                    time.sleep(0.1)
                    self._line("firmware polling switched back on (poll on)", "dim")
                except Exception:
                    pass
            elif not fw_ok:
                self._line("firmware polling left OFF: its MP 204 table does not fit this unit", "dim")
            self.bus.close()
        self.bus = None
        self.conn_btn.config(text="Connect")
        self.conn_lbl.config(text="disconnected", foreground="grey")
        self._line("--- disconnected ---", "info")

    def raw(self, cmd, match, timeout=4.0, cb=None):
        if not self.bus:
            messagebox.showwarning("Serial", "Not connected.")
            return
        self.bus.submit(cmd, match, timeout, cb)

    # -------------------------------------------------------------- units
    def scan(self):
        if not self.bus:
            messagebox.showwarning("Serial", "Not connected.")
            return
        self._line("scanning... units addressed in the last 20 s stay quiet, so 'units' lists everything known", "dim")
        self.bus.submit("scan", starts("scan"), timeout=30, cb=lambda r, x: None)
        self.bus.submit("units", starts("units"), cb=self._units_reply)

    def _units_reply(self, reply, extra):
        for l in extra:
            try:
                j = json.loads(l)
            except Exception:
                continue
            if "unit" in j:
                u = j["unit"]
                self.units[u["addr"]] = u
        self.unit_tree.delete(*self.unit_tree.get_children())
        for a, u in sorted(self.units.items()):
            p = match_profile(self.profiles, u.get("family"), u.get("type"))
            self.unit_tree.insert("", "end", iid=str(a),
                                  values=(a, u.get("family"), u.get("type"), p["name"] if p else "-"))
        if not self.units:
            self._line("no units known yet: check wiring, or give a manual address", "err")

    def _unit_selected(self, _e):
        sel = self.unit_tree.selection()
        if not sel:
            return
        u = self.units[int(sel[0])]
        self.addr_var.set(str(u["addr"]))
        p = match_profile(self.profiles, u.get("family"), u.get("type"))
        if p:
            self.prof_var.set(self._plabel(p))
            self.prof_note.config(text=p.get("notes", ""))

    def use_unit(self):
        if not self.bus:
            messagebox.showwarning("Serial", "Not connected.")
            return
        try:
            addr = int(self.addr_var.get(), 0)
            assert 1 <= addr <= 231
        except Exception:
            messagebox.showwarning("Unit", "Select a unit or enter an address (1-231).")
            return
        label = self.prof_var.get()
        p = next((x for x in self.profiles if self._plabel(x) == label), None)
        if not p:
            messagebox.showwarning("Profile", "Choose a device family / profile.")
            return
        u = self.units.get(addr, {"addr": addr})
        fam, typ = u.get("family"), u.get("type")
        if p.get("family") is not None and fam is not None and (fam, typ) != (p["family"], p.get("type")):
            if not messagebox.askyesno(
                    "Profile mismatch",
                    f"Unit {addr} reports family {fam}, type {typ}, but '{p['name']}' is for "
                    f"family {p['family']}, type {p.get('type')}.\n\nItem IDs differ between products: "
                    "values may be wrong and its commands could act on the wrong thing.\n\nUse it anyway?",
                    icon="warning"):
                return
        self.polling = False
        self.poll_btn.config(text="Start polling")
        self.unit, self.profile = u, p
        self.infos, self.values, self.last_watch, self.unknown = {}, {}, {}, set()
        self.link, self.fail_streak, self.last_ok, self.last_counter = None, 0, None, None
        self.phase_low, self._alerted = None, False
        if self.csv:
            self.csv.close()
        try:
            self.csv = CsvLog(u, p, p.get("items", []))
            self.logdir_lbl.config(text=f"logging to {self.csv.dir}")
            self._line(f"logging to {self.csv.dir}", "dim")
        except Exception as e:
            self.csv = None
            self._line(f"cannot create log folder: {e}", "err")
        self.unit_hdr.config(text=f"Unit {addr}: {p['name']}  (family {fam}, type {typ})")
        self.alert_lbl.config(text="", bg=self.cget("bg"))
        self.prof_note.config(text=p.get("notes", ""))
        self._fill_items()
        self._fill_commands()
        self.raw(f"target {addr}", starts("target"))
        self.raw("poll off", starts("poll"))
        self._read_infos()

    def _fill_items(self):
        self.read_tree.delete(*self.read_tree.get_children())
        for it in self.profile.get("items", []):
            self.read_tree.insert("", "end", iid=it["name"], values=(it.get("label", it["name"]), "-", "", ""))

    def _fill_commands(self):
        for w in self.cmd_frame.winfo_children():
            w.destroy()
        cmds = self.profile.get("commands", [])
        if not cmds:
            ttk.Label(self.cmd_frame, text="This profile has no commands.\n"
                                           "Add them from the product's GENIbus profile.",
                      justify="left").pack(anchor="w")
        colours = {"read": None, "warn": "#f0c36d", "danger": "#e57373"}
        for c in cmds:
            b = tk.Button(self.cmd_frame, text=c["label"], command=lambda c=c: self.run_command(c))
            if colours.get(c.get("kind")):
                b.config(bg=colours[c.get("kind")])
            b.pack(fill="x", pady=1)

    # ------------------------------------------------------------ scaling
    def _read_infos(self):
        items = [i for i in self.profile.get("items", []) if i.get("cls") in (2, 3, 4, 5)]
        self._line(f"reading scaling (INFO) for {len(items)} items from the unit...", "dim")
        for it in items:
            pat = re.compile(r'^\{"info":\{("name":"-","class":%d,"id":%d,|"err")' % (it["cls"], it["id"]))
            self.bus.submit(f"info {it['cls']} {it['id']}", lambda l, p=pat: bool(p.match(l)),
                            timeout=2.0, cb=lambda r, x, it=it: self._info_reply(it, r), log=False)
        self.bus.submit("stats", starts("stats"), cb=lambda r, x: self._infos_done(), log=False)

    def _info_reply(self, it, reply):
        try:
            j = json.loads(reply)["info"]
        except Exception:
            return
        if "err" in j:
            if j.get("ack") == 2:
                self.unknown.add(it["name"])
            return
        self.infos[it["name"]] = j

    def _infos_done(self):
        n = len(self.profile.get("items", []))
        self._line(f"scaling read for {len(self.infos)} of {n} items"
                   + (f"; unit does not know: {', '.join(sorted(self.unknown))}" if self.unknown else ""), "ok")
        self.poll_btn.config(state="normal")
        self.poll_once()
        self.toggle_poll(start=True)

    # ------------------------------------------------------------ polling
    def _ranges(self):
        """Consecutive ID runs per class -> [(cls, first_id, count, [(item, offset)])]."""
        ids = []
        for it in self.profile.get("items", []):
            if it["name"] in self.unknown or it.get("cls") not in (0, 2, 4, 5):
                continue
            for b in range(it.get("size", 1)):
                ids.append((it["cls"], it["id"] + b, it, b))
        ids.sort(key=lambda t: (t[0], t[1]))
        runs = []
        for cls, i, it, b in ids:
            if runs and runs[-1][0] == cls and runs[-1][1] + runs[-1][2] == i and runs[-1][2] < 62:
                runs[-1][2] += 1
                runs[-1][3].append((i, it, b))
            elif runs and runs[-1][0] == cls and runs[-1][1] + runs[-1][2] > i:
                runs[-1][3].append((i, it, b))  # overlapping item, already covered
            else:
                runs.append([cls, i, 1, [(i, it, b)]])
        return runs

    def toggle_poll(self, start=None):
        if start and self.polling:
            return  # already running: a second tick chain would double the traffic
        self.polling = (not self.polling) if start is None else start
        self.poll_btn.config(text="Stop polling" if self.polling else "Start polling")
        if self.polling:
            self.after(10, self._poll_tick)

    def _poll_tick(self):
        if not self.polling or not self.bus:
            return
        if not self.poll_busy and self.bus.pending() == 0:
            self.poll_once()
        self.after(int(POLL_S * 1000), self._poll_tick)

    def poll_once(self):
        if not self.bus or not self.profile:
            return
        runs = self._ranges()
        if not runs:
            return
        self.poll_busy = True
        self.poll_result = {"ok": 0, "bad": 0}
        self.fresh = set()
        for k, (cls, first, count, parts) in enumerate(runs):
            last = k == len(runs) - 1
            self.bus.submit(f"get {cls} {first} {count}", starts("get"), timeout=2.0, log=False,
                            cb=lambda r, x, cls=cls, first=first, count=count, parts=parts, last=last:
                            self._get_reply(cls, first, count, parts, r, last))

    def _get_reply(self, cls, first, count, parts, reply, last):
        try:
            g = json.loads(reply)["get"]
        except Exception:
            g = {"err": "no_reply"}
        if "vals" in g and len(g["vals"]) == count:
            byid = {first + k: v for k, v in enumerate(g["vals"])}
            for i, it, b in parts:
                buf = self.values.setdefault(it["name"], [0] * it.get("size", 1))
                buf[b] = byid[i]
                self.fresh.add(it["name"])
            self.poll_result["ok"] += 1
        else:
            self.poll_result["bad"] += 1
            if g.get("err") == "nack" and g.get("ack") == 2:
                # an unknown ID in this run: test its items one by one next time
                for _, it, _ in parts:
                    self.bus.submit(f"get {it['cls']} {it['id']} {it.get('size', 1)}", starts("get"),
                                    timeout=2.0, log=False, cb=lambda r, x, it=it: self._single(it, r))
            if self.link != "down":  # one line per failure, not per poll while it lasts
                self._line(f"get {cls} {first} {count}: {g.get('err')}", "err")
        if last:
            self.poll_busy = False
            self._show()

    def _single(self, it, reply):
        try:
            g = json.loads(reply)["get"]
        except Exception:
            return
        if g.get("err") == "nack" and g.get("ack") == 2:
            self.unknown.add(it["name"])
            self._line(f"unit does not know {it['name']} (class {it['cls']} id {it['id']}): skipped from now on", "dim")

    def _show(self):
        r = self.poll_result
        if r["ok"] == 0:
            self._poll_failed()
            return
        self.fail_streak = 0
        first = self.last_ok is None
        recovered = self.link == "down"
        self.link = "up"
        decs = self.profile.get("decoders", {})
        row, units = {}, {}
        for it in self.profile.get("items", []):
            name = it["name"]
            if name not in self.fresh and name not in self.unknown:
                if name in self.values:  # this run failed: keep it visibly stale
                    v = self.read_tree.item(name)["values"]
                    self.read_tree.item(name, values=(v[0], v[1], "(no reply this poll)", v[3]), tags=("na",))
                continue
            if name in self.unknown:
                self.read_tree.item(name, values=(it.get("label", name), "not supported", "", ""), tags=("na",))
                continue
            raw = self.values.get(name)
            if raw is None:
                continue
            val, unit = scale(self.infos.get(name), raw)
            rawtxt = "0x" + "".join(f"{b:02X}" for b in raw)
            dec = decs.get(it.get("decode")) if it.get("decode") else None
            summary, fields = decode(dec, raw[0]) if dec else (None, {})
            shown = "n/a" if val is None else (f"{val} {unit}".strip())
            tag = ("na",) if val is None else ()
            self.read_tree.item(name, values=(it.get("label", name), shown, summary or "", rawtxt), tags=tag)
            row[name] = val if not summary else f"{val} ({summary})"
            units[name] = unit if val is not None else ""
            watch = it.get("watch") or (dec or {}).get("watch")
            if watch:
                self._watch(it, raw[0], summary, fields)
        if self.csv:
            self.csv.reading(row, units)
        if recovered:
            self._link_up()
        self._phase_check()
        self._alert()
        self.last_ok = time.time()
        pc = self.profile.get("power_on_counter")
        if pc and pc in self.fresh:
            self.last_counter = scale(self.infos.get(pc), self.values[pc])[0]
        self.last_snapshot = self._snapshot()
        if first and self.csv:
            self.csv.event("FIRST READING", self.last_snapshot)
        self.age_lbl.config(text=f"read {datetime.datetime.now():%H:%M:%S}   "
                                 f"{r['ok']} ok, {r['bad']} failed requests")

    # ------------------------------------------------- link / power events
    def _snapshot(self):
        keys = self.profile.get("snapshot", [])
        out = []
        for k in keys:
            if k in self.values and k in self.fresh:
                v, u = scale(self.infos.get(k), self.values[k])
                out.append(f"{k}={'n/a' if v is None else v}{(' ' + u) if u and v is not None else ''}")
        for k in ("act_mode1", "alarm_code"):
            if k in self.values and k in self.fresh:
                it = next((i for i in self.profile["items"] if i["name"] == k), None)
                dec = self.profile.get("decoders", {}).get((it or {}).get("decode"))
                if dec:
                    out.append(f"{k}: {decode(dec, self.values[k][0])[0]}")
        return "; ".join(out)

    def _poll_failed(self):
        self.fail_streak += 1
        self.age_lbl.config(text=f"NO REPLY {datetime.datetime.now():%H:%M:%S}  "
                                 f"({self.fail_streak} polls in a row)")
        if self.fail_streak < DOWN_AFTER or self.link == "down":
            return
        self.link = "down"
        since = datetime.datetime.fromtimestamp(self.last_ok).strftime("%H:%M:%S") if self.last_ok else "?"
        detail = f"no reply since {since}. Last readings: {self.last_snapshot or 'none'}"
        self._line(f"LINK DOWN: unit {self.unit['addr']} not answering. {detail}", "err")
        if self.csv:
            self.csv.event("LINK DOWN", detail)
        for name in self.read_tree.get_children():
            v = self.read_tree.item(name)["values"]
            self.read_tree.item(name, values=(v[0], v[1], "(stale - no reply)", v[3]), tags=("na",))
        self.alert_lbl.config(text=f"NO REPLY since {since}: unit unpowered, or bus fault",
                              fg="white", bg="#6d4c41")
        self.title("GENIbus master - NO REPLY")
        self._notify("Unit not answering",
                     f"Unit {self.unit['addr']} stopped answering at about {since}.\n\n"
                     "Power failure (all phases, or the phases feeding it) or a bus fault.\n"
                     "Logging continues; recovery will be logged with the outage length.")

    def _link_up(self):
        now = time.time()
        gap_min = (now - self.last_ok) / 60 if self.last_ok else None
        pc = self.profile.get("power_on_counter")
        verdict = ""
        if gap_min is not None and pc and pc in self.fresh and self.last_counter is not None:
            counter = scale(self.infos.get(pc), self.values[pc])[0]
            if counter is not None:
                ran = counter - self.last_counter
                off = gap_min - ran
                if off >= 1.5:
                    verdict = (f"unit was WITHOUT POWER for about {off:.0f} min "
                               f"(its power-on counter advanced {ran:.0f} of {gap_min:.0f} min)")
                elif counter < self.last_counter:
                    verdict = "power-on counter went backwards (reset?): outage length unknown"
                else:
                    verdict = (f"unit STAYED POWERED (counter advanced {ran:.0f} of {gap_min:.1f} min): "
                               "communication / wiring fault, not a power failure")
        detail = (f"answering again after {gap_min:.1f} min" if gap_min is not None else "answering again")
        detail += (f"; {verdict}" if verdict else "")
        detail += f". Now: {self._snapshot_now()}"
        self._line(f"LINK UP: {detail}", "ok")
        if self.csv:
            self.csv.event("LINK UP", detail)
        rc = self.profile.get("on_recovery")
        if rc and self.bus:
            self.bus.submit(rc, starts(rc.split()[0]), timeout=5, log=False,
                            cb=lambda r, x: self.csv and r and self.csv.event("STATUS AFTER RECOVERY", r))
        if len(self.infos) < len(self.profile.get("items", [])) // 2:
            self._read_infos()  # scaling was never read (unit was off at start)
        self._notify("Unit answering again", detail)

    def _snapshot_now(self):
        saved, self.fresh = self.fresh, set(self.values)
        try:
            return self._snapshot()
        finally:
            self.fresh = saved

    def _phase_check(self):
        ph = self.profile.get("phases")
        if not ph:
            return
        vals = {}
        for k in ph["items"]:
            if k in self.fresh:
                vals[k] = scale(self.infos.get(k), self.values[k])[0]
        if len(vals) != len(ph["items"]):
            return
        good = [v for v in vals.values() if v is not None]
        top = max(good) if good else 0
        low = frozenset(k for k, v in vals.items()
                        if v is None or v < ph.get("low_abs", 50) or v < ph.get("low_ratio", 0.85) * top)
        if low == self.phase_low:
            return
        first = self.phase_low is None
        self.phase_low = low
        txt = ", ".join(f"{k}={'n/a' if v is None else v}" for k, v in vals.items())
        if low:
            kind = "ALL PHASES LOW" if len(low) == len(vals) else "PHASE VOLTAGE LOW"
            detail = f"{', '.join(sorted(low))} low: {txt}"
            self._line(f"{kind}: {detail}", "event")
            if self.csv:
                self.csv.event(kind, detail)
            self._notify(kind, detail)
        elif not first:
            self._line(f"PHASE VOLTAGES NORMAL: {txt}", "ok")
            if self.csv:
                self.csv.event("PHASE VOLTAGES NORMAL", txt)

    def _notify(self, title, text):
        """Non-blocking pop-up: a modal dialog would stop polling until someone
        clicked it, and an unattended outage would then go unlogged."""
        w = tk.Toplevel(self)
        w.title(title)
        w.attributes("-topmost", True)
        tk.Label(w, text=f"{datetime.datetime.now():%H:%M:%S}\n\n{text}", justify="left",
                 wraplength=420, padx=16, pady=12).pack()
        ttk.Button(w, text="OK", command=w.destroy).pack(pady=(0, 10))

    def _watch(self, it, byte, summary, fields):
        name = it["name"]
        old = self.last_watch.get(name)
        self.last_watch[name] = byte
        if old is None or old == byte:
            return
        dec = self.profile.get("decoders", {}).get(it.get("decode"))
        was = decode(dec, old)[0] if dec else str(old)
        now = summary if summary else str(byte)
        self._line(f"EVENT {it.get('label', name)}: {was}  ->  {now}", "event")
        if self.csv:
            self.csv.event("CHANGE", f"{it.get('label', name)}: {was} -> {now}")

    def _alert(self):
        a = self.profile.get("alert")
        if not a or a["item"] not in self.values:
            return
        dec = self.profile["decoders"].get(
            next(i.get("decode") for i in self.profile["items"] if i["name"] == a["item"]))
        _, fields = decode(dec, self.values[a["item"]][0])
        state = fields.get(a["field"])
        cause = ""
        if a.get("cause_item") in self.values:
            cit = next(i for i in self.profile["items"] if i["name"] == a["cause_item"])
            cause = decode(self.profile["decoders"].get(cit.get("decode")), self.values[a["cause_item"]][0])[0] or ""
        if state == a["normal"]:
            self.alert_lbl.config(text=f"OK: {state}", fg="#2e7d32", bg=self.cget("bg"))
            if self._alerted and self.csv:
                self.csv.event("ALERT CLEARED", f"{state}")
            self._alerted = False
            self.title(f"GENIbus master - unit {self.unit['addr']}")
        else:
            self.alert_lbl.config(text=f"{state}   alarm {cause}", fg="white", bg="#c62828")
            self.title(f"GENIbus master - {state}")
            if not self._alerted:
                self._alerted = True
                if self.csv:
                    self.csv.event("ALERT", f"{state}; alarm {cause}")
                self._notify(self.profile["name"], f"{state}\nAlarm {cause}")

    # ----------------------------------------------------------- commands
    def run_command(self, c):
        if c.get("confirm") and not messagebox.askyesno("Confirm", c["confirm"], icon="warning"):
            return
        if self.csv and c.get("kind") != "read":
            self.csv.event("COMMAND", f"{c['label']} ({c['send']})")
        arm = c.get("arm")
        if arm:
            def armed(reply, extra, arm=arm):
                if reply and arm["expect"] in reply:
                    self.raw(arm["then"], starts(c["send"].split()[0]), timeout=30,
                             cb=lambda r, x: self.poll_once())
                else:
                    self._line(f"{c['label']}: not armed, nothing sent", "err")
            self.raw(c["send"], lambda l: l.startswith('{"' + c["send"].split()[0]) and
                     (arm["expect"] in l or '"err"' in l), timeout=15, cb=armed)
        else:
            key = c.get("reply", c["send"].split()[0])
            self.raw(c["send"], lambda l: l.startswith('{"' + key) and
                     ('"cmd"' in l or '"err"' in l or c.get("kind") == "read"),
                     timeout=30, cb=lambda r, x: self.poll_once())

    def probe(self, cls):
        if not self.bus:
            messagebox.showwarning("Serial", "Not connected.")
            return
        if not self.unit:
            messagebox.showwarning("Probe", "Use a unit first (step 3).")
            return
        self.probe_lbl.config(text=f"probing class {cls}, about 15-30 s...")
        found, values = [], []

        def done(reply, extra):
            for l in [*extra, reply or ""]:
                try:
                    j = json.loads(l)
                except Exception:
                    continue
                if "probe" in j:
                    for k, v in j["probe"].get("known", []):
                        found.append(k)
                        values.append((k, v))
            na = [k for k, v in values if v == 255]
            self.probe_lbl.config(text=f"class {cls}: {len(found)} IDs answer: {ranges(found)}\n"
                                       f"reading 255 (n/a now): {len(na)}")
            self._line(f"probe class {cls}: {len(found)} IDs answer: {ranges(found)}", "ok")
            self._line(f"  with data now: " + ", ".join(f"{k}={v}" for k, v in values if v != 255), "dim")

        self.bus.submit(f"probe {cls} 1 255", lambda l: l.startswith('{"probe"') and
                        (l.rstrip().endswith("]}}") or '"err"' in l) and self._probe_final(l),
                        timeout=60, cb=done)

    @staticmethod
    def _probe_final(l):
        # the firmware splits long lists over several lines; the last one ends
        # the scan of 1..255. Accept any probe line whose list does not reach
        # the buffer limit, i.e. the final one (or an error).
        return '"err"' in l or len(l) < 1900

    # -------------------------------------------------------------- input
    def _typed(self):
        c = self.entry.get().strip()
        if not c:
            return
        if re.match(r"^(cmd|set|setv|mp204 (trip|stop|reset yes))\b", c):
            if not messagebox.askyesno("Confirm", f"Send '{c}'?\n\nThis acts on the unit.", icon="warning"):
                return
        self.history.append(c)
        self.hpos = len(self.history)
        self.entry.delete(0, "end")
        if self.bus:
            key = c.split()[0]
            self.bus.submit(c, lambda l: l.startswith('{"' + key) or l.startswith('{"err"'), timeout=6)
        else:
            messagebox.showwarning("Serial", "Not connected.")

    def _hist(self, d):
        if not self.history:
            return
        self.hpos = max(0, min(len(self.history), self.hpos + d))
        self.entry.delete(0, "end")
        if self.hpos < len(self.history):
            self.entry.insert(0, self.history[self.hpos])

    # --------------------------------------------------------------- pump
    def _pump(self):
        try:
            while True:
                m = self.ui_q.get_nowait()
                if m[0] == "line":
                    self._incoming(m[1], m[2])
                elif m[0] == "tx":
                    self._line(f">> {m[1]}", "tx")
                elif m[0] == "cb":
                    try:
                        m[1](m[2], m[3])
                    except Exception as e:
                        self._line(f"internal error: {e}", "err")
                elif m[0] == "error":
                    self._line(f"serial error: {m[1]} (adapter unplugged?)", "err")
                    if self.bus:
                        self.disconnect()
        except queue.Empty:
            pass
        self.after(50, self._pump)

    def _incoming(self, l, kind):
        try:
            j = json.loads(l)
        except Exception:
            j = None
        tag = "dim"
        if isinstance(j, dict):
            k = next(iter(j), "")
            v = j.get(k)
            if k in ("info", "t") and kind != "reply":
                return  # firmware's own table INFO / poll lines: noise here
            if k == "probe":
                return  # summarised by probe()
            if k == "event":
                tag = "event"
            elif (isinstance(v, dict) and "err" in v and v["err"] != "ok") or k == "err":
                tag = "err"
            elif k in ("unit", "boot", "link", "units", "scan"):
                tag = "info"
            elif kind == "reply":
                tag = "ok"
            if k == "unit":
                self.units[v["addr"]] = v
        self._line(l, tag)

    def _line(self, text, tag=None):
        s = f"{datetime.datetime.now():%H:%M:%S}  {text}"
        self.session.append(s)
        at_end = self.log.yview()[1] > 0.99
        self.log.insert("end", s + "\n", tag or ())
        if at_end:
            self.log.see("end")

    def save(self):
        f = filedialog.asksaveasfilename(defaultextension=".txt",
                                         initialfile=f"genibus_{datetime.datetime.now():%Y%m%d_%H%M%S}.txt")
        if f:
            with open(f, "w") as fh:
                fh.write("\n".join(self.session) + "\n")

    def close(self):
        if self.bus:
            self.disconnect()
        if self.csv:
            self.csv.close()
        self.destroy()


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    app = Master(args[0] if args else "/dev/ttyUSB0")
    app.after(200, app.toggle)
    app.mainloop()
