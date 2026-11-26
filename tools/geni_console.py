#!/usr/bin/env python3
"""GENIbus master console: serial log, commands and MP 204 status in one window.

    python3 tools/geni_console.py [port]          (default /dev/ttyUSB0)

Needs pyserial and tkinter (both in ~/.platformio/penv/bin/python).
Only one program can use the port: close the VS Code serial monitor first.

Left: buttons for the firmware's console commands. Trip, stop and reset ask
for confirmation first; reset uses the firmware's two-step arm/confirm.
Right: MP 204 state and latest readings, from `mp204` replies and poll lines.
Bottom: every line from the board, colour coded, with a command box
(Up/Down for history). "Save log" writes the whole session to a text file.
"""
import datetime
import json
import queue
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import serial
import serial.tools.list_ports

BAUD = 115200
ALARMS = {0: "none", 2: "missing phase", 3: "PTC / thermal switch", 4: "too many auto restarts",
          9: "phase sequence", 15: "communication", 18: "commanded trip",
          20: "insulation resistance low", 32: "overvoltage", 40: "undervoltage",
          48: "OVERLOAD (current)", 56: "UNDERLOAD (current)", 64: "overtemperature (Tempcon)",
          71: "overtemperature (Pt100/1000)", 111: "CURRENT ASYMMETRY",
          112: "cos phi max", 113: "cos phi min", 120: "aux winding",
          123: "start capacitor", 124: "run capacitor"}
MODES = {"started": "Started (running allowed)", "tripped1_protection": "TRIPPED - protection",
         "tripped2_test": "TRIPPED - test / commanded", "tripped3_stop": "STOPPED - commanded (no alarm)"}
READINGS = [("v_line", "Line voltage"), ("i1", "I1"), ("i2", "I2"), ("i3", "I3"),
            ("i_line", "Mean current"), ("i_asym", "Asymmetry"), ("p", "Power"),
            ("f_line", "Frequency"), ("cos_phi", "cos phi"), ("t_run", "Run time"),
            ("start_cnt", "Starts")]


def alarm_text(code):
    return ALARMS.get(code, f"unknown ({code})")


