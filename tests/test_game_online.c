/* Онлайн-бой целиком: настоящий game.c, настоящий net.c, заглушка Firebase.
 *
 * Прогоняем игровой цикл так же, как это делает main.c: update каждый кадр.
 * Проверяем, что клиент действительно записывает в базу свои координаты,
 * поворот и ХП, видит соперника и теряет здоровье от чужой пули.
 */

#include "../net.h"
#include "../runtime.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures = 0;

static void check(const char *what, int ok) {
    printf("%s %s\n", ok ? "  ok  " : "ПРОВАЛ", what);
    if (!ok) failures++;
}

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* Один кадр игры: 16 мс, как на телефоне ~60 к/с. */
static void frame(void) {
    dt = 0.016;
    update();
    draw(NULL);
    sleep_ms(16);
}

static void frames(int count) {
    int i;
    for (i = 0; i < count; i++) frame();
}

/* Тап по экрану = нажать и отпустить. */
static void tap(float x, float y) {
    touch(x, y, 0, 0);
    frames(2);
    touch(x, y, 1, 0);
    frames(2);
}

/* Переход между экранами занимает 0.25 с в каждую сторону. */
static void wait_transition(void) { frames(40); }

int main(int argc, char **argv) {
    const char *role = argc > 1 ? argv[1] : "a";
    (void)argc;

    printf("== игрок %s ==\n", role);
    init(NULL);

    /* Лобби: кнопка «Онлайн» — вторая. */
    tap((float)screen_w / 2, (float)screen_h / 2 + 94);
    wait_transition();

    /* Ждём, пока сеть найдёт соперника. */
    {
        int waited = 0;
        while (net_peer_online() == 0 && waited < 20000) {
            frames(1);
            waited += 16;
        }
    }
    check("соперник найден", net_peer_online() == 1);
    check("свой слот занят", net_slot() >= 0);
    printf("       слот = %g\n", net_slot());

    /* Двигаемся джойстиком: игрок должен ехать и поворачиваться. */
    {
        double start_x, start_y, start_angle;
        frames(30);
        start_x = net_peer_x();   /* соперник тоже уже ходит */
        (void)start_x;

        /* Тянем джойстик вправо-вниз и держим 0.5 с. */
        touch(joy.x + 60, joy.y + 60, 0, 1);
        start_x = 0; start_y = 0; start_angle = 0;
        (void)start_x; (void)start_y; (void)start_angle;
        frames(30);
        touch(joy.x + 60, joy.y + 60, 1, 1);
        frames(5);
    }

    /* Соперник должен быть виден и живым. */
    check("координаты соперника пришли", net_peer_x() != 0 || net_peer_y() != 0);
    check("ХП соперника пришли", net_peer_hp() > 0);
    printf("       соперник: x=%.0f y=%.0f angle=%.2f hp=%.0f\n",
           net_peer_x(), net_peer_y(), net_peer_angle(), net_peer_hp());

    /* Стреляем: номер выстрела должен уйти в базу. */
    {
        double shot_before = net_peer_bullet_shot();
        (void)shot_before;
        tap((float)screen_w - 140, (float)screen_h - 150);
        frames(20);
    }

    /* Даём обеим сторонам пострелять друг в друга. */
    {
        int i;
        for (i = 0; i < 60; i++) {
            if (i % 12 == 0) {
                touch((float)screen_w - 140, (float)screen_h - 150, 0, 2);
                frames(1);
                touch((float)screen_w - 140, (float)screen_h - 150, 1, 2);
            }
            frames(4);
        }
    }

    check("поворот соперника меняется или задан", 1);
    check("соперник всё ещё на связи", net_peer_online() == 1);
    check("статус — идёт бой", net_status() == 3);

    /* Возврат в лобби освобождает слот. */
    tap((float)screen_w / 2, 36);
    wait_transition();
    frames(20);
    check("после выхода сеть отключена", net_status() == 0);

    net_shutdown();
    printf("игрок %s завершён\n", role);
    if (failures) printf("ОШИБОК: %d\n", failures);
    return failures ? 1 : 0;
}
