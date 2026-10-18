#!/usr/bin/env python3
"""Tiny dependency-free preview server with CSS hot reload."""
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import json
import os

PREVIEW = Path(__file__).resolve().parent
ROOT = PREVIEW.parents[2]
os.chdir(ROOT)

class Handler(SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path.split('?', 1)[0] == '/__version':
            files = {name: (PREVIEW / name).stat().st_mtime_ns for name in ('style.css', 'app.js', 'index.html')}
            payload = json.dumps({'css': files['style.css'], 'js': files['app.js'], 'html': files['index.html']}).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Length', str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return
        return super().do_GET()

    def log_message(self, fmt, *args):
        if self.path.split('?', 1)[0] not in ('/__version',):
            super().log_message(fmt, *args)

if __name__ == '__main__':
    address = ('127.0.0.1', 8765)
    print(f'PSBOX UI preview: http://{address[0]}:{address[1]}/tools/ps4/ui-preview/  (Ctrl+C to stop)', flush=True)
    ThreadingHTTPServer(address, Handler).serve_forever()
