#include "runtime.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#define DS_ERROR_MESSAGE_SIZE 1024

/* --- Консоль (показ лога и ошибок в игре) --- */
#define DS_CONSOLE_MAX 256
#define DS_CONSOLE_LINE_MAX 192
static char ds_console_buf[DS_CONSOLE_MAX][DS_CONSOLE_LINE_MAX];
static int ds_console_type_buf[DS_CONSOLE_MAX];
static int ds_console_head = 0;   /* индекс следующей записи (кольцо) */
static int ds_console_count = 0;  /* сколько всего строк хранится */
static pthread_mutex_t ds_console_lock = PTHREAD_MUTEX_INITIALIZER;

/* копии для чтения из игрового потока — защищены мьютексом,
 * чтобы сетевые потоки не перезаписали строку во время отрисовки */
#define DS_CONSOLE_READ_SLOTS 8
static char ds_console_read[DS_CONSOLE_READ_SLOTS][DS_CONSOLE_LINE_MAX];
static int ds_console_read_pos = 0;

static void console_add(const char *line, int is_error) {
    if (!line) return;
    char tmp[DS_CONSOLE_LINE_MAX];
    /* одна строка — без переводов */
    size_t n = strlen(line);
    size_t w = 0;
    for (size_t i = 0; i < n && w + 1 < sizeof(tmp); i++) {
        char c = line[i];
        tmp[w++] = (c == '\n' || c == '\r') ? ' ' : c;
    }
    tmp[w] = '\0';
    pthread_mutex_lock(&ds_console_lock);
    snprintf(ds_console_buf[ds_console_head], DS_CONSOLE_LINE_MAX, "%s", tmp);
    ds_console_type_buf[ds_console_head] = is_error ? 1 : 0;
    ds_console_head = (ds_console_head + 1) % DS_CONSOLE_MAX;
    if (ds_console_count < DS_CONSOLE_MAX) ds_console_count++;
    pthread_mutex_unlock(&ds_console_lock);
}

int console_count(void) { return ds_console_count; }
int console_type(int index) {
    int t = 0;
    pthread_mutex_lock(&ds_console_lock);
    if (index >= 0 && index < ds_console_count) {
        int pos = (ds_console_head - ds_console_count + index) % DS_CONSOLE_MAX;
        if (pos < 0) pos += DS_CONSOLE_MAX;
        t = ds_console_type_buf[pos];
    }
    pthread_mutex_unlock(&ds_console_lock);
    return t;
}
const char *console_line(int index) {
    pthread_mutex_lock(&ds_console_lock);
    char *slot = ds_console_read[ds_console_read_pos];
    ds_console_read_pos = (ds_console_read_pos + 1) % DS_CONSOLE_READ_SLOTS;
    if (index >= 0 && index < ds_console_count) {
        int pos = (ds_console_head - ds_console_count + index) % DS_CONSOLE_MAX;
        if (pos < 0) pos += DS_CONSOLE_MAX;
        snprintf(slot, DS_CONSOLE_LINE_MAX, "%s", ds_console_buf[pos]);
    } else {
        slot[0] = '\0';
    }
    pthread_mutex_unlock(&ds_console_lock);
    return slot;
}
void console_clear(void) {
    pthread_mutex_lock(&ds_console_lock);
    ds_console_count = 0; ds_console_head = 0;
    pthread_mutex_unlock(&ds_console_lock);
}

Joy joy = {0};
int screen_w = 0;
int screen_h = 0;
double dt = 0.0;

static jmp_buf ds_error_jump;
static int ds_error_handler_active = 0;
static int ds_has_error = 0;
static int ds_restart_requested = 0;
static char ds_last_error[DS_ERROR_MESSAGE_SIZE] = {0};

typedef struct DSStringNode DSStringNode;
struct DSStringNode { DSStringNode *next; char *string; };
static DSStringNode *ds_strings = NULL;

void ds_log(const char *format, ...) {
    char tmp[DS_CONSOLE_LINE_MAX];
    va_list args;
    va_start(args, format);
    __android_log_vprint(ANDROID_LOG_INFO, "DimScript", format, args);
    va_end(args);
    va_start(args, format);
    vsnprintf(tmp, sizeof(tmp), format, args);
    va_end(args);
    console_add(tmp, 0);
}

