#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <android_native_app_glue.h>
#include "runtime.h"
#include "net.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

/* main.c владеет только окном Android и жизненным циклом скрипта;
 * graphics.c владеет command buffer и растеризацией. */

static int init_done = 0;
static int script_active = 0;
static AAssetManager *script_assets = NULL;
static uint64_t restart_after_ns = 0;
static unsigned int restart_failures = 0;
static unsigned int frame_count = 0;
static uint64_t prev_frame_ns = 0;

void ds_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    __android_log_vprint(ANDROID_LOG_INFO, "DimScript", format, args);
    va_end(args);
}

static uint64_t monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static void protected_init(void *userdata) { init((AAssetManager *)userdata); }
static void protected_reset(void *userdata) { (void)userdata; reset(); }
static void protected_update(void *userdata) { (void)userdata; update(); }
static void protected_draw(void *userdata) { draw((Buffer *)userdata); }

typedef struct { float x; float y; int action; int id; } TouchCall;
static void protected_touch(void *userdata) {
    TouchCall *call = (TouchCall *)userdata;
    touch(call->x, call->y, call->action, call->id);
}

static void mark_script_failed(const char *hook) {
    const char *message = ds_runtime_error_message();
    __android_log_print(ANDROID_LOG_ERROR, "DimScript",
                        "script hook '%s' stopped: %s; scheduling a restart",
                        hook ? hook : "unknown", message);
    {
        unsigned int shift = restart_failures < 5 ? restart_failures : 5;
        uint64_t delay = 1000000000ull << shift;
        script_active = 0;
        ds_request_script_restart();
        restart_after_ns = monotonic_ns() + delay;
        ++restart_failures;
    }
}

static int start_script(int reset_state) {
    int ok;
    ds_clear_runtime_error();
    ds_clear_script_restart();
    ds_string_pool_reset();
    if (reset_state) {
        ok = ds_call_protected(protected_reset, NULL, "reset");
        if (!ok) { mark_script_failed("reset"); return 0; }
    }
    ds_clear_runtime_error();
    ok = ds_call_protected(protected_init, script_assets, "init");
    if (!ok) { mark_script_failed("init"); return 0; }
    ds_clear_runtime_error();
    restart_failures = 0;
    script_active = 1;
    return 1;
}

static void restart_script_if_due(void) {
    uint64_t now;
    if (script_active || !ds_script_restart_requested()) return;
    now = monotonic_ns();
    if (now < restart_after_ns) return;
    (void)start_script(1);
}

static void handle_cmd(struct android_app *app, int32_t command) {
    if (!app) {
        ds_runtime_error("received an Android command without an app instance");
        return;
    }
    switch (command) {
        case APP_CMD_INIT_WINDOW:
            if (!app->window) {
                init_done = 0;
                ds_runtime_error("APP_CMD_INIT_WINDOW arrived without a window");
                return;
            }
            screen_w = ANativeWindow_getWidth(app->window);
            screen_h = ANativeWindow_getHeight(app->window);
            if (screen_w <= 0 || screen_h <= 0) {
                init_done = 0;
                ds_runtime_error("Android returned an invalid window size: %dx%d", screen_w, screen_h);
                return;
            }
            script_assets = app->activity ? app->activity->assetManager : NULL;
            if (ANativeWindow_setBuffersGeometry(app->window, 0, 0, WINDOW_FORMAT_RGBA_8888) != 0) {
                init_done = 0;
                ds_runtime_error("could not select RGBA_8888 software-renderer buffers");
                return;
            }
            if (!ds_graphics_init(script_assets)) {
                init_done = 0;
                ds_runtime_error("could not initialise the software renderer");
                return;
            }
            init_done = 1;
            frame_count = 0;
            script_active = 0;
            restart_failures = 0;
            ds_clear_script_restart();
            (void)start_script(0);
            break;
        case APP_CMD_TERM_WINDOW:
            init_done = 0;
            script_active = 0;
            ds_graphics_shutdown();
            break;
        default:
            break;
    }
}

/* Мультитач: скрипт получает координаты, действие и id пальца.
 * Вторичные пальцы приходят как POINTER_DOWN/UP — приводим их к DOWN/UP,
 * а событие MOVE отдаём отдельно для каждого активного пальца. */
