/* Проверка онлайн-слоя на локальной заглушке Firebase.
 *
 * Два клиента в одном процессе не запустить (net.c держит одно глобальное
 * соединение), поэтому тест запускается дважды: ролью "a" и ролью "b",
 * а сверяет их скрипт tests/run_net_test.sh.
 */

#include "../net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int failures = 0;

static void check(const char *what, int ok) {
    printf("%s %s\n", ok ? "  ok  " : "ПРОВАЛ", what);
    if (!ok) failures++;
}

/* Ждём, пока условие станет истинным, но не дольше timeout_ms. */
#define WAIT_FOR(condition, timeout_ms)                    \
    do {                                                   \
        int waited = 0;                                    \
        while (!(condition) && waited < (timeout_ms)) {    \
            sleep_ms(50);                                  \
            waited += 50;                                  \
        }                                                  \
    } while (0)

int main(int argc, char **argv) {
    const char *url = argc > 1 ? argv[1] : "http://127.0.0.1:8765";
    const char *role = argc > 2 ? argv[2] : "a";
    int is_a = strcmp(role, "a") == 0;
    double x = is_a ? 100 : 700;
    double y = is_a ? 200 : 400;
    double angle = is_a ? 1.5 : -2.25;
    double hp = is_a ? 7 : 3;

    printf("== клиент %s ==\n", role);
    net_connect(url, "test");

    WAIT_FOR(net_slot() >= 0, 5000);
    check("слот в комнате занят", net_slot() >= 0);
    printf("       слот = %g\n", net_slot());

    /* Публикуем своё состояние: координаты, поворот, ХП. */
    net_publish(x, y, angle, hp, 1);
    net_publish_bullet(x + 10, y - 5, 0.6, -0.8, 1, is_a ? 4 : 9);

    WAIT_FOR(net_peer_online() == 1, 15000);
    check("второй игрок виден", net_peer_online() == 1);

    /* Держим связь, пока сосед читает наши данные. */
    {
        int i;
        for (i = 0; i < 40; i++) {
            net_publish(x, y, angle, hp, 1);
            net_publish_bullet(x + 10, y - 5, 0.6, -0.8, 1, is_a ? 4 : 9);
            sleep_ms(50);
        }
    }

    {
        double peer_x = is_a ? 700 : 100;
        double peer_y = is_a ? 400 : 200;
        double peer_angle = is_a ? -2.25 : 1.5;
        double peer_hp = is_a ? 3 : 7;
        double peer_shot = is_a ? 9 : 4;

        printf("       сосед: x=%g y=%g angle=%g hp=%g\n",
               net_peer_x(), net_peer_y(), net_peer_angle(), net_peer_hp());

        check("координата x соседа", net_peer_x() == peer_x);
        check("координата y соседа", net_peer_y() == peer_y);
        check("поворот соседа", net_peer_angle() - peer_angle < 0.001 &&
                                peer_angle - net_peer_angle() < 0.001);
        check("ХП соседа", net_peer_hp() == peer_hp);
        check("сосед жив", net_peer_alive() == 1);
        check("пуля соседа летит", net_peer_bullet_active() == 1);
        check("номер выстрела соседа", net_peer_bullet_shot() == peer_shot);
        check("статус — идёт бой", net_status() == 3);
        check("net_online", net_online() == 1);
    }

    net_disconnect();
    check("после отключения статус offline", net_status() == 0);

    printf(is_a ? "клиент a завершён\n" : "клиент b завершён\n");
    if (failures) printf("ОШИБОК: %d\n", failures);
    return failures ? 1 : 0;
}
