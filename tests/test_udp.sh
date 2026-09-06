#!/usr/bin/env bash
set -e

UDP_PORT=5001

echo "[TEST] UDP telemetry listener"
echo
echo "Start this in another terminal:"
echo "./cli/monitor_cli --udp ${UDP_PORT}"
echo

read -p "Press Enter when the UDP listener is running..."

echo
echo "[TEST 1] Sending valid STAT packet"
echo "STAT 1 25 42000 1 3 1" | nc -u 127.0.0.1 "${UDP_PORT}"

sleep 1

echo
echo "[TEST 2] Sending malformed packet"
echo "INVALID PACKET" | nc -u 127.0.0.1 "${UDP_PORT}"

echo
echo "[INFO] Verify that:"
echo "- Valid STAT packet was printed"
echo "- Malformed packet was rejected"