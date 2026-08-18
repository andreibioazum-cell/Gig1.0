/* test/ai_sim.c — headless AI benchmark for the solo bot.
 *
 * Гоняет настоящий скрипт боя (menu -> solo) без окна и меряет, насколько
 * бот на самом деле опасен: сколько секунд ему нужно на первое попадание и
 * сколько ударов в секунду он проводит против разных «игроков»:
 *
 *   idle   — игрок стоит на месте и ничего не делает (главная жалоба:
 *            «стоишь, а он бьёт через миллион секунд»);
 *   punchy — игрок стоит и спамит удар (бот должен наказывать промахи);
 *   runner — игрок бегает по кругу (бот должен догонять и попадать).
 *
 * Заодно следит за спрайтом бота: в свободном движении (state 0/3/4) угол
 * спрайта обязан совпадать с вектором скорости (смотрит, куда идёт, а не
 * пялится на игрока), а сам угол меняется плавно — без мгновенных скачков.
 * Разворот к игроку допустим только в замахе/ударе (state 1/2), иначе цель
 * удара не совпадёт с проверкой попадания.
 *
 * Сборка: см. test/run_ai_sim.sh
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "runtime.h"

extern double game_state, t_dir;
extern void *player, *enemy;
extern double finished;

/* Поля объектов идут в порядке объявления из entities.ds */
#define P_X 0
#define P_Y 1
#define P_HP 4
#define E_X 0
#define E_Y 1
#define E_HP 3
#define E_ANGLE 5
#define E_STATE 6
#define E_MVX 19
#define E_MVY 20

static uint32_t *g_pixels = NULL;
static int g_w = 1280, g_h = 720;

static void protected_init(void *u) { (void)u; init(NULL); }
static void protected_update(void *u) { (void)u; update(); }
static void protected_draw(void *u) { draw((Buffer *)u); }
typedef struct { float x, y; int action, id; } TouchCall;
static void protected_touch(void *u) { TouchCall *c = (TouchCall *)u; touch(c->x, c->y, c->action, c->id); }

static int script_active = 0;

static void run_frames(int n) {
    for (int i = 0; i < n; i++) {
        dt = 1.0 / 60.0;
        if (script_active && !ds_call_protected(protected_update, NULL, "update")) {
            printf("UPDATE FAILED: %s\n", ds_runtime_error_message());
            script_active = 0; return;
        }
        Buffer buf = { g_pixels, g_w, g_h, g_w };
        if (!ds_graphics_begin_frame(&buf)) return;
        if (script_active && !ds_call_protected(protected_draw, &buf, "draw")) {
            printf("DRAW FAILED: %s\n", ds_runtime_error_message());
            script_active = 0;
        }
        ds_graphics_end_frame();
    }
}

static void do_tap(float x, float y) {
    TouchCall c; c.x = x; c.y = y; c.action = 0; c.id = 1;
    ds_call_protected(protected_touch, &c, "touch");
    c.action = 1;
    ds_call_protected(protected_touch, &c, "touch");
}

static void touch_at(float x, float y, int action, int id) {
    TouchCall c; c.x = x; c.y = y; c.action = action; c.id = id;
    ds_call_protected(protected_touch, &c, "touch");
}

static int wait_state(double want, int max_iters) {
    for (int i = 0; i < max_iters; i++) {
        run_frames(2);
        if (game_state == want && t_dir == 0) return 1;
    }
    return 0;
}

static int goto_solo(void) {
    do_tap((float)(g_w / 2), (float)(g_h / 2));         /* lobby: Играть */
    if (!wait_state(2, 200)) return 0;
    do_tap((float)(g_w / 2), (float)(g_h / 2));         /* modes: Соло */
    if (!wait_state(1, 200)) return 0;
    return 1;
}

static int enter_solo(void) {
    ds_clear_runtime_error(); ds_clear_script_restart(); ds_string_pool_reset();
    if (!ds_call_protected(protected_init, NULL, "init")) return 0;
    script_active = 1;
    run_frames(4);
    return goto_solo();
}

/* Раунд закончился: тап возвращает в лобби, оттуда снова заходим в соло. */
static int restart_round(void) {
    do_tap((float)(g_w / 2), (float)(g_h / 2));
    if (!wait_state(0, 200)) return 0;
    return goto_solo();
}

typedef struct {
    const char *name;
    double first_hit;   /* сек до первого попадания бота */
    double hits;        /* попаданий бота за прогон */
    double taken;       /* попаданий игрока по боту */
    double seconds;
    int bot_wins, player_wins;
    int move_frames;    /* кадры свободного движения бота (state 0/3/4) */
    int face_aligned;   /* из них — голова по вектору скорости */
    double worst_jerk;  /* макс. скачок угла за кадр внутри одного состояния */
} Result;

static double wrapd(double a) {
    const double pi = 3.14159265358979323846;
    while (a > pi) a -= 2 * pi;
    while (a < -pi) a += 2 * pi;
    return a;
}