void ds_log_err(const char *format, ...) {
    char tmp[DS_CONSOLE_LINE_MAX];
    va_list args;
    va_start(args, format);
    __android_log_vprint(ANDROID_LOG_ERROR, "DimScript", format, args);
    va_end(args);
    va_start(args, format);
    vsnprintf(tmp, sizeof(tmp), format, args);
    va_end(args);
    console_add(tmp, 1);
}

/* потокобезопасная запись в консоль + logcat (для net.c и прочих потоков) */
void ds_console_log(int is_error, const char *format, ...) {
    char tmp[DS_CONSOLE_LINE_MAX];
    va_list args;
    va_start(args, format);
    __android_log_vprint(is_error ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, "DimScript", format, args);
    va_end(args);
    va_start(args, format);
    vsnprintf(tmp, sizeof(tmp), format, args);
    va_end(args);
    console_add(tmp, is_error ? 1 : 0);
}

void ds_runtime_error(const char *format, ...) {
    char tmp[DS_CONSOLE_LINE_MAX];
    va_list args, copy;
    va_start(args, format);
    va_copy(copy, args);
    vsnprintf(ds_last_error, sizeof(ds_last_error), format, copy);
    va_end(copy);
    __android_log_vprint(ANDROID_LOG_ERROR, "DimScript", format, args);
    va_end(args);
    va_start(args, format);
    vsnprintf(tmp, sizeof(tmp), format, args);
    va_end(args);
    console_add(tmp, 1);
    ds_has_error = 1;
    if (ds_error_handler_active) longjmp(ds_error_jump, 1);
}

int ds_call_protected(DSProtectedFunction function, void *userdata, const char *label) {
    int jumped;
    if (!function) {
        if (label && *label) ds_runtime_error("cannot call an empty script hook '%s'", label);
        else ds_runtime_error("cannot call an empty script hook");
        return 0;
    }
    if (ds_error_handler_active) {
        function(userdata);
        return !ds_has_error;
    }
    ds_error_handler_active = 1;
    jumped = setjmp(ds_error_jump);
    if (jumped == 0) {
        function(userdata);
        ds_error_handler_active = 0;
        return !ds_has_error;
    }
    ds_error_handler_active = 0;
    if (label && *label && ds_last_error[0] == '\0') {
        snprintf(ds_last_error, sizeof(ds_last_error), "script hook '%s' failed", label);
    }
    return 0;
}

const char *ds_runtime_error_message(void) { return ds_last_error[0] ? ds_last_error : "unknown DimScript runtime error"; }
int ds_script_has_error(void) { return ds_has_error; }
void ds_clear_runtime_error(void) { ds_has_error = 0; ds_last_error[0] = '\0'; }
void ds_request_script_restart(void) { ds_restart_requested = 1; }
int ds_script_restart_requested(void) { return ds_restart_requested; }
void ds_clear_script_restart(void) { ds_restart_requested = 0; }

static char *ds_strdup(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s)+1;
    char *c = (char*)malloc(n);
    if (c) memcpy(c, s, n);
    return c;
}
static char *ds_track_string(char *s) {
    if (!s) { ds_runtime_error("out of memory string"); return NULL; }
    DSStringNode *node = (DSStringNode*)malloc(sizeof(*node));
    if (!node) { free(s); ds_runtime_error("out of memory tracking"); return NULL; }
    node->string = s; node->next = ds_strings; ds_strings = node;
    return s;
}
char *ds_num_to_string(double number) {
    char buf[96];
    if (snprintf(buf, sizeof(buf), "%g", number) < 0) return NULL;
    return ds_track_string(ds_strdup(buf));
}
void ds_string_pool_reset(void) {
    DSStringNode *node = ds_strings;
    while (node) { DSStringNode *next = node->next; free(node->string); free(node); node = next; }
    ds_strings = NULL;
}
char *ds_concat(const char *left, const char *right) {
    size_t la = left ? strlen(left) : 0, lb = right ? strlen(right) : 0;
    char *out = (char*)malloc(la+lb+1);
    if (!out) return ds_track_string(ds_strdup(""));
    if (la) memcpy(out, left, la);
    if (lb) memcpy(out+la, right, lb);
    out[la+lb] = '\0';
    return ds_track_string(out);
}

