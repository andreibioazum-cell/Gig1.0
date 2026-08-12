#!/usr/bin/env python3
"""Поддельный соперник для проверки урона.

Занимает слот 1, читает позицию живого клиента из слота 0 и стреляет
точно в него. Настоящий клиент должен сам увидеть попадание и снять
себе жизнь — именно эту логику и проверяем.
"""

import json
import math
import sys
import time
import urllib.request

BASE = sys.argv[1] if len(sys.argv) > 1 else 'http://127.0.0.1:8766'
ROOM = sys.argv[2] if len(sys.argv) > 2 else 'main'
SECONDS = float(sys.argv[3]) if len(sys.argv) > 3 else 12.0


def request(method, path, payload=None):
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(f'{BASE}/rooms/{ROOM}{path}.json', data=data,
                                 method=method,
                                 headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=4) as response:
        body = response.read().decode()
    return json.loads(body) if body else None


def main():
    shot = 0
    seq = 0
    deadline = time.time() + SECONDS
    # Держимся на расстоянии, чтобы пуля летела, а не появлялась внутри игрока.
    while time.time() < deadline:
        seq += 1
        state = request('GET', '') or {}
        players = state.get('players') or {}
        target = players.get('0') or {}
        tx, ty = target.get('x', 0), target.get('y', 0)

        # Стоим в 150 пикселях слева от игрока и целимся прямо в него.
        my_x, my_y = tx - 150, ty
        angle = math.atan2(ty - my_y, tx - my_x)

        request('PUT', '/players/1', {
            'uid': 'fake-opponent', 'x': my_x, 'y': my_y,
            'angle': angle, 'hp': 10, 'alive': 1, 'seq': seq,
        })

        # Новый выстрел раз в ~0.6 с, прямо в игрока.
        if seq % 6 == 0:
            shot += 1
            request('PUT', '/bullets/1', {
                'x': my_x + math.cos(angle) * 46,
                'y': my_y + math.sin(angle) * 46,
                'dx': math.cos(angle), 'dy': math.sin(angle),
                'active': 1, 'shot': shot,
            })
        time.sleep(0.1)
    print(f'поддельный соперник сделал {shot} выстрел(ов)')


if __name__ == '__main__':
    main()
