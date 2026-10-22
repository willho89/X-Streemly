#!/usr/bin/env python3
# X-Streemly — local HTTP(S) test server (CLAUDE.md §7 M0, §8).
#
#   python3 -I tests/server.py <root> [--port N] [--tls CERT KEY]
#
# Prints "PORT <n>" once it is listening, then one line per request:
# "REQ <method> <path> <status>".
#
# Serves <root> as static files, plus:
#   .../live.m3u8   a live HLS media playlist built from the vod.m3u8 next to
#                   it: a sliding window of 6 segments that advances in real
#                   time and loops forever, with EXT-X-DISCONTINUITY at each
#                   loop (media sequence and discontinuity sequence kept right)
#
# Fault injection, as path prefixes (they nest, and relative URLs inside a
# playlist inherit them, so a whole stream is affected):
#   /_delay/<ms>/...    wait before answering
#   /_status/<code>/... answer with that HTTP status and no body
#   /_stall/<ms>/...    send headers and half the body, wait, send the rest
#   /_drop/...          send headers and half the body, then close
import argparse
import os
import re
import socket
import ssl
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

WINDOW = 6
START = time.monotonic()

TYPES = {
    ".m3u8": "application/vnd.apple.mpegurl",
    ".m3u": "audio/x-mpegurl",
    ".ts": "video/mp2t",
    ".mp4": "video/mp4",
    ".m4s": "video/iso.segment",
    ".aac": "audio/aac",
    ".json": "application/json",
}


def parse_vod(path):
    """[(duration, uri)], target duration of a VOD media playlist."""
    segs, dur, target = [], None, 2
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line.startswith("#EXT-X-TARGETDURATION:"):
                target = int(line.split(":", 1)[1])
            elif line.startswith("#EXTINF:"):
                dur = float(line[8:].split(",", 1)[0])
            elif line and not line.startswith("#") and dur is not None:
                segs.append((dur, line))
                dur = None
    return segs, target


def live_playlist(vod_path):
    segs, target = parse_vod(vod_path)
    n = len(segs)
    total = sum(d for d, _ in segs)
    elapsed = time.monotonic() - START
    loops, rem = divmod(elapsed, total)
    i = 0
    while i < n - 1 and rem >= segs[i][0]:
        rem -= segs[i][0]
        i += 1
    # Absolute index of the newest complete segment; pretend the stream has
    # been live for a while so the window is full from the start.
    newest = int(loops) * n + i + WINDOW
    first = newest - WINDOW + 1
    disc_seq = (first - 1) // n if first > 0 else 0
    out = ["#EXTM3U", "#EXT-X-VERSION:3", f"#EXT-X-TARGETDURATION:{target}",
           f"#EXT-X-MEDIA-SEQUENCE:{first}", f"#EXT-X-DISCONTINUITY-SEQUENCE:{disc_seq}"]
    for a in range(first, newest + 1):
        if a > 0 and a % n == 0:
            out.append("#EXT-X-DISCONTINUITY")
        d, uri = segs[a % n]
        out.append(f"#EXTINF:{d:.6f},")
        out.append(uri)
    return ("\n".join(out) + "\n").encode()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    root = "."

    def log_message(self, fmt, *args):
        pass

    def log_req(self, code):
        print(f"REQ {self.command} {self.path} {code}", flush=True)

    def do_HEAD(self):
        self.handle_req(head=True)

    def do_GET(self):
        self.handle_req(head=False)

    def handle_req(self, head):
        path = self.path.split("?", 1)[0]
        stall = drop = None
        while True:
            m = re.match(r"^/_(delay|status|stall)/(\d+)(/.*)$", path) or re.match(r"^/_(drop)()(/.*)$", path)
            if not m:
                break
            kind, arg, path = m.group(1), m.group(2), m.group(3)
            if kind == "delay":
                time.sleep(int(arg) / 1000)
            elif kind == "status":
                code = int(arg)
                self.send_response(code)
                self.send_header("Content-Length", "0")
                self.end_headers()
                self.log_req(code)
                return
            elif kind == "stall":
                stall = int(arg) / 1000
            else:
                drop = True

        rel = os.path.normpath(path.lstrip("/"))
        if rel.startswith(".."):
            return self.fail(403)
        full = os.path.join(self.root, rel)
        if os.path.basename(full) == "live.m3u8":
            vod = os.path.join(os.path.dirname(full), "vod.m3u8")
            if not os.path.isfile(vod):
                return self.fail(404)
            body = live_playlist(vod)
        elif os.path.isfile(full):
            with open(full, "rb") as f:
                body = f.read()
        else:
            return self.fail(404)

        self.send_response(200)
        self.send_header("Content-Type", TYPES.get(os.path.splitext(full)[1], "application/octet-stream"))
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.log_req(200)
        if head:
            return
        try:
            if stall is not None or drop:
                half = len(body) // 2
                self.wfile.write(body[:half])
                self.wfile.flush()
                if drop:
                    self.close_connection = True
                    self.connection.shutdown(socket.SHUT_RDWR)
                    return
                time.sleep(stall)
                self.wfile.write(body[half:])
            else:
                self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            self.close_connection = True

    def fail(self, code):
        self.send_response(code)
        self.send_header("Content-Length", "0")
        self.end_headers()
        self.log_req(code)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--tls", nargs=2, metavar=("CERT", "KEY"))
    args = ap.parse_args()

    Handler.root = os.path.abspath(args.root)
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    srv.daemon_threads = True
    if args.tls:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(*args.tls)
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    print(f"PORT {srv.server_address[1]}", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