/* ---------- новые типы: массивы ---------- */
struct DSArray { double *data; size_t len, cap; };
DSArray* arr_new(void) {
    DSArray *a = (DSArray*)calloc(1, sizeof(*a));
    if (!a) { ds_runtime_error("arr_new OOM"); return NULL; }
    a->cap = 8; a->data = (double*)malloc(a->cap*sizeof(double));
    if (!a->data) { free(a); ds_runtime_error("arr_new OOM"); return NULL; }
    return a;
}
void arr_push(DSArray* a, double v) {
    if (!a) return;
    if (a->len >= a->cap) {
        size_t nc = a->cap*2; if (nc<8) nc=8;
        double *nd = (double*)realloc(a->data, nc*sizeof(double));
        if (!nd) { ds_runtime_error("arr_push OOM"); return; }
        a->data = nd; a->cap = nc;
    }
    a->data[a->len++] = v;
}
double arr_pop(DSArray* a) {
    if (!a || a->len==0) return 0;
    return a->data[--a->len];
}
double arr_get(DSArray* a, double idx) {
    if (!a) return 0;
    long i = (long)idx;
    if (i<0 || (size_t)i>=a->len) return 0;
    return a->data[i];
}
void arr_set(DSArray* a, double idx, double v) {
    if (!a) return;
    long i = (long)idx;
    if (i<0) return;
    if ((size_t)i>=a->len) {
        // расширяем нулями
        while (a->len <= (size_t)i) arr_push(a, 0);
    }
    a->data[i] = v;
}
double arr_len(DSArray* a) { return a ? (double)a->len : 0; }
void arr_clear(DSArray* a) { if (a) a->len=0; }
void arr_free(DSArray* a) { if (!a) return; free(a->data); free(a); }

/* ---------- словари ---------- */
typedef struct DSDictEntry { char *key; double val; struct DSDictEntry *next; } DSDictEntry;
struct DSDict { DSDictEntry *head; };
DSDict* dict_new(void) { DSDict *d = (DSDict*)calloc(1,sizeof(*d)); if(!d) ds_runtime_error("dict_new OOM"); return d; }
void dict_set(DSDict* d, const char* key, double val) {
    if (!d||!key) return;
    for (DSDictEntry *e=d->head; e; e=e->next) if (strcmp(e->key,key)==0) { e->val=val; return; }
    DSDictEntry *e = (DSDictEntry*)malloc(sizeof(*e)); if(!e){ ds_runtime_error("dict_set OOM"); return; }
    e->key=ds_strdup(key); e->val=val; e->next=d->head; d->head=e;
}
double dict_get(DSDict* d, const char* key) {
    if (!d||!key) return 0;
    for (DSDictEntry *e=d->head; e; e=e->next) if (strcmp(e->key,key)==0) return e->val;
    return 0;
}
int dict_has(DSDict* d, const char* key) {
    if (!d||!key) return 0;
    for (DSDictEntry *e=d->head; e; e=e->next) if (strcmp(e->key,key)==0) return 1;
    return 0;
}
void dict_del(DSDict* d, const char* key) {
    if (!d||!key) return;
    DSDictEntry **pp=&d->head;
    while(*pp){ if(strcmp((*pp)->key,key)==0){ DSDictEntry *t=*pp; *pp=t->next; free(t->key); free(t); return; } pp=&(*pp)->next; }
}
void dict_free(DSDict* d) {
    if (!d) return;
    DSDictEntry *e=d->head;
    while(e){ DSDictEntry *n=e->next; free(e->key); free(e); e=n; }
    free(d);
}

/* ---------- таймеры ---------- */
struct DSTimer { long long start_ms; };
static long long now_ms(void){
    struct timespec ts;
#ifdef CLOCK_MONOTONIC
    if (clock_gettime(CLOCK_MONOTONIC,&ts)==0) return (long long)ts.tv_sec*1000 + ts.tv_nsec/1000000;
#endif
    return (long long)time(NULL)*1000;
}
DSTimer* timer_new(void){ DSTimer *t=(DSTimer*)malloc(sizeof(*t)); if(!t){ ds_runtime_error("timer_new OOM"); return NULL; } t->start_ms=now_ms(); return t; }
void timer_start(DSTimer* t){ if(t) t->start_ms=now_ms(); }
double timer_elapsed(DSTimer* t){ if(!t) return 0; return (now_ms()-t->start_ms)/1000.0; }
void timer_reset(DSTimer* t){ if(t) t->start_ms=now_ms(); }
void timer_free(DSTimer* t){ free(t); }

