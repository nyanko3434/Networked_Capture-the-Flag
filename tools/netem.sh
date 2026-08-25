#!/usr/bin/env bash
# Adverse network condition harness for local testing (README §10).
#
# Usage:
#   ./tools/netem.sh on [--delay MS] [--jitter MS] [--loss PCT] [--preset NAME]
#   ./tools/netem.sh off
#   ./tools/netem.sh status
#
# Defaults match the README §10 example exactly: delay 80ms 20ms loss 5%.
# An explicit --delay/--jitter/--loss overrides the corresponding value from
# --preset if both are given (preset is applied first, flags after).
set -euo pipefail

IFACE="lo"

DELAY="80ms"
JITTER="20ms"
LOSS="5%"

usage() {
    cat >&2 <<EOF
usage:
  $0 on [--delay MS] [--jitter MS] [--loss PCT] [--preset NAME]
  $0 off
  $0 status

presets:
  default   delay=80ms  jitter=20ms loss=5%   (the README §10 example)
  light     delay=20ms  jitter=5ms  loss=0%
  heavy     delay=200ms jitter=50ms loss=15%
EOF
    exit 1
}

apply_preset() {
    case "$1" in
        default) DELAY="80ms";  JITTER="20ms"; LOSS="5%"  ;;
        light)   DELAY="20ms";  JITTER="5ms";  LOSS="0%"  ;;
        heavy)   DELAY="200ms"; JITTER="50ms"; LOSS="15%" ;;
        *)
            echo "unknown preset: $1" >&2
            usage
            ;;
    esac
}

has_netem() {
    tc qdisc show dev "$IFACE" | grep -q netem
}

cmd_on() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --delay)  DELAY="$2";  shift 2 ;;
            --jitter) JITTER="$2"; shift 2 ;;
            --loss)   LOSS="$2";   shift 2 ;;
            --preset) apply_preset "$2"; shift 2 ;;
            *) echo "unknown option: $1" >&2; usage ;;
        esac
    done

    # Idempotent / safe re-run: tear down any existing netem qdisc on this
    # iface first. Without this, re-running 'on' with new params fails with
    # tc's "Exclusivity flag on, cannot modify" instead of replacing it, and
    # a leftover qdisc from an earlier run could otherwise survive a crashed
    # or skipped 'off' and leak into unrelated later testing.
    if has_netem; then
        sudo tc qdisc del dev "$IFACE" root
    fi
    sudo tc qdisc add dev "$IFACE" root netem delay "$DELAY" "$JITTER" loss "$LOSS"
    echo "netem on ($IFACE): delay=$DELAY jitter=$JITTER loss=$LOSS"
}

cmd_off() {
    # Safe teardown: don't error if netem was never applied (e.g. after an
    # 'on' that failed partway, or if run twice by mistake).
    if has_netem; then
        sudo tc qdisc del dev "$IFACE" root
        echo "netem off ($IFACE)"
    else
        echo "netem already off ($IFACE)"
    fi
}

cmd_status() {
    tc qdisc show dev "$IFACE"
}

[[ $# -ge 1 ]] || usage
sub="$1"; shift

case "$sub" in
    on)     cmd_on "$@" ;;
    off)    [[ $# -eq 0 ]] || usage; cmd_off ;;
    status) [[ $# -eq 0 ]] || usage; cmd_status ;;
    *)      usage ;;
esac