/* mode: 0 idle, 1 punchy, 2 runner, 3 chaser (идёт на бота и бьёт в упор) */
static Result run_case(const char *name, int mode, double seconds) {
    Result r; memset(&r, 0, sizeof(r));
    r.name = name; r.first_hit = -1; r.seconds = seconds;
    double *pl = (double *)player, *en = (double *)enemy;
    double php = pl[P_HP], ehp = en[E_HP];
    int frames = (int)(seconds * 60.0);
    int atk_x = g_w - 140, atk_y = g_h - 150;
    double prev_angle = en[E_ANGLE], prev_st = -1;
    for (int f = 0; f < frames; f++) {
        if (mode == 1 && f % 24 == 0) do_tap((float)atk_x, (float)atk_y);
        if (mode == 3) {
            /* игрок идёт на бота (джойстик в его сторону) и бьёт в упор */
            double ex = en[E_X] - pl[P_X], ey = en[E_Y] - pl[P_Y];
            double dd = sqrt(ex * ex + ey * ey); if (dd < 1) dd = 1;
            touch_at((float)(130 + 60 * ex / dd), (float)(g_h - 150 + 60 * ey / dd), f == 0 ? 0 : 2, 7);
            if (dd < 110) do_tap((float)atk_x, (float)atk_y);
        }
        if (mode == 2) {
            /* держим джойстик: игрок кружит по арене */
            double a = f * 0.02;
            touch_at((float)(130 + 60 * cos(a)), (float)(g_h - 150 + 60 * sin(a)), f == 0 ? 0 : 2, 7);
            if (f % 30 == 0) do_tap((float)atk_x, (float)atk_y);
        }
        run_frames(1);
        if (!script_active) break;
        {
            double st = en[E_STATE], ang = en[E_ANGLE];
            double mvx = en[E_MVX], mvy = en[E_MVY];
            double sp = sqrt(mvx * mvx + mvy * mvy);
            if ((st == 0 || st == 3 || st == 4) && sp > 10) {
                r.move_frames++;
                if (fabs(wrapd(ang - atan2(mvy, mvx))) < 0.5) r.face_aligned++;
                if (st == prev_st) {
                    double jump = fabs(wrapd(ang - prev_angle));
                    if (jump > r.worst_jerk) r.worst_jerk = jump;
                }
            }
            prev_angle = ang; prev_st = st;
        }
        if (pl[P_HP] < php) {
            r.hits += php - pl[P_HP];
            php = pl[P_HP];
            if (r.first_hit < 0) r.first_hit = f / 60.0;
        }
        if (en[E_HP] < ehp) { r.taken += ehp - en[E_HP]; ehp = en[E_HP]; }
        if (finished != 0) { /* новый раунд: перезапускаем бой */
            if (finished == 2) r.bot_wins++; else r.player_wins++;
            if (mode >= 2) touch_at((float)130, (float)(g_h - 150), 1, 7);
            if (!restart_round()) break;
            pl = (double *)player; en = (double *)enemy;
            php = pl[P_HP]; ehp = en[E_HP];
            /* между боями лобби-кадры: угол со старого боя сравнивать нельзя */
            prev_angle = en[E_ANGLE]; prev_st = -1;
        }
    }
    if (mode >= 2) touch_at((float)130, (float)(g_h - 150), 1, 7);
    return r;
}

int main(void) {
    g_pixels = (uint32_t *)calloc((size_t)g_w * g_h, 4);
    screen_w = g_w; screen_h = g_h;
    if (!enter_solo()) { printf("cannot enter solo battle\n"); return 1; }
    Result rs[4];
    rs[0] = run_case("idle  ", 0, 20.0);
    rs[1] = run_case("punchy", 1, 20.0);
    rs[2] = run_case("runner", 2, 20.0);
    rs[3] = run_case("chaser", 3, 60.0);
    printf("%-8s %10s %12s %14s %10s %13s %11s\n", "case", "first hit", "bot hits/s",
           "player hits/s", "rounds W-L", "face aligned", "angle jerk");
    for (int i = 0; i < 4; i++) {
        printf("%-8s %9.2fs %12.2f %14.2f %7d-%d %12.0f%% %9.1fd\n", rs[i].name,
               rs[i].first_hit, rs[i].hits / rs[i].seconds, rs[i].taken / rs[i].seconds,
               rs[i].bot_wins, rs[i].player_wins,
               rs[i].move_frames ? 100.0 * rs[i].face_aligned / rs[i].move_frames : 0.0,
               rs[i].worst_jerk * 180.0 / 3.14159265358979323846);
    }
    /* Спрайт бота в движении смотрит по вектору скорости (не на игрока) и
     * поворачивается плавно: worst_jerk ограничен шагом enemy_face_speed —
     * pi*14/60 ~= 42 градуса за кадр (первый шаг разворота при обходе). */
    for (int i = 0; i < 4; i++) {
        if (rs[i].move_frames > 50) {
            double ratio = (double)rs[i].face_aligned / rs[i].move_frames;
            if (ratio < 0.7) {
                printf("!! bot faces its movement only %.0f%% of the time (%s)\n", ratio * 100, rs[i].name);
                return 1;
            }
            if (rs[i].worst_jerk > 0.8) {
                printf("!! bot rotation snaps: worst per-frame jump %.2f rad (%s)\n",
                       rs[i].worst_jerk, rs[i].name);
                return 1;
            }
        }
    }
    printf("=== bot faces its movement direction, rotation is smooth\n");
    reset();
    free(g_pixels);
    return 0;
}