/* ---------- файлы ---------- */
const char* file_read(const char* path){
    if(!path) return ds_track_string(ds_strdup(""));
    FILE *f=fopen(path,"rb");
    if(!f){
        // пробуем с префиксами
        char buf[512];
        snprintf(buf,sizeof(buf),"game/assets/%s",path);
        f=fopen(buf,"rb");
        if(!f){ snprintf(buf,sizeof(buf),"assets/%s",path); f=fopen(buf,"rb"); }
    }
    if(!f) return ds_track_string(ds_strdup(""));
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if(sz<0) sz=0; if(sz>10*1024*1024) sz=10*1024*1024;
    char *data=(char*)malloc(sz+1);
    if(!data){ fclose(f); return ds_track_string(ds_strdup("")); }
    size_t r=fread(data,1,sz,f); fclose(f);
    data[r]='\0';
    return ds_track_string(data);
}
int file_write(const char* path, const char* content){
    if(!path) return 0;
    FILE *f=fopen(path,"wb");
    if(!f) return 0;
    size_t len=content?strlen(content):0;
    size_t w=fwrite(content?content:"",1,len,f);
    fclose(f);
    return w==len;
}
int file_exists(const char* path){
    if(!path) return 0;
    FILE *f=fopen(path,"rb");
    if(f){ fclose(f); return 1; }
    char buf[512];
    snprintf(buf,sizeof(buf),"game/assets/%s",path);
    f=fopen(buf,"rb"); if(f){ fclose(f); return 1; }
    snprintf(buf,sizeof(buf),"assets/%s",path);
    f=fopen(buf,"rb"); if(f){ fclose(f); return 1; }
    return 0;
}
int file_del(const char* path){ if(!path) return 0; return remove(path)==0; }

/* ---------- json (мини-парсер, использует логику из net.c) ---------- */
static const char* skip_ws(const char* p){ while(p&&*p&&( *p==' '||*p=='\n'||*p=='\r'||*p=='\t')) p++; return p; }
static double parse_number(const char* p){ char *e; double v=strtod(p,&e); return v; }
double json_get_num(const char* json, const char* path){
    if(!json||!path) return 0;
    // очень простой: ищем "path": number   путь вида a/b/c
    // для простоты ищем последнее имя
    const char *key = strrchr(path,'/');
    if (key) key++; else key=path;
    char pattern[128];
    snprintf(pattern,sizeof(pattern),"\"%s\"",key);
    const char *pos=strstr(json,pattern);
    if(!pos) return 0;
    pos=strchr(pos,':');
    if(!pos) return 0;
    pos=skip_ws(pos+1);
    return parse_number(pos);
}
const char* json_get_str(const char* json, const char* path){
    if(!json||!path) return ds_track_string(ds_strdup(""));
    const char *key = strrchr(path,'/');
    if (key) key++; else key=path;
    char pattern[128];
    snprintf(pattern,sizeof(pattern),"\"%s\"",key);
    const char *pos=strstr(json,pattern);
    if(!pos) return ds_track_string(ds_strdup(""));
    pos=strchr(pos,':');
    if(!pos) return ds_track_string(ds_strdup(""));
    pos=skip_ws(pos+1);
    if(*pos!='"') return ds_track_string(ds_strdup(""));
    pos++;
    const char *end=strchr(pos,'"');
    if(!end) return ds_track_string(ds_strdup(""));
    size_t len=end-pos;
    char *out=(char*)malloc(len+1);
    if(!out) return ds_track_string(ds_strdup(""));
    memcpy(out,pos,len); out[len]='\0';
    return ds_track_string(out);
}
int json_get_bool(const char* json, const char* path){
    if(!json||!path) return 0;
    const char *key = strrchr(path,'/');
    if (key) key++; else key=path;
    char pattern[128];
    snprintf(pattern,sizeof(pattern),"\"%s\"",key);
    const char *pos=strstr(json,pattern);
    if(!pos) return 0;
    pos=strchr(pos,':');
    if(!pos) return 0;
    pos=skip_ws(pos+1);
    if(strncmp(pos,"true",4)==0) return 1;
    if(strncmp(pos,"false",5)==0) return 0;
    return parse_number(pos)!=0;
}

