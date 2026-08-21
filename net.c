#include "net.h"
#include "runtime.h"
#include <math.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <process.h>
#include <wininet.h>
#else
#include <pthread.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif
#endif
#ifdef __ANDROID__
#define LOG(...) do { __android_log_print(ANDROID_LOG_INFO, "DimScriptNet", __VA_ARGS__); ds_console_log(0, __VA_ARGS__); } while (0)
#define LOGERR(...) do { __android_log_print(ANDROID_LOG_ERROR, "DimScriptNet", __VA_ARGS__); ds_console_log(1, __VA_ARGS__); } while (0)
#else
#define LOG(...) do { ds_console_log(0, __VA_ARGS__); } while (0)
#define LOGERR(...) do { ds_console_log(1, __VA_ARGS__); } while (0)
#endif
#define URL 512
#define BODY 1024
#define RESP 4096
#define CHAT_RESP 8192
#define WRITE_TICK 60
#define READ_TICK 60
#define CHAT_TICK 1000
#define EVENT_TICK 500
#define TIMEOUT 4000
#define STALE 6000
#define CHAT_MAX 32
#define CHAT_TEXT_MAX 96
#define LOGIN_NICK_MAX 16
#define SESSION_FILE "auth.dat"
#define PROGRESS_FILE "progress.dat"
#ifdef _WIN32
typedef SRWLOCK DSMutex;
#define DS_MUTEX_INIT SRWLOCK_INIT
#define ds_mutex_lock(m) AcquireSRWLockExclusive(&(m))
#define ds_mutex_unlock(m) ReleaseSRWLockExclusive(&(m))
typedef HANDLE DSThread;
static DSThread ds_thread_start(unsigned (__stdcall *fn)(void *), void *arg) {
    return (DSThread)_beginthreadex(NULL, 0, fn, arg, 0, NULL);
}
static void ds_thread_join(DSThread t) { if (t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); } }
static void ds_thread_detach(DSThread t) { if (t) CloseHandle(t); }
#else
typedef pthread_mutex_t DSMutex;
#define DS_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#define ds_mutex_lock(m) pthread_mutex_lock(&(m))
#define ds_mutex_unlock(m) pthread_mutex_unlock(&(m))
typedef pthread_t DSThread;
static DSThread ds_thread_start(void *(*fn)(void *), void *arg) {
    pthread_t t;
    if (pthread_create(&t, NULL, fn, arg) != 0) return (pthread_t)0;
    return t;
}
static void ds_thread_join(DSThread t) { if (t) pthread_join(t, NULL); }
static void ds_thread_detach(DSThread t) { if (t) pthread_detach(t); }
#endif

typedef struct {
    double x,y,a,hp,alive;
    double punch_x,punch_y,punch_dx,punch_dy,punch;
    double cls;
    double frz;                                   /* сколько секунд игрок ещё заморожен */
    double gift,gift_x,gift_y,gift_dx,gift_dy;    /* суператака «Подарок» */
    int online;
    char nick[24];
} Actor;
typedef struct { char key[28]; char uid[24]; char nick[24]; char text[CHAT_TEXT_MAX]; int valid; } ChatMsg;
static int chat_keep = 20;
static struct {
    DSThread thread, rthread; DSMutex lock;
#ifdef __ANDROID__
    JavaVM *vm;
#endif
    int run, started, slot, status;
    char base[256], room[48], uid[24];
    Actor me; unsigned long seq, count;
    Actor players[NET_SLOTS];
    ChatMsg chats[CHAT_MAX]; int chat_count;
    int event;
} net = { .lock = DS_MUTEX_INIT };

static volatile sig_atomic_t net_fast = 0;
static char s_chat_text_ret[CHAT_TEXT_MAX];
static char s_chat_uid_ret[24];
static char s_nick_ret[24];
static char s_lg_nick_ret[24];
static char s_lg_pass_ret[64];
static long long now_ms(void);
static void lock(void);
static void unlock(void);

typedef struct {
    DSMutex lock;
    int status;
    char session_nick[LOGIN_NICK_MAX+1];
    char session_pass[64];
    char path[256];
} Login;
static Login lg = { .lock = DS_MUTEX_INIT };
static void lg_lock(void) { ds_mutex_lock(lg.lock); }
static void lg_unlock(void) { ds_mutex_unlock(lg.lock); }

typedef struct {
    DSMutex lock;
    int loaded, cups, candies, cls, azum, santa, level, levels_unlocked;
} Progress;
static Progress pg = { .lock = DS_MUTEX_INIT };
static int pending_cls = 0;
static void pg_lock(void) { ds_mutex_lock(pg.lock); }
static void pg_unlock(void) { ds_mutex_unlock(pg.lock); }
static void data_file_path(char *path, size_t cap, const char *name) {
    lg_lock();
    if (lg.path[0]) snprintf(path, cap, "%s/%s", lg.path, name);
    else snprintf(path, cap, "%s", name);
    lg_unlock();
}
static void progress_write(int cups, int candies, int cls, int azum, int santa,
                           int level, int levels_unlocked) {
    char path[320]; FILE *f;
    data_file_path(path, sizeof(path), PROGRESS_FILE);
    f = fopen(path, "w");
    if (f) {
        /* Старые сборки писали пять чисел. Новые два поля — выбранный и
         * максимально открытый уровень — добавлены в конец для совместимости. */
        fprintf(f, "%d %d %d %d %d %d %d\n",
                cups, cls, azum, santa, candies, level, levels_unlocked);
        fclose(f);
    }
}
static void progress_read(void) {
    char path[320]; FILE *f;
    int cups=0, candies=0, cls=0, azum=0, santa=0, level=0, levels_unlocked=0;
    pg_lock();
    if (pg.loaded) { pg_unlock(); return; }
    pg_unlock();
    data_file_path(path, sizeof(path), PROGRESS_FILE);
    f = fopen(path, "r");
    if (f) {
        int n = fscanf(f, "%d %d %d %d %d %d %d",
                       &cups, &cls, &azum, &santa, &candies,
                       &level, &levels_unlocked);
        if (n < 3) {
            cups=0; candies=0; cls=0; azum=0; santa=0; level=0; levels_unlocked=0;
        } else if (n < 6) {
            level=0; levels_unlocked=0;
        } else if (n < 7) {
            /* A file with only the selected level predates the separate
             * selection/unlock fields, so that level is the highest unlocked. */
            levels_unlocked=level;
        }
        fclose(f);
    }
    if (cups < 0) cups = 0;
    if (candies < 0) candies = 0;
    if (cls != 1 && cls != 2) cls = 0;
    azum = azum ? 1 : 0;
    santa = santa ? 1 : 0;
    if (cls == 1 && !azum) cls = 0;
    if (cls == 2 && !santa) cls = 0;
    if (levels_unlocked < 0) levels_unlocked = 0;
    if (levels_unlocked > 5) levels_unlocked = 5;
    if (level < 0 || level > levels_unlocked) level = 0;
    pg_lock();
    pg.cups = cups; pg.candies = candies; pg.cls = cls; pg.azum = azum; pg.santa = santa;
    pg.level = level; pg.levels_unlocked = levels_unlocked; pg.loaded = 1;
    pg_unlock();
}
double net_load_cups(void) { double v; progress_read(); pg_lock(); v = (double)pg.cups; pg_unlock(); return v; }
double net_load_candies(void) { double v; progress_read(); pg_lock(); v = (double)pg.candies; pg_unlock(); return v; }
double net_load_class(void) { double v; progress_read(); pg_lock(); v = (double)pg.cls; pg_unlock(); return v; }
double net_load_azum(void) { double v; progress_read(); pg_lock(); v = (double)pg.azum; pg_unlock(); return v; }
double net_load_santa(void) { double v; progress_read(); pg_lock(); v = (double)pg.santa; pg_unlock(); return v; }
double net_load_level(void) { double v; progress_read(); pg_lock(); v = (double)pg.level; pg_unlock(); return v; }
double net_load_levels_unlocked(void) { double v; progress_read(); pg_lock(); v = (double)pg.levels_unlocked; pg_unlock(); return v; }

/* Асинхронный патч в Firebase при сохранении прогресса */
typedef struct { char url[URL]; char body[BODY*2]; } HttpJob;
static int http(const char *method,const char *url,const char *body,char *out,size_t cap);
static void *http_patch_job(void *arg) {
    HttpJob *j = (HttpJob*)arg;
    if (j) { http("PATCH", j->url, j->body, NULL, 0); free(j); }
    return NULL;
}
#ifdef _WIN32
static unsigned __stdcall win_http_patch(void *arg) { http_patch_job(arg); return 0; }
#endif
static void http_patch_async(const char *url, const char *body) {
    HttpJob *j = (HttpJob*)malloc(sizeof(*j));
    DSThread t;
    if (!j) return;
    snprintf(j->url, sizeof(j->url), "%s", url);
    snprintf(j->body, sizeof(j->body), "%s", body);
#ifdef _WIN32
    t = ds_thread_start(win_http_patch, j);
#else
    t = ds_thread_start(http_patch_job, j);
#endif
    if (t) { ds_thread_detach(t); return; }
    free(j);
}

