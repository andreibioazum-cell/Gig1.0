#!/usr/bin/env bash
# Прогон онлайн-тестов на настоящей игровой логике.
#
#   bash tests/run_online_tests.sh
#
# Поднимает mock-Firebase, собирает игру и гоняет три сценария:
#   1. экран настроек и переключение языка (без сети);
#   2. два реальных клиента находят друг друга и видят состояние соперника;
#   3. поддельный соперник расстреливает клиента, у того падает ХП.
set -u

cd "$(dirname "$0")/.."

PORT="${PORT:-8766}"
BASE="http://127.0.0.1:$PORT"
BUILD_DIR="$(mktemp -d)"
MOCK_PID=""
status=0

cleanup() {
    [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null
    rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

echo "== сборка =="
python3 gen.py >/dev/null || { echo "не удалось сгенерировать game.c"; exit 1; }

cat > "$BUILD_DIR/hp_probe.c" <<'EOF'
#include "game/game.c"
double player_hp(void) { return player ? player->hp : -1; }
EOF

# Пробник для настроек: игровые глобалы и переводчик закрыты в game.c,
# поэтому дотягиваемся до них включением файла целиком.
cat > "$BUILD_DIR/settings_probe.c" <<'EOF'
#include "game/game.c"
double game_state_value(void) { return game_state; }
double lang_value(void) { return lang; }
const char *translate(const char *ru, const char *en) { return ds_fn_t(ru, en); }
EOF

gcc -O1 -Wall -Wextra -I. -Itests/android_stub \
    -o "$BUILD_DIR/test_game" \
    tests/test_game_online.c tests/host_stubs.c game/game.c net.c \
    -lpthread -lm || { echo "сборка сквозного теста не удалась"; exit 1; }

gcc -O1 -Wall -Wextra -I. -Itests/android_stub \
    -o "$BUILD_DIR/test_settings" \
    tests/test_settings.c tests/host_stubs.c "$BUILD_DIR/settings_probe.c" net.c \
    -lpthread -lm || { echo "сборка теста настроек не удалась"; exit 1; }

gcc -O1 -Wall -I. -Itests/android_stub \
    -o "$BUILD_DIR/test_dmg" \
    tests/test_online_damage.c tests/host_stubs.c "$BUILD_DIR/hp_probe.c" net.c \
    -lpthread -lm || { echo "сборка теста урона не удалась"; exit 1; }

echo "== проверка компиляции Android-ветки net.c =="
gcc -fsyntax-only -D__ANDROID__ -Wall -Wextra -I. -Itests/android_stub net.c \
    || { echo "Android-ветка net.c не компилируется"; exit 1; }

echo "== mock Firebase на порту $PORT =="
python3 tests/mock_firebase.py "$PORT" >/dev/null 2>&1 &
MOCK_PID=$!
sleep 1

export NET_BASE_URL="$BASE"

echo
echo "== сценарий 1: настройки и выбор языка =="
"$BUILD_DIR/test_settings" || status=1

echo
echo "== сценарий 2: два живых клиента =="
curl -s -X DELETE "$BASE/rooms/main.json" >/dev/null
"$BUILD_DIR/test_game" a > "$BUILD_DIR/a.log" 2>&1 &
pid_a=$!
"$BUILD_DIR/test_game" b > "$BUILD_DIR/b.log" 2>&1 &
pid_b=$!
wait $pid_a || status=1
wait $pid_b || status=1
cat "$BUILD_DIR/a.log" "$BUILD_DIR/b.log"

echo
echo "== сценарий 3: урон от чужой пули =="
curl -s -X DELETE "$BASE/rooms/main.json" >/dev/null
"$BUILD_DIR/test_dmg" > "$BUILD_DIR/dmg.log" 2>&1 &
pid_d=$!
sleep 3
python3 tests/fake_opponent.py "$BASE" main 9 >/dev/null
wait $pid_d || status=1
cat "$BUILD_DIR/dmg.log"

echo
if [ "$status" -eq 0 ]; then
    echo "ВСЕ ОНЛАЙН-ТЕСТЫ ПРОШЛИ"
else
    echo "ЕСТЬ ПРОВАЛЕННЫЕ ТЕСТЫ"
fi
exit "$status"
