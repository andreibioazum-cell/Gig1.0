#!/usr/bin/env python3
"""Локальная заглушка Firebase Realtime Database для тестов онлайна.

Понимает то же, что использует net.c: GET/PUT/PATCH/DELETE по путям вида
/rooms/main.json и /rooms/main/players/0.json. PATCH с ключами через слеш
("players/0") обновляет вложенный узел, как это делает настоящая база.
"""

import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DATA = {}
LOCK = threading.Lock()


def split_path(path):
    path = path.split('?', 1)[0]
    if path.endswith('.json'):
        path = path[:-len('.json')]
    return [part for part in path.split('/') if part]


def get_node(parts):
    node = DATA
    for part in parts:
        if not isinstance(node, dict) or part not in node:
            return None
        node = node[part]
    return node


def set_node(parts, value):
    if not parts:
        global DATA
        DATA = value if isinstance(value, dict) else {}
        return
    node = DATA
    for part in parts[:-1]:
        if not isinstance(node.get(part), dict):
            node[part] = {}
        node = node[part]
    if value is None:
        node.pop(parts[-1], None)
    else:
        node[parts[-1]] = value


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def reply(self, payload, status=200):
        body = json.dumps(payload).encode() if payload is not None else b'null'
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def read_body(self):
        length = int(self.headers.get('Content-Length') or 0)
        raw = self.rfile.read(length) if length else b''
        try:
            return json.loads(raw or b'null')
        except json.JSONDecodeError:
            return None

    def do_GET(self):
        with LOCK:
            self.reply(get_node(split_path(self.path)))

    def do_PUT(self):
        value = self.read_body()
        with LOCK:
            set_node(split_path(self.path), value)
        self.reply(value)

    def do_PATCH(self):
        value = self.read_body()
        parts = split_path(self.path)
        with LOCK:
            if isinstance(value, dict):
                for key, item in value.items():
                    set_node(parts + [p for p in key.split('/') if p], item)
        self.reply(value)

    def do_DELETE(self):
        with LOCK:
            set_node(split_path(self.path), None)
        self.reply(None)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    server = ThreadingHTTPServer(('127.0.0.1', port), Handler)
    print(f'mock firebase на порту {port}', flush=True)
    server.serve_forever()


if __name__ == '__main__':
    main()
