#!/usr/bin/env python3
# Copyright (c) 2026 WIZnet Co., Ltd.
# SPDX-License-Identifier: Apache-2.0
"""How much does one client's secure handshake delay everyone else?

The board serves all sockets from one cooperative loop, and an asymmetric OPN
(RSA or ECC) is processed synchronously inside it. While that runs, an
established session's Read waits. This tool measures by how much, and
whether a client with default keep-alive settings survives it.

Needs the measurement image (-DOPCUA_HS_MARKERS=ON): its console prints one
"[hs] begin" / "[hs] end" pair per asymmetric OPN and "[hs] priv outside-opn"
for the CreateSession signature, and GP2/GP3 carry the same windows for a
logic analyser.

    python tools/opcua_handshake_impact.py --console COM12 --mode new
           [--a-security none|secure] [--seconds 120] [--read-hz 20]
           [--period 10] [--burst 4] [--lifetime-ms 10000]
           [--pcap capture.pcapng] [--iface "Ethernet"]

Clients
  A          the measured session. Reads Server_ServerStatus_CurrentTime at
             --read-hz and subscribes to the same node (sampling 50 ms,
             publishing 100 ms); CurrentTime changes on every sample, so a
             notification is due every 100 ms and a gap is a stall.
             Its own keep-alive is set to 60 s so it never drops itself.
  W1, W5     observers with asyncua's watchdog at 1.0 s (the library
             default) and 5.0 s. They only exist to show whether a default
             client is dropped, and why: every keep-alive probe is logged with
             its start, duration, timeout and outcome. A dropped observer
             reconnects after 2 s so later windows are still observed.
  disturber  --mode new    a secure client connects, reads once, disconnects,
                           every --period s (asymmetric OPN + CreateSession)
             --mode burst  --burst secure clients connect at once every
                           --period s (admission control refuses some)
             --mode renew  one secure client with --lifetime-ms; asyncua
                           renews at 75 % of it (asymmetric OPN, no session)
             --mode none   baseline, no handshakes

Reported, separately for "inside a handshake window" and "outside":
  Read RTT p50 / p95 / p99 / max, subscription notification gap max,
  observer probe failures and which window each failure overlapped.
With --pcap (written by dumpcap during the run when --iface is given, or an
existing capture): the board's ACK delay (tcp.analysis.ack_rtt on segments the
board sends), retransmissions, zero-window and RST, each split the same way.

Output goes to tools/soak_out/hs_<timestamp>/ (summary.txt, result.json,
console.log, asyncua.log).
"""

import argparse
import asyncio
import bisect
import json
import logging
import os
import re
import shutil
import statistics
import subprocess
import threading
import time

import serial
from asyncua import Client, ua
from asyncua.client.ua_client import UaClientState

from opcua_concurrent_test import APP_URI, CERT, DEFAULT_URL, KEY, POLICY

HERE = os.path.dirname(os.path.abspath(__file__))
NODE_TIME = ua.ObjectIds.Server_ServerStatus_CurrentTime
MARGIN = 0.05            # s added to each side of a window when classifying

HS_BEGIN = re.compile(r"\[hs\] begin n=(\d+) s(\d+) (\w+)")
HS_END = re.compile(r"\[hs\] end\s+n=(\d+) s(\d+) (\w+) opn_us=(\d+) priv_us=(\d+) "
                    r"priv_ops=(\d+) pub_us=(\d+) pub_ops=(\d+) aborted=(\d)")
HS_PRIV = re.compile(r"\[hs\] priv\s+outside-opn us=(\d+) op=(\w+)")


def wall_of(t_perf: float, anchor: tuple) -> float:
    """perf_counter -> wall clock, for lining up with a pcap."""
    return anchor[1] + (t_perf - anchor[0])


def pct(xs, p):
    if not xs:
        return None
    xs = sorted(xs)
    k = min(len(xs) - 1, max(0, int(round(p / 100.0 * (len(xs) - 1)))))
    return xs[k]


