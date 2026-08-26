#!/usr/bin/env python3
"""Headless load generator for ctf_server (README §10 integration load).

Spawns N protocol-speaking clients over loopback: TCP handshake, host
START_REQUEST, then every client sends UDP_HELLO and PLAYER_INPUT at ~30 Hz
while recording snapshot arrivals (WORLD_SNAPSHOT keyframes plus
DELTA_SNAPSHOTs) and per-client UDP byte totals. Prints per-client stats as
JSON so the poll-vs-epoll and full-vs-delta comparisons can be scripted.
"""

import argparse
import json
import socket
import struct
import threading
import time

MAGIC = 0x4346
VERSION = 2

JOIN_LOBBY, JOIN_ACCEPT, JOIN_REJECT = 1, 2, 3
LOBBY_STATE, START_REQUEST, GAME_START = 4, 5, 6
UDP_HELLO, PLAYER_INPUT, WORLD_SNAPSHOT, SHOT_FIRED = 7, 8, 9, 10
DELTA_SNAPSHOT = 19


def udp_header(mtype, tick):
    return struct.pack(">HBBI", MAGIC, VERSION, mtype, tick)


def tcp_frame(mtype, payload):
    return struct.pack(">HB", len(payload), mtype) + payload


class Client(threading.Thread):
    def __init__(self, idx, host, tcp_port, udp_port, duration, results,
                 expected_count=10):
        super().__init__(daemon=True)
        self.idx = idx
        self.host = host
        self.tcp_port = tcp_port
        self.udp_port = udp_port
        self.duration = duration
        self.results = results
        self.count = expected_count

    def run(self):
        info = {"snapshots": 0, "keyframes": 0, "deltas": 0,
                "udp_bytes": 0, "tcp_frames": {}, "other_udp": {}}
        snap_times = []
        tcp_acc = b""
        stop = threading.Event()
        try:
            tcp = socket.create_connection((self.host, self.tcp_port),
                                           timeout=5)
            name = b"bot%02d" % self.idx + b"\x00" * 11
            tcp.sendall(tcp_frame(JOIN_LOBBY, name))

            acc = b""
            deadline = time.time() + 5
            player_id = token = None
            while time.time() < deadline:
                d = tcp.recv(4096)
                if not d:
                    break
                acc += d
                # Frame-by-frame so JOIN_ACCEPT is found wherever it sits
                # and any bytes AFTER it survive into tcp_acc (with 10
                # concurrent joins, LOBBY_STATE broadcasts race the accept).
                while len(acc) >= 3:
                    plen = struct.unpack(">H", acc[:2])[0]
                    if len(acc) < 3 + plen:
                        break
                    ftype = acc[2]
                    payload = acc[3:3 + plen]
                    acc = acc[3 + plen:]
                    if ftype == JOIN_ACCEPT and len(payload) >= 7:
                        player_id, token, _u = struct.unpack(
                            ">BIH", payload[:7])
                    elif ftype == JOIN_REJECT:
                        info["error"] = "JOIN_REJECT during handshake"
                        self.results[self.idx] = info
                        return
                if player_id is not None:
                    break
            if player_id is None:
                info["error"] = "no JOIN_ACCEPT"
                self.results[self.idx] = info
                return
            # Anything buffered past JOIN_ACCEPT stays in the stream.
            tcp_acc = acc

            udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            udp.bind(("127.0.0.1", 0))
            udp.settimeout(0.05)

            hello = udp_header(UDP_HELLO, 0) + struct.pack(
                ">BI", player_id, token)
            for _ in range(25):
                udp.sendto(hello, (self.host, self.udp_port))
                time.sleep(0.02)

            # Whoever the server actually made HOST starts the match once
            # the whole roster is in. ("First joiner is host" is
            # nondeterministic under concurrent connects — never assume
            # idx 0 won that race.)
            started = threading.Event()

            seq_holder = [0]

            def sender():
                aim = 0
                while not stop.is_set():
                    s = seq_holder[0]
                    # Intermittent movement with a per-bot phase: ~50% of
                    # each bot's ticks are idle (aiming/camping), like real
                    # play. Constant motion would be the pathological
                    # worst case for delta compression. Aim is frozen while
                    # idle — a still player changes nothing.
                    if (s + self.idx * 17) % 60 < 30:
                        buttons = 1 << (s % 4)
                        aim = s * 97 % 65536
                    else:
                        buttons = 0
                    body = (struct.pack(">BIIB", player_id, token, s, 1) +
                            struct.pack(">BH", buttons, aim))
                    udp.sendto(udp_header(PLAYER_INPUT, s) + body,
                               (self.host, self.udp_port))
                    seq_holder[0] += 1
                    time.sleep(1.0 / 30)

            threading.Thread(target=sender, daemon=True).start()

            t_end = time.time() + self.duration
            while time.time() < t_end:
                # Drain UDP.
                try:
                    data, _ = udp.recvfrom(2048)
                    if len(data) >= 8:
                        info["udp_bytes"] += len(data)
                        mtype = data[3]
                        if mtype in (WORLD_SNAPSHOT, DELTA_SNAPSHOT):
                            snap_times.append(time.time())
                            if mtype == WORLD_SNAPSHOT:
                                info["keyframes"] = \
                                    info.get("keyframes", 0) + 1
                            else:
                                info["deltas"] = \
                                    info.get("deltas", 0) + 1
                        else:
                            info["other_udp"][mtype] = \
                                info["other_udp"].get(mtype, 0) + 1
                except socket.timeout:
                    pass
                # Drain TCP with proper frame reassembly.
                tcp.settimeout(0)
                try:
                    d = tcp.recv(4096)
                    if d:
                        tcp_acc += d
                        while len(tcp_acc) >= 3:
                            plen = struct.unpack(">H", tcp_acc[:2])[0]
                            if len(tcp_acc) < 3 + plen:
                                break
                            ftype = tcp_acc[2]
                            payload = tcp_acc[3:3 + plen]
                            info["tcp_frames"][ftype] = \
                                info["tcp_frames"].get(ftype, 0) + 1
                            # Host: START_REQUEST once the roster is full.
                            if (ftype == LOBBY_STATE and not started.is_set()
                                    and len(payload) >= 1):
                                pcnt = payload[0]
                                if (pcnt == self.count
                                        and len(payload) >= 2 + pcnt * 17):
                                    host_id = payload[1 + pcnt * 17]
                                    if host_id == player_id:
                                        tcp.sendall(
                                            tcp_frame(START_REQUEST, b""))
                                        started.set()
                            tcp_acc = tcp_acc[3 + plen:]
                except socket.timeout:
                    pass
                except BlockingIOError:
                    pass

            stop.set()
            intervals = [1000 * (b - a) for a, b in zip(snap_times,
                                                       snap_times[1:])]
            info["snapshots"] = len(snap_times)
            if intervals:
                mean = sum(intervals) / len(intervals)
                var = sum((x - mean) ** 2 for x in intervals) / len(intervals)
                info["interval_ms_mean"] = round(mean, 2)
                info["interval_ms_stddev"] = round(var ** 0.5, 2)
                info["rate_hz"] = round(1000.0 / mean, 2)
        except Exception:
            import traceback
            info["error"] = traceback.format_exc(limit=2)
        self.results[self.idx] = info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--udp-port", type=int, required=True)
    ap.add_argument("--count", type=int, default=10)
    ap.add_argument("--duration", type=float, default=10.0)
    args = ap.parse_args()

    results = {}
    threads = [
        Client(i, args.host, args.port, args.udp_port, args.duration,
               results, expected_count=args.count)
        for i in range(args.count)
    ]
    t0 = time.time()
    for t in threads:
        t.start()
    for t in threads:
        t.join(args.duration + 15)

    print(json.dumps({"duration_s": round(time.time() - t0, 1),
                      "clients": results}, indent=1))


if __name__ == "__main__":
    main()
