/* Заглушки Android-функций, чтобы гонять логику игры на обычном Linux.
 *
 * Рисование ничего не выводит: тесту важно, что игра считает координаты,
 * поворот, ХП и попадания, а не как это выглядит на экране.
 */

#include "../runtime.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Joy joy = {0};
int screen_w = 1280;
int screen_h = 720;
double dt = 0.016;

static int has_error = 0;
static int restart_requested = 0;
static char last_error[512] = {0};

void ds_log(const char *format, ...) {
    va_list args;
    if (!getenv("DS_DEBUG")) return;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

void ds_runtime_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(last_error, sizeof(last_error), format, args);
    va_end(args);
    has_error = 1;
    fprintf(stderr, "[ошибка скрипта] %s\n", last_error);
}

int ds_call_protected(DSProtectedFunction function, void *userdata, const char *label) {
    (void)label;
    if (!function) return 0;
    function(userdata);
    return !has_error;
}

const char *ds_runtime_error_message(void) { return last_error[0] ? last_error : "нет ошибок"; }
int ds_script_has_error(void) { return has_error; }
void ds_clear_runtime_error(void) { has_error = 0; last_error[0] = '\0'; }
void ds_request_script_restart(void) { restart_requested = 1; }
int ds_script_restart_requested(void) { return restart_requested; }
void ds_clear_script_restart(void) { restart_requested = 0; }

/* Строки: тот же пул, что и в runtime.c, но без лишних деталей. */
static char *pool[4096];
static size_t pool_n = 0;

static char *track(char *string) {
    if (pool_n < sizeof(pool) / sizeof(*pool)) pool[pool_n++] = string;
    return string;
}

void ds_string_pool_reset(void) {
    size_t i;
    for (i = 0; i < pool_n; i++) free(pool[i]);
    pool_n = 0;
}

char *ds_num_to_string(double number) {
    char buffer[96];
    snprintf(buffer, sizeof(buffer), "%g", number);
    return track(strdup(buffer));
}

char *ds_concat(const char *left, const char *right) {
    size_t la = left ? strlen(left) : 0;
    size_t lb = right ? strlen(right) : 0;
    char *out = (char *)malloc(la + lb + 1);
    if (!out) return track(strdup(""));
    if (la) memcpy(out, left, la);
    if (lb) memcpy(out + la, right, lb);
    out[la + lb] = '\0';
    return track(out);
}

/* Рендер: пустышки. */
void rect(float x, float y, float w, float h, uint32_t c) { (void)x;(void)y;(void)w;(void)h;(void)c; }
void roundrect(float x, float y, float w, float h, float r, uint32_t c) { (void)x;(void)y;(void)w;(void)h;(void)r;(void)c; }
void circle(float x, float y, float r, uint32_t c) { (void)x;(void)y;(void)r;(void)c; }
void ring(float x, float y, float r, float t, uint32_t c) { (void)x;(void)y;(void)r;(void)t;(void)c; }
void line(float x1, float y1, float x2, float y2, float t, uint32_t c) { (void)x1;(void)y1;(void)x2;(void)y2;(void)t;(void)c; }
void tex(float x, float y, const char *n, float a, float s) { (void)x;(void)y;(void)n;(void)a;(void)s; }
void text(const char *s, float x, float y, uint32_t c) { (void)s;(void)x;(void)y;(void)c; }
void text_scaled(const char *s, float x, float y, uint32_t c, float sc) { (void)s;(void)x;(void)y;(void)c;(void)sc; }
int text_ink_width(const char *s) { return s ? (int)strlen(s) * 12 : 0; }
int text_ink_height(const char *s) { (void)s; return 20; }
int png_load(const char *n) { (void)n; return 1; }
void ds_set_asset_manager(AAssetManager *a) { (void)a; }
void ds_release_assets(void) {}
void init_stars(int count, uint32_t color) { (void)count; (void)color; }
void update_stars(void) {}
void draw_stars(void) {}
