#!/usr/bin/env python3
"""
A Prometheus exposition endpoint for testing the panel against.

Serves live-ish data -- counters that actually climb, a gauge that wanders, a
histogram whose buckets fill -- so rate() and histogram_quantile() have
something real to chew on. It also injects the failure modes the panel is
supposed to survive, which is the part a real exporter will not do on demand:

    /metrics            normal
    /metrics?slow=5     stall 5s before responding (timeout handling)
    /metrics?chunked=1  no Content-Length, chunked transfer
    /metrics?html=1     an HTML page (wrong URL -> "no metrics found")
    /metrics?truncate=1 cut the body off mid-line
    /metrics?huge=1     ~4MB body (streaming parser must not care)
    /metrics?empty=1    200 with no samples
    /reset              restart every counter (counter-reset detection)
    /down               stop answering until /up  (staleness ladder)
    /up                 start answering again

    python3 tools/fake_exporter.py [--port 9100] [--host 0.0.0.0]
"""
import argparse
import math
import random
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

START = time.time()
EPOCH = [START]          # reset moves this forward
# A real process restart zeroes its counters. The large constant bases below
# exist to exercise the float64 baseline (a float32 subtraction of consecutive
# samples at 1.2e13 is exactly zero), but /reset must drop them too or it
# simulates a small backward step rather than a restart -- and Prometheus
# reset semantics (delta = v_now) then legitimately produce an absurd spike.
BASE = [1.0]
SERVING = [True]
MODE = [""]        # sticky shape for /metrics, set via /mode?set=...

