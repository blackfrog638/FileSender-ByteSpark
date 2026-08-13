#!/usr/bin/env python3
"""Compatibility-free Harness V2 CLI shim."""

from agent_v2 import main


if __name__ == "__main__":
    raise SystemExit(main())