def stats_ms(xs):
    if not xs:
        return {"n": 0}
    return {"n": len(xs), "p50": round(pct(xs, 50) * 1e3, 1),
            "p95": round(pct(xs, 95) * 1e3, 1), "p99": round(pct(xs, 99) * 1e3, 1),
            "max": round(max(xs) * 1e3, 1)}


# ------------------------------------------------------------------ windows
class Windows:
    """Handshake windows on the PC's perf_counter clock.

    The end line carries the exact on-board duration, so a window is
    [end_arrival - opn_us, end_arrival]. USB CDC latency shifts both edges by
    the same few ms; MARGIN absorbs it. The begin line is kept as a check.
    """

    def __init__(self):
        self.items = []           # dicts with start, end, n, kind, opn_us, ...
        self.begin_at = {}
        self.priv_outside = []    # (t_end, us, op)
        self._lock = threading.Lock()

    def on_line(self, t, line):
        m = HS_BEGIN.search(line)
        if m:
            self.begin_at[int(m.group(1))] = t
            return
        m = HS_END.search(line)
        if m:
            n, opn_us = int(m.group(1)), int(m.group(4))
            w = {"n": n, "sock": int(m.group(2)), "kind": m.group(3),
                 "opn_us": opn_us, "priv_us": int(m.group(5)),
                 "priv_ops": int(m.group(6)), "pub_us": int(m.group(7)),
                 "pub_ops": int(m.group(8)), "aborted": int(m.group(9)),
                 "end": t, "start": t - opn_us / 1e6,
                 "begin_line_at": self.begin_at.pop(n, None)}
            with self._lock:
                self.items.append(w)
            return
        m = HS_PRIV.search(line)
        if m:
            us = int(m.group(1))
            with self._lock:
                self.priv_outside.append({"start": t - us / 1e6, "end": t,
                                          "us": us, "op": m.group(2)})

    def spans(self, include_priv_outside=True):
        with self._lock:
            s = [(w["start"], w["end"]) for w in self.items]
            if include_priv_outside:
                s += [(p["start"], p["end"]) for p in self.priv_outside]
        return sorted(s)

    def overlaps(self, a, b, spans=None):
        spans = self.spans() if spans is None else spans
        for s, e in spans:
            if a <= e + MARGIN and b >= s - MARGIN:
                return True
        return False


class Console(threading.Thread):
    """Reads the board's USB console; same pattern as opcua_soak_test.Console."""

    def __init__(self, port, outdir, windows):
        super().__init__(daemon=True)
        self.port, self.windows = port, windows
        self.f = open(os.path.join(outdir, "console.log"), "a", encoding="utf-8")
        self.ser = serial.Serial(port, 115200, timeout=0.2)
        self.marker_banner = None
        self._stop = threading.Event()

    def stop(self):
        self._stop.set()
        self.join(timeout=2)
        self.ser.close()
        self.f.close()

    def run(self):
        buf = b""
        while not self._stop.is_set():
            try:
                chunk = self.ser.read(4096)
            except serial.SerialException:
                time.sleep(1.0)
                continue
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").rstrip("\r")
                if not line.strip():
                    continue
                t = time.perf_counter()
                self.f.write(f"{t:.4f} {line}\n")
                self.f.flush()
                if "[hs] markers on" in line:
                    self.marker_banner = line
                self.windows.on_line(t, line)


# ------------------------------------------------- watchdog instrumentation
PROBES = {}       # client label -> list of probe records