void net_save_progress(double cups, double candies, double cls, double azum, double santa,
                       double level, double levels_unlocked) {
    int c = (int)cups, cd = (int)candies, k = (int)cls, a = azum ? 1 : 0, sn = santa ? 1 : 0;
    int lv = (int)level, lu = (int)levels_unlocked;
    if (c < 0) c = 0;
    if (cd < 0) cd = 0;
    if (k != 1 && k != 2) k = 0;
    if (k == 1 && !a) k = 0;
    if (k == 2 && !sn) k = 0;
    if (lu < 0) lu = 0;
    if (lu > 5) lu = 5;
    if (lv < 0 || lv > lu) lv = 0;
    pg_lock();
    pg.cups = c; pg.candies = cd; pg.cls = k; pg.azum = a; pg.santa = sn;
    pg.level = lv; pg.levels_unlocked = lu; pg.loaded = 1;
    pg_unlock();
    pending_cls = k;
    progress_write(c, cd, k, a, sn, lv, lu);

    char nick[LOGIN_NICK_MAX + 1] = "";
    lg_lock();
    if (lg.status == NET_LOGIN_OK && lg.session_nick[0]) {
        snprintf(nick, sizeof(nick), "%s", lg.session_nick);
    }
    lg_unlock();
    if (nick[0] && net.base[0]) {
        char url[URL], body[BODY];
        snprintf(url, sizeof(url), "%s/users/%s.json", net.base, nick);
        snprintf(body, sizeof(body), "{\"cups\":%d,\"candies\":%d,\"cls\":%d,\"azum\":%d,\"santa\":%d,\"level\":%d,\"levels\":%d}",
                 c, cd, k, a, sn, lv, lu);
        http_patch_async(url, body);
    }
}
void net_set_class(double cls) {
    int k = (int)cls;
    if (k != 1 && k != 2) k = 0;
    pending_cls = k;
    lock(); net.me.cls = (double)k;
    if (net.slot >= 0) net.players[net.slot].cls = (double)k;
    unlock();
}

static int nick_valid(const char *n) {
    size_t i=0, bytes=n ? strlen(n) : 0, chars=0;
    if (bytes < 1 || bytes > LOGIN_NICK_MAX) return 0;
    while (i < bytes) {
        unsigned char c=(unsigned char)n[i]; unsigned long cp=0; size_t need=0;
        if (c<0x80) { cp=c; need=1; }
        else if ((c&0xE0)==0xC0) { cp=c&0x1F; need=2; }
        else if ((c&0xF0)==0xE0) { cp=c&0x0F; need=3; }
        else if ((c&0xF8)==0xF0) { cp=c&0x07; need=4; }
        else return 0;
        if (i+need>bytes) return 0;
        size_t k; for(k=1;k<need;k++) { unsigned char q=(unsigned char)n[i+k]; if((q&0xC0)!=0x80) return 0; cp=(cp<<6)|(q&0x3F); }
        if ((need==2&&cp<0x80)||(need==3&&cp<0x800)||(need==4&&cp<0x10000)||cp>0x10FFFF) return 0;
        if ((cp>='a'&&cp<='z')||(cp>='A'&&cp<='Z')||(cp>='0'&&cp<='9')||cp=='_'||(cp>=0x0400&&cp<=0x04FF)) chars++;
        else return 0;
        i+=need;
    }
    return chars>=1 && chars<=16;
}

static int pass_valid(const char *p) {
    if (!p) return 0;
    size_t len = strlen(p);
    if (len < 1 || len > 32) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c < 0x20 || c == 0x7F || c == '"' || c == '\\' || c == ' ') return 0;
    }
    return 1;
}

static void session_save(const char *nick, const char *pass) {
    char path[320]; FILE *f;
    if (lg.path[0]) snprintf(path, sizeof(path), "%s/%s", lg.path, SESSION_FILE);
    else snprintf(path, sizeof(path), "%s", SESSION_FILE);
    f = fopen(path, "w");
    if (f) {
        if (pass && *pass) fprintf(f, "%s %s\n", nick, pass);
        else fprintf(f, "%s\n", nick);
        fclose(f);
    }
}
static long long now_ms(void) {
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec*1000+t.tv_nsec/1000000;
#endif
}
static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec t = { ms/1000, (long)(ms%1000)*1000000L };
    nanosleep(&t, NULL);
#endif
}
static double safe(double v) { if (v != v) return 0; if (v > 1e300 || v < -1e300) return 0; return v; }
static void lock(void) { ds_mutex_lock(net.lock); }
static void unlock(void) { ds_mutex_unlock(net.lock); }
static void status(int s) { lock(); net.status=s; unlock(); }

