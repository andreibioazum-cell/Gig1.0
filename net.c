/* Онлайн-слой: обмен состоянием через Firebase Realtime Database (REST).
 *
 * Здесь три части:
 *   1. HTTP-клиент. На Android — java.net.HttpURLConnection через JNI
 *      (даёт HTTPS без OpenSSL и лишних зависимостей), на Linux и прочих
 *      POSIX — обычные сокеты для http:// (используется в тестах).
 *   2. Крошечный разборщик JSON: нужны только числа и строки по пути
 *      вида "players/1/x", полноценный парсер тут ни к чему.
 *   3. Поток синхронизации: раз в 100 мс отправляет своё состояние
 *      и забирает чужое. Игровой цикл в сеть никогда не блокируется.
 */

#include "net.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#include <jni.h>
#define NET_LOG(...) __android_log_print(ANDROID_LOG_INFO, "DimScriptNet", __VA_ARGS__)
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#define NET_LOG(...) do { \
    if (getenv("NET_DEBUG")) { fprintf(stderr, "[net] " __VA_ARGS__); fputc('\n', stderr); } \
} while (0)
#endif

#define NET_URL_SIZE      512
#define NET_BASE_SIZE     256    /* корень базы: URL с запасом на путь */
#define NET_BODY_SIZE     1024
#define NET_RESPONSE_SIZE 4096
#define NET_TICK_MS       100    /* 10 обновлений в секунду */
#define NET_PEER_TIMEOUT  3000   /* молчит дольше — считаем, что вышел */
#define NET_TAKEOVER_MS   5000   /* столько ждём, прежде чем занять чужой слот */

/* ---------------------------------------------------------------- время */

static long long net_now_ms(void) {
    struct timespec ts;
#ifdef CLOCK_MONOTONIC
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
    return (long long)time(NULL) * 1000;
}

static void net_sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------ HTTP: JNI */

#ifdef __ANDROID__

static JavaVM *net_jvm = NULL;

void net_set_java_vm(JavaVM *vm) { net_jvm = vm; }

/* Тело запроса уходит в HttpURLConnection, ответ читается целиком в out.
 * Возвращает HTTP-код или 0, если соединение не удалось. */
