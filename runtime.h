#ifndef RUNTIME_H
#define RUNTIME_H

#include <android/asset_manager.h>
#include <android/log.h>
#include <android/native_window.h>
#include <math.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t *pixels;
    int width;
    int height;
    int stride;
} Buffer;

extern int screen_w, screen_h;

/* Секунды с прошлого кадра (обновляет хост, ограничено 0.1 с).
 * Плавные анимации считайте через dt, а не через счётчики кадров:
 * скорость игрового цикла не фиксирована. */
extern double dt;

/* Джойстик: позиция, направление, смещение ручки и радиус. */
typedef struct { float x, y, dx, dy, ox, oy, r; } Joy;
extern Joy joy;

void ds_log(const char *format, ...);

/* Защищённый вызов хуков скрипта. При ошибке управление возвращается сюда,
 * а не в сломанный код. */
typedef void (*DSProtectedFunction)(void *userdata);
int ds_call_protected(DSProtectedFunction function, void *userdata, const char *label);
void ds_runtime_error(const char *format, ...);
const char *ds_runtime_error_message(void);
int ds_script_has_error(void);
void ds_clear_runtime_error(void);
void ds_request_script_restart(void);
int ds_script_restart_requested(void);
void ds_clear_script_restart(void);

/* --- Networking (UDP relay for online multiplayer) --- */

/* Global networking state — readable/writable from DimScript. */
extern double net_state;       /* 0=off 1=connecting 2=server_ok 3=room_waiting 4=peer_ready */
extern double net_am_host;     /* 1=host (created room) 0=guest (joined) */
extern double net_peer_x, net_peer_y, net_peer_angle;
extern double net_peer_bx, net_peer_by, net_peer_bdx, net_peer_bdy;
extern double net_peer_fire;   /* 1 if peer fired this frame */
extern double net_peer_hit;    /* 1 if peer was hit this frame */

void net_connect(const char *host, double port);
void net_close(void);
void net_create(const char *name);
void net_join(const char *name);
void net_send_pos(double x, double y, double angle);
void net_send_bullet(double x, double y, double dx, double dy);
void net_send_hit(void);
void net_poll(void);

char *ds_concat(const char *left, const char *right);
char *ds_num_to_string(double number);
void ds_string_pool_reset(void);

void rect(float x, float y, float w, float h, uint32_t color);
void roundrect(float x, float y, float w, float h, float r, uint32_t color);
void circle(float x, float y, float r, uint32_t color);
void ring(float x, float y, float r, float t, uint32_t color);
/* Толстая линия с альфа-смешиванием — нужна для траектории прицела. */
void line(float x1, float y1, float x2, float y2, float thickness, uint32_t color);

void ds_set_asset_manager(AAssetManager *assets);
void ds_release_assets(void);
int png_load(const char *name);
void tex(float x, float y, const char *name, float angle, float scale);
void text(const char *string, float x, float y, uint32_t color);
void text_scaled(const char *string, float x, float y, uint32_t color, float scale);
int text_ink_width(const char *string);
int text_ink_height(const char *string);

/* Звёзды фона лобби: полёт из верхнего-левого в правый-нижний угол. */
void init_stars(int count, uint32_t color);
void update_stars(void);
void draw_stars(void);

int ds_graphics_init(AAssetManager *assets);
int ds_graphics_begin_frame(Buffer *buffer);
void ds_graphics_end_frame(void);
void ds_graphics_cancel_frame(void);
void ds_graphics_shutdown(void);
void ds_graphics_error_screen(const char *message);

void init(AAssetManager *assets);
void update(void);
void draw(Buffer *buffer);
/* Мультитач: pointer_id — стабильный id пальца внутри жеста;
 * действия: 0 — down, 1 — up, 2 — move, 3 — cancel. */
void touch(float x, float y, int action, int pointer_id);
void reset(void);

#endif
