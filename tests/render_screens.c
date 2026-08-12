/* Рендер экранов в PNG настоящим растеризатором игры.
 *
 * Подменяем только доступ к ассетам (на Android — AAssetManager,
 * здесь — обычные файлы из game/assets), а рисование, шрифт и вся
 * логика экранов берутся из реального кода игры.
 *
 * Сборка и запуск — см. tests/run_ui_test.sh.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"

/* --- Ассеты поверх файловой системы -------------------------------- */

struct AAsset {
    unsigned char *data;
    long size;
    long pos;
};

/* Корень ассетов: переменная DS_ASSETS или game/assets рядом с запуском. */
static const char *assets_root(void) {
    const char *env = getenv("DS_ASSETS");
    return (env && *env) ? env : "game/assets";
}

AAsset *AAssetManager_open(AAssetManager *mgr, const char *name, int mode) {
    char path[512];
    FILE *f;
    AAsset *a;
    (void)mgr;
    (void)mode;
    snprintf(path, sizeof(path), "%s/%s", assets_root(), name);
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "не открылся ассет: %s\n", path);
        return NULL;
    }
    a = (AAsset *)calloc(1, sizeof(*a));
    if (!a) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_END);
    a->size = ftell(f);
    fseek(f, 0, SEEK_SET);
    a->data = (unsigned char *)malloc((size_t)a->size);
    if (!a->data || fread(a->data, 1, (size_t)a->size, f) != (size_t)a->size) {
        free(a->data); free(a); fclose(f); return NULL;
    }
    fclose(f);
    return a;
}

off_t AAsset_getLength(AAsset *a) { return a ? (off_t)a->size : 0; }

int AAsset_read(AAsset *a, void *buf, size_t count) {
    long left;
    if (!a) return 0;
    left = a->size - a->pos;
    if (left <= 0) return 0;
    if ((long)count > left) count = (size_t)left;
    memcpy(buf, a->data + a->pos, count);
    a->pos += (long)count;
    return (int)count;
}

void AAsset_close(AAsset *a) {
    if (!a) return;
    free(a->data);
    free(a);
}

/* --- Сохранение кадра в PNG ---------------------------------------- */

extern void ds_set_asset_manager(AAssetManager *assets);

/* Минимальный PNG-писатель: zlib-поток "stored" + CRC. Зависимостей нет,
 * а картинку открывает любой просмотрщик. */