static DSMutex net_log_lock = DS_MUTEX_INIT;
static int net_log_ok(void) {
    static long long last = 0;
    long long now = now_ms();
    int ok = 0;
    ds_mutex_lock(net_log_lock);
    if (now - last >= 2000) { last = now; ok = 1; }
    ds_mutex_unlock(net_log_lock);
    return ok;
}
#ifdef __ANDROID__
void net_set_java_vm(JavaVM *vm) { net.vm=vm; }
#endif
#ifdef _WIN32
static int parse_http_url(const char *url, char *host, size_t host_cap, int *port, const char **path, int *secure) {
    const char *p = url;
    *secure = 0; *port = 80;
    if (strncmp(p, "https://", 8) == 0) { *secure = 1; *port = 443; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) p += 7;
    else return 0;
    const char *slash = strchr(p, '/');
    size_t hl = slash ? (size_t)(slash - p) : strlen(p);
    if (!hl || hl >= host_cap) return 0;
    memcpy(host, p, hl); host[hl] = 0;
    char *colon = strchr(host, ':');
    if (colon) { *colon = 0; *port = atoi(colon + 1); if (*port <= 0) *port = *secure ? 443 : 80; }
    *path = slash ? slash : "/";
    return 1;
}
static void log_win32_net_error(const char *what, int code, int stage) {
    if (!net_log_ok()) return;
    DWORD err = GetLastError();
    char msg[256] = "";
    if (err) {
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, err, 0, msg, sizeof(msg) - 1, NULL);
        size_t mlen = strlen(msg);
        while (mlen && (msg[mlen-1]=='\r'||msg[mlen-1]=='\n'||msg[mlen-1]==' '||msg[mlen-1]=='.')) msg[--mlen] = '\0';
    }
    if (stage == 2) {
        char srv[512] = "";
        DWORD slen = sizeof(srv) - 1, sidx = 0;
        if (InternetGetLastResponseInfoA(&sidx, srv, &slen) && slen > 0) {
            srv[slen] = '\0';
            LOGERR("http %s: HTTP %d, wininet err %lu (%s); server: %s",
                   what, code, err, msg[0] ? msg : "unknown", srv);
            return;
        }
    }
    LOGERR("http %s: HTTP %d, wininet err %lu (%s)", what, code, err, msg[0] ? msg : "unknown");
}
static int http_ex(const char *method,const char *url,const char *body,char *out,size_t cap,
                   const char *header,const char *value,char *etag,size_t etag_cap) {
    char host[256]; int port = 80, secure = 0; const char *path = NULL;
    if (out && cap) out[0] = '\0';
    if (etag && etag_cap) etag[0] = '\0';
    if (!url || !parse_http_url(url, host, sizeof(host), &port, &path, &secure)) return 0;
    int modes[2] = { INTERNET_OPEN_TYPE_PRECONFIG, INTERNET_OPEN_TYPE_DIRECT };
    int last_code = 0, last_stage = 1;
    for (int mi = 0; mi < 2; mi++) {
        HINTERNET inet = InternetOpenA("CubicBattle/1.0", (DWORD)modes[mi], NULL, NULL, 0);
        if (!inet) continue;
        HINTERNET conn = InternetConnectA(inet, host, (INTERNET_PORT)port, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
        if (!conn) { InternetCloseHandle(inet); continue; }
        {
            DWORD tmo = (DWORD)(net_fast ? 1200 : TIMEOUT);
            InternetSetOptionA(conn, INTERNET_OPTION_CONNECT_TIMEOUT, &tmo, sizeof(tmo));
            InternetSetOptionA(conn, INTERNET_OPTION_SEND_TIMEOUT, &tmo, sizeof(tmo));
            InternetSetOptionA(conn, INTERNET_OPTION_RECEIVE_TIMEOUT, &tmo, sizeof(tmo));
        }
        DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI;
        if (secure) flags |= INTERNET_FLAG_SECURE;
        HINTERNET req = HttpOpenRequestA(conn, method, path, NULL, NULL, NULL, flags, 0);
        if (!req) { InternetCloseHandle(conn); InternetCloseHandle(inet); continue; }
        char hdrs[512]; int hl = 0;
        hl += snprintf(hdrs + hl, sizeof(hdrs) - (size_t)hl, "Content-Type: application/json\r\n");
        if (header && value) hl += snprintf(hdrs + hl, sizeof(hdrs) - (size_t)hl, "%s: %s\r\n", header, value);
        if (hl < 0 || (size_t)hl >= sizeof(hdrs)) hl = (int)sizeof(hdrs) - 1;
        hdrs[hl] = '\0';
        BOOL ok = HttpSendRequestA(req, hl ? hdrs : NULL, (DWORD)hl, (LPVOID)body, (DWORD)(body ? strlen(body) : 0));
        int code = 0;
        if (ok) {
            DWORD len = sizeof(code), idx = 0;
            if (HttpQueryInfoA(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &len, &idx)) last_code = code;
        }
        if (etag && etag_cap) {
            DWORD len = 0, idx = 0;
            if (HttpQueryInfoA(req, HTTP_QUERY_ETAG, NULL, &len, &idx) || GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                if (len && len < etag_cap) {
                    DWORD got = len;
                    if (HttpQueryInfoA(req, HTTP_QUERY_ETAG, etag, &got, &idx) && got < etag_cap) etag[got] = '\0';
                }
            }
        }
        if (ok && out && cap) {
            size_t total = 0;
            for (;;) {
                DWORD rd = 0;
                if (!InternetReadFile(req, out + total, (DWORD)(cap - 1 - total), &rd) || rd == 0) break;
                total += (size_t)rd;
                out[total] = '\0';
                if (total + 1 >= cap) break;
            }
        }
        InternetCloseHandle(req);
        InternetCloseHandle(conn);
        InternetCloseHandle(inet);
        if (ok) return code;
        if (code) { last_code = code; last_stage = 2; break; }
        last_code = 0; last_stage = 1;
    }
    log_win32_net_error(method, last_code, last_stage);
    return 0;
}
#elif defined(__ANDROID__)
static int http_ex(const char *method,const char *url,const char *body,char *out,size_t cap,
                   const char *header,const char *value,char *etag,size_t etag_cap) {
    JNIEnv *env=NULL; jobject conn=NULL, stream=NULL, urlobj=NULL;
    jclass urlc, connc, streamc; jbyteArray buf; jstring ju, jm;
    int code=0, attached=0, ok=0; size_t total=0;
    if(out&&cap)out[0]='\0';
    if(etag&&etag_cap)etag[0]='\0';
    if (!net.vm) return 0;
    if ((*net.vm)->GetEnv(net.vm,(void**)&env,JNI_VERSION_1_6)!=JNI_OK) {
        if ((*net.vm)->AttachCurrentThread(net.vm,&env,NULL)!=JNI_OK) return 0;
        attached=1;
    }
#define JNI_CHECK() do { if ((*env)->ExceptionCheck(env)) goto done; } while (0)
    if ((*env)->PushLocalFrame(env,32)!=0) goto done;
    urlc=(*env)->FindClass(env,"java/net/URL"); JNI_CHECK();
    connc=(*env)->FindClass(env,"java/net/HttpURLConnection"); JNI_CHECK();
    ju=(*env)->NewStringUTF(env,url); JNI_CHECK();
    urlobj=(*env)->NewObject(env,urlc,(*env)->GetMethodID(env,urlc,"<init>","(Ljava/lang/String;)V"),ju); JNI_CHECK();
    conn=(*env)->CallObjectMethod(env,urlobj,(*env)->GetMethodID(env,urlc,"openConnection","()Ljava/net/URLConnection;")); JNI_CHECK();
    jm=(*env)->NewStringUTF(env,method); JNI_CHECK();
    (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setRequestMethod","(Ljava/lang/String;)V"),jm); JNI_CHECK();
    {
        int tmo = net_fast ? 1200 : TIMEOUT;
        (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setConnectTimeout","(I)V"),tmo); JNI_CHECK();
        (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setReadTimeout","(I)V"),tmo); JNI_CHECK();
    }
    (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setUseCaches","(Z)V"),JNI_FALSE); JNI_CHECK();
    {
        jmethodID set_header=(*env)->GetMethodID(env,connc,"setRequestProperty","(Ljava/lang/String;Ljava/lang/String;)V");
        jstring k=(*env)->NewStringUTF(env,"Content-Type"), v=(*env)->NewStringUTF(env,"application/json");
        (*env)->CallVoidMethod(env,conn,set_header,k,v); JNI_CHECK();
        if(header&&value) { k=(*env)->NewStringUTF(env,header); v=(*env)->NewStringUTF(env,value); (*env)->CallVoidMethod(env,conn,set_header,k,v); JNI_CHECK(); }
    }
    if (body&&*body) {
        jobject os=NULL; jbyteArray data; jsize len=(jsize)strlen(body);
        (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setDoOutput","(Z)V"),JNI_TRUE); JNI_CHECK();
        (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setFixedLengthStreamingMode","(I)V"),len); JNI_CHECK();
        os=(*env)->CallObjectMethod(env,conn,(*env)->GetMethodID(env,connc,"getOutputStream","()Ljava/io/OutputStream;")); JNI_CHECK();
        if(!os) goto done;
        data=(*env)->NewByteArray(env,len); JNI_CHECK();
        (*env)->SetByteArrayRegion(env,data,0,len,(const jbyte*)body);
        (*env)->CallVoidMethod(env,os,(*env)->GetMethodID(env,(*env)->GetObjectClass(env,os),"write","([B)V"),data); JNI_CHECK();
        (*env)->CallVoidMethod(env,os,(*env)->GetMethodID(env,(*env)->GetObjectClass(env,os),"close","()V")); JNI_CHECK();
    }
    code=(int)(*env)->CallIntMethod(env,conn,(*env)->GetMethodID(env,connc,"getResponseCode","()I")); JNI_CHECK();
    if(etag&&etag_cap) {
        jstring key=(*env)->NewStringUTF(env,"ETag"); JNI_CHECK();
        jstring val=(jstring)(*env)->CallObjectMethod(env,conn,(*env)->GetMethodID(env,connc,"getHeaderField","(Ljava/lang/String;)Ljava/lang/String;"),key); JNI_CHECK();
        if(val) { const char *s=(*env)->GetStringUTFChars(env,val,NULL); if(s){snprintf(etag,etag_cap,"%s",s);(*env)->ReleaseStringUTFChars(env,val,s);} }
    }
    stream=(*env)->CallObjectMethod(env,conn,(*env)->GetMethodID(env,connc,code>=400?"getErrorStream":"getInputStream","()Ljava/io/InputStream;")); JNI_CHECK();
    if(!stream) goto closeconn;
    streamc=(*env)->GetObjectClass(env,stream); JNI_CHECK();
    buf=(*env)->NewByteArray(env,2048); JNI_CHECK();
    for (;;) {
        jint n=(*env)->CallIntMethod(env,stream,(*env)->GetMethodID(env,streamc,"read","([B)I"),buf); JNI_CHECK();
        if (n<=0) break;
        if (out&&cap&&total+(size_t)n<cap) { (*env)->GetByteArrayRegion(env,buf,0,n,(jbyte*)(out+total)); total+=(size_t)n; out[total]='\0'; }
    }
    (*env)->CallVoidMethod(env,stream,(*env)->GetMethodID(env,streamc,"close","()V")); JNI_CHECK();
closeconn:
    if (conn) { (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"disconnect","()V")); (*env)->ExceptionClear(env); }
    ok=1;
done:
    if ((*env)->ExceptionCheck(env)) {
        jthrowable ex = (*env)->ExceptionOccurred(env);
        (*env)->ExceptionClear(env);
        if (net_log_ok() && ex) {
            jclass tcls = (*env)->GetObjectClass(env, ex);
            jmethodID gm = tcls ? (*env)->GetMethodID(env, tcls, "getMessage", "()Ljava/lang/String;") : NULL;
            if (gm) {
                jstring jmsg = (jstring)(*env)->CallObjectMethod(env, ex, gm);
                if (jmsg && !(*env)->ExceptionCheck(env)) {
                    const char *s = (*env)->GetStringUTFChars(env, jmsg, NULL);
                    if (s) { LOGERR("http %s: %s", method, s); (*env)->ReleaseStringUTFChars(env, jmsg, s); }
                }
            }
        }
        (*env)->ExceptionClear(env);
    }
    (*env)->PopLocalFrame(env,NULL);
    if (attached) (*net.vm)->DetachCurrentThread(net.vm);
    return ok ? code : 0;
#undef JNI_CHECK
}
#else
static int http_ex(const char *method,const char *url,const char *body,char *out,size_t cap,
                   const char *header,const char *value,char *etag,size_t etag_cap) {
    (void)method; (void)url; (void)body; (void)header; (void)value;
    if (out && cap) out[0] = '\0';
    if (etag && etag_cap) etag[0] = '\0';
    return 0;
}
#endif
static int http(const char *method,const char *url,const char *body,char *out,size_t cap) {
    return http_ex(method,url,body,out,cap,NULL,NULL,NULL,0);
}
static const char *skip_ws(const char *p) { while(p&&*p&&(*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) p++; return p; }
static const char *skip_str(const char *p) { if(!p||*p!='\"') return NULL; for(p++;*p;p++){ if(*p=='\\'&&p[1]){p++;continue;} if(*p=='\"') return p+1; } return NULL; }
static const char *skip_box(const char *p,char open,char close) { int d=0; if(!p||*p!=open) return NULL; for(;*p;p++){ if(*p=='\"'){p=skip_str(p); if(!p)return NULL; p--; continue;} if(*p==open)d++; else if(*p==close&&--d==0)return p+1;} return NULL; }
static const char *skip_val(const char *p) { p=skip_ws(p); if(!p||!*p)return NULL; if(*p=='\"')return skip_str(p); if(*p=='{')return skip_box(p,'{','}'); if(*p=='[')return skip_box(p,'[',']'); while(*p&&*p!=','&&*p!='}'&&*p!=']')p++; return p; }
static const char *member(const char *o,const char *key) {
    size_t n=strlen(key); const char *p=skip_ws(o);
    if(!p||*p!='{') return NULL;
    for(p=skip_ws(p+1);p&&*p&&*p!='}';) {
        const char *name=p,*end=skip_str(p); if(!end)return NULL; p=skip_ws(end); if(*p!=':')return NULL; p=skip_ws(p+1);
        if((size_t)(end-name-2)==n&&strncmp(name+1,key,n)==0) return p;
        p=skip_val(p); p=skip_ws(p); if(p&&*p==',')p=skip_ws(p+1);
    }
    return NULL;
}
static const char *element(const char *a,size_t wanted) {
    const char *p=skip_ws(a); size_t index=0;
    if(!p||*p!='[')return NULL;
    for(p=skip_ws(p+1);p&&*p&&*p!=']';index++) {
        if(index==wanted)return p;
        p=skip_val(p); p=skip_ws(p); if(p&&*p==',')p=skip_ws(p+1); else break;
    }
    return NULL;
}
static const char *path_val(const char *json,const char *path) {
    char part[48],*end; const char *v=json;
    while(path&&*path&&v) {
        const char *slash=strchr(path,'/'); size_t n=slash?(size_t)(slash-path):strlen(path),index;
        if(!n||n>=sizeof(part))return NULL;
        memcpy(part,path,n); part[n]=0; v=skip_ws(v);
        if(*v=='[') { index=strtoul(part,&end,10); if(!*part||*end)return NULL; v=element(v,index); }
        else v=member(v,part);
        path=slash?slash+1:path+n;
    }
    return v;
}
static double num(const char *json,const char *path,double fb) { const char *v=path_val(json,path); if(!v||!strncmp(v,"null",4))return fb; if(*v=='t')return 1; if(*v=='f')return 0; if(*v=='\"')v++; return atof(v); }
static void strv(const char *json,const char *path,char *out,size_t cap) {
    const char *v=path_val(json,path); size_t i=0; if(cap)out[0]=0; if(!v||*v!='\"')return;
    for(v++;*v&&*v!='\"'&&i+1<cap;v++){ if(*v=='\\'&&v[1])v++; out[i++]=*v; } out[i]=0;
}
static void json_escape(const char *src, char *dst, size_t cap){
    size_t o=0;
    if(cap==0) return;
    for(size_t i=0; src[i] && o+2<cap; i++){
        char c=src[i];
        if(c=='\"'){ dst[o++]='\\'; dst[o++]='\"'; }
        else if(c=='\\'){ dst[o++]='\\'; dst[o++]='\\'; }
        else if(c=='\n'){ dst[o++]='\\'; dst[o++]='n'; }
        else if(c=='\r'){ dst[o++]='\\'; dst[o++]='r'; }
        else if((unsigned char)c<0x20){  }
        else { dst[o++]=c; }
    }
    dst[o]='\0';
}

/* Вход и регистрация по нику и паролю */
double net_auth(const char *url, const char *nick, const char *pass) {
    if (!nick || !nick_valid(nick)) return (double)NET_LOGIN_BAD_NICK;
    if (!pass || !pass_valid(pass)) return (double)NET_LOGIN_BAD_PASS;
    const char *base = (url && *url) ? url : net.base;
    if (!base || !*base) return (double)NET_ERROR;
    if (!net.base[0] && url && *url) {
        snprintf(net.base, sizeof(net.base), "%s", url);
        size_t n = strlen(net.base);
        while (n && net.base[n - 1] == '/') net.base[--n] = 0;
    }

    char req_url[URL], resp[RESP];
    snprintf(req_url, sizeof(req_url), "%s/users/%s.json", net.base, nick);
    int code = http("GET", req_url, NULL, resp, sizeof(resp));
    if (code == 0) {
        if (net_log_ok()) LOGERR("net_auth: network error connecting to %s", req_url);
        return (double)NET_ERROR;
    }

    const char *p = skip_ws(resp);
    if (!p || !*p || !strncmp(p, "null", 4) || code == 404) {
        /* Аккаунт не существует -> Регистрация и вход! */
        char body[BODY], epass[128], enick[64];
        json_escape(pass, epass, sizeof(epass));
        json_escape(nick, enick, sizeof(enick));
        snprintf(body, sizeof(body),
                 "{\"nick\":\"%s\",\"pass\":\"%s\",\"cups\":0,\"candies\":0,\"cls\":0,\"azum\":0,\"santa\":0,\"level\":0,\"levels\":0}",
                 enick, epass);
        /* PUT только если узел всё ещё пуст. Это не даёт гонке двух
         * регистраций перезаписать уже существующий ник. */
        int put_code = http_ex("PUT", req_url, body, NULL, 0,
                               "if-match", "null", NULL, 0);
        if (put_code == 412) {
            /* Кто-то успел зарегистрировать ник между GET и PUT. Повторный
             * вызов увидит готовую запись и выполнит обычный вход, не создавая
             * дубликат и не затирая её пароль/прогресс. */
            return net_auth(base, nick, pass);
        }
        if (put_code != 200) {
            if (net_log_ok()) LOGERR("net_auth: register failed HTTP %d", put_code);
            return (double)NET_ERROR;
        }
        pg_lock();
        pg.cups = 0; pg.candies = 0; pg.cls = 0; pg.azum = 0; pg.santa = 0;
        pg.level = 0; pg.levels_unlocked = 0; pg.loaded = 1;
        pg_unlock();
        pending_cls = 0;
        progress_write(0, 0, 0, 0, 0, 0, 0);
        lg_lock();
        snprintf(lg.session_nick, sizeof(lg.session_nick), "%s", nick);
        snprintf(lg.session_pass, sizeof(lg.session_pass), "%s", pass);
        lg.status = NET_LOGIN_OK;
        lg_unlock();
        session_save(nick, pass);
        LOG("registered and logged in: '%s'", nick);
        return (double)NET_LOGIN_OK;
    }

    if (*p == '{') {
        char got_pass[64] = "";
        strv(resp, "pass", got_pass, sizeof(got_pass));
        if (strcmp(got_pass, pass) == 0) {
            /* Пароль совпал -> Вход и загрузка облачного прогресса */
            int cups = (int)num(resp, "cups", 0);
            int candies = (int)num(resp, "candies", 0);
            int cls = (int)num(resp, "cls", 0);
            int azum = (int)num(resp, "azum", 0);
            int santa = (int)num(resp, "santa", 0);
            int level = (int)num(resp, "level", 0);
            int levels_unlocked = (int)num(resp, "levels", -1);
            /* Старые аккаунты имели только level=1. Сохраняем этот прогресс
             * как уже открытый первый уровень, а новые аккаунты начинают с 0. */
            if (levels_unlocked < 0) levels_unlocked = level;
            if (cups < 0) cups = 0;
            if (candies < 0) candies = 0;
            if (cls != 1 && cls != 2) cls = 0;
            azum = azum ? 1 : 0;
            santa = santa ? 1 : 0;
            if (levels_unlocked < 0) levels_unlocked = 0;
            if (levels_unlocked > 5) levels_unlocked = 5;
            if (level < 0 || level > levels_unlocked) level = 0;
            if (cls == 1 && !azum) cls = 0;
            if (cls == 2 && !santa) cls = 0;
            pg_lock();
            pg.cups = cups; pg.candies = candies; pg.cls = cls; pg.azum = azum; pg.santa = santa;
            pg.level = level; pg.levels_unlocked = levels_unlocked; pg.loaded = 1;
            pg_unlock();
            pending_cls = cls;
            progress_write(cups, candies, cls, azum, santa, level, levels_unlocked);
            lg_lock();
            snprintf(lg.session_nick, sizeof(lg.session_nick), "%s", nick);
            snprintf(lg.session_pass, sizeof(lg.session_pass), "%s", pass);
            lg.status = NET_LOGIN_OK;
            lg_unlock();
            session_save(nick, pass);
            LOG("logged in: '%s' (cups=%d, candies=%d)", nick, cups, candies);
            return (double)NET_LOGIN_OK;
        } else {
            if (net_log_ok()) LOG("wrong password for '%s'", nick);
            return (double)NET_LOGIN_WRONG_PASS;
        }
    }

    return (double)NET_ERROR;
}

double net_set_nick(const char *nick) {
    if (!nick || !nick_valid(nick)) return 0.0;
    return net_auth(net.base, nick, "123456") == (double)NET_LOGIN_OK ? 1.0 : 0.0;
}

void net_logout(void) {
    char path[320];
    lg_lock();
    lg.session_nick[0] = 0;
    lg.session_pass[0] = 0;
    lg.status = NET_LOGIN_IDLE;
    lg_unlock();
    if (lg.path[0]) snprintf(path, sizeof(path), "%s/%s", lg.path, SESSION_FILE);
    else snprintf(path, sizeof(path), "%s", SESSION_FILE);
    remove(path);
}

void net_set_data_path(const char *path) {
    lg_lock();
    if (path && *path) snprintf(lg.path, sizeof(lg.path), "%s", path);
    else lg.path[0] = 0;
    lg_unlock();
}

void net_autologin(const char *url) {
    char path[320], nick[LOGIN_NICK_MAX+2] = "", pass[64] = "";
    FILE *f;
    lg_lock();
    if (lg.status == NET_LOGIN_OK) { lg_unlock(); return; }
    if (lg.path[0]) snprintf(path, sizeof(path), "%s/%s", lg.path, SESSION_FILE);
    else snprintf(path, sizeof(path), "%s", SESSION_FILE);
    lg_unlock();
    f = fopen(path, "r");
    if (!f) return;
    int n = fscanf(f, "%17s %63s", nick, pass);
    fclose(f);
    if (n < 1 || !nick_valid(nick)) return;
    if (url && *url && !net.base[0]) {
        snprintf(net.base, sizeof(net.base), "%s", url);
        size_t bl = strlen(net.base);
        while (bl && net.base[bl - 1] == '/') net.base[--bl] = 0;
    }
    if (pass[0] && url && *url) {
        double res = net_auth(url, nick, pass);
        if (res == 2.0) {
            LOG("autologin (cloud): nick '%s'", nick);
            return;
        }
    }
    lg_lock();
    snprintf(lg.session_nick, sizeof(lg.session_nick), "%s", nick);
    snprintf(lg.session_pass, sizeof(lg.session_pass), "%s", pass);
    lg.status = NET_LOGIN_OK;
    lg_unlock();
    LOG("autologin (cached): nick '%s'", nick);
}

double net_login_status(void) {
    double v;
    lg_lock(); v = (double)lg.status; lg_unlock();
    return v;
}
const char *net_login_nick(void) {
    lg_lock();
    if (lg.status == NET_LOGIN_OK && lg.session_nick[0])
        snprintf(s_lg_nick_ret, sizeof(s_lg_nick_ret), "%s", lg.session_nick);
    else s_lg_nick_ret[0] = 0;
    lg_unlock();
    return s_lg_nick_ret;
}
const char *net_login_pass(void) {
    lg_lock();
    if (lg.status == NET_LOGIN_OK && lg.session_pass[0])
        snprintf(s_lg_pass_ret, sizeof(s_lg_pass_ret), "%s", lg.session_pass);
    else s_lg_pass_ret[0] = 0;
    lg_unlock();
    return s_lg_pass_ret;
}

/* Лидерборд по кубкам */
typedef struct {
    char nick[24];
    int cups;
} LeaderboardEntry;

typedef struct {
    DSMutex lock;
    int status; /* 0 idle, 1 loading, 2 ready, 4 error */
    int count;
    LeaderboardEntry entries[16];
} Leaderboard;
static Leaderboard lb = { .lock = DS_MUTEX_INIT };

static int parse_leaderboard(const char *json, LeaderboardEntry *out, int max_out) {
    const char *p = skip_ws(json);
    int count = 0;
    if (!p || *p != '{') return 0;
    for (p = skip_ws(p + 1); p && *p && *p != '}' && count < 64; ) {
        const char *kend = skip_str(p);
        if (!kend) break;
        char key[32] = {0};
        size_t kl = (size_t)(kend - p - 2);
        if (kl >= sizeof(key)) kl = sizeof(key) - 1;
        memcpy(key, p + 1, kl); key[kl] = 0;
        p = skip_ws(kend);
        if (*p != ':') break;
        p = skip_ws(p + 1);
        const char *obj = p;
        p = skip_val(p);
        if (!p) break;
        if (*obj == '{') {
            char nick[24] = {0};
            strv(obj, "nick", nick, sizeof(nick));
            if (!nick[0]) snprintf(nick, sizeof(nick), "%s", key);
            int cups = (int)num(obj, "cups", 0);
            if (cups < 0) cups = 0;
            snprintf(out[count].nick, sizeof(out[count].nick), "%s", nick);
            out[count].cups = cups;
            count++;
        }
        p = skip_ws(p);
        if (p && *p == ',') p = skip_ws(p + 1);
    }
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - 1 - i; j++) {
            if (out[j].cups < out[j + 1].cups) {
                LeaderboardEntry tmp = out[j];
                out[j] = out[j + 1];
                out[j + 1] = tmp;
            }
        }
    }
    if (count > max_out) count = max_out;
    return count;
}

static void *lb_fetch_job(void *arg) {
    char *base = (char *)arg;
    char url[URL], resp[65536];
    snprintf(url, sizeof(url), "%s/users.json", base);
    free(base);
    int code = http("GET", url, NULL, resp, sizeof(resp));
    if (code != 200) {
        ds_mutex_lock(lb.lock);
        lb.status = 4;
        ds_mutex_unlock(lb.lock);
        return NULL;
    }
    LeaderboardEntry tmp[64];
    int cnt = parse_leaderboard(resp, tmp, 10);
    ds_mutex_lock(lb.lock);
    lb.count = cnt;
    for (int i = 0; i < cnt; i++) lb.entries[i] = tmp[i];
    lb.status = 2;
    ds_mutex_unlock(lb.lock);
    return NULL;
}
#ifdef _WIN32
static unsigned __stdcall win_lb_fetch(void *arg) { lb_fetch_job(arg); return 0; }
#endif
void net_leaderboard_fetch(const char *url) {
    const char *base = (url && *url) ? url : net.base;
    if (!base || !*base) return;
    ds_mutex_lock(lb.lock);
    lb.status = 1;
    lb.count = 0;
    ds_mutex_unlock(lb.lock);
    char *arg = (char*)malloc(URL);
    if (!arg) return;
    snprintf(arg, URL, "%s", base);
    DSThread t;
#ifdef _WIN32
    t = ds_thread_start(win_lb_fetch, arg);
#else
    t = ds_thread_start(lb_fetch_job, arg);
#endif
    if (t) ds_thread_detach(t);
    else {
        ds_mutex_lock(lb.lock); lb.status = 4; ds_mutex_unlock(lb.lock);
        free(arg);
    }
}
double net_leaderboard_status(void) {
    double s;
    ds_mutex_lock(lb.lock); s = (double)lb.status; ds_mutex_unlock(lb.lock);
    return s;
}
double net_leaderboard_count(void) {
    double c;
    ds_mutex_lock(lb.lock); c = (double)lb.count; ds_mutex_unlock(lb.lock);
    return c;
}
static char s_lb_nick_ret[24];
const char *net_leaderboard_nick(double idx) {
    int i = (int)idx;
    ds_mutex_lock(lb.lock);
    if (i >= 0 && i < lb.count) snprintf(s_lb_nick_ret, sizeof(s_lb_nick_ret), "%s", lb.entries[i].nick);
    else s_lb_nick_ret[0] = 0;
    ds_mutex_unlock(lb.lock);
    return s_lb_nick_ret;
}
double net_leaderboard_cups(double idx) {
    int i = (int)idx;
    double cups = 0;
    ds_mutex_lock(lb.lock);
    if (i >= 0 && i < lb.count) cups = (double)lb.entries[i].cups;
    ds_mutex_unlock(lb.lock);
    return cups;
}

static void *http_post_job(void *arg) {
    HttpJob *j = (HttpJob*)arg;
    if (j) { http("POST", j->url, j->body, NULL, 0); free(j); }
    return NULL;
}
#ifdef _WIN32
static unsigned __stdcall win_http_post(void *arg) { http_post_job(arg); return 0; }
#endif
static void http_post_async(const char *url, const char *body) {
    HttpJob *j = (HttpJob*)malloc(sizeof(*j));
    DSThread t;
    if (!j) return;
    snprintf(j->url, sizeof(j->url), "%s", url);
    snprintf(j->body, sizeof(j->body), "%s", body);
#ifdef _WIN32
    t = ds_thread_start(win_http_post, j);
#else
    t = ds_thread_start(http_post_job, j);
#endif
    if (t) { ds_thread_detach(t); return; }
    free(j);
}
static void *http_put_job(void *arg) {
    HttpJob *j = (HttpJob*)arg;
    if (j) { http("PUT", j->url, j->body, NULL, 0); free(j); }
    return NULL;
}
#ifdef _WIN32
static unsigned __stdcall win_http_put(void *arg) { http_put_job(arg); return 0; }
#endif
static void http_put_async(const char *url, const char *body) {
    HttpJob *j = (HttpJob*)malloc(sizeof(*j));
    DSThread t;
    if (!j) return;
    snprintf(j->url, sizeof(j->url), "%s", url);
    snprintf(j->body, sizeof(j->body), "%s", body);
#ifdef _WIN32
    t = ds_thread_start(win_http_put, j);
#else
    t = ds_thread_start(http_put_job, j);
#endif
    if (t) { ds_thread_detach(t); return; }
    free(j);
}
static void *http_delete_job(void *arg) {
    char *url = (char*)arg;
    if (url) { http("DELETE", url, NULL, NULL, 0); free(url); }
    return NULL;
}
#ifdef _WIN32
static unsigned __stdcall win_http_delete(void *arg) { http_delete_job(arg); return 0; }
#endif
static void chat_delete_key_async(const char *key) {
    char *url;
    DSThread t;
    if (!key || !*key) return;
    if (strchr(key, '/') || strchr(key, '.') || strchr(key, '#')) return;
    url = (char*)malloc(URL);
    if (!url) return;
    snprintf(url, URL, "%s/rooms/%s/chat/%s.json", net.base, net.room, key);
#ifdef _WIN32
    t = ds_thread_start(win_http_delete, url);
#else
    t = ds_thread_start(http_delete_job, url);
#endif
    if (t) { ds_thread_detach(t); return; }
    free(url);
}
static int push_state(void) {
    Actor a; int slot; unsigned long seq; char url[URL], body[BODY], enick[64];
    lock(); a=net.me; slot=net.slot; seq=++net.seq; unlock();
    if(slot<0) return 0;
    json_escape(a.nick,enick,sizeof(enick));
    snprintf(url,sizeof(url),"%s/rooms/%s/players/%d.json",net.base,net.room,slot);
    snprintf(body,sizeof(body),"{\"uid\":\"%s\",\"nick\":\"%s\",\"x\":%.5f,\"y\":%.5f,\"angle\":%.5f,\"hp\":%.0f,\"alive\":%.0f,\"seq\":%lu,\"px\":%.5f,\"py\":%.5f,\"pdx\":%.5f,\"pdy\":%.5f,\"punch\":%.0f,\"cls\":%.0f,\"frz\":%.2f,\"gift\":%.0f,\"gx\":%.5f,\"gy\":%.5f,\"gdx\":%.5f,\"gdy\":%.5f}",
        net.uid,enick,safe(a.x),safe(a.y),safe(a.a),safe(a.hp),safe(a.alive),seq,
        safe(a.punch_x),safe(a.punch_y),safe(a.punch_dx),safe(a.punch_dy),safe(a.punch),safe(a.cls),
        safe(a.frz),safe(a.gift),safe(a.gift_x),safe(a.gift_y),safe(a.gift_dx),safe(a.gift_dy));
    int c = http("PUT",url,body,NULL,0);
    if (c != 200 && net_log_ok()) LOGERR("push state: HTTP %d (room write denied? check Firebase rules)", c);
    return c == 200;
}
static int pull_state(char *resp,size_t cap) {
    char url[URL];
    snprintf(url,sizeof(url),"%s/rooms/%s/players.json",net.base,net.room);
    int c = http("GET",url,NULL,resp,cap);
    if (c != 200 && net_log_ok()) LOGERR("pull state: HTTP %d", c);
    return c == 200;
}
static int pull_event(char *resp,size_t cap) {
    char url[URL];
    snprintf(url,sizeof(url),"%s/event.json",net.base);
    int c = http("GET",url,NULL,resp,cap);
    if (c != 200 && net_log_ok()) LOGERR("pull event: HTTP %d", c);
    return c == 200;
}

static void release_slot(void) {
    int slot; char url[URL]; lock(); slot=net.slot; net.slot=-1; unlock(); if(slot<0)return;
    snprintf(url,sizeof(url),"%s/rooms/%s/players/%d.json",net.base,net.room,slot); http("DELETE",url,NULL,NULL,0);
}
static int claim_slot(void) {
    static unsigned long seen_seq[NET_SLOTS]; static long long seen_at[NET_SLOTS]; static char seen_uid[NET_SLOTS][24];
    char resp[RESP],url[URL],body[BODY],uid[24],etag[96]; long long t=now_ms(); int slot;
    for(slot=0;slot<NET_SLOTS;slot++) {
        unsigned long seq; int claim=0,code;
        if (!net.run) return -1;
        snprintf(url,sizeof(url),"%s/rooms/%s/players/%d.json",net.base,net.room,slot);
        code=http_ex("GET",url,NULL,resp,sizeof(resp),"X-Firebase-ETag","true",etag,sizeof(etag));
        if(code!=200) { if(net_log_ok()) LOGERR("claim slot %d: HTTP %d", slot, code); continue; }
        strv(resp,"uid",uid,sizeof(uid));
        if(uid[0]&&!strcmp(uid,net.uid))return slot;
        seq=(unsigned long)num(resp,"seq",0);
        if(!uid[0])claim=1;
        else if(etag[0]&&(strcmp(uid,seen_uid[slot])||seq!=seen_seq[slot])) { snprintf(seen_uid[slot],sizeof(seen_uid[slot]),"%s",uid); seen_seq[slot]=seq; seen_at[slot]=t; }
        else if(etag[0]&&t-seen_at[slot]>=STALE)claim=1;
        if(!claim)continue;
        if(!etag[0]) {
            if(net_log_ok()) LOG("claim slot %d: no ETag, claiming empty slot without if-match", slot);
        }
        {
            char enick[64];
            json_escape(net.me.nick,enick,sizeof(enick));
            snprintf(body,sizeof(body),"{\"uid\":\"%s\",\"nick\":\"%s\",\"x\":0,\"y\":0,\"angle\":0,\"hp\":0,\"alive\":0,\"seq\":0,\"px\":0,\"py\":0,\"pdx\":0,\"pdy\":0,\"punch\":0,\"cls\":%.0f,\"frz\":0,\"gift\":0,\"gx\":0,\"gy\":0,\"gdx\":0,\"gdy\":0}",net.uid,enick,safe(net.me.cls));
        }
        code=http_ex("PUT",url,body,NULL,0, etag[0] ? "if-match" : NULL, etag[0] ? etag : NULL, NULL, 0);
        if(code==200) { seen_uid[slot][0]=0; seen_seq[slot]=0; seen_at[slot]=0; LOG("slot %d uid %s",slot,net.uid); return slot; }
        if(net_log_ok()) LOGERR("claim slot %d: PUT failed, HTTP %d", slot, code);
    }
    return -1;
}
static void read_players(const char *resp) {
    static unsigned long lseq[NET_SLOTS]; static long long lch[NET_SLOTS];
    Actor ps[NET_SLOTS]; long long t=now_ms(); int local,count=0,slot;
    memset(ps,0,sizeof(ps));
    lock(); local=net.slot; unlock();
    for(slot=0;slot<NET_SLOTS;slot++) {
        char bp[24],p[40],uid[24]; unsigned long sq; int online;
        if(slot==local) { lock(); ps[slot]=net.me; ps[slot].online=local>=0; unlock(); if(local>=0)count++; continue; }
        snprintf(bp,sizeof(bp),"%d",slot); snprintf(p,sizeof(p),"%s/uid",bp); strv(resp,p,uid,sizeof(uid));
        if(!uid[0]||!strcmp(uid,net.uid)){ lseq[slot]=0; lch[slot]=0; continue; }
        snprintf(p,sizeof(p),"%s/seq",bp); sq=(unsigned long)num(resp,p,0);
        if(sq!=lseq[slot]){ lseq[slot]=sq; lch[slot]=t; } else if(!lch[slot]) lch[slot]=t;
        online=t-lch[slot]<TIMEOUT; if(!online)continue;
        snprintf(p,sizeof(p),"%s/nick",bp); strv(resp,p,ps[slot].nick,sizeof(ps[slot].nick));
        snprintf(p,sizeof(p),"%s/x",bp); ps[slot].x=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/y",bp); ps[slot].y=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/angle",bp); ps[slot].a=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/hp",bp); ps[slot].hp=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/alive",bp); ps[slot].alive=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/px",bp); ps[slot].punch_x=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/py",bp); ps[slot].punch_y=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/pdx",bp); ps[slot].punch_dx=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/pdy",bp); ps[slot].punch_dy=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/punch",bp); ps[slot].punch=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/cls",bp); ps[slot].cls=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/frz",bp); ps[slot].frz=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/gift",bp); ps[slot].gift=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/gx",bp); ps[slot].gift_x=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/gy",bp); ps[slot].gift_y=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/gdx",bp); ps[slot].gift_dx=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/gdy",bp); ps[slot].gift_dy=num(resp,p,0);
        ps[slot].online=1; count++;
    }
    lock(); memcpy(net.players,ps,sizeof(ps)); net.count=count; unlock();
}
static int parse_chat_list(const char *json, ChatMsg *tmp, int cap){
    int tmp_cnt=0;
    const char *p=skip_ws(json);
    if(!p||!*p||!strncmp(p,"null",4)) return 0;
    if(*p!='{') return 0;
    for(p=skip_ws(p+1); p&&*p&&*p!='}'&&tmp_cnt<cap; ){
        if(*p!='"') break;
        const char *kend=skip_str(p);
        if(!kend) break;
        char key[28]={0};
        size_t kl=(size_t)(kend-p-2);
        if(kl>=sizeof(key)) kl=sizeof(key)-1;
        memcpy(key,p+1,kl); key[kl]=0;
        p=skip_ws(kend);
        if(*p!=':') break;
        p=skip_ws(p+1);
        const char *obj=p;
        p=skip_val(p);
        if(!p) break;
        memset(&tmp[tmp_cnt], 0, sizeof(tmp[tmp_cnt]));
        strncpy(tmp[tmp_cnt].key, key, sizeof(tmp[tmp_cnt].key)-1);
        strv(obj,"uid",tmp[tmp_cnt].uid,sizeof(tmp[tmp_cnt].uid));
        strv(obj,"nick",tmp[tmp_cnt].nick,sizeof(tmp[tmp_cnt].nick));
        strv(obj,"text",tmp[tmp_cnt].text,sizeof(tmp[tmp_cnt].text));
        tmp[tmp_cnt].valid=1;
        tmp_cnt++;
        p=skip_ws(p);
        if(p&&*p==',') p=skip_ws(p+1);
    }
    return tmp_cnt;
}
static void parse_and_store_chat(const char *json){
    ChatMsg tmp[CHAT_MAX+16];
    int tmp_cnt, keep, start, i;
    char extra[CHAT_MAX+16][28];
    int nextra=0;
    if(!json || !*json) return;
    tmp_cnt=parse_chat_list(json, tmp, CHAT_MAX+16);
    if(tmp_cnt<=0) return;
    lock(); keep=chat_keep; unlock();
    if(keep<1) keep=1;
    if(keep>CHAT_MAX) keep=CHAT_MAX;
    start=0;
    if(tmp_cnt>keep){
        int drop=tmp_cnt-keep;
        for(i=0;i<drop;i++){
            if(tmp[i].key[0]){
                strncpy(extra[nextra], tmp[i].key, sizeof(extra[0])-1);
                extra[nextra][sizeof(extra[0])-1]=0;
                nextra++;
            }
        }
        start=drop;
    }
    lock();
    net.chat_count=0;
    for(i=start;i<tmp_cnt && net.chat_count<CHAT_MAX;i++)
        net.chats[net.chat_count++]=tmp[i];
    unlock();
    for(i=0;i<nextra;i++) chat_delete_key_async(extra[i]);
}
static int pull_chat(char *resp, size_t cap){
    char url[URL];
    snprintf(url,sizeof(url),"%s/rooms/%s/chat.json?orderBy=%%22$key%%22&limitToLast=%d",net.base,net.room,CHAT_MAX);
    return http("GET",url,NULL,resp,cap)==200;
}
static void prune_old_chat(void){
    char url[URL], resp[CHAT_RESP];
    ChatMsg oldm[16];
    char keepkeys[CHAT_MAX][28];
    int n, nk=0, count, keep, i, j;
    lock(); keep=chat_keep; count=net.chat_count;
    for(i=0;i<net.chat_count;i++){
        strncpy(keepkeys[nk], net.chats[i].key, sizeof(keepkeys[0])-1);
        keepkeys[nk][sizeof(keepkeys[0])-1]=0;
        nk++;
    }
    unlock();
    if(count<keep || keep<1) return;
    snprintf(url,sizeof(url),"%s/rooms/%s/chat.json?orderBy=%%22$key%%22&limitToFirst=16",net.base,net.room);
    if(http("GET",url,NULL,resp,sizeof(resp))!=200) return;
    n=parse_chat_list(resp, oldm, 16);
    for(i=0;i<n;i++){
        int kept=0;
        if(!oldm[i].key[0]) continue;
        for(j=0;j<nk;j++) if(!strcmp(oldm[i].key, keepkeys[j])){ kept=1; break; }
        if(!kept) chat_delete_key_async(oldm[i].key);
    }
}

