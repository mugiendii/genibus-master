#!/usr/bin/env python3
"""Log the GENIbus master console to spreadsheet files, and type commands.

    ~/.platformio/penv/bin/python tools/bench_log.py [port]

Writes logs/<date-time>/ :
  readings.csv  one row per poll, one column per item (blank = no data)
  events.csv    trips, restarts, alarm/warning changes, commands and replies
  raw.txt       every line exactly as received
Rows are flushed as they arrive. Ctrl-C or 'quit' ends.
"""
import csv, datetime, json, os, queue, re, sys, threading, time

MODES = {0: "started", 1: "tripped1_protection", 2: "tripped2_test", 3: "tripped3_stop"}
ALARMS = {0: "none", 2: "missing_phase", 3: "ptc_external", 4: "too_many_auto_restarts",
          9: "phase_sequence", 15: "communication", 18: "commanded_trip",
          20: "insulation_low", 32: "overvoltage", 40: "undervoltage", 48: "overload",
          56: "underload", 64: "overtemp_tempcon", 71: "overtemp_pt",
          111: "current_asymmetry", 112: "cos_phi_max", 113: "cos_phi_min",
          120: "aux_winding", 123: "start_capacitor", 124: "run_capacitor"}
WATCH = ("mode", "relay1_closed", "motor_off", "alarm_code",
         "warnings_1", "warnings_2", "warnings_3")
CAUSE = {"mp204 stop": MODES[3], "mp204 trip": MODES[2], "mp204 reset yes": MODES[0]}
EXTRA = ["mode", "relay1_closed", "motor_off", "alarm_text"]


def item_names():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../src/poll_table.c")
    return re.findall(r'^\s*\{\s*"(\w+)"\s*,', open(path).read(), re.M)


def flatten(items):
    """items of one poll -> {name: value}; missing data stays out."""
    out, units = {}, {}
    for k, i in items.items():
        if not isinstance(i, dict):
            continue
        if "v" in i and i["v"] is not None:
            out[k], units[k] = i["v"], i.get("u", "")
        elif "raw" in i:
            out[k] = i["raw"]
    m = out.get("act_mode1")
    if m is not None and m != 255:
        m = int(m)
        out["mode"] = MODES.get(m & 7, "unknown")
        out["motor_off"], out["relay1_closed"] = m >> 3 & 1, m >> 4 & 1
    if "alarm_code" in out:
        out["alarm_text"] = ALARMS.get(int(out["alarm_code"]), "unknown")
    return out, units


class Log:
    def __init__(self, directory, names):
        os.makedirs(directory)
        self.names = names
        self.prev = {}
        self.ok = None
        self.polls = 0
        self.cmd_t, self.cmd = -1e9, ""
        self.rf = open(os.path.join(directory, "readings.csv"), "w", newline="")
        self.ef = open(os.path.join(directory, "events.csv"), "w", newline="")
        self.raw = open(os.path.join(directory, "raw.txt"), "w")
        self.r, self.e = csv.writer(self.rf), csv.writer(self.ef)
        self.units = None
        self.r.writerow(["time", "board_ms", "ok", "error", *EXTRA, *names])
        self.e.writerow(["time", "event", "from", "to", "detail"])
        self.rf.flush(); self.ef.flush()

    @staticmethod
    def now():
        return datetime.datetime.now().isoformat(" ", "milliseconds")

    def event(self, what, a="", b="", detail=""):
        self.e.writerow([self.now(), what, a, b, detail])
        self.ef.flush()
        print(f"* {what} {a} -> {b} {detail}" if a != "" or b != "" else f"* {what} {detail}")

    def command(self, text):
        self.cmd_t, self.cmd = time.monotonic(), text
        self.event("command", detail=text)

    def line(self, text):
        self.raw.write(f"{self.now()} {text}\n")
        self.raw.flush()
        try:
            d = json.loads(text)
        except ValueError:
            d = None
        if not isinstance(d, dict):
            print(text)
        elif "items" in d and "ok" in d:
            self.poll(d)
        elif "boot" in d:
            self.prev = {}
            self.ok = None
            self.event("board_boot", detail=text)
        elif "unit" in d or "info" in d or "scan" in d:
            print(text)
        else:
            self.event("reply", detail=text)

    def poll(self, d):
        v, units = flatten(d["items"])
        self.polls += 1
        if units and self.units is None:
            self.units = units
            self.r.writerow(["", "", "", "", *[""] * len(EXTRA),
                             *[units.get(n, "") for n in self.names]])
        self.r.writerow([self.now(), d.get("t", ""), d["ok"], d.get("err", ""),
                         *[v.get(n, "") for n in EXTRA + self.names]])
        self.rf.flush()
        if self.ok is not None and self.ok != d["ok"]:
            self.event("bus", "ok" if self.ok else "lost", "ok" if d["ok"] else "lost",
                       d.get("err", ""))
        self.ok = d["ok"]
        for k in WATCH:
            if k not in v:
                continue
            old, new = self.prev.get(k), v[k]
            self.prev[k] = new
            if old == new:
                continue
            detail = v.get("alarm_text", "") if k in ("mode", "alarm_code") else ""
            if old is None:
                self.event(f"initial_{k}", "", new, detail)
                continue
            if k == "mode" and time.monotonic() - self.cmd_t < 5 and CAUSE.get(self.cmd) == new:
                detail += f" (follows our command: {self.cmd})"
                self.cmd = ""  # explains one change only
            self.event(k, old, new, detail.strip())

    def close(self):
        for f in (self.rf, self.ef, self.raw):
            f.close()


def pick_port():
    from serial.tools import list_ports
    ports = [p.device for p in list_ports.comports() if p.vid is not None]
    if len(ports) != 1:
        sys.exit("USB serial ports found: " + (", ".join(ports) or "none") +
                 ". Give one: bench_log.py <port>")
    return ports[0]


def main():
    import serial
    port = sys.argv[1] if len(sys.argv) > 1 else pick_port()
    try:
        s = serial.Serial(port, 115200, timeout=0.2)
    except serial.SerialException as e:
        sys.exit(f"cannot open {port}: {e}")
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs")
    directory = os.path.normpath(
        os.path.join(root, datetime.datetime.now().strftime("%Y%m%d-%H%M%S")))
    log = Log(directory, item_names())
    print(f"logging {port} to {directory}\ntype commands here (help, mp204, ...); quit ends")
    typed = queue.Queue()

    def keys():
        for t in sys.stdin:
            typed.put(t.strip())
    threading.Thread(target=keys, daemon=True).start()
    buf = b""
    try:
        while True:
            while not typed.empty():
                t = typed.get()
                if t == "quit":
                    return
                if t:
                    log.command(t)
                    s.write(t.encode("ascii", "ignore") + b"\n")
            buf += s.read(s.in_waiting or 1)
            while b"\n" in buf:
                one, buf = buf.split(b"\n", 1)
                text = one.decode("ascii", "replace").strip()
                if text:
                    log.line(text)
    except KeyboardInterrupt:
        pass
    except (serial.SerialException, OSError) as e:
        log.event("serial_port_lost", detail=str(e))
    finally:
        log.close()
        print(f"\n{log.polls} polls saved in {directory}")


if __name__ == "__main__":
    main()
