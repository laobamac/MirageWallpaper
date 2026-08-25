#!/usr/bin/env python3
"""Require an exact, non-zero set of passing Xcode test cases."""

from __future__ import annotations

import json
import sys
from typing import Any, Iterator


def walk(node: Any) -> Iterator[dict[str, Any]]:
    if isinstance(node, dict):
        yield node
        for value in node.values():
            yield from walk(value)
    elif isinstance(node, list):
        for value in node:
            yield from walk(value)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <expected-test-count>", file=sys.stderr)
        return 2

    expected_count = int(sys.argv[1])
    payload = json.load(sys.stdin)
    test_cases = [node for node in walk(payload) if node.get("nodeType") == "Test Case"]
    failures = [
        f"{node.get('name', '<unnamed>')}: {node.get('result', '<missing result>')}"
        for node in test_cases
        if node.get("result") != "Passed"
    ]

    if len(test_cases) != expected_count:
        print(
            f"ERROR: expected {expected_count} Swift tests, found {len(test_cases)}.",
            file=sys.stderr,
        )
        return 1
    if failures:
        print("ERROR: Swift test failures:\n  " + "\n  ".join(failures), file=sys.stderr)
        return 1

    print(f"Mirage Swift tests passed ({len(test_cases)} test cases).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