#ifdef _WIN32
static void *thread_main(void *arg);
static void *reader_thread(void *arg);
static unsigned __stdcall win_thread_main(void *arg){ thread_main(arg); return 0; }
static unsigned __stdcall win_reader_thread(void *arg){ reader_thread(arg); return 0; }
#endif
static void *thread_main(void *arg) {
    int fails=0; (void)arg;
    while(net.run) {
        long long start=now_ms(); int slot;
        lock(); slot=net.slot; unlock();
        if(slot<0) {
            status(NET_CONNECTING); slot=claim_slot();
            if(slot<0){ if(++fails>6){status(NET_ERROR); LOGERR("network error: cannot claim player slot in room '%s'", net.room);} sleep_ms(500); continue; }
            lock(); net.slot=slot; net.seq=0; net.players[slot]=net.me; net.players[slot].online=1; unlock(); fails=0;
            LOG("slot %d claimed", (int)net.slot);
        }
        if(!push_state()){ if(++fails>6){status(NET_ERROR); LOGERR("network error: failed to push player state");} sleep_ms(300); continue; }
        fails=0; status(NET_PLAYING);
        long long spent=now_ms()-start; if(spent<WRITE_TICK)sleep_ms((int)(WRITE_TICK-spent));
    }
    release_slot(); status(NET_OFFLINE); return NULL;
}
static void *reader_thread(void *arg) {
    char resp[RESP], chat_resp[CHAT_RESP], event_resp[RESP];
    long long next_chat=0, next_event=0;
    (void)arg;
    while(net.run) {
        long long start=now_ms(); int slot;
        lock(); slot=net.slot; unlock();
        if(slot<0){ sleep_ms(50); continue; }
        if(pull_state(resp,sizeof(resp))) read_players(resp);
        if(start>=next_chat) {
            if(pull_chat(chat_resp,sizeof(chat_resp))) {
                parse_and_store_chat(chat_resp);
                prune_old_chat();
            }
            next_chat=now_ms()+CHAT_TICK;
        }
        if(start>=next_event) {
            /* Раньше здесь не было скобок: lock() выполнялся по условию, а
             * присваивание и unlock() — всегда, поэтому номер ивента мог
             * прийти из старого буфера, а мьютекс отпускался незанятым. */
            if(pull_event(event_resp,sizeof(event_resp))) {
                int ev=(int)num(event_resp,"",0);
                if(ev<0) ev=0;
                lock(); net.event=ev; unlock();
            }
            next_event=now_ms()+EVENT_TICK;
        }
        long long spent=now_ms()-start; if(spent<READ_TICK)sleep_ms((int)(READ_TICK-spent));
    }
    return NULL;
}
static void make_uid(void) {
    unsigned long a,b; int local=0;
#ifdef _WIN32
    a=(unsigned long)time(NULL)^((unsigned long)GetTickCount64()<<8);
    b=(unsigned long)_getpid()^(unsigned long)(uintptr_t)&local;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    a=(unsigned long)time(NULL)^((unsigned long)t.tv_nsec<<8);
    b=(unsigned long)getpid()^(unsigned long)(uintptr_t)&local;
#endif
    snprintf(net.uid,sizeof(net.uid),"%08lx%08lx",a&0xfffffffful,b&0xfffffffful);
}
void net_connect(const char *url, const char *room) {
    size_t n; if(!url||!*url)return;
    net_fast = 0;
    if(net.started)net_disconnect();
    memset(&net.me,0,sizeof(net.me));
    snprintf(net.base,sizeof(net.base),"%s",url); n=strlen(net.base); while(n&&net.base[n-1]=='/')net.base[--n]=0;
    snprintf(net.room,sizeof(net.room),"%s",(room&&*room)?room:"main");
    if(!net.uid[0])make_uid();
    {
        char snick[LOGIN_NICK_MAX+1];
        lg_lock(); if (lg.status == NET_LOGIN_OK) snprintf(snick,sizeof(snick),"%s",lg.session_nick); else snick[0]=0; lg_unlock();
        if (snick[0]) snprintf(net.me.nick,sizeof(net.me.nick),"%s",snick);
    }
    net.me.cls = (double)pending_cls;
    net.started=1;
    net.status=NET_CONNECTING; net.run=1;
    LOG("connect %s/%s (write %dms read %dms)", net.base, net.room, WRITE_TICK, READ_TICK);
#ifdef _WIN32
    net.thread = ds_thread_start(win_thread_main, NULL);
    net.rthread = ds_thread_start(win_reader_thread, NULL);
#else
    net.thread = ds_thread_start(thread_main, NULL);
    net.rthread = ds_thread_start(reader_thread, NULL);
#endif
}
void net_disconnect(void) {
    if(!net.started)return;
    net_fast = 1;
    net.run=0;
    ds_thread_join(net.thread); net.thread=(DSThread)0;
    ds_thread_join(net.rthread); net.rthread=(DSThread)0;
    net.started=0; net.slot=-1; status(NET_OFFLINE);
    net_fast = 0;
}
void net_publish(double x, double y, double angle, double hp, double alive, double freeze) {
    lock();
    net.me.x=x; net.me.y=y; net.me.a=angle; net.me.hp=hp; net.me.alive=alive; net.me.frz=freeze;
    if(net.slot>=0){ net.players[net.slot]=net.me; net.players[net.slot].online=1; }
    unlock();
}
void net_publish_punch(double x, double y, double dx, double dy, double punch) {
    lock();
    net.me.punch_x=x; net.me.punch_y=y;
    net.me.punch_dx=dx; net.me.punch_dy=dy; net.me.punch=punch;
    if(net.slot>=0){ net.players[net.slot]=net.me; net.players[net.slot].online=1; }
    unlock();
}
void net_publish_gift(double x, double y, double dx, double dy, double gift) {
    lock();
    net.me.gift_x=x; net.me.gift_y=y;
    net.me.gift_dx=dx; net.me.gift_dy=dy; net.me.gift=gift;
    if(net.slot>=0){ net.players[net.slot]=net.me; net.players[net.slot].online=1; }
    unlock();
}
/* Ивенты: номер лежит в корневом узле /event и виден всем клиентам сразу.
 * 0 — ивента нет, 1 — «диско», 2 — снегопад. Запускает их админ из панели. */
