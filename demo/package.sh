#!/usr/bin/env bash
set -euo pipefail

# Packages the client into a distributable zip for GitHub Releases.
# Run from the project root after building: ./demo/package.sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
OUT_DIR="$PROJECT_DIR/demo/dist"

mkdir -p "$OUT_DIR"

# Check binaries
if [ ! -f "$BUILD_DIR/ctf_client" ]; then
    echo "ERROR: ctf_client not found. Build first: cmake --build build -j"
    exit 1
fi

# Create staging directory
STAGE="$OUT_DIR/ctf-client-linux"
rm -rf "$STAGE"
mkdir -p "$STAGE"

# Copy binary
cp "$BUILD_DIR/ctf_client" "$STAGE/"

# Copy join script
cat > "$STAGE/join.sh" << 'SCRIPT'
#!/usr/bin/env bash
set -euo pipefail

SERVER_IP="${1:-}"
if [ -z "$SERVER_IP" ]; then
    echo "Usage: ./join.sh <server-ip>"
    echo ""
    echo "The server host will tell you their IP."
    echo "Example: ./join.sh 192.168.1.105"
    exit 1
fi

NAME="${2:-player$(shuf -i 1-999 -n 1)}"
PORT=7777

echo ""
echo "  Joining CTF server at $SERVER_IP:$PORT as '$NAME'"
echo "  (Ctrl+C to quit)"
echo ""

./ctf_client --host "$SERVER_IP" --port "$PORT" --name "$NAME"
SCRIPT
chmod +x "$STAGE/join.sh"

# Create README
cat > "$STAGE/README.txt" << 'README'
CTF Client — Quick Start
========================

1. Connect to the host's WiFi hotspot
2. Run: ./join.sh <server-ip>
   Example: ./join.sh 192.168.1.105

The server host will tell you their IP address.

Controls:
  WASD       - Move
  Mouse      - Aim
  Left Click - Shoot
  ENTER      - Start match (host only)

Requires: Linux (Ubuntu/Debian/Fedora/Arch)
README

# Create archive (tar.gz, always available without extra packages)
cd "$OUT_DIR"
rm -f ctf-client-linux.tar.gz
tar czf ctf-client-linux.tar.gz ctf-client-linux/
rm -rf "$STAGE"

echo ""
echo "Packaged: $OUT_DIR/ctf-client-linux.tar.gz"
echo "Upload to GitHub Releases for easy distribution."
echo ""
echo "Teacher runs:"
echo "  tar xzf ctf-client-linux.tar.gz"
echo "  cd ctf-client-linux"
echo "  ./join.sh <server-ip>"