static int net_http(const char *method, const char *url, const char *body,
                    char *out, size_t out_size) {
    JNIEnv *env = NULL;
    jclass url_class, conn_class, stream_class;
    jobject url_object, connection, stream;
    jbyteArray buffer;
    jstring jurl, jmethod;
    int status = 0, attached = 0;
    size_t total = 0;

    if (out && out_size) out[0] = '\0';
    if (!net_jvm) return 0;

    if ((*net_jvm)->GetEnv(net_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*net_jvm)->AttachCurrentThread(net_jvm, &env, NULL) != JNI_OK) return 0;
        attached = 1;
    }
    if ((*env)->PushLocalFrame(env, 32) != 0) goto detach;

    url_class = (*env)->FindClass(env, "java/net/URL");
    conn_class = (*env)->FindClass(env, "java/net/HttpURLConnection");
    if (!url_class || !conn_class) goto done;

    jurl = (*env)->NewStringUTF(env, url);
    url_object = (*env)->NewObject(env, url_class,
        (*env)->GetMethodID(env, url_class, "<init>", "(Ljava/lang/String;)V"), jurl);
    if (!url_object || (*env)->ExceptionCheck(env)) goto done;

    connection = (*env)->CallObjectMethod(env, url_object,
        (*env)->GetMethodID(env, url_class, "openConnection", "()Ljava/net/URLConnection;"));
    if (!connection || (*env)->ExceptionCheck(env)) goto done;

    jmethod = (*env)->NewStringUTF(env, method);
    (*env)->CallVoidMethod(env, connection,
        (*env)->GetMethodID(env, conn_class, "setRequestMethod", "(Ljava/lang/String;)V"), jmethod);
    (*env)->CallVoidMethod(env, connection,
        (*env)->GetMethodID(env, conn_class, "setConnectTimeout", "(I)V"), 4000);
    (*env)->CallVoidMethod(env, connection,
        (*env)->GetMethodID(env, conn_class, "setReadTimeout", "(I)V"), 4000);
    (*env)->CallVoidMethod(env, connection,
        (*env)->GetMethodID(env, conn_class, "setUseCaches", "(Z)V"), JNI_FALSE);
    {
        jstring key = (*env)->NewStringUTF(env, "Content-Type");
        jstring value = (*env)->NewStringUTF(env, "application/json");
        (*env)->CallVoidMethod(env, connection,
            (*env)->GetMethodID(env, conn_class, "setRequestProperty",
                                "(Ljava/lang/String;Ljava/lang/String;)V"), key, value);
    }
    if ((*env)->ExceptionCheck(env)) goto done;

    if (body && *body) {
        jobject out_stream;
        jbyteArray data;
        jsize length = (jsize)strlen(body);
        (*env)->CallVoidMethod(env, connection,
            (*env)->GetMethodID(env, conn_class, "setDoOutput", "(Z)V"), JNI_TRUE);
        (*env)->CallVoidMethod(env, connection,
            (*env)->GetMethodID(env, conn_class, "setFixedLengthStreamingMode", "(I)V"), (jint)length);
        out_stream = (*env)->CallObjectMethod(env, connection,
            (*env)->GetMethodID(env, conn_class, "getOutputStream", "()Ljava/io/OutputStream;"));
        if (!out_stream || (*env)->ExceptionCheck(env)) goto done;
        data = (*env)->NewByteArray(env, length);
        (*env)->SetByteArrayRegion(env, data, 0, length, (const jbyte *)body);
        {
            jclass os_class = (*env)->GetObjectClass(env, out_stream);
            (*env)->CallVoidMethod(env, out_stream,
                (*env)->GetMethodID(env, os_class, "write", "([B)V"), data);
            (*env)->CallVoidMethod(env, out_stream,
                (*env)->GetMethodID(env, os_class, "close", "()V"));
        }
        if ((*env)->ExceptionCheck(env)) goto done;
    }

    status = (int)(*env)->CallIntMethod(env, connection,
        (*env)->GetMethodID(env, conn_class, "getResponseCode", "()I"));
    if ((*env)->ExceptionCheck(env)) { status = 0; goto done; }

    stream = (*env)->CallObjectMethod(env, connection,
        (*env)->GetMethodID(env, conn_class,
            status >= 400 ? "getErrorStream" : "getInputStream", "()Ljava/io/InputStream;"));
    if ((*env)->ExceptionCheck(env) || !stream) goto close;

    stream_class = (*env)->GetObjectClass(env, stream);
    buffer = (*env)->NewByteArray(env, 1024);
    for (;;) {
        jint read = (*env)->CallIntMethod(env, stream,
            (*env)->GetMethodID(env, stream_class, "read", "([B)I"), buffer);
        if ((*env)->ExceptionCheck(env) || read <= 0) break;
        if (out && out_size && total + (size_t)read < out_size) {
            (*env)->GetByteArrayRegion(env, buffer, 0, read, (jbyte *)(out + total));
            total += (size_t)read;
            out[total] = '\0';
        }
    }
    (*env)->CallVoidMethod(env, stream,
        (*env)->GetMethodID(env, stream_class, "close", "()V"));

close:
    (*env)->CallVoidMethod(env, connection,
        (*env)->GetMethodID(env, conn_class, "disconnect", "()V"));
done:
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
detach:
    if (attached) (*net_jvm)->DetachCurrentThread(net_jvm);
    return status;
}

#else /* -------------------------------------------------- HTTP: сокеты */

/* Разбор "http://host:port/path" — на POSIX используется тестовым
 * сервером, поэтому TLS здесь не нужен. */
static int net_parse_url(const char *url, char *host, size_t host_size,
                         char *port, size_t port_size, const char **path) {
    const char *p = url, *slash, *colon;
    size_t length;
    if (strncmp(p, "http://", 7) == 0) { p += 7; snprintf(port, port_size, "80"); }
    else if (strncmp(p, "https://", 8) == 0) { p += 8; snprintf(port, port_size, "443"); }
    else return 0;
    slash = strchr(p, '/');
    *path = slash ? slash : "/";
    length = slash ? (size_t)(slash - p) : strlen(p);
    colon = memchr(p, ':', length);
    if (colon) {
        size_t port_length = length - (size_t)(colon - p) - 1;
        if (port_length >= port_size) return 0;
        memcpy(port, colon + 1, port_length);
        port[port_length] = '\0';
        length = (size_t)(colon - p);
    }
    if (length == 0 || length >= host_size) return 0;
    memcpy(host, p, length);
    host[length] = '\0';
    return 1;
}