/* ---------- сеть высокого уровня (обёртка над http в net.c) ---------- */
const char* http_get(const char* url);
const char* http_post(const char* url, const char* body);
const char* http_get(const char* url){
    // используем file_read как заглушку для десктопа, на Android — net.c сделает
    // для простоты пробуем через file_read если url — путь
    if(!url) return ds_track_string(ds_strdup(""));
    if(strncmp(url,"http://",7)!=0 && strncmp(url,"https://",8)!=0){
        return file_read(url);
    }
    // если сеть доступна через net.c, можно было бы вызвать, но пока возвращаем пусто
    // реальная реализация есть в net.c через net_http, но требует java vm
    return ds_track_string(ds_strdup(""));
}
const char* http_post(const char* url, const char* body){
    (void)url; (void)body;
    return ds_track_string(ds_strdup(""));
}

/* ---------- утилиты ---------- */
double clamp(double v, double lo, double hi){ if(v<lo) return lo; if(v>hi) return hi; return v; }
double lerp(double a, double b, double t){ return a + (b-a)*t; }
double dist(double x1, double y1, double x2, double y2){ double dx=x2-x1, dy=y2-y1; return sqrt(dx*dx+dy*dy); }
double now(void){ return now_ms()/1000.0; }

double str_len(const char *s){ return s ? (double)strlen(s) : 0; }
int str_eq(const char *a, const char *b){ if(a==b) return 1; if(!a||!b) return 0; return strcmp(a,b)==0; }

/* ---------- реальная (системная) клавиатура через JNI (Android) ----------
 * Кастомной нарисованной клавиатуры больше нет: текст вводится системной
 * клавиатурой Android. Символы берём не из голого keycode, а через
 * android.view.KeyEvent.getUnicodeChar(metaState) — поэтому работают
 * заглавные буквы, кириллица и любая раскладка. */
#include <android/native_activity.h>
#include <android/keycodes.h>
#include <jni.h>
#define KB_BUF 256
static ANativeActivity *kb_activity = NULL;
static char kb_text[KB_BUF] = {0};
static int kb_len = 0;
static int kb_show = 0;
static int kb_enter = 0;

void ds_set_activity(void *act){ kb_activity = (ANativeActivity*)act; }

/* ANativeActivity_showSoftInput с NativeActivity почти никогда не работает
 * (известный баг: окно без фокусируемого View). Поэтому клавиатуру
 * открываем/закрываем напрямую через JNI:
 *   показать — InputMethodManager.toggleSoftInput(SHOW_FORCED, 0)
 *   скрыть   — InputMethodManager.hideSoftInputFromWindow(token, 0)   */
