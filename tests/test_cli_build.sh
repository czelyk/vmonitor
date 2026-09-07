#!/usr/bin/env bash
set -e

echo "[TEST] Building monitor_cli"

make -C cli clean
make -C cli

echo "[PASS] monitor_cli build succeeded"