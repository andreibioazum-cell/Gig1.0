#!/usr/bin/env bash
# Проверка онлайн-слоя: поднимает заглушку Firebase и сводит в комнате
# двух клиентов. Каждый должен увидеть координаты, поворот, ХП и пулю
# соседа. Настоящая база отвечает так же, только по HTTPS.
set -u

cd "$(dirname "$0")/.."
PORT="${PORT:-8765}"
URL="http://127.0.0.1:$PORT"
BIN="${TMPDIR:-/tmp}/ds_test_net"

cleanup() {
    [ -n "${MOCK_PID:-}" ] && kill "$MOCK_PID" 2>/dev/null
    wait "$MOCK_PID" 2>/dev/null
}
trap cleanup EXIT

echo "== сборка теста =="
gcc -O1 -Wall -Wextra -o "$BIN" tests/test_net.c net.c -lpthread -lm || exit 1

echo "== заглушка Firebase на порту $PORT =="
python3 tests/mock_firebase.py "$PORT" &
MOCK_PID=$!
sleep 1

# Комнату чистим, чтобы прошлый прогон не мешал занять слоты.
curl -s -X DELETE "$URL/rooms/test.json" >/dev/null 2>&1

"$BIN" "$URL" a > "${TMPDIR:-/tmp}/net_a.log" 2>&1 &
A_PID=$!
sleep 0.3
"$BIN" "$URL" b > "${TMPDIR:-/tmp}/net_b.log" 2>&1 &
B_PID=$!

wait "$A_PID"; A_RC=$?
wait "$B_PID"; B_RC=$?

cat "${TMPDIR:-/tmp}/net_a.log"
cat "${TMPDIR:-/tmp}/net_b.log"

if [ "$A_RC" -eq 0 ] && [ "$B_RC" -eq 0 ]; then
    echo "ИТОГ: онлайн работает, оба клиента видят друг друга"
    exit 0
fi
echo "ИТОГ: тест провален (a=$A_RC b=$B_RC)"
exit 1