static void *event_fetch_job(void *arg) {
    char *base=(char*)arg, url[URL], resp[RESP];
    snprintf(url,sizeof(url),"%s/event.json",base);
    free(base);
    if (http("GET",url,NULL,resp,sizeof(resp))==200) {
        int v=(int)num(resp,"",0);
        if (v<0) v=0;
        lock(); net.event=v; unlock();
    }
    return NULL;
}
#ifdef _WIN32
static unsigned __stdcall win_event_fetch(void *arg) { event_fetch_job(arg); return 0; }
#endif
void net_event_fetch(const char *url) {
    const char *base=(url&&*url)?url:net.base;
    char *arg; DSThread t;
    if(!base||!*base) return;
    arg=(char*)malloc(URL);
    if(!arg) return;
    snprintf(arg,URL,"%s",base);
#ifdef _WIN32
    t = ds_thread_start(win_event_fetch, arg);
#else
    t = ds_thread_start(event_fetch_job, arg);
#endif
    if (t) ds_thread_detach(t); else free(arg);
}
void net_event_set(const char *url, double value) {
    const char *base=(url&&*url)?url:net.base;
    char full[URL], body[32];
    int v=(int)value;
    if(v<0) v=0;
    lock(); net.event=v; unlock();
    if(!base||!*base) return;
    snprintf(full,sizeof(full),"%s/event.json",base);
    snprintf(body,sizeof(body),"%d",v);
    http_put_async(full, body);
    LOG("event set to %d",v);
}
void net_chat_send(const char *text){
    if(!net.started || !text || !*text) return;
    if(!net.run) return;
    char url[URL], body[BODY*2], esc[CHAT_TEXT_MAX*2], enick[64];
    const char *nick;
    json_escape(text, esc, sizeof(esc));
    lock(); nick = net.me.nick[0] ? net.me.nick : net.uid; json_escape(nick, enick, sizeof(enick)); unlock();
    snprintf(url,sizeof(url),"%s/rooms/%s/chat.json",net.base,net.room);
    snprintf(body,sizeof(body),"{\"uid\":\"%s\",\"nick\":\"%s\",\"text\":\"%s\"}",net.uid,enick,esc);
    http_post_async(url, body);
    LOG("chat send %s",text);
}
void net_chat_trim(double keep){
    int k=(int)keep;
    char extra[CHAT_MAX][28];
    int nextra=0, drop, i;
    if(k<1) k=1;
    if(k>CHAT_MAX) k=CHAT_MAX;
    lock();
    chat_keep=k;
    if(net.chat_count>k){
        drop=net.chat_count-k;
        for(i=0;i<drop;i++){
            if(net.chats[i].key[0]){
                strncpy(extra[nextra], net.chats[i].key, sizeof(extra[0])-1);
                extra[nextra][sizeof(extra[0])-1]=0;
                nextra++;
            }
        }
        memmove(net.chats, net.chats+drop, (size_t)k*sizeof(net.chats[0]));
        net.chat_count=k;
        if(k<CHAT_MAX) memset(net.chats+k, 0, (size_t)(CHAT_MAX-k)*sizeof(net.chats[0]));
    }
    unlock();
    for(i=0;i<nextra;i++) chat_delete_key_async(extra[i]);
}
double net_chat_count(void){ double v; lock(); v=net.chat_count; unlock(); return v; }
const char* net_chat_text(double idx){
    int i=(int)idx;
    lock();
    if(i>=0 && i<net.chat_count && net.chats[i].valid){
        strncpy(s_chat_text_ret, net.chats[i].text, sizeof(s_chat_text_ret)-1);
        s_chat_text_ret[sizeof(s_chat_text_ret)-1]='\0';
    }else{
        s_chat_text_ret[0]='\0';
    }
    unlock();
    return s_chat_text_ret;
}
const char* net_chat_uid(double idx){
    int i=(int)idx;
    lock();
    if(i>=0 && i<net.chat_count && net.chats[i].valid){
        const char *src = net.chats[i].nick[0] ? net.chats[i].nick : net.chats[i].uid;
        strncpy(s_chat_uid_ret, src, sizeof(s_chat_uid_ret)-1);
        s_chat_uid_ret[sizeof(s_chat_uid_ret)-1]='\0';
    }else{
        s_chat_uid_ret[0]='\0';
    }
    unlock();
    return s_chat_uid_ret;
}
static int sidx(double slot){ int i=(int)slot; return i>=0&&i<NET_SLOTS?i:-1; }
double net_status(void){ double v; lock(); v=net.status; unlock(); return v; }
double net_slot(void){ double v; lock(); v=net.slot; unlock(); return v; }
double net_event(void){ double v; lock(); v=net.event; unlock(); return v; }
double net_count(void){ double v; lock(); v=net.count; unlock(); return v; }
const char* net_player_nick(double slot){
    int i=sidx(slot);
    lock();
    if(i>=0 && net.players[i].nick[0]){
        strncpy(s_nick_ret, net.players[i].nick, sizeof(s_nick_ret)-1);
        s_nick_ret[sizeof(s_nick_ret)-1]='\0';
    }else{
        s_nick_ret[0]='\0';
    }
    unlock();
    return s_nick_ret;
}
#define READER(name, field) double name(double slot){ int i=sidx(slot); double v=0; if(i>=0){lock();v=field;unlock();} return v; }
READER(net_player_online, net.players[i].online?1:0)
READER(net_player_x, net.players[i].x)
READER(net_player_y, net.players[i].y)
READER(net_player_angle, net.players[i].a)
READER(net_player_hp, net.players[i].hp)
READER(net_player_alive, net.players[i].alive)
READER(net_player_punch_x, net.players[i].punch_x)
READER(net_player_punch_y, net.players[i].punch_y)
READER(net_player_punch_dx, net.players[i].punch_dx)
READER(net_player_punch_dy, net.players[i].punch_dy)
READER(net_player_punch, net.players[i].punch)
READER(net_player_class, net.players[i].cls)
READER(net_player_freeze, net.players[i].frz)
READER(net_player_gift, net.players[i].gift)
READER(net_player_gift_x, net.players[i].gift_x)
READER(net_player_gift_y, net.players[i].gift_y)
READER(net_player_gift_dx, net.players[i].gift_dx)
READER(net_player_gift_dy, net.players[i].gift_dy)
#undef READER
