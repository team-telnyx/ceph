#!/usr/bin/env python3

import argparse
import json
import os
import socket
import threading
import time
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
DEBUG_LABELS = ("bucket", "bucket_id", "data_shard", "bucket_shard", "shard")
REMOTE_LABELS = DEFAULT_LABELS + ("remote_op",)
REMOTE_FAILURE_LABELS = REMOTE_LABELS + ("failure_stage", "reason")
FAILURE_LABELS = DEFAULT_LABELS + ("operation", "op_state", "failure_stage", "reason")
ERROR_LABELS = FAILURE_LABELS + DEBUG_LABELS
LEASE_LABELS = ("realm", "zonegroup", "source_zone", "dest_zone", "result", "error")
LEASE_SHARD_LABELS = LEASE_LABELS + ("data_shard", "shard")
MARKER_LABELS = ("realm", "zonegroup", "source_zone", "dest_zone", "sync_type", "data_shard", "shard")
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


def failed_event(event):
    result = event.get("result")
    error = event.get("error")
    if result in ("success", "skipped"):
        return False
    if result in ("retry", "error"):
        return True
    return error not in (None, "", "none", "unknown")


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
        now = time.time()

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
                if failed_event(event):
                    failure_labels = labels_for(event, REMOTE_FAILURE_LABELS)
                    self.inc("rgw_sync_remote_request_failures_total", failure_labels)
                    self.set_gauge(
                        "rgw_sync_remote_request_last_failure_timestamp_seconds",
                        failure_labels,
                        now,
                    )
            elif metric == "retries":
                self.inc("rgw_sync_retries_total", labels_for(event, DEFAULT_LABELS))
            elif metric == "errors":
                labels = labels_for(event, ERROR_LABELS)
                self.inc("rgw_sync_errors_total", labels)
                self.set_gauge("rgw_sync_error_last_seen_timestamp_seconds", labels, now)
            elif metric == "lease":
                self.inc("rgw_sync_lease_total", labels_for(event, LEASE_LABELS))
                if failed_event(event):
                    self.set_gauge(
                        "rgw_sync_lease_last_failure_timestamp_seconds",
                        labels_for(event, LEASE_SHARD_LABELS),
                        now,
                    )
            elif metric == "shard_marker_lag" and value is not None:
                self.set_gauge("rgw_sync_shard_marker_lag_seconds", labels_for(event, MARKER_LABELS), value)
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


def run_self_test():
    metrics = Metrics(max_series=1000)
    base = {
        "realm": "r",
        "zonegroup": "zg",
        "source_zone": "src",
        "dest_zone": "dst",
        "sync_type": "incremental",
    }
    metrics.handle({
        **base,
        "metric": "errors",
        "phase": "object_sync",
        "result": "retry",
        "error": "ebusy",
        "operation": "write",
        "op_state": "link_olh",
        "failure_stage": "remote_fetch_or_local_put",
        "reason": "retryable",
        "bucket": "example",
        "bucket_id": "abcd1234",
        "data_shard": "68",
        "bucket_shard": "7",
        "shard": "7",
    })
    metrics.handle({
        **base,
        "metric": "remote_requests",
        "phase": "object_fetch",
        "result": "retry",
        "error": "ebusy",
        "remote_op": "object_fetch",
        "failure_stage": "remote_fetch_or_local_put",
        "reason": "retryable",
        "data_shard": "68",
        "duration_seconds": 1.25,
    })
    metrics.handle({
        **base,
        "metric": "lease",
        "phase": "lease",
        "result": "retry",
        "error": "ebusy",
        "data_shard": "68",
        "shard": "68",
    })
    metrics.handle({
        **base,
        "metric": "shard_marker_lag",
        "phase": "marker",
        "result": "success",
        "error": "none",
        "data_shard": "68",
        "shard": "68",
        "value": 120.0,
    })

    rendered = metrics.render()
    required = (
        "rgw_sync_error_last_seen_timestamp_seconds",
        'rgw_sync_remote_request_failures_total{realm="r",zonegroup="zg",source_zone="src",dest_zone="dst",sync_type="incremental",phase="object_fetch",result="retry",error="ebusy",remote_op="object_fetch",failure_stage="remote_fetch_or_local_put",reason="retryable",data_shard="68"} 1.0',
        "rgw_sync_remote_request_last_failure_timestamp_seconds",
        'rgw_sync_lease_last_failure_timestamp_seconds{realm="r",zonegroup="zg",source_zone="src",dest_zone="dst",result="retry",error="ebusy",data_shard="68",shard="68"}',
        'rgw_sync_shard_marker_lag_seconds{realm="r",zonegroup="zg",source_zone="src",dest_zone="dst",sync_type="incremental",data_shard="68",shard="68"} 120.0',
    )
    missing = [item for item in required if item not in rendered]
    if missing:
        raise AssertionError("missing expected metrics: " + ", ".join(missing))


def main():
    parser = argparse.ArgumentParser(description="Prometheus exporter for RGW sync debug events")
    parser.add_argument("--socket", default=os.environ.get("RGW_SYNC_EXPORTER_SOCKET", "/run/ceph/rgw-sync-debug.sock"))
    parser.add_argument("--listen", default=os.environ.get("RGW_SYNC_EXPORTER_LISTEN", "0.0.0.0:9284"))
    parser.add_argument("--max-series", type=int, default=int(os.environ.get("RGW_SYNC_EXPORTER_MAX_SERIES", "20000")))
    parser.add_argument("--self-test", action="store_true", help="run exporter metric mapping self-test and exit")
    args = parser.parse_args()

    if args.self_test:
        run_self_test()
        return

    host, port = args.listen.rsplit(":", 1)
    metrics = Metrics(args.max_series)
    Handler.metrics = metrics

    thread = threading.Thread(target=datagram_loop, args=(args.socket, metrics), daemon=True)
    thread.start()

    server = ThreadingHTTPServer((host, int(port)), Handler)
    server.serve_forever()


if __name__ == "__main__":
    main()
