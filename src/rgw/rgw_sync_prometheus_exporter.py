#!/usr/bin/env python3

import argparse
import json
import os
import socket
import threading
from collections import defaultdict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


DEFAULT_LABELS = (
    "realm",
    "zonegroup",
    "source_zone",
    "dest_zone",
    "sync_type",
    "phase",
    "result",
    "error",
)
DEBUG_LABELS = ("bucket", "bucket_id", "shard")
REMOTE_LABELS = DEFAULT_LABELS + ("remote_op",)
FAILURE_LABELS = DEFAULT_LABELS + ("operation", "op_state", "failure_stage", "reason")
HISTOGRAM_BUCKETS = (0.01, 0.05, 0.1, 0.5, 1.0, 5.0, 10.0, 30.0, 60.0, 300.0, 900.0)


def label_value(event, name):
    value = event.get(name)
    if value is None or value == "":
        return "unknown"
    return str(value)


def labels_for(event, base):
    labels = tuple((name, label_value(event, name)) for name in base)
    base_names = set(base)
    debug = tuple(
        (name, str(event[name]))
        for name in DEBUG_LABELS
        if name not in base_names and event.get(name) not in (None, "")
    )
    return labels + debug


def escape_label(value):
    return str(value).replace("\\", "\\\\").replace("\n", "\\n").replace('"', '\\"')


def labels_text(labels):
    if not labels:
        return ""
    return "{" + ",".join(f'{name}="{escape_label(value)}"' for name, value in labels) + "}"


class Metrics:
    def __init__(self, max_series):
        self.max_series = max_series
        self.lock = threading.Lock()
        self.counters = defaultdict(float)
        self.gauges = {}
        self.histograms = {}
        self.dropped_events = 0
        self.bad_events = 0

    def _track(self, collection, key, default):
        if key not in collection:
            total = len(self.counters) + len(self.gauges) + len(self.histograms)
            if total >= self.max_series:
                self.dropped_events += 1
                return None
            collection[key] = default
        return collection[key]

    def inc(self, name, labels, value=1.0):
        key = (name, labels)
        current = self._track(self.counters, key, 0.0)
        if current is None:
            return
        self.counters[key] = current + value

    def set_gauge(self, name, labels, value):
        key = (name, labels)
        current = self._track(self.gauges, key, None)
        if current is None and key not in self.gauges:
            return
        self.gauges[key] = float(value)

    def observe(self, name, labels, value):
        key = (name, labels)
        current = self._track(
            self.histograms,
            key,
            {"buckets": [0 for _ in HISTOGRAM_BUCKETS], "sum": 0.0, "count": 0},
        )
        if current is None:
            return
        value = float(value)
        current["sum"] += value
        current["count"] += 1
        for index, bucket in enumerate(HISTOGRAM_BUCKETS):
            if value <= bucket:
                current["buckets"][index] += 1

    def handle(self, event):
        metric = event.get("metric")
        duration = event.get("duration_seconds")
        value = event.get("value")

        with self.lock:
            if metric == "entries":
                labels = labels_for(event, DEFAULT_LABELS)
                self.inc("rgw_sync_entries_total", labels)
                if duration is not None:
                    self.observe("rgw_sync_entry_duration_seconds", labels, duration)
            elif metric == "remote_requests":
                labels = labels_for(event, REMOTE_LABELS)
                self.inc("rgw_sync_remote_requests_total", labels)
                if duration is not None:
                    self.observe("rgw_sync_remote_request_duration_seconds", labels, duration)
            elif metric == "retries":
                self.inc("rgw_sync_retries_total", labels_for(event, DEFAULT_LABELS))
            elif metric == "errors":
                self.inc("rgw_sync_errors_total", labels_for(event, FAILURE_LABELS))
            elif metric == "lease":
                lease_labels = ("realm", "zonegroup", "source_zone", "dest_zone", "result", "error")
                self.inc("rgw_sync_lease_total", labels_for(event, lease_labels))
            elif metric == "shard_marker_lag" and value is not None:
                marker_labels = ("realm", "zonegroup", "source_zone", "dest_zone", "sync_type", "shard")
                self.set_gauge("rgw_sync_shard_marker_lag_seconds", labels_for(event, marker_labels), value)
            elif metric == "debug_bucket_shard_state" and value is not None:
                self.set_gauge("rgw_sync_debug_bucket_shard_state", labels_for(event, DEFAULT_LABELS), value)
            else:
                self.bad_events += 1

    def render(self):
        with self.lock:
            lines = [
                "# HELP rgw_sync_exporter_dropped_events_total Events dropped after the configured series limit.",
                "# TYPE rgw_sync_exporter_dropped_events_total counter",
                f"rgw_sync_exporter_dropped_events_total {self.dropped_events}",
                "# HELP rgw_sync_exporter_bad_events_total Events with unknown or invalid metric names.",
                "# TYPE rgw_sync_exporter_bad_events_total counter",
                f"rgw_sync_exporter_bad_events_total {self.bad_events}",
            ]

            for (name, labels), value in sorted(self.counters.items()):
                lines.append(f"# TYPE {name} counter")
                lines.append(f"{name}{labels_text(labels)} {value}")

            for (name, labels), value in sorted(self.gauges.items()):
                lines.append(f"# TYPE {name} gauge")
                lines.append(f"{name}{labels_text(labels)} {value}")

            for (name, labels), data in sorted(self.histograms.items()):
                lines.append(f"# TYPE {name} histogram")
                cumulative_labels = dict(labels)
                for bucket, count in zip(HISTOGRAM_BUCKETS, data["buckets"]):
                    bucket_labels = tuple(cumulative_labels.items()) + (("le", f"{bucket:g}"),)
                    lines.append(f"{name}_bucket{labels_text(bucket_labels)} {count}")
                inf_labels = tuple(cumulative_labels.items()) + (("le", "+Inf"),)
                lines.append(f"{name}_bucket{labels_text(inf_labels)} {data['count']}")
                lines.append(f"{name}_sum{labels_text(labels)} {data['sum']}")
                lines.append(f"{name}_count{labels_text(labels)} {data['count']}")

            return "\n".join(lines) + "\n"