static int net_http(const char *method, const char *url, const char *body,
                    char *out, size_t out_size) {
    char host[256], port[16], request[NET_URL_SIZE + NET_BODY_SIZE + 256];
    char response[NET_RESPONSE_SIZE];
    const char *path;
    struct addrinfo hints, *list = NULL, *it;
    struct timeval timeout;
    int fd = -1, status = 0, length;
    size_t total = 0;
    char *header_end;

    if (out && out_size) out[0] = '\0';
    if (!net_parse_url(url, host, sizeof(host), port, sizeof(port), &path)) return 0;
    if (strcmp(port, "443") == 0) {
        NET_LOG("на этой платформе поддерживается только http:// (собран без TLS)");
        return 0;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &list) != 0) return 0;
    for (it = list; it; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        timeout.tv_sec = 4; timeout.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(list);
    if (fd < 0) return 0;

    length = snprintf(request, sizeof(request),
                      "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
                      "Content-Type: application/json\r\nContent-Length: %u\r\n\r\n%s",
                      method, path, host, (unsigned)(body ? strlen(body) : 0),
                      body ? body : "");
    if (length <= 0 || (size_t)length >= sizeof(request)) { close(fd); return 0; }
    if (send(fd, request, (size_t)length, 0) != length) { close(fd); return 0; }

    for (;;) {
        ssize_t read_bytes = recv(fd, response + total, sizeof(response) - total - 1, 0);
        if (read_bytes <= 0) break;
        total += (size_t)read_bytes;
        if (total + 1 >= sizeof(response)) break;
    }
    close(fd);
    response[total] = '\0';
    if (total == 0) return 0;
    if (sscanf(response, "HTTP/1.%*d %d", &status) != 1) return 0;

    header_end = strstr(response, "\r\n\r\n");
    if (header_end && out && out_size) {
        const char *payload = header_end + 4;
        /* Firebase отдаёт ответ chunked, когда длина заранее неизвестна:
         * первая строка — размер в hex, её нужно пропустить. */
        if (strstr(response, "Transfer-Encoding: chunked")) {
            const char *chunk = strstr(payload, "\r\n");
            if (chunk) payload = chunk + 2;
        }
        snprintf(out, out_size, "%s", payload);
        {   /* хвост chunked-ответа скрипту не нужен */
            char *tail = strstr(out, "\r\n");
            if (tail) *tail = '\0';
        }
    }
    return status;
}

#endif

/* --------------------------------------------------------- разбор JSON */