def instrument_watchdog():
    """Re-implement asyncua 2.0.1 Client._wait_for_health_signal with timing.

    The body is the library's own (checked against 2.0.1): wait for a state
    change or one watchdog interval, then read ServerState with
    timeout = min(session_timeout / 2, watchdog_intervall). A timeout there is
    what the supervisor reports as a lost connection - the client drops a
    healthy TCP connection by its own decision. Logging each probe shows
    exactly which probe failed and whether it overlapped a handshake.
    """
    import asyncua
    if not asyncua.__version__.startswith("2.0"):
        print(f"warning: asyncua {asyncua.__version__}, probe logging assumes 2.0.x")

    async def wait_for_health_signal(self):
        async with self.uaclient.subscribe_state() as sub:
            try:
                await sub.next_change(self._watchdog_intervall)
            except asyncio.TimeoutError:
                pass
        if self.uaclient.state is UaClientState.CONNECTED:
            probe_timeout = min(self.session_timeout / 1000 / 2, self._watchdog_intervall)
            label = getattr(self, "_hs_label", None)
            t0 = time.perf_counter()
            try:
                await asyncio.wait_for(self.nodes.server_state.read_value(),
                                       timeout=probe_timeout)
            except BaseException as exc:
                if label:
                    PROBES.setdefault(label, []).append(
                        {"start": t0, "dur": time.perf_counter() - t0,
                         "timeout": probe_timeout, "ok": False,
                         "exc": f"{type(exc).__name__}: {exc}"})
                raise
            if label:
                PROBES.setdefault(label, []).append(
                    {"start": t0, "dur": time.perf_counter() - t0,
                     "timeout": probe_timeout, "ok": True})

    Client._wait_for_health_signal = wait_for_health_signal


def make_client(args, secure, label=None, watchdog=60.0, lifetime_ms=None):
    c = Client(url=args.url, timeout=15, watchdog_intervall=watchdog)
    c._hs_label = label
    if secure:
        c.application_uri = APP_URI
    if lifetime_ms:
        c.secure_channel_timeout = lifetime_ms
    return c


async def apply_security(c, secure):
    if secure:
        await c.set_security_string(f"{POLICY},{CERT},{KEY}")


# ------------------------------------------------------------------ clients
class ClientA:
    def __init__(self):
        self.reads = []           # (t_send, t_recv, ok)
        self.notif = []           # arrival times
        self.error = None

    def datachange_notification(self, node, val, data):
        self.notif.append(time.perf_counter())

    async def run(self, args, stop):
        secure = args.a_security == "secure"
        c = make_client(args, secure, label=None, watchdog=60.0)
        await apply_security(c, secure)
        try:
            async with c:
                node = c.get_node(NODE_TIME)
                sub = await c.create_subscription(100, self)
                await sub.subscribe_data_change(node, sampling_interval=50)
                period = 1.0 / args.read_hz
                nxt = time.perf_counter()
                while not stop.is_set():
                    t0 = time.perf_counter()
                    try:
                        await node.read_value()
                        self.reads.append((t0, time.perf_counter(), True))
                    except Exception:                    # noqa: BLE001
                        self.reads.append((t0, time.perf_counter(), False))
                    nxt += period
                    await asyncio.sleep(max(0.0, nxt - time.perf_counter()))
        except Exception as exc:                         # noqa: BLE001
            self.error = f"{type(exc).__name__}: {exc}"


class Observer:
    """A client with a given watchdog interval; counts the times it drops."""

    def __init__(self, label, watchdog):
        self.label, self.watchdog = label, watchdog
        self.losses = []          # (t, exc)
        self.connects = 0

    async def run(self, args, stop):
        secure = args.a_security == "secure"
        while not stop.is_set():
            c = make_client(args, secure, label=self.label, watchdog=self.watchdog)
            await apply_security(c, secure)
            lost = asyncio.Event()

            async def on_lost(exc, _lost=lost):
                self.losses.append((time.perf_counter(), f"{type(exc).__name__}: {exc}"))
                _lost.set()

            c.connection_lost_callback = on_lost
            try:
                await c.connect()
                self.connects += 1
                while not stop.is_set() and not lost.is_set():
                    await asyncio.sleep(0.2)
            except Exception as exc:                     # noqa: BLE001
                self.losses.append((time.perf_counter(), f"connect {type(exc).__name__}: {exc}"))
            finally:
                try:
                    await asyncio.wait_for(c.disconnect(), timeout=3)
                except Exception:                        # noqa: BLE001
                    pass
            if not stop.is_set():
                await asyncio.sleep(2.0)


