/* Проверка урона по сети.
 *
 * Настоящий игровой клиент выходит в онлайн, а поддельный соперник
 * (tests/fake_opponent.py) стреляет прямо в него. Клиент обязан сам
 * увидеть чужую пулю и снять себе жизни — это и проверяем.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "runtime.h"
#include "net.h"

/* Игрок объявлен в сгенерированном game.c — читаем его ХП напрямую. */
typedef struct Player Player;
extern Player *player;
double player_hp(void);


static int failures = 0;

static void check(const char *what, int ok) {
    if (!ok) failures++;
    printf("%s %s\n", ok ? "  ok  " : "ПРОВАЛ", what);
}

static void frames(int n) {
    int i;
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 16 * 1000 * 1000;
    for (i = 0; i < n; i++) {
        dt = 0.016;
        update();
        draw(NULL);
        nanosleep(&ts, NULL);
    }
}

static void tap(float x, float y) {
    touch(x, y, 0, 1);
    frames(2);
    touch(x, y, 1, 1);
    frames(2);
}

int main(void) {
    double hp_start, hp_end;
    int waited;

    screen_w = 1280;
    screen_h = 720;
    dt = 0.016;
    init(NULL);
    frames(5);

    /* Лобби -> Онлайн */
    tap((float)screen_w / 2, (float)screen_h / 2 + 94);
    frames(40);

    /* Ждём, пока сеть отдаст слот и увидит поддельного соперника. */
    for (waited = 0; waited < 600 && !net_peer_online(); waited++) {
        frames(2);
    }
    check("поддельный соперник виден", net_peer_online() == 1);
    check("слот получен", net_slot() >= 0);

    hp_start = player_hp();
    printf("       ХП до обстрела: %.0f\n", hp_start);

    /* Стоим на месте под огнём. */
    frames(430);

    hp_end = player_hp();
    printf("       ХП после обстрела: %.0f\n", hp_end);
    check("чужая пуля сняла жизни", hp_end < hp_start);

    /* Собственное ХП должно уйти в базу — соперник обязан его видеть. */
    printf("       статус сети: %.0f\n", net_status());

    net_disconnect();
    net_shutdown();
    if (failures) printf("ОШИБОК: %d\n", failures);
    else printf("урон по сети работает\n");
    return failures ? 1 : 0;
}