class Handler(BaseHTTPRequestHandler):
    metrics = None

    def do_GET(self):
        if self.path not in ("/metrics", "/"):
            self.send_response(404)
            self.end_headers()
            return
        body = self.metrics.render().encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        return


def datagram_loop(path, metrics):
    if os.path.exists(path):
        os.unlink(path)
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    sock.bind(path)
    os.chmod(path, 0o666)
    while True:
        data, _ = sock.recvfrom(65535)
        try:
            event = json.loads(data.decode("utf-8"))
            metrics.handle(event)
        except Exception:
            with metrics.lock:
                metrics.bad_events += 1


def main():
    parser = argparse.ArgumentParser(description="Prometheus exporter for RGW sync debug events")
    parser.add_argument("--socket", default=os.environ.get("RGW_SYNC_EXPORTER_SOCKET", "/run/ceph/rgw-sync-debug.sock"))
    parser.add_argument("--listen", default=os.environ.get("RGW_SYNC_EXPORTER_LISTEN", "0.0.0.0:9284"))
    parser.add_argument("--max-series", type=int, default=int(os.environ.get("RGW_SYNC_EXPORTER_MAX_SERIES", "20000")))
    args = parser.parse_args()

    host, port = args.listen.rsplit(":", 1)
    metrics = Metrics(args.max_series)
    Handler.metrics = metrics

    thread = threading.Thread(target=datagram_loop, args=(args.socket, metrics), daemon=True)
    thread.start()

    server = ThreadingHTTPServer((host, int(port)), Handler)
    server.serve_forever()


if __name__ == "__main__":
    main()
