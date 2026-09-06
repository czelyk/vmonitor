#!/usr/bin/env bash
set -e

echo "[TEST] Building monitor_server"

make -C server clean
make -C server

echo "[PASS] monitor_server build succeeded"