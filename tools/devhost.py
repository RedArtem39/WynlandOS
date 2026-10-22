#!/usr/bin/env python3
"""
WynlandOS - devhost: the developer machine's side of testing on real hardware.

  * the kernel log of a machine booted with boot.cfg "netlog=THIS_IP:5140"
    (kernel/netlog.c, UDP): written to build/netlog/boot-<time>.log, a new
    file whenever the log falls silent for a while (a reboot), and echoed
  * a directory served over HTTP (port 8765), for new test programs:
    on the machine, `curl -so /tmp/t http://THIS_IP:8765/t && chmod +x /tmp/t && /tmp/t`

Usage: python3 tools/devhost.py [directory to serve, default build/devhost]
Windows asks once whether Python may take connections: allow it (private network).
"""
import http.server
import os
import socket
import sys
import threading
import time

LOG_PORT = 5140
HTTP_PORT = 8765
NEW_BOOT_AFTER = 20.0          # seconds of silence: the next datagram starts a new file


class Handler(http.server.SimpleHTTPRequestHandler):
    """GET: the files; POST /report: a program's output, kept in build/netlog"""
    logs = ''

    def do_POST(self) -> None:
        n = int(self.headers.get('Content-Length') or 0)
        body = self.rfile.read(n) if n else b''
        name = os.path.join(self.logs, time.strftime('report-%Y%m%d-%H%M%S.txt'))
        with open(name, 'wb') as f:
            f.write(body)
        print(f'\n== a report from {self.client_address[0]}: {name}\n' + body.decode('utf-8', 'replace'), flush=True)
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b'ok\n')


def serve_files(root: str, logs: str) -> None:
    Handler.logs = logs
    handler = lambda *a, **k: Handler(*a, directory=root, **k)
    http.server.ThreadingHTTPServer(('0.0.0.0', HTTP_PORT), handler).serve_forever()


def main() -> None:
    here = os.path.dirname(os.path.abspath(__file__))
    top = os.path.dirname(here)
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(top, 'build', 'devhost')
    logs = os.path.join(top, 'build', 'netlog')
    os.makedirs(root, exist_ok=True)
    os.makedirs(logs, exist_ok=True)
    threading.Thread(target=serve_files, args=(root, logs), daemon=True).start()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('0.0.0.0', LOG_PORT))
    print(f'devhost: log on UDP {LOG_PORT} -> {logs}; files from {root} on http://<this machine>:{HTTP_PORT}/',
          flush=True)
    out, last = None, 0.0
    while True:
        data, peer = sock.recvfrom(65536)
        now = time.time()
        if out is None or now - last > NEW_BOOT_AFTER:
            if out:
                out.close()
            name = os.path.join(logs, time.strftime('boot-%Y%m%d-%H%M%S.log'))
            out = open(name, 'ab')
            print(f'\n== a new boot from {peer[0]}: {name}', flush=True)
            latest = os.path.join(logs, 'latest.txt')
            with open(latest, 'w') as f:
                f.write(name + '\n')
        last = now
        out.write(data)
        out.flush()
        sys.stdout.write(data.decode('utf-8', 'replace'))
        sys.stdout.flush()


if __name__ == '__main__':
    main()