static int kb_ime(int show){
    JNIEnv *env=NULL; JavaVM *vm; int attached=0; int ok=0;
    if(!kb_activity || !kb_activity->vm || !kb_activity->clazz) return 0;
    vm = kb_activity->vm;
    if((*vm)->GetEnv(vm,(void**)&env,JNI_VERSION_1_6)!=JNI_OK){
        if((*vm)->AttachCurrentThread(vm,&env,NULL)!=JNI_OK) return 0;
        attached=1;
    }
    if((*env)->PushLocalFrame(env,16)!=0){ if(attached)(*vm)->DetachCurrentThread(vm); return 0; }
    {
        jobject activity = kb_activity->clazz;
        jclass act_cls = (*env)->GetObjectClass(env,activity);
        jmethodID get_svc = (*env)->GetMethodID(env,act_cls,"getSystemService","(Ljava/lang/String;)Ljava/lang/Object;");
        jstring svc_name; jobject imm; jclass imm_cls;
        if(!get_svc) goto done;
        svc_name = (*env)->NewStringUTF(env,"input_method");
        if(!svc_name) goto done;
        imm = (*env)->CallObjectMethod(env,activity,get_svc,svc_name);
        if((*env)->ExceptionCheck(env) || !imm) goto done;
        imm_cls = (*env)->GetObjectClass(env,imm);
        if(show){
            /* SHOW_FORCED=2 + HIDE_IMPLICIT_ONLY=1: клавиатура выезжает даже без
             * фокусного View, а повторный вызов «показать» её НЕ прячет
             * (toggle не скрывает явно показанную клавиатуру). */
            jmethodID toggle = (*env)->GetMethodID(env,imm_cls,"toggleSoftInput","(II)V");
            if(!toggle) goto done;
            (*env)->CallVoidMethod(env,imm,toggle,(jint)2,(jint)1);
            if((*env)->ExceptionCheck(env)) goto done;
            ok=1;
        } else {
            jmethodID get_win = (*env)->GetMethodID(env,act_cls,"getWindow","()Landroid/view/Window;");
            jobject window; jclass win_cls; jmethodID get_decor; jobject decor;
            jclass view_cls; jmethodID get_tok; jobject token; jmethodID hide;
            if(!get_win) goto done;
            window = (*env)->CallObjectMethod(env,activity,get_win);
            if((*env)->ExceptionCheck(env) || !window) goto done;
            win_cls = (*env)->GetObjectClass(env,window);
            get_decor = (*env)->GetMethodID(env,win_cls,"getDecorView","()Landroid/view/View;");
            if(!get_decor) goto done;
            decor = (*env)->CallObjectMethod(env,window,get_decor);
            if((*env)->ExceptionCheck(env) || !decor) goto done;
            view_cls = (*env)->GetObjectClass(env,decor);
            get_tok = (*env)->GetMethodID(env,view_cls,"getWindowToken","()Landroid/os/IBinder;");
            if(!get_tok) goto done;
            token = (*env)->CallObjectMethod(env,decor,get_tok);
            if((*env)->ExceptionCheck(env)) goto done;
            hide = (*env)->GetMethodID(env,imm_cls,"hideSoftInputFromWindow","(Landroid/os/IBinder;I)Z");
            if(!hide) goto done;
            (*env)->CallBooleanMethod(env,imm,hide,token,(jint)0);
            if((*env)->ExceptionCheck(env)) goto done;
            ok=1;
        }
    }
done:
    if((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env,NULL);
    if(attached) (*vm)->DetachCurrentThread(vm);
    return ok;
}
void keyboard_show(void){
    if(!kb_activity) return;
    if(!kb_ime(1)) /* запасной путь, если JNI недоступен */
        ANativeActivity_showSoftInput(kb_activity, ANATIVEACTIVITY_SHOW_SOFT_INPUT_FORCED);
    kb_show = 1;
}
void keyboard_hide(void){
    if(!kb_activity) return;
    if(!kb_ime(0))
        ANativeActivity_hideSoftInput(kb_activity, ANATIVEACTIVITY_HIDE_SOFT_INPUT_IMPLICIT_ONLY);
    kb_show = 0;
}
const char* keyboard_get_text(void){ return ds_track_string(ds_strdup(kb_text)); }
const char* keyboard_get_raw(void){ return kb_text; }
void keyboard_clear(void){ kb_text[0]='\0'; kb_len=0; kb_enter=0; }
int keyboard_visible(void){ return kb_show; }
int keyboard_enter_pressed(void){ int e=kb_enter; kb_enter=0; return e; }
void keyboard_type(const char *text){
    if(!text) return;
    size_t n=strlen(text);
    for(size_t i=0;i<n && kb_len+1<KB_BUF-1;i++){
        char c=text[i];
        if(c=='\n'||c=='\r'){ kb_enter=1; continue; }
        kb_text[kb_len++]=c;
    }
    kb_text[kb_len]='\0';
}
/* стираем целый UTF-8 символ, а не один байт — иначе кириллица «ломается» */
void keyboard_backspace(void){
    while(kb_len>0){
        unsigned char c=(unsigned char)kb_text[--kb_len];
        kb_text[kb_len]='\0';
        if((c & 0xC0) != 0x80) break;   /* дошли до ведущего байта */
    }
}

/* Кодовая точка -> UTF-8 в буфер ввода (кириллица и прочий юникод) */
static void kb_append_cp(unsigned int cp){
    char u[5]; int n=0;
    if(cp<0x80){ u[0]=(char)cp; n=1; }
    else if(cp<0x800){ u[0]=(char)(0xC0|(cp>>6)); u[1]=(char)(0x80|(cp&0x3F)); n=2; }
    else if(cp<0x10000){ u[0]=(char)(0xE0|(cp>>12)); u[1]=(char)(0x80|((cp>>6)&0x3F)); u[2]=(char)(0x80|(cp&0x3F)); n=3; }
    else { u[0]=(char)(0xF0|(cp>>18)); u[1]=(char)(0x80|((cp>>12)&0x3F)); u[2]=(char)(0x80|((cp>>6)&0x3F)); u[3]=(char)(0x80|(cp&0x3F)); n=4; }
    u[n]='\0';
    if(kb_len+n < KB_BUF-1){ memcpy(kb_text+kb_len,u,(size_t)n); kb_len+=n; kb_text[kb_len]='\0'; }
}

/* JNI: KeyEvent(action, keycode).getUnicodeChar(metaState) — настоящий символ
 * системной клавиатуры с учётом раскладки, Shift и Caps. */
static unsigned int kb_unicode(int keycode, int meta){
    JNIEnv *env=NULL; JavaVM *vm; jclass cls; jmethodID ctor, get_uni;
    jobject ev; jint uni=0; int attached=0;
    if(!kb_activity || !kb_activity->vm) return 0;
    vm = kb_activity->vm;
    if((*vm)->GetEnv(vm,(void**)&env,JNI_VERSION_1_6)!=JNI_OK){
        if((*vm)->AttachCurrentThread(vm,&env,NULL)!=JNI_OK) return 0;
        attached=1;
    }
    if((*env)->PushLocalFrame(env,8)!=0){ if(attached)(*vm)->DetachCurrentThread(vm); return 0; }
    cls=(*env)->FindClass(env,"android/view/KeyEvent");
    if(!cls) goto done;
    ctor=(*env)->GetMethodID(env,cls,"<init>","(II)V");
    get_uni=(*env)->GetMethodID(env,cls,"getUnicodeChar","(I)I");
    if(!ctor||!get_uni) goto done;
    ev=(*env)->NewObject(env,cls,ctor,(jint)0,(jint)keycode);  /* ACTION_DOWN */
    if(!ev||(*env)->ExceptionCheck(env)) goto done;
    uni=(*env)->CallIntMethod(env,ev,get_uni,(jint)meta);
    if((*env)->ExceptionCheck(env)) uni=0;
done:
    if((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env,NULL);
    if(attached) (*vm)->DetachCurrentThread(vm);
    return uni>0 ? (unsigned int)uni : 0;
}

int keyboard_handle_key(int keycode, int action, int meta){
    (void)action;
    if(keycode==AKEYCODE_DEL){ keyboard_backspace(); return 1; }
    if(keycode==AKEYCODE_FORWARD_DEL){ keyboard_backspace(); return 1; }
    if(keycode==AKEYCODE_ENTER || keycode==AKEYCODE_NUMPAD_ENTER || keycode==AKEYCODE_DPAD_CENTER){ kb_enter=1; return 1; }
    if(keycode==AKEYCODE_SPACE){ kb_append_cp(' '); return 1; }
    /* настоящий символ с учётом раскладки/Shift — работает и для кириллицы */
    {
        unsigned int cp = kb_unicode(keycode, meta);
        if(cp>=0x20 && cp!=0x7F){ kb_append_cp(cp); return 1; }
    }
    /* запасной путь, если JNI недоступен */
    if(keycode>=AKEYCODE_A && keycode<=AKEYCODE_Z){ kb_append_cp((unsigned int)('a'+(keycode-AKEYCODE_A))); return 1; }
    if(keycode>=AKEYCODE_0 && keycode<=AKEYCODE_9){ kb_append_cp((unsigned int)('0'+(keycode-AKEYCODE_0))); return 1; }
    if(keycode==AKEYCODE_COMMA){ kb_append_cp(','); return 1; }
    if(keycode==AKEYCODE_PERIOD){ kb_append_cp('.'); return 1; }
    if(keycode==AKEYCODE_MINUS){ kb_append_cp('-'); return 1; }
    return 0;
}

/* ACTION_MULTIPLE / вставка строки из системной клавиатуры */
void keyboard_commit_utf8(const char *utf8){
    if(!utf8) return;
    size_t n=strlen(utf8);
    for(size_t i=0;i<n && kb_len+1<KB_BUF-1;i++){
        char c=utf8[i];
        if(c=='\n'||c=='\r'){ kb_enter=1; continue; }
        kb_text[kb_len++]=c;
    }
    kb_text[kb_len]='\0';
}