class Console(tk.Tk):
    def __init__(self, port):
        super().__init__()
        self.title("GENIbus console")
        self.geometry("1100x720")
        self.ser = None
        self.rx = queue.Queue()
        self.reader = None
        self.stop_reader = threading.Event()
        self.history, self.hpos = [], 0
        self.session = []
        self.pending_reset = False
        self.hide = {"mp204": 0, "t": 0, "stats": 0}  # replies to auto-refresh: update panel, don't log
        self._build(port)
        self.after(50, self._pump)
        self.after(5000, self._auto_status)

    # ---------------------------------------------------------------- UI
    def _build(self, port):
        top = ttk.Frame(self, padding=6)
        top.pack(fill="x")
        ttk.Label(top, text="Port").pack(side="left")
        ports = [p.device for p in serial.tools.list_ports.comports()] or [port]
        self.port = tk.StringVar(value=port)
        ttk.Combobox(top, textvariable=self.port, values=ports, width=18).pack(side="left", padx=4)
        self.conn_btn = ttk.Button(top, text="Connect", command=self.toggle)
        self.conn_btn.pack(side="left")
        self.conn_lbl = ttk.Label(top, text="disconnected", foreground="grey")
        self.conn_lbl.pack(side="left", padx=8)
        self.auto = tk.BooleanVar(value=True)
        ttk.Checkbutton(top, text="Refresh MP 204 status every 5 s", variable=self.auto).pack(side="left", padx=12)
        ttk.Button(top, text="Save log", command=self.save).pack(side="right")
        ttk.Button(top, text="Clear", command=lambda: self.log.delete("1.0", "end")).pack(side="right", padx=4)

        mid = ttk.Frame(self, padding=(6, 0))
        mid.pack(fill="x")

        btns = ttk.LabelFrame(mid, text="Commands", padding=6)
        btns.pack(side="left", fill="y")
        read_only = [("Status (mp204)", "mp204"), ("Latest readings (show)", "show"),
                     ("Counters (stats)", "stats"), ("Units", "units"),
                     ("Identify (get 2 148 2)", "get 2 148 2"), ("Scan bus", "scan"),
                     ("Live lines ON", "print on"), ("Live lines OFF", "print off"),
                     ("Help", "help")]
        for i, (label, cmd) in enumerate(read_only):
            ttk.Button(btns, text=label, width=22, command=lambda c=cmd: self.send(c)).grid(row=i, column=0, pady=1)
        ttk.Separator(btns).grid(row=len(read_only), column=0, sticky="ew", pady=6)
        r = len(read_only) + 1
        tk.Button(btns, text="TRIP (like Test button)", width=22, bg="#f0c36d",
                  command=lambda: self.confirm("mp204 trip", "Trip the MP 204?\n\nThe trip relay opens; the motor stops / cannot start.")).grid(row=r, column=0, pady=1)
        tk.Button(btns, text="STOP (no alarm)", width=22, bg="#f0c36d",
                  command=lambda: self.confirm("mp204 stop", "Stop the motor (TRIP_A)?\n\nThe trip relay opens without an alarm.")).grid(row=r + 1, column=0, pady=1)
        tk.Button(btns, text="RESET - may START motor", width=22, bg="#e57373",
                  command=self.reset).grid(row=r + 2, column=0, pady=1)

        # Readings get their own frame on the right, packed first so the
        # state panel's long texts can never push them off the window.
        rd = ttk.LabelFrame(mid, text="Readings", padding=8)
        rd.pack(side="right", fill="y")
        stat = ttk.LabelFrame(mid, text="MP 204", padding=8)
        stat.pack(side="left", fill="both", expand=True, padx=6)
        self.v = {}
        rows = [("link", "Link"), ("mode", "Mode"), ("alarm", "Active alarm"), ("relay1", "Trip relay 95-96"),
                ("relay2", "Signal relay 97-98"), ("motor", "Motor"), ("protection", "Protection"),
                ("pending", "Pending alarm"), ("log", "Alarm log (newest first)"), ("bus", "Bus counters")]
        for i, (k, label) in enumerate(rows):
            ttk.Label(stat, text=label + ":").grid(row=i, column=0, sticky="nw")
            self.v[k] = tk.Label(stat, text="-", anchor="w", justify="left", wraplength=420,
                                 font=("TkDefaultFont", 10, "bold"))
            self.v[k].grid(row=i, column=1, sticky="w", padx=6)
        for i, (k, label) in enumerate(READINGS):
            ttk.Label(rd, text=label + ":").grid(row=i, column=0, sticky="w")
            self.v[k] = tk.Label(rd, text="-", anchor="w", width=12, font=("TkDefaultFont", 10, "bold"))
            self.v[k].grid(row=i, column=1, sticky="w", padx=6)
        self.v["age"] = ttk.Label(rd, text="", foreground="grey", wraplength=200)
        self.v["age"].grid(row=len(READINGS), column=0, columnspan=2, sticky="w", pady=(4, 0))

        logf = ttk.Frame(self, padding=6)
        logf.pack(fill="both", expand=True)
        self.log = tk.Text(logf, wrap="none", font=("Monospace", 9), bg="#1e1e1e", fg="#d4d4d4",
                           insertbackground="white")
        ys = ttk.Scrollbar(logf, command=self.log.yview)
        xs = ttk.Scrollbar(logf, orient="horizontal", command=self.log.xview)
        self.log.configure(yscrollcommand=ys.set, xscrollcommand=xs.set)
        ys.pack(side="right", fill="y")
        xs.pack(side="bottom", fill="x")
        self.log.pack(fill="both", expand=True)
        for tag, col in [("tx", "#569cd6"), ("err", "#f44747"), ("ok", "#6a9955"), ("trip", "#ffcc00"),
                         ("poll", "#808080"), ("info", "#c586c0")]:
            self.log.tag_configure(tag, foreground=col)

        bot = ttk.Frame(self, padding=6)
        bot.pack(fill="x")
        ttk.Label(bot, text="Command:").pack(side="left")
        self.entry = ttk.Entry(bot)
        self.entry.pack(side="left", fill="x", expand=True, padx=4)
        self.entry.bind("<Return>", lambda e: self._typed())
        self.entry.bind("<Up>", lambda e: self._hist(-1))
        self.entry.bind("<Down>", lambda e: self._hist(1))
        ttk.Button(bot, text="Send", command=self._typed).pack(side="left")
        self.protocol("WM_DELETE_WINDOW", self.close)

    # ------------------------------------------------------------ serial
    def toggle(self):
        if self.ser:
            self.disconnect()
            return
        try:
            self.ser = serial.Serial(self.port.get(), BAUD, timeout=0.1)
        except Exception as e:
            messagebox.showerror("Serial", f"Cannot open {self.port.get()}:\n{e}\n\n"
                                 "Close the VS Code serial monitor (only one program can use the port).")
            self.ser = None
            return
        self.stop_reader = threading.Event()  # fresh per connection; old readers keep theirs
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self.conn_btn.config(text="Disconnect")
        self.conn_lbl.config(text=f"connected {self.port.get()} @ {BAUD}", foreground="green")
        self._line(f"--- connected {self.port.get()} ---", "info")
        self.after(300, lambda: self.send("mp204", quiet=True))
        self.after(600, lambda: self.send("stats", quiet=True))

    def disconnect(self):
        self.stop_reader.set()
        try:
            self.ser.close()
        except Exception:
            pass
        self.ser = None
        self.conn_btn.config(text="Connect")
        self.conn_lbl.config(text="disconnected", foreground="grey")
        self._line("--- disconnected ---", "info")

    def _read(self):
        buf = b""
        port, stop = self.ser, self.stop_reader  # own references: disconnect() clears self.ser
        while not stop.is_set():
            try:
                buf += port.read(4096)
            except Exception as e:
                if not stop.is_set():  # a deliberate disconnect is not an error
                    self.rx.put(("__error__", str(e)))
                return
            *lines, buf = buf.split(b"\n")
            for l in lines:
                l = l.strip().decode("ascii", "replace")
                if l:
                    self.rx.put(("line", l))

    def send(self, cmd, quiet=False):
        if not self.ser:
            if not quiet:
                messagebox.showwarning("Serial", "Not connected.")
            return
        try:
            self.ser.write(cmd.encode() + b"\n")
        except Exception as e:
            self._line(f"write failed: {e}", "err")
            return
        if quiet:
            key = {"mp204": "mp204", "show": "t", "stats": "stats"}.get(cmd)
            if key:
                self.hide[key] += 1
        else:
            self._line(f">> {cmd}", "tx")

    def confirm(self, cmd, text):
        if messagebox.askyesno("Confirm", text, icon="warning"):
            self.send(cmd)

    def reset(self):
        if not messagebox.askyesno(
                "Confirm reset",
                "Reset the MP 204?\n\nThis clears the trip and CLOSES the trip relay:\n"
                "THE MOTOR MAY START IMMEDIATELY.\n\nOnly continue if the pump is safe to run.",
                icon="warning"):
            return
        self.pending_reset = True
        self.send("mp204 reset")  # firmware arms for 15 s; confirmed when the armed line arrives

    # -------------------------------------------------------------- input
    def _typed(self):
        c = self.entry.get().strip()
        if not c:
            return
        if c.startswith("mp204 reset yes") or c in ("mp204 trip", "mp204 stop"):
            if not messagebox.askyesno("Confirm", f"Send '{c}'? This acts on the MP 204.", icon="warning"):
                return
        self.history.append(c)
        self.hpos = len(self.history)
        self.entry.delete(0, "end")
        self.send(c)

    def _hist(self, d):
        if not self.history:
            return
        self.hpos = max(0, min(len(self.history), self.hpos + d))
        self.entry.delete(0, "end")
        if self.hpos < len(self.history):
            self.entry.insert(0, self.history[self.hpos])

    # ------------------------------------------------------------ parsing
    def _pump(self):
        try:
            while True:
                kind, l = self.rx.get_nowait()
                if kind == "__error__":
                    self._line(f"serial error: {l} (adapter unplugged?)", "err")
                    if self.ser:
                        self.disconnect()
                    break
                self._handle(l)
        except queue.Empty:
            pass
        self.after(50, self._pump)

    def _handle(self, l):
        tag = None
        try:
            j = json.loads(l)
        except Exception:
            j = None
        if isinstance(j, dict):
            key = next(iter(j), None)
            hidden = key in self.hide and self.hide[key] > 0 and not (
                key == "mp204" and ("cmd" in j["mp204"] or "armed_s" in j["mp204"]))
            if hidden:
                self.hide[key] -= 1
            if "t" in j and "items" in j:
                tag = "poll" if j.get("ok") else "err"
                self._readings(j)
            elif "mp204" in j:
                m = j["mp204"]
                if "armed_s" in m and self.pending_reset:
                    self.pending_reset = False
                    self._line(l, "trip")
                    self.send("mp204 reset yes")
                    return
                if "state" in m:
                    self._state(m)
                if "after" in m:
                    self._state(m["after"] if "mode" in m["after"] else m)
                tag = "err" if ("err" in m or m.get("result") not in (None, "ok")) else (
                    "trip" if "cmd" in m else "ok")
            elif "event" in j:
                ev = j["event"]
                self._state(ev)
                was = ev.get("was")
                if was and ev.get("mode") != "started" and was.get("mode") == "started":
                    self._line(l, "trip")
                    messagebox.showwarning(
                        "MP 204 tripped",
                        f"{MODES.get(ev['mode'], ev['mode'])}\nAlarm {ev.get('alarm')}: "
                        f"{alarm_text(ev.get('alarm', 0))}")
                    return
                tag = "trip"
            elif "link" in j:
                up = j["link"].get("up")
                self._set("link", "UP" if up else f"DOWN ({j['link'].get('err')})", "green" if up else "red")
                tag = "ok" if up else "err"
            elif "stats" in j:
                s = j["stats"]
                bad = sum(v for k, v in s.items() if k not in ("requests", "ok", "timeout"))
                self._set("bus", f"{s['ok']}/{s['requests']} ok, {s['timeout']} timeout, {bad} errors",
                          "black" if bad == 0 else "red")
                tag = "info"
            elif "err" in j or any(isinstance(v, dict) and "err" in v for v in j.values()):
                tag = "err"
            elif "unit" in j or "boot" in j:
                tag = "info"
        if isinstance(j, dict) and hidden and tag != "err":
            return
        self._line(l, tag)

    def _state(self, m):
        st = m.get("state", m)
        if "mode" not in st:
            return
        mode = st["mode"]
        self._set("mode", MODES.get(mode, mode), "green" if mode == "started" else "red")
        code = st.get("alarm", 0)
        self._set("alarm", f"{code}: {alarm_text(code)}", "black" if code == 0 else "red")
        self._set("relay1", "closed (run allowed)" if st.get("relay1_closed") else "OPEN (tripped)",
                  "green" if st.get("relay1_closed") else "red")
        self._set("relay2", "closed (alarm signal)" if st.get("relay2_closed") else "open")
        self._set("motor", "off" if st.get("motor_off") else "RUNNING")
        self._set("pending", "yes" if st.get("pending_alarm") else "no")
        if "protection" in m:
            self._set("protection", "on" if m["protection"] else "DISABLED", "black" if m["protection"] else "red")
        if "log" in m:
            self._set("log", ", ".join(alarm_text(c) for c in m["log"]))
        if mode != "started":
            self.title(f"GENIbus console - {MODES.get(mode, mode)}: {alarm_text(code)}")
        else:
            self.title("GENIbus console")

    def _readings(self, j):
        it = j.get("items", {})
        for k, _ in READINGS:
            i = it.get(k)
            if isinstance(i, dict) and "v" in i:
                self.v[k].config(text=f"{i['v']} {i.get('u', '')}")
            elif isinstance(i, dict) and "raw" in i:
                self.v[k].config(text=str(i["raw"]))
            elif j.get("ok"):
                self.v[k].config(text="n/a")
        self._set("link", "UP" if j.get("ok") else f"DOWN ({j.get('err')})",
                  "green" if j.get("ok") else "red")
        a = it.get("alarm_code")
        if isinstance(a, dict) and "raw" in a:
            self._set("alarm", f"{a['raw']}: {alarm_text(a['raw'])}", "black" if a["raw"] == 0 else "red")
        self.v["age"].config(text=f"readings at board time {j.get('t', 0) / 1000:.0f} s, "
                                  f"{'ok' if j.get('ok') else j.get('err')}")

    def _set(self, k, text, colour="black"):
        self.v[k].config(text=text, fg=colour)

    def _line(self, text, tag=None):
        stamp = datetime.datetime.now().strftime("%H:%M:%S")
        s = f"{stamp}  {text}"
        self.session.append(s)
        at_end = self.log.yview()[1] > 0.99
        self.log.insert("end", s + "\n", tag or ())
        if at_end:
            self.log.see("end")

    def _auto_status(self):
        if self.ser and self.auto.get() and not self.pending_reset:
            self.send("mp204", quiet=True)
            self.after(400, lambda: self.send("show", quiet=True))
        self.after(5000, self._auto_status)

    def save(self):
        f = filedialog.asksaveasfilename(defaultextension=".txt",
                                         initialfile=datetime.datetime.now().strftime("geni_%Y%m%d_%H%M%S.txt"))
        if f:
            with open(f, "w") as fh:
                fh.write("\n".join(self.session) + "\n")

    def close(self):
        if self.ser:
            self.disconnect()
        self.destroy()


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    app = Console(args[0] if args else "/dev/ttyUSB0")
    app.after(200, app.toggle)  # connect straight away
    app.mainloop()