class Disturber:
    def __init__(self):
        self.attempts = []        # (t_start, t_end, ok, err)

    async def one(self, args):
        c = make_client(args, True)
        await apply_security(c, True)
        t0 = time.perf_counter()
        try:
            await c.connect()
            await c.get_node(NODE_TIME).read_value()
            self.attempts.append((t0, time.perf_counter(), True, None))
        except Exception as exc:                         # noqa: BLE001
            self.attempts.append((t0, time.perf_counter(), False,
                                  f"{type(exc).__name__}: {exc}"))
        finally:
            try:
                await asyncio.wait_for(c.disconnect(), timeout=3)
            except Exception:                            # noqa: BLE001
                pass

    async def run(self, args, stop):
        await asyncio.sleep(args.warmup)
        if args.mode == "none":
            return
        if args.mode == "renew":
            c = make_client(args, True, lifetime_ms=args.lifetime_ms)
            await apply_security(c, True)
            async with c:
                while not stop.is_set():
                    await asyncio.sleep(0.5)
            return
        while not stop.is_set():
            if args.mode == "new":
                await self.one(args)
            else:
                await asyncio.gather(*(self.one(args) for _ in range(args.burst)))
            for _ in range(int(args.period * 10)):
                if stop.is_set():
                    break
                await asyncio.sleep(0.1)


# --------------------------------------------------------------- analysis
def analyse_clients(win, a, observers):
    spans = win.spans()
    rin, rout, fails = [], [], 0
    for t0, t1, ok in a.reads:
        if not ok:
            fails += 1
            continue
        (rin if win.overlaps(t0, t1, spans) else rout).append(t1 - t0)

    gin, gout = [], []
    for p, q in zip(a.notif, a.notif[1:]):
        (gin if win.overlaps(p, q, spans) else gout).append(q - p)

    obs = {}
    for o in observers:
        probes = PROBES.get(o.label, [])
        failed = [p for p in probes if not p["ok"]]
        obs[o.label] = {
            "watchdog_s": o.watchdog, "connects": o.connects,
            "losses": len(o.losses),
            "probes": len(probes),
            "probe_dur_ms": stats_ms([p["dur"] for p in probes if p["ok"]]),
            "failed_probes": [{
                "dur_ms": round(p["dur"] * 1e3, 1), "timeout_s": p["timeout"],
                "exc": p["exc"],
                "in_handshake": win.overlaps(p["start"], p["start"] + p["dur"], spans),
            } for p in failed],
            "loss_reasons": [e for _, e in o.losses],
        }
    return {"read_rtt_in": stats_ms(rin), "read_rtt_out": stats_ms(rout),
            "read_failures": fails,
            "notif_gap_in": stats_ms(gin), "notif_gap_out": stats_ms(gout),
            "observers": obs}


def run_tshark(pcap, fields, display):
    exe = shutil.which("tshark") or r"C:\Program Files\Wireshark\tshark.exe"
    cmd = [exe, "-r", pcap, "-Y", display, "-T", "fields", "-E", "separator=\t"]
    for f in fields:
        cmd += ["-e", f]
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    rows = []
    for line in out.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) == len(fields):
            rows.append(parts)
    return rows


