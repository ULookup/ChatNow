#!/usr/bin/env python3
"""Regression contract for RedisClient cluster scanning and PresenceRedis fallbacks.

This test intentionally inspects the header because the unit-test build is supported
without redis++ in developer environments.  Redis integration coverage still belongs
in environments that provide a Redis cluster.
"""

from pathlib import Path
import re
import sys


HEADER = Path(__file__).parents[1] / "dao" / "data_redis.hpp"


def method_body(source: str, name: str) -> str:
    match = re.search(r"\b" + re.escape(name) + r"\s*\([^)]*\)\s*\{", source)
    if not match:
        raise AssertionError(f"missing method {name}")
    depth = 1
    pos = match.end()
    while depth:
        if pos == len(source):
            raise AssertionError(f"unterminated method {name}")
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[match.start():pos]


def main() -> int:
    source = HEADER.read_text()
    check_scan = len(sys.argv) == 1 or "--scan" in sys.argv
    check_presence = len(sys.argv) == 1 or "--presence" in sys.argv
    if check_scan:
        scan = method_body(source, "scan")
        assert "_rc->for_each" in scan, "cluster scan must visit every cluster node"
        assert "abort()" not in scan, "cluster scan must not terminate the process on a cursor"
        assert "return 0;" in scan, "cluster scan must finish with cursor zero"

    if check_presence:
        presence = source[source.index("class PresenceRedis"):]
        for method in (
            "set_state", "get_state", "touch_active", "set_custom_status",
            "add_device", "get_devices", "set_typing", "subscribe", "unsubscribe",
        ):
            body = method_body(presence, method)
            assert "try" in body and "catch" in body, f"{method} must catch Redis errors"

        assert 'return "offline";' in method_body(presence, "get_state")
        assert "return out;" in method_body(presence, "get_devices")
    print("data_redis contract tests passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        print(f"data_redis contract test failed: {error}", file=sys.stderr)
        sys.exit(1)
