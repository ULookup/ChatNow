#!/usr/bin/env python3
"""Read bounded, allowlisted evidence from a disposable Compose test stack."""

import argparse
import base64
import json
import os
from pathlib import Path
import re
import subprocess
import time
from datetime import datetime, timezone

SERVICES = ("gateway", "identity", "relationship", "conversation", "transmite",
            "message", "media", "presence", "push")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True)
    parser.add_argument("--timeout-sec", type=float, default=20)
    args = parser.parse_args()
    if not 0 < args.timeout_sec <= 60:
        parser.error("timeout must be greater than zero and at most 60 seconds")
    deadline = time.monotonic() + args.timeout_sec

    def command(arguments):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return None, "deadline"
        try:
            result = subprocess.run(["docker", *arguments], capture_output=True,
                                    timeout=min(2, remaining), check=False)
        except subprocess.TimeoutExpired:
            return None, "deadline"
        except OSError:
            return None, "command_unavailable"
        if result.returncode:
            return None, "command_failed"
        return result.stdout.decode("utf-8", errors="replace"), None

    snapshot = {"time": datetime.now(timezone.utc).isoformat(), "services": {},
                "registrations": {}, "events": [], "errors": []}
    for service in SERVICES:
        name = service + "_server"
        ids, error = command(["compose", "ps", "--all", "--quiet", name])
        state = {"available": False}
        if not error and re.fullmatch(r"[a-f0-9]{64}\s*", ids or ""):
            raw, error = command(["inspect", "--format",
                                  '{"State":{{json .State}},"RestartCount":{{.RestartCount}}}', ids.strip()])
            if not error:
                try:
                    record = json.loads(raw)
                    source = record["State"]
                    state = {"available": True, "restart_count": int(record["RestartCount"])}
                    for key in ("Running", "Paused", "Restarting", "OOMKilled"):
                        state[key] = source.get(key) is True
                    for key in ("Pid", "ExitCode"):
                        state[key] = int(source[key])
                    for key in ("StartedAt", "FinishedAt"):
                        value = source.get(key, "")
                        if re.fullmatch(r"[0-9TZ:.+-]{1,40}", value):
                            state[key] = value
                except (ValueError, KeyError, TypeError, AttributeError):
                    state = {"available": False}
                    error = "invalid_state"
        elif not error:
            error = "missing_or_multiple_containers"
        if error:
            state["error"] = error
        snapshot["services"][service] = state

    raw, error = command(["compose", "exec", "-T", "etcd", "etcdctl", "get",
                          "/service/", "--prefix", "--write-out=json"])
    keys = set()
    if not error:
        try:
            for entry in json.loads(raw).get("kvs", []):
                keys.add(base64.b64decode(entry["key"], validate=True).decode("utf-8"))
        except (ValueError, KeyError, TypeError, UnicodeError, AttributeError):
            error = "invalid_registration"
    for service in SERVICES[1:]:
        snapshot["registrations"][service] = {
            "available": error is None,
            "registered": (f"/service/{service}_service/instance" in keys) if not error else None,
        }
    if error:
        snapshot["errors"].append({"source": "etcd", "error": error})

    for service in ("gateway", "message", "transmite", "identity"):
        events = []
        raw, error = command(["compose", "exec", "-T", service + "_server", "tail",
                              "-c", "262144", "/im/logs/" + service + ".log"])
        if error:
            snapshot["errors"].append({"source": service + "_log", "error": error})
            continue
        for line in raw.splitlines():
            try:
                record = json.loads(line)
            except ValueError:
                continue
            if not isinstance(record, dict):
                continue
            message = record.get("msg", "")
            if not isinstance(message, str):
                continue
            event = {"service": service}
            if message.startswith("Gateway RPC"):
                event["event"] = "gateway_rpc_failure"
                code = re.search(r"err=\[([0-9]{1,5})\]", message)
                if code:
                    event["rpc_code"] = int(code[1])
            elif message.startswith("Gateway forward"):
                event["event"] = "gateway_no_backend"
            elif message.startswith("rpc_failed code="):
                event["event"] = "application_failure"
                code = re.match(r"rpc_failed code=([0-9]{1,5})\b", message)
                if code:
                    event["error_code"] = int(code[1])
            elif message.startswith("rpc_exception"):
                event["event"] = "application_exception"
            else:
                continue
            for source, target, pattern in (("ts", "time", r"[0-9TZ:.+-]{1,40}"),
                                             ("trace_id", "trace_id", r"[a-f0-9]{32}")):
                value = record.get(source, "")
                if isinstance(value, str) and re.fullmatch(pattern, value):
                    event[target] = value
            # Never retain msg, fields, identities, request bodies, environment,
            # container configuration, registration values, or command stderr.
            events.append(event)
        # Reserve each service's last events so a noisy Identity error path
        # cannot evict all Gateway transport evidence.
        snapshot["events"].extend(events[-125:])
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC | getattr(os, "O_NOFOLLOW", 0)
    with os.fdopen(os.open(output, flags, 0o600), "w") as stream:
        json.dump(snapshot, stream, indent=2)
        stream.write("\n")


if __name__ == "__main__":
    main()