def analyse_pcap(pcap, board_ip, win, anchor):
    """Board ACK delay, retransmissions, zero window, RST - inside/outside windows.

    Windows are converted to wall-clock time (dumpcap's timestamps) through the
    anchor taken at start. PC clock only, no cross-host sync is needed.
    """
    spans = [(wall_of(s, anchor), wall_of(e, anchor)) for s, e in win.spans()]

    def inside(t):
        i = bisect.bisect_right([s for s, _ in spans], t + MARGIN) - 1
        return i >= 0 and t <= spans[i][1] + MARGIN

    base = f"tcp.port==4840"
    res = {}
    # ACK delay: how long the board took to acknowledge client data.
    rows = run_tshark(pcap, ["frame.time_epoch", "tcp.analysis.ack_rtt"],
                      f"{base} && ip.src=={board_ip} && tcp.analysis.ack_rtt")
    ai, ao = [], []
    for t, rtt in rows:
        (ai if inside(float(t)) else ao).append(float(rtt))
    res["board_ack_delay_in"] = stats_ms(ai)
    res["board_ack_delay_out"] = stats_ms(ao)

    for key, flt in (("retransmission", "tcp.analysis.retransmission"),
                     ("fast_retransmission", "tcp.analysis.fast_retransmission"),
                     ("zero_window", "tcp.analysis.zero_window"),
                     ("rst", "tcp.flags.reset==1")):
        rows = run_tshark(pcap, ["frame.time_epoch", "ip.src"], f"{base} && {flt}")
        cnt = {"board_in": 0, "board_out": 0, "pc_in": 0, "pc_out": 0}
        for t, src in rows:
            who = "board" if src == board_ip else "pc"
            cnt[f"{who}_{'in' if inside(float(t)) else 'out'}"] += 1
        res[key] = cnt
    return res


def fmt(s):
    if s.get("n", 0) == 0:
        return "n=0"
    return f"n={s['n']} p50={s['p50']} p95={s['p95']} p99={s['p99']} max={s['max']} ms"


# ------------------------------------------------------------------- main
def check_sockets(args):
    """The server listens on --server-sockets hardware sockets and a 5th client
    is refused. A, every observer and every disturber client each hold one;
    asyncua's GetEndpoints connection comes and goes before the secure one, so
    a disturber needs one socket at a time."""
    need = 1 + len(args.observers)
    need += {"none": 0, "new": 1, "renew": 1, "burst": args.burst}[args.mode]
    if need > args.server_sockets:
        raise SystemExit(
            f"needs {need} sockets (A + {len(args.observers)} observers + "
            f"disturber) but the server has {args.server_sockets}. "
            f"Use --observers with fewer values (e.g. --observers 1) or a "
            f"smaller --burst.")


