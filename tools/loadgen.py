#!/usr/bin/env python3
"""Headless load generator for ctf_server (README §10 integration load).

Spawns N protocol-speaking clients over loopback: TCP handshake, host
START_REQUEST, then every client sends UDP_HELLO and PLAYER_INPUT at ~30 Hz
while recording WORLD_SNAPSHOT arrivals. Prints per-client stats as JSON so
the poll-vs-epoll comparison can be scripted.
"""

import argparse
import json
import socket
import struct
import threading
import time

MAGIC = 0x4346
VERSION = 1

JOIN_LOBBY, JOIN_ACCEPT, JOIN_REJECT = 1, 2, 3
LOBBY_STATE, START_REQUEST, GAME_START = 4, 5, 6
UDP_HELLO, PLAYER_INPUT, WORLD_SNAPSHOT = 7, 8, 9


def udp_header(mtype, tick):
    return struct.pack(">HBBI", MAGIC, VERSION, mtype, tick)


def tcp_frame(mtype, payload):
    return struct.pack(">HB", len(payload), mtype) + payload


class Client(threading.Thread):
    def __init__(self, idx, host, tcp_port, udp_port, duration, results):
        super().__init__(daemon=True)
        self.idx = idx
        self.host = host
        self.tcp_port = tcp_port
        self.udp_port = udp_port
        self.duration = duration
        self.results = results

    def run(self):
        info = {"snapshots": 0, "tcp_frames": {}, "other_udp": {}}
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
                d = tcp.recv(256)
                if not d:
                    break
                acc += d
                if len(acc) >= 10 and acc[2] == JOIN_ACCEPT:
                    plen = struct.unpack(">H", acc[:2])[0]
                    if len(acc) >= 3 + plen:
                        player_id, token, _u = struct.unpack(
                            ">BIH", acc[3:10])
                        break
            if player_id is None:
                info["error"] = "no JOIN_ACCEPT"
                self.results[self.idx] = info
                return

            udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            udp.bind(("127.0.0.1", 0))
            udp.settimeout(0.05)

            hello = udp_header(UDP_HELLO, 0) + struct.pack(
                ">BI", player_id, token)
            for _ in range(25):
                udp.sendto(hello, (self.host, self.udp_port))
                time.sleep(0.02)

            # Host starts the match once everyone has joined.
            if self.idx == 0:
                time.sleep(0.6)
                tcp.sendall(tcp_frame(START_REQUEST, b""))

            seq_holder = [0]

            def sender():
                while not stop.is_set():
                    s = seq_holder[0]
                    body = (struct.pack(">BIIB", player_id, token, s, 1) +
                            struct.pack(">BH", 1 << (s % 4),
                                        s * 97 % 65536))
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
                        mtype = data[3]
                        if mtype == WORLD_SNAPSHOT:
                            snap_times.append(time.time())
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
                            info["tcp_frames"][tcp_acc[2]] = \
                                info["tcp_frames"].get(tcp_acc[2], 0) + 1
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
               results)
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