static unsigned long crc_table[256];
static void crc_init(void) {
    unsigned long c; int n, k;
    for (n = 0; n < 256; n++) {
        c = (unsigned long)n;
        for (k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320UL ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
static unsigned long crc_up(unsigned long c, const unsigned char *b, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) c = crc_table[(c ^ b[i]) & 0xff] ^ (c >> 8);
    return c;
}
static void be32(unsigned char *p, unsigned long v) {
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}
static void chunk(FILE *f, const char *tag, const unsigned char *data, size_t len) {
    unsigned char hdr[4];
    unsigned long c;
    be32(hdr, (unsigned long)len);
    fwrite(hdr, 1, 4, f);
    fwrite(tag, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    c = crc_up(0xffffffffUL, (const unsigned char *)tag, 4);
    if (len) c = crc_up(c, data, len);
    be32(hdr, c ^ 0xffffffffUL);
    fwrite(hdr, 1, 4, f);
}
static void write_png(const char *path, const unsigned char *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    unsigned char ihdr[13], *raw, *z;
    size_t raw_len, z_len, pos = 0, i;
    unsigned long a = 1, b = 0;
    int y;
    if (!f) return;
    crc_init();
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    be32(ihdr, (unsigned long)w); be32(ihdr + 4, (unsigned long)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);

    raw_len = (size_t)h * ((size_t)w * 3 + 1);
    raw = (unsigned char *)malloc(raw_len);
    for (y = 0; y < h; y++) {
        raw[pos++] = 0;
        memcpy(raw + pos, rgb + (size_t)y * w * 3, (size_t)w * 3);
        pos += (size_t)w * 3;
    }
    for (i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521; b = (b + a) % 65521;
    }
    z_len = 2 + raw_len + 5 * ((raw_len + 65534) / 65535) + 4;
    z = (unsigned char *)malloc(z_len);
    pos = 0; z[pos++] = 0x78; z[pos++] = 0x01;
    for (i = 0; i < raw_len; ) {
        size_t n = raw_len - i > 65535 ? 65535 : raw_len - i;
        z[pos++] = (i + n >= raw_len) ? 1 : 0;
        z[pos++] = (unsigned char)(n & 0xff); z[pos++] = (unsigned char)(n >> 8);
        z[pos++] = (unsigned char)(~n & 0xff); z[pos++] = (unsigned char)((~n >> 8) & 0xff);
        memcpy(z + pos, raw + i, n); pos += n; i += n;
    }
    be32(z + pos, (b << 16) | a); pos += 4;
    chunk(f, "IDAT", z, pos);
    chunk(f, "IEND", NULL, 0);
    fclose(f); free(raw); free(z);
}

static uint32_t *pixels;

static void save(const char *path) {
    /* Кадр лежит в BGRA/XRGB, PNG хочет RGBA. */
    int i, n = screen_w * screen_h;
    unsigned char *rgb = (unsigned char *)malloc((size_t)n * 3);
    for (i = 0; i < n; i++) {
        uint32_t p = pixels[i];
        rgb[i * 3 + 0] = (unsigned char)(p & 0xFF);
        rgb[i * 3 + 1] = (unsigned char)((p >> 8) & 0xFF);
        rgb[i * 3 + 2] = (unsigned char)((p >> 16) & 0xFF);
    }
    write_png(path, rgb, screen_w, screen_h);
    free(rgb);
    if (ds_script_has_error()) {
        printf("  ВНИМАНИЕ: %s\n", ds_runtime_error_message());
    }
    printf("сохранено: %s\n", path);
}

static void frames(int n) {
    Buffer b;
    int i;
    for (i = 0; i < n; i++) {
        dt = 0.016;
        update();
        b.pixels = pixels;
        b.width = screen_w;
        b.height = screen_h;
        b.stride = screen_w;
        /* Команды рисования копятся в буфере и растеризуются в end_frame. */
        if (ds_graphics_begin_frame(&b)) {
            draw(&b);
            ds_graphics_end_frame();
        }
    }
}

static void tap(float x, float y) {
    touch(x, y, 0, 1);
    frames(2);
    touch(x, y, 1, 1);
    frames(2);
}

int main(void) {
    screen_w = 1280;
    screen_h = 720;
    dt = 0.016;
    pixels = (uint32_t *)calloc((size_t)screen_w * screen_h, 4);

    /* init() сам ставит менеджер ассетов из аргумента, поэтому передаём
     * ненулевой описатель: реальные файлы читает наш AAssetManager_open. */
    ds_graphics_init((AAssetManager *)1);
    init((AAssetManager *)1);
    frames(5);
    save("/tmp/shot_lobby_ru.png");

    /* Настройки, русский. */
    tap((float)screen_w / 2, (float)screen_h / 2 + 142);
    frames(45);
    save("/tmp/shot_settings_ru.png");

    /* Переключаем на English — экран тот же, подписи другие. */
    tap((float)screen_w / 2 + 130, (float)screen_h / 2);
    frames(6);
    save("/tmp/shot_settings_en.png");

    /* Возврат в лобби: теперь оно английское. */
    tap((float)screen_w / 2, (float)screen_h / 2 + 114);
    frames(45);
    save("/tmp/shot_lobby_en.png");

    /* Бой на английском. */
    tap((float)screen_w / 2, (float)screen_h / 2 - 42);
    frames(45);
    save("/tmp/shot_battle_en.png");

    free(pixels);
    return 0;
}
