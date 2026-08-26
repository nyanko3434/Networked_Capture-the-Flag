#!/usr/bin/env bash
set -euo pipefail

# HOST SCRIPT — Run this on your machine to start the server + bots.
# Usage: ./demo/host.sh [number-of-bots]

BOTS=${1:-8}
PORT=7777

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../build"

if [ ! -f "$BUILD_DIR/ctf_server" ]; then
    echo "ERROR: Build first: cmake --build build -j"
    exit 1
fi

# Get hotspot/LAN IP — prefer non-loopback, non-docker, non-WSL-internal addresses
LAN_IP=$(ip -4 addr show | grep -oP '(?<=inet\s)\d+(\.\d+){3}' | grep -v '127.0.0.1' | grep -v '^10\.255\.' | grep -v '^172\.1[7-9]\.' | grep -v '^172\.2[0-9]\.' | grep -v '^172\.3[0-1]\.' | head -1)
[ -z "$LAN_IP" ] && LAN_IP="127.0.0.1"

echo ""
echo "  ========================================"
echo "    CTF Server"
echo "  ========================================"
echo "    IP:    $LAN_IP"
echo "    Port:  $PORT"
echo "    Bots:  $BOTS"
echo ""
echo "    Tell others:  ./join.sh $LAN_IP"
echo "  ========================================"
echo ""

# Open firewall (best effort)
sudo ufw allow $PORT/tcp 2>/dev/null || true
sudo ufw allow $PORT/udp 2>/dev/null || true

# Start server
"$BUILD_DIR/ctf_server" --port $PORT --tick 30 &
SERVER_PID=$!
sleep 1

# Start bots
if [ "$BOTS" -gt 0 ]; then
    "$BUILD_DIR/ctf_bot" --host 127.0.0.1 --port $PORT --count "$BOTS" &
    BOT_PID=$!
fi

# Trap cleanup
cleanup() {
    kill $SERVER_PID 2>/dev/null || true
    [ -n "${BOT_PID:-}" ] && kill $BOT_PID 2>/dev/null || true
}
trap cleanup EXIT INT TERM

echo "Server running. Press Ctrl+C to stop."
wait $SERVER_PID