static int32_t handle_input(struct android_app *app, AInputEvent *event) {
    TouchCall call;
    size_t count, index, i;
    int raw, action;
    (void)app;
    if (!script_active || !event || AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) return 0;
    count = AMotionEvent_getPointerCount(event);
    if (count == 0) return 0;
    raw = AMotionEvent_getAction(event);
    action = raw & AMOTION_EVENT_ACTION_MASK;
    if (action == AMOTION_EVENT_ACTION_POINTER_DOWN) action = AMOTION_EVENT_ACTION_DOWN;
    else if (action == AMOTION_EVENT_ACTION_POINTER_UP) action = AMOTION_EVENT_ACTION_UP;
    index = (size_t)((raw & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    if (index >= count) index = 0;
    i = (action == AMOTION_EVENT_ACTION_MOVE) ? 0 : index;
    count = (action == AMOTION_EVENT_ACTION_MOVE) ? count : index + 1;
    for (; i < count; i++) {
        call.x = AMotionEvent_getX(event, i);
        call.y = AMotionEvent_getY(event, i);
        call.action = action;
        call.id = AMotionEvent_getPointerId(event, i);
        if (!ds_call_protected(protected_touch, &call, "touch")) {
            mark_script_failed("touch");
            break;
        }
    }
    return 1;
}

void android_main(struct android_app *app) {
    Buffer frame = {0};
    if (!app) {
        ds_runtime_error("android_main received a null app instance");
        return;
    }
    app->onAppCmd = handle_cmd;
    app->onInputEvent = handle_input;
    /* Сетевой слой ходит в Firebase через java.net.HttpURLConnection,
     * поэтому ему нужна JavaVM этого процесса. */
    net_set_java_vm(app->activity->vm);
    ds_log("DimScript application started");
    for (;;) {
        struct android_poll_source *source = NULL;
        int ident;
        while ((ident = ALooper_pollOnce(script_active ? 0 : 10, NULL, NULL,
                                         (void **)&source)) >= 0) {
            if (source && source->process) source->process(app, source);
            if (app->destroyRequested) {
                init_done = 0;
                script_active = 0;
                ds_graphics_shutdown();
                return;
            }
        }
        if (!app->window || !init_done || app->destroyRequested) continue;
        restart_script_if_due();
        if (script_active) {
            uint64_t now = monotonic_ns();
            dt = prev_frame_ns ? (double)(now - prev_frame_ns) / 1000000000.0 : 0.0;
            if (dt < 0.0) dt = 0.0;
            if (dt > 0.1) dt = 0.1;
            prev_frame_ns = now;
            if (!ds_call_protected(protected_update, NULL, "update")) {
                mark_script_failed("update");
            } else if (ds_script_restart_requested()) {
                script_active = 0;
                restart_after_ns = monotonic_ns();
            }
        }
        {
            ANativeWindow_Buffer native_buffer;
            if (ANativeWindow_lock(app->window, &native_buffer, NULL) == 0) {
                int frame_valid;
                frame.pixels = (uint32_t *)native_buffer.bits;
                frame.width = native_buffer.width;
                frame.height = native_buffer.height;
                frame.stride = native_buffer.stride;
                frame_valid = frame.pixels && frame.width > 0 && frame.height > 0 &&
                              frame.stride >= frame.width &&
                              native_buffer.format == WINDOW_FORMAT_RGBA_8888;
                if (frame_valid && ds_graphics_begin_frame(&frame)) {
                    int draw_failed = 0;
                    if (script_active) {
                        if (!ds_call_protected(protected_draw, &frame, "draw")) {
                            mark_script_failed("draw");
                            draw_failed = 1;
                        } else if (ds_script_restart_requested()) {
                            script_active = 0;
                            restart_after_ns = monotonic_ns();
                        }
                    }
                    if (!script_active) {
                        if (draw_failed || ds_script_has_error()) {
                            ds_graphics_error_screen(ds_runtime_error_message());
                        }
                        ds_graphics_cancel_frame();
                    } else {
                        ds_graphics_end_frame();
                    }
                } else {
                    ds_runtime_error("Android supplied an invalid software framebuffer");
                }
                ANativeWindow_unlockAndPost(app->window);
                ++frame_count;
            } else if ((frame_count % 60u) == 0u) {
                ds_runtime_error("ANativeWindow_lock failed");
            }
        }
    }
}

/* graphics.c и net.c встраиваются в main.c, чтобы workflow остался без правок
 * (собираем только main.c + runtime.c + game/game.c). */
#include "graphics.c"
#include "net.c"