static const char *js_skip_ws(const char *p) {
    while (p && *p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static const char *js_skip_value(const char *p);

static const char *js_skip_string(const char *p) {
    if (!p || *p != '"') return NULL;
    for (p++; *p; p++) {
        if (*p == '\\' && p[1]) { p++; continue; }
        if (*p == '"') return p + 1;
    }
    return NULL;
}

static const char *js_skip_container(const char *p, char open, char close) {
    int depth = 0;
    if (!p || *p != open) return NULL;
    for (; *p; p++) {
        if (*p == '"') { p = js_skip_string(p); if (!p) return NULL; p--; continue; }
        if (*p == open) depth++;
        else if (*p == close && --depth == 0) return p + 1;
    }
    return NULL;
}

static const char *js_skip_value(const char *p) {
    p = js_skip_ws(p);
    if (!p || !*p) return NULL;
    if (*p == '"') return js_skip_string(p);
    if (*p == '{') return js_skip_container(p, '{', '}');
    if (*p == '[') return js_skip_container(p, '[', ']');
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

/* Значение поля key внутри объекта object (без рекурсии по вложенным). */
static const char *js_member(const char *object, const char *key) {
    size_t key_length = strlen(key);
    const char *p = js_skip_ws(object);
    if (!p || *p != '{') return NULL;
    p = js_skip_ws(p + 1);
    while (p && *p && *p != '}') {
        const char *name = p;
        const char *name_end = js_skip_string(p);
        if (!name_end) return NULL;
        p = js_skip_ws(name_end);
        if (*p != ':') return NULL;
        p = js_skip_ws(p + 1);
        if ((size_t)(name_end - name - 2) == key_length &&
            strncmp(name + 1, key, key_length) == 0) {
            return p;
        }
        p = js_skip_value(p);
        p = js_skip_ws(p);
        if (p && *p == ',') p = js_skip_ws(p + 1);
    }
    return NULL;
}

/* Значение по пути "players/1/x". */
static const char *js_path(const char *json, const char *path) {
    char part[64];
    const char *value = json;
    while (path && *path && value) {
        const char *slash = strchr(path, '/');
        size_t length = slash ? (size_t)(slash - path) : strlen(path);
        if (length == 0 || length >= sizeof(part)) return NULL;
        memcpy(part, path, length);
        part[length] = '\0';
        value = js_member(value, part);
        path = slash ? slash + 1 : path + length;
    }
    return value;
}

static int js_is_null(const char *value) {
    return !value || strncmp(value, "null", 4) == 0;
}

static double js_number(const char *json, const char *path, double fallback) {
    const char *value = js_path(json, path);
    if (js_is_null(value)) return fallback;
    if (*value == 't') return 1;             /* true  */
    if (*value == 'f') return 0;             /* false */
    if (*value == '"') value++;              /* число, записанное строкой */
    return atof(value);
}

static void js_string(const char *json, const char *path, char *out, size_t out_size) {
    const char *value = js_path(json, path);
    size_t i = 0;
    if (out_size == 0) return;
    out[0] = '\0';
    if (js_is_null(value) || *value != '"') return;
    for (value++; *value && *value != '"' && i + 1 < out_size; value++) {
        if (*value == '\\' && value[1]) value++;
        out[i++] = *value;
    }
    out[i] = '\0';
}

/* -------------------------------------------------------- общее состояние */

typedef struct {
    double x, y, angle, hp, alive;
} NetActor;

typedef struct {
    double x, y, dx, dy, active, shot;
} NetBullet;

static struct {
    pthread_t thread;
    pthread_mutex_t lock;
    int running;
    int started;

    char base[NET_BASE_SIZE];
    char room[48];
    char uid[24];
    int slot;
    int status;

    NetActor local;
    NetBullet local_bullet;
    unsigned long seq;

    int peer_online;
    NetActor peer;
    NetBullet peer_bullet;
} net;

/* Уникальный id клиента. Только time() мало: два устройства, запущенные
 * в одну секунду, получили бы один uid и подрались за слот. Подмешиваем
 * наносекунды, pid и адрес стека — этого достаточно, чтобы совпадений
 * на практике не было. */
static void net_make_uid(void) {
    struct timespec ts;
    unsigned long a, b;
    int local = 0;
#ifdef CLOCK_MONOTONIC
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    ts.tv_sec = time(NULL);
    ts.tv_nsec = 0;
#endif
    a = (unsigned long)time(NULL) ^ ((unsigned long)ts.tv_nsec << 8);
    b = (unsigned long)getpid() ^ (unsigned long)(uintptr_t)&local;
    snprintf(net.uid, sizeof(net.uid), "%08lx%08lx",
             a & 0xFFFFFFFFul, b & 0xFFFFFFFFul);
}

static void net_lock(void) { pthread_mutex_lock(&net.lock); }
static void net_unlock(void) { pthread_mutex_unlock(&net.lock); }

static void net_set_status(int status) {
    net_lock();
    net.status = status;
    net_unlock();
}

/* Числа для Firebase печатаем сами: "%g" даёт короткий и валидный JSON,
 * а бесконечности и NaN в базу пускать нельзя. */
static double net_safe(double value) {
    if (isnan(value) || isinf(value)) return 0;
    return value;
}

/* ------------------------------------------------------- запросы к базе */

/* Одним PATCH пишем и себя, и свою пулю: меньше запросов — меньше лага. */
static int net_push_state(void) {
    char url[NET_URL_SIZE], body[NET_BODY_SIZE];
    NetActor actor;
    NetBullet bullet;
    unsigned long seq;
    int slot;

    net_lock();
    actor = net.local;
    bullet = net.local_bullet;
    slot = net.slot;
    seq = ++net.seq;
    net_unlock();
    if (slot < 0) return 0;

    snprintf(url, sizeof(url), "%s/rooms/%s.json", net.base, net.room);
    snprintf(body, sizeof(body),
             "{\"players/%d\":{\"uid\":\"%s\",\"x\":%.1f,\"y\":%.1f,\"angle\":%.4f,"
             "\"hp\":%.0f,\"alive\":%.0f,\"seq\":%lu},"
             "\"bullets/%d\":{\"x\":%.1f,\"y\":%.1f,\"dx\":%.4f,\"dy\":%.4f,"
             "\"active\":%.0f,\"shot\":%.0f}}",
             slot, net.uid, net_safe(actor.x), net_safe(actor.y), net_safe(actor.angle),
             net_safe(actor.hp), net_safe(actor.alive), seq,
             slot, net_safe(bullet.x), net_safe(bullet.y), net_safe(bullet.dx),
             net_safe(bullet.dy), net_safe(bullet.active), net_safe(bullet.shot));
    return net_http("PATCH", url, body, NULL, 0) == 200;
}

/* Чужой слот читаем целиком одним GET комнаты. */
static int net_pull_state(char *response, size_t size) {
    char url[NET_URL_SIZE];
    snprintf(url, sizeof(url), "%s/rooms/%s.json", net.base, net.room);
    return net_http("GET", url, NULL, response, size) == 200;
}

static void net_release_slot(void) {
    char url[NET_URL_SIZE];
    int slot;
    net_lock();
    slot = net.slot;
    net.slot = -1;
    net_unlock();
    if (slot < 0) return;
    snprintf(url, sizeof(url), "%s/rooms/%s/players/%d.json", net.base, net.room, slot);
    net_http("DELETE", url, NULL, NULL, 0);
    snprintf(url, sizeof(url), "%s/rooms/%s/bullets/%d.json", net.base, net.room, slot);
    net_http("DELETE", url, NULL, NULL, 0);
}

/* Занять слот: сначала свободный, потом — молчащий дольше NET_TAKEOVER_MS.
 * Занятие подтверждаем повторным чтением: если два клиента подключились
 * одновременно, проигравший увидит чужой uid и возьмёт другой слот. */
static int net_claim_slot(void) {
    static long long watch_since = 0;
    static unsigned long watch_seq[2] = {0, 0};
    static int watch_valid = 0;
    char response[NET_RESPONSE_SIZE], url[NET_URL_SIZE], body[NET_BODY_SIZE], uid[24];
    long long now = net_now_ms();
    int slot;

    if (!net_pull_state(response, sizeof(response))) return -1;

    for (slot = 0; slot < 2; slot++) {
        char path[32];
        snprintf(path, sizeof(path), "players/%d/uid", slot);
        js_string(response, path, uid, sizeof(uid));
        if (uid[0] == '\0' || strcmp(uid, net.uid) == 0) break;
    }

    if (slot == 2) {
        /* Оба слота заняты. Смотрим, не бросил ли кто игру. */
        unsigned long seq[2];
        seq[0] = (unsigned long)js_number(response, "players/0/seq", 0);
        seq[1] = (unsigned long)js_number(response, "players/1/seq", 0);
        if (!watch_valid || seq[0] != watch_seq[0] || seq[1] != watch_seq[1]) {
            watch_seq[0] = seq[0];
            watch_seq[1] = seq[1];
            watch_since = now;
            watch_valid = 1;
            return -1;
        }
        if (now - watch_since < NET_TAKEOVER_MS) return -1;
        slot = 0;   /* оба молчат — комната брошена, занимаем первый слот */
        watch_valid = 0;
    }

    snprintf(url, sizeof(url), "%s/rooms/%s/players/%d.json", net.base, net.room, slot);
    snprintf(body, sizeof(body),
             "{\"uid\":\"%s\",\"x\":0,\"y\":0,\"angle\":0,\"hp\":0,\"alive\":0,\"seq\":0}",
             net.uid);
    if (net_http("PUT", url, body, NULL, 0) != 200) return -1;

    /* Проверяем, что слот действительно наш. */
    if (net_http("GET", url, NULL, response, sizeof(response)) != 200) return -1;
    js_string(response, "uid", uid, sizeof(uid));
    if (strcmp(uid, net.uid) != 0) return -1;

    NET_LOG("занят слот %d (uid %s)", slot, net.uid);
    return slot;
}

/* Разбираем чужой слот и решаем, на связи ли он. */
static void net_read_peer(const char *response) {
    static unsigned long last_seq = 0;
    static long long last_change = 0;
    char base_path[32], path[48], uid[24];
    unsigned long seq;
    long long now = net_now_ms();
    int peer = net.slot == 0 ? 1 : 0;
    NetActor actor;
    NetBullet bullet;
    int online;

    snprintf(base_path, sizeof(base_path), "players/%d", peer);
    snprintf(path, sizeof(path), "%s/uid", base_path);
    js_string(response, path, uid, sizeof(uid));
    if (uid[0] == '\0' || strcmp(uid, net.uid) == 0) {
        net_lock();
        net.peer_online = 0;
        net_unlock();
        last_seq = 0;
        return;
    }

    snprintf(path, sizeof(path), "%s/seq", base_path);
    seq = (unsigned long)js_number(response, path, 0);
    if (seq != last_seq) { last_seq = seq; last_change = now; }
    else if (last_change == 0) last_change = now;
    online = (now - last_change) < NET_PEER_TIMEOUT;

    snprintf(path, sizeof(path), "%s/x", base_path);
    actor.x = js_number(response, path, 0);
    snprintf(path, sizeof(path), "%s/y", base_path);
    actor.y = js_number(response, path, 0);
    snprintf(path, sizeof(path), "%s/angle", base_path);
    actor.angle = js_number(response, path, 0);
    snprintf(path, sizeof(path), "%s/hp", base_path);
    actor.hp = js_number(response, path, 0);
    snprintf(path, sizeof(path), "%s/alive", base_path);
    actor.alive = js_number(response, path, 0);

    snprintf(base_path, sizeof(base_path), "bullets/%d", peer);
    snprintf(path, sizeof(path), "%s/x", base_path);
    bullet.x = js_number(response, path, 0);
    snprintf(path, sizeof(path), "%s/y", base_path);
    bullet.y = js_number(response, path, 0);
    snprintf(path, sizeof(path), "%s/dx", base_path);
    bullet.dx = js_number(response, path, 0);
    snprintf(path, sizeof(path), "%s/dy", base_path);
    bullet.dy = js_number(response, path, 0);
    snprintf(path, sizeof(path), "%s/active", base_path);
    bullet.active = js_number(response, path, 0);
    snprintf(path, sizeof(path), "%s/shot", base_path);
    bullet.shot = js_number(response, path, 0);

    net_lock();
    net.peer = actor;
    net.peer_bullet = bullet;
    net.peer_online = online;
    net_unlock();
}

/* ------------------------------------------------------------ поток сети */

static void *net_thread_main(void *unused) {
    char response[NET_RESPONSE_SIZE];
    int failures = 0;
    (void)unused;

    while (net.running) {
        long long started = net_now_ms();
        int slot;

        net_lock();
        slot = net.slot;
        net_unlock();

        if (slot < 0) {
            net_set_status(NET_CONNECTING);
            slot = net_claim_slot();
            if (slot < 0) {
                if (++failures > 3) net_set_status(NET_ERROR);
                net_sleep_ms(500);
                continue;
            }
            net_lock();
            net.slot = slot;
            net.seq = 0;
            net_unlock();
            failures = 0;
        }

        if (!net_push_state() || !net_pull_state(response, sizeof(response))) {
            if (++failures > 3) net_set_status(NET_ERROR);
            net_sleep_ms(300);
            continue;
        }
        failures = 0;
        net_read_peer(response);
        net_set_status(net.peer_online ? NET_PLAYING : NET_WAITING);

        {
            long long spent = net_now_ms() - started;
            if (spent < NET_TICK_MS) net_sleep_ms((int)(NET_TICK_MS - spent));
        }
    }

    net_release_slot();
    net_set_status(NET_OFFLINE);
    return NULL;
}

/* -------------------------------------------------------------- API игры */

void net_connect(const char *base_url, const char *room) {
    size_t length;
    if (net.running) return;
    if (!base_url || !*base_url) return;

    memset(&net.local, 0, sizeof(net.local));
    memset(&net.local_bullet, 0, sizeof(net.local_bullet));
    memset(&net.peer, 0, sizeof(net.peer));
    memset(&net.peer_bullet, 0, sizeof(net.peer_bullet));
    net.peer_online = 0;
    net.slot = -1;
    net.seq = 0;

#ifndef __ANDROID__
    /* Тестам нужен локальный сервер вместо боевой базы. */
    {
        const char *override = getenv("NET_BASE_URL");
        if (override && *override) base_url = override;
    }
#endif
    snprintf(net.base, sizeof(net.base), "%s", base_url);
    length = strlen(net.base);
    while (length && net.base[length - 1] == '/') net.base[--length] = '\0';
    snprintf(net.room, sizeof(net.room), "%s", (room && *room) ? room : "main");

    if (!net.uid[0]) net_make_uid();
    if (!net.started) {
        pthread_mutex_init(&net.lock, NULL);
        net.started = 1;
    }
    net.status = NET_CONNECTING;
    net.running = 1;
    if (pthread_create(&net.thread, NULL, net_thread_main, NULL) != 0) {
        net.running = 0;
        net.status = NET_ERROR;
        NET_LOG("не удалось запустить сетевой поток");
        return;
    }
    NET_LOG("подключение к %s, комната %s", net.base, net.room);
}

void net_disconnect(void) {
    if (!net.running) return;
    net.running = 0;
    pthread_join(net.thread, NULL);
    net.status = NET_OFFLINE;
    net.slot = -1;
    net.peer_online = 0;
}

void net_shutdown(void) { net_disconnect(); }

void net_publish(double x, double y, double angle, double hp, double alive) {
    if (!net.started) return;
    net_lock();
    net.local.x = x;
    net.local.y = y;
    net.local.angle = angle;
    net.local.hp = hp;
    net.local.alive = alive;
    net_unlock();
}

void net_publish_bullet(double x, double y, double dx, double dy,
                        double active, double shot) {
    if (!net.started) return;
    net_lock();
    net.local_bullet.x = x;
    net.local_bullet.y = y;
    net.local_bullet.dx = dx;
    net.local_bullet.dy = dy;
    net.local_bullet.active = active;
    net.local_bullet.shot = shot;
    net_unlock();
}

/* Читатели состояния: короткие, чтобы скрипт мог звать их каждый кадр. */
#define NET_READER(name, expression)      \
    double name(void) {                   \
        double value;                     \
        if (!net.started) return 0;       \
        net_lock();                       \
        value = (expression);             \
        net_unlock();                     \
        return value;                     \
    }

NET_READER(net_status, (double)net.status)
NET_READER(net_online, net.status == NET_PLAYING ? 1 : 0)
NET_READER(net_slot, (double)net.slot)
NET_READER(net_peer_online, net.peer_online ? 1 : 0)
NET_READER(net_peer_x, net.peer.x)
NET_READER(net_peer_y, net.peer.y)
NET_READER(net_peer_angle, net.peer.angle)
NET_READER(net_peer_hp, net.peer.hp)
NET_READER(net_peer_alive, net.peer.alive)
NET_READER(net_peer_bullet_active, net.peer_bullet.active)
NET_READER(net_peer_bullet_x, net.peer_bullet.x)
NET_READER(net_peer_bullet_y, net.peer_bullet.y)
NET_READER(net_peer_bullet_dx, net.peer_bullet.dx)
NET_READER(net_peer_bullet_dy, net.peer_bullet.dy)
NET_READER(net_peer_bullet_shot, net.peer_bullet.shot)

#undef NET_READER