async def main(args):
    check_sockets(args)
    outdir = os.path.join(HERE, "soak_out", time.strftime("hs_%Y%m%d_%H%M%S"))
    os.makedirs(outdir, exist_ok=True)
    logging.basicConfig(filename=os.path.join(outdir, "asyncua.log"), level=logging.INFO,
                        format="%(asctime)s.%(msecs)03d %(name)s %(levelname)s %(message)s",
                        datefmt="%H:%M:%S")
    instrument_watchdog()

    win = Windows()
    con = Console(args.console, outdir, win)
    con.start()

    dump = None
    pcap = args.pcap
    if args.iface:
        pcap = pcap or os.path.join(outdir, "capture.pcapng")
        exe = shutil.which("dumpcap") or r"C:\Program Files\Wireshark\dumpcap.exe"
        dump = subprocess.Popen([exe, "-i", args.iface, "-f", "tcp port 4840",
                                 "-w", pcap, "-q"])
        await asyncio.sleep(2.0)

    anchor = (time.perf_counter(), time.time())
    stop = asyncio.Event()
    a = ClientA()
    observers = [Observer(f"W{w:g}", w) for w in args.observers]
    dist = Disturber()
    tasks = [asyncio.create_task(a.run(args, stop))]
    tasks += [asyncio.create_task(o.run(args, stop)) for o in observers]
    tasks.append(asyncio.create_task(dist.run(args, stop)))

    await asyncio.sleep(args.seconds)
    stop.set()
    await asyncio.wait(tasks, timeout=30)
    await asyncio.sleep(1.0)           # last console lines
    con.stop()
    if dump:
        dump.terminate()
        dump.wait(timeout=10)

    result = {
        "args": vars(args),
        "marker_banner": con.marker_banner,
        "windows": win.items,
        "priv_outside_opn": win.priv_outside,
        "disturber": [{"dur_ms": round((e - s) * 1e3, 1), "ok": ok, "err": err}
                      for s, e, ok, err in dist.attempts],
        "client_a_error": a.error,
    }
    result.update(analyse_clients(win, a, observers))
    if pcap and os.path.exists(pcap):
        result["pcap"] = analyse_pcap(pcap, args.board_ip, win, anchor)

    with open(os.path.join(outdir, "result.json"), "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2, default=str)

    opn = [w["opn_us"] / 1e3 for w in win.items if not w["aborted"]]
    priv = [w["priv_us"] / 1e3 for w in win.items if not w["aborted"]]
    lines = [
        f"mode={args.mode} A={args.a_security} seconds={args.seconds} "
        f"read_hz={args.read_hz} period={args.period}",
        f"image: {con.marker_banner or 'NO MARKER BANNER SEEN - wrong image? windows empty'}",
        f"handshake windows: {len(win.items)}  "
        f"opn_ms p50={pct(opn, 50)} max={max(opn) if opn else None}  "
        f"priv_ms p50={pct(priv, 50)}  "
        f"priv outside OPN (CreateSession): {len(win.priv_outside)}",
        f"disturber: {sum(1 for x in dist.attempts if x[2])} ok / "
        f"{len(dist.attempts)} attempts",
        f"A read RTT  inside : {fmt(result['read_rtt_in'])}",
        f"A read RTT  outside: {fmt(result['read_rtt_out'])}   failures={result['read_failures']}",
        f"A notif gap inside : {fmt(result['notif_gap_in'])}",
        f"A notif gap outside: {fmt(result['notif_gap_out'])}",
    ]
    for label, o in result["observers"].items():
        inh = sum(1 for p in o["failed_probes"] if p["in_handshake"])
        lines.append(f"{label} (watchdog {o['watchdog_s']} s): connects={o['connects']} "
                     f"losses={o['losses']} failed probes={len(o['failed_probes'])} "
                     f"({inh} inside a handshake)")
        for r in o["loss_reasons"][:5]:
            lines.append(f"    loss: {r}")
    if "pcap" in result:
        p = result["pcap"]
        lines.append(f"board ACK delay inside : {fmt(p['board_ack_delay_in'])}")
        lines.append(f"board ACK delay outside: {fmt(p['board_ack_delay_out'])}")
        for k in ("retransmission", "fast_retransmission", "zero_window", "rst"):
            lines.append(f"{k}: {p[k]}")
    if a.error:
        lines.append(f"client A error: {a.error}")
    text = "\n".join(lines)
    with open(os.path.join(outdir, "summary.txt"), "w", encoding="utf-8") as f:
        f.write(text + "\n")
    print(text)
    print(f"\noutput: {outdir}")
    return 0 if con.marker_banner else 2


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--console", required=True, help="board USB console port")
    ap.add_argument("--url", default=DEFAULT_URL)
    ap.add_argument("--board-ip", default="192.168.11.2")
    ap.add_argument("--mode", choices=("none", "new", "burst", "renew"), default="new")
    ap.add_argument("--a-security", choices=("none", "secure"), default="secure",
                    help="security of the measured session A and the observers")
    ap.add_argument("--seconds", type=float, default=120.0)
    ap.add_argument("--warmup", type=float, default=10.0,
                    help="seconds of undisturbed baseline before the first handshake")
    ap.add_argument("--read-hz", type=float, default=20.0)
    ap.add_argument("--period", type=float, default=10.0)
    ap.add_argument("--burst", type=int, default=2,
                    help="clients per burst; A + observers + burst <= --server-sockets")
    ap.add_argument("--observers", type=float, nargs="*", default=[1.0, 5.0],
                    help="watchdog intervals of observer clients (default 1.0 5.0); "
                         "an observer that drops reconnects, and that reconnect is "
                         "itself a handshake window")
    ap.add_argument("--server-sockets", type=int, default=4)
    ap.add_argument("--lifetime-ms", type=int, default=10000)
    ap.add_argument("--pcap", help="existing capture to analyse, or output path with --iface")
    ap.add_argument("--iface", help="capture with dumpcap on this interface during the run")
    raise SystemExit(asyncio.run(main(ap.parse_args())))