def body():
    t = time.time()
    up = t - EPOCH[0]
    # Counters climb from the (possibly reset) epoch.
    cpu_user = up * 0.37
    cpu_sys = up * 0.11
    cpu_idle = up * 3.2
    net_rx = BASE[0] * 1.2345e13 + up * 8.4e6   # deliberately huge: float32
    net_tx = BASE[0] * 4.4e11 + up * 1.2e6      # would lose the delta entirely
    reqs = up * 12.4
    errs = up * 0.02
    load = 0.84 + 0.4 * math.sin(t / 37.0) + random.uniform(-0.05, 0.05)
    memavail = 4.93e9 + 3e8 * math.sin(t / 53.0)
    temp = 61 + 6 * math.sin(t / 91.0)

    # Histogram buckets, cumulative, filling over time.
    n = up * 12.4
    b = [0.6, 0.78, 0.9, 0.96, 0.99, 1.0]
    buckets = [n * f for f in b]

    return f"""# HELP node_load1 1m load average.
# TYPE node_load1 gauge
node_load1 {load:.3f}
# HELP node_memory_MemAvailable_bytes Memory available in bytes.
# TYPE node_memory_MemAvailable_bytes gauge
node_memory_MemAvailable_bytes {memavail:.0f}
# HELP node_hwmon_temp_celsius Hardware monitor temperature.
# TYPE node_hwmon_temp_celsius gauge
node_hwmon_temp_celsius{{chip="coretemp",sensor="temp1"}} {temp:.1f}
node_hwmon_temp_celsius{{chip="coretemp",sensor="temp9"}} NaN
# HELP node_cpu_seconds_total Seconds the CPUs spent in each mode.
# TYPE node_cpu_seconds_total counter
node_cpu_seconds_total{{cpu="0",mode="user"}} {cpu_user:.2f}
node_cpu_seconds_total{{cpu="0",mode="system"}} {cpu_sys:.2f}
node_cpu_seconds_total{{cpu="0",mode="idle"}} {cpu_idle:.2f}
# HELP node_network_receive_bytes_total Network device receive bytes.
# TYPE node_network_receive_bytes_total counter
node_network_receive_bytes_total{{device="eth0"}} {net_rx:.0f}
node_network_transmit_bytes_total{{device="eth0"}} {net_tx:.0f}
# HELP http_requests_total Total HTTP requests.
# TYPE http_requests_total counter
http_requests_total{{code="200",method="get"}} {reqs:.0f}
http_requests_total{{code="500",method="get"}} {errs:.0f}
# HELP http_request_duration_seconds Request latency.
# TYPE http_request_duration_seconds histogram
http_request_duration_seconds_bucket{{le="0.005"}} {buckets[0]:.0f}
http_request_duration_seconds_bucket{{le="0.01"}} {buckets[1]:.0f}
http_request_duration_seconds_bucket{{le="0.025"}} {buckets[2]:.0f}
http_request_duration_seconds_bucket{{le="0.05"}} {buckets[3]:.0f}
http_request_duration_seconds_bucket{{le="0.1"}} {buckets[4]:.0f}
http_request_duration_seconds_bucket{{le="+Inf"}} {buckets[5]:.0f}
http_request_duration_seconds_sum {n * 0.0182:.3f}
http_request_duration_seconds_count {n:.0f}
# HELP up 1 if the target is reachable.
# TYPE up gauge
up{{instance="fake:9100",job="node"}} 1
# HELP node_os_info Label carrier, always 1.
# TYPE node_os_info gauge
node_os_info{{name="Fake Linux",version="1.0"}} 1
"""

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):
        print(f"  {self.address_string()}  {fmt % a}", flush=True)

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)

        if u.path == "/mode":
            MODE[0] = q.get("set", [""])[0]
            return self._text(f"mode={MODE[0] or 'normal'}\n")
        if u.path == "/reset":
            EPOCH[0] = time.time(); BASE[0] = 0.0
            return self._text("counters reset (process-restart semantics)\n")
        if u.path == "/down":
            SERVING[0] = False;     return self._text("now refusing\n")
        if u.path == "/up":
            SERVING[0] = True;      return self._text("now serving\n")

        if not SERVING[0]:
            self.send_response(503); self.send_header("Content-Length", "0")
            self.end_headers(); return

        if u.path not in ("/metrics", "/"):
            self.send_response(404); self.send_header("Content-Length", "0")
            self.end_headers(); return

        # A sticky mode behaves as if the query string were present, so the
        # panel can be pointed at one fixed URL while the body shape changes.
        if MODE[0] and MODE[0] not in q:
            q[MODE[0]] = ["1"]

        if "slow" in q:
            time.sleep(float(q["slow"][0]))
        if "html" in q:
            return self._send(b"<html><body><h1>It works!</h1></body></html>",
                              "text/html")
        if "empty" in q:
            return self._send(b"# HELP nothing Here.\n", "text/plain")

        data = body().encode()
        if "huge" in q:
            # A VALID large exposition: unique family names per block. Simply
            # repeating the body would emit duplicate series, which no real
            # exporter does and which tests robustness rather than scale.
            base = body()
            blocks = [data]
            for k in range(260):
                blocks.append(
                    base.replace("node_", f"n{k}_")
                        .replace("http_", f"h{k}_")
                        .replace("up{", f"u{k}{{")
                        .encode())
            data = b"".join(blocks)
        if "dup" in q:
            # The opposite test: the same series repeated, which a buggy
            # exporter or a proxy concatenating two scrapes can produce.
            data = data * 40
        if "truncate" in q:
            data = data[: len(data) // 2]

        if "chunked" in q:
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; version=0.0.4")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            step = 1500
            for i in range(0, len(data), step):
                c = data[i:i + step]
                self.wfile.write(f"{len(c):X}\r\n".encode() + c + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
            return

        self._send(data, "text/plain; version=0.0.4")

    def _text(self, s):
        self._send(s.encode(), "text/plain")

    def _send(self, data, ctype):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9100)
    ap.add_argument("--host", default="0.0.0.0")
    a = ap.parse_args()
    print(f"fake exporter on http://{a.host}:{a.port}/metrics", flush=True)
    ThreadingHTTPServer((a.host, a.port), H).serve_forever()
