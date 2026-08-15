#!/usr/bin/env bash
# Adverse network condition harness for local testing (README §10).
#
# Usage:
#   ./tools/netem.sh on     # add delay/jitter/loss to loopback
#   ./tools/netem.sh off    # remove it
set -euo pipefail

IFACE="lo"

usage() {
    echo "usage: $0 {on|off}" >&2
    exit 1
}

[[ $# -eq 1 ]] || usage

case "$1" in
    on)
        sudo tc qdisc add dev "$IFACE" root netem delay 80ms 20ms loss 5%
        ;;
    off)
        sudo tc qdisc del dev "$IFACE" root
        ;;
    *)
        usage
        ;;
esac
