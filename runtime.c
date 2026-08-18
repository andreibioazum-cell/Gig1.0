#include "runtime.h"
#include <stdarg.h>
#include <stdio.h>
#ifndef _WIN32
#include <pthread.h>
#endif
#define DS_ERROR_MESSAGE_SIZE 1024
#define DS_CONSOLE_MAX 256
#define DS_CONSOLE_LINE_MAX 192
static char ds_console_buf[DS_CONSOLE_MAX][DS_CONSOLE_LINE_MAX];
static int ds_console_type_buf[DS_CONSOLE_MAX];
static int ds_console_head = 0;
static int ds_console_count = 0;
#ifdef _WIN32
static SRWLOCK ds_console_lock = SRWLOCK_INIT;
static void console_lock(void) { AcquireSRWLockExclusive(&ds_console_lock); }
static void console_unlock(void) { ReleaseSRWLockExclusive(&ds_console_lock); }
#else
static pthread_mutex_t ds_console_lock = PTHREAD_MUTEX_INITIALIZER;
static void console_lock(void) { pthread_mutex_lock(&ds_console_lock); }
static void console_unlock(void) { pthread_mutex_unlock(&ds_console_lock); }
#endif
#define DS_CONSOLE_READ_SLOTS 8
static char ds_console_read[DS_CONSOLE_READ_SLOTS][DS_CONSOLE_LINE_MAX];
static int ds_console_read_pos = 0;
static void console_add(const char *line, int is_error) {
    if (!line) return;
    char tmp[DS_CONSOLE_LINE_MAX];
    size_t n = strlen(line);
    size_t w = 0;
    for (size_t i = 0; i < n && w + 1 < sizeof(tmp); i++) {
        char c = line[i];
        tmp[w++] = (c == '\n' || c == '\r') ? ' ' : c;
    }
    tmp[w] = '\0';
    console_lock();
    snprintf(ds_console_buf[ds_console_head], DS_CONSOLE_LINE_MAX, "%s", tmp);
    ds_console_type_buf[ds_console_head] = is_error ? 1 : 0;
    ds_console_head = (ds_console_head + 1) % DS_CONSOLE_MAX;
    if (ds_console_count < DS_CONSOLE_MAX) ds_console_count++;
    console_unlock();
}
int console_count(void) { return ds_console_count; }
int console_type(int index) {
    int t = 0;
    console_lock();
    if (index >= 0 && index < ds_console_count) {
        int pos = (ds_console_head - ds_console_count + index) % DS_CONSOLE_MAX;
        if (pos < 0) pos += DS_CONSOLE_MAX;
        t = ds_console_type_buf[pos];
    }
    console_unlock();
    return t;
}
const char *console_line(int index) {
    console_lock();
    char *slot = ds_console_read[ds_console_read_pos];
    ds_console_read_pos = (ds_console_read_pos + 1) % DS_CONSOLE_READ_SLOTS;
    if (index >= 0 && index < ds_console_count) {
        int pos = (ds_console_head - ds_console_count + index) % DS_CONSOLE_MAX;
        if (pos < 0) pos += DS_CONSOLE_MAX;
        snprintf(slot, DS_CONSOLE_LINE_MAX, "%s", ds_console_buf[pos]);
    } else {
        slot[0] = '\0';
    }
    console_unlock();
    return slot;
}
void console_clear(void) {
    console_lock();
    ds_console_count = 0; ds_console_head = 0;
    console_unlock();
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
static void platform_log(int is_error, const char *format, va_list args) {
#ifdef _WIN32
    char tmp[DS_CONSOLE_LINE_MAX]; (void)is_error;
    vsnprintf(tmp, sizeof(tmp), format, args);
    OutputDebugStringA(tmp); OutputDebugStringA("\n");
#elif defined(__ANDROID__)
    __android_log_vprint(is_error ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, "DimScript", format, args);
#else
    char tmp[DS_CONSOLE_LINE_MAX]; (void)is_error;
    vsnprintf(tmp, sizeof(tmp), format, args);
    fprintf(stderr, "%s\n", tmp);
#endif
}
void ds_log(const char *format, ...) {
    char tmp[DS_CONSOLE_LINE_MAX];
    va_list args;
    va_start(args, format);
    platform_log(0, format, args);
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
    platform_log(1, format, args);
    va_end(args);
    va_start(args, format);
    vsnprintf(tmp, sizeof(tmp), format, args);
    va_end(args);
    console_add(tmp, 1);
}
void ds_console_log(int is_error, const char *format, ...) {
    char tmp[DS_CONSOLE_LINE_MAX];
    va_list args;
    va_start(args, format);
    platform_log(is_error, format, args);
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
    platform_log(1, format, args);
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
        while (a->len <= (size_t)i) arr_push(a, 0);
    }
    a->data[i] = v;
}
double arr_len(DSArray* a) { return a ? (double)a->len : 0; }
void arr_clear(DSArray* a) { if (a) a->len=0; }
void arr_free(DSArray* a) { if (!a) return; free(a->data); free(a); }
double clamp(double v, double lo, double hi){ if(v<lo) return lo; if(v>hi) return hi; return v; }
double lerp(double a, double b, double t){ return a + (b-a)*t; }
double dist(double x1, double y1, double x2, double y2){ double dx=x2-x1, dy=y2-y1; return sqrt(dx*dx+dy*dy); }
double str_len(const char *s){ return s ? (double)strlen(s) : 0; }
int str_eq(const char *a, const char *b){ if(a==b) return 1; if(!a||!b) return 0; return strcmp(a,b)==0; }

#ifdef __ANDROID__
#include <android/native_activity.h>
#include <android/keycodes.h>
#include <jni.h>
#define KB_BUF 384
static ANativeActivity *kb_activity = NULL;
static char kb_text[KB_BUF] = {0};
static char kb_raw_snapshot[KB_BUF] = {0};
static int kb_len = 0;
static int kb_show = 0;
static int kb_enter = 0;
static pthread_mutex_t kb_mutex = PTHREAD_MUTEX_INITIALIZER;

static JNIEnv *kb_get_env(int *attached) {
    JNIEnv *env = NULL;
    JavaVM *vm;
    if (attached) *attached = 0;
    if (!kb_activity || !kb_activity->vm) return NULL;
    vm = kb_activity->vm;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return NULL;
        if (attached) *attached = 1;
    }
    return env;
}

static void kb_release_env(int attached) {
    if (attached && kb_activity && kb_activity->vm)
        (*kb_activity->vm)->DetachCurrentThread(kb_activity->vm);
}

static jstring kb_new_java_string(JNIEnv *env, const char *value) {
    const unsigned char *p = (const unsigned char *)(value ? value : "");
    jchar chars[KB_BUF];
    jsize n = 0;
    while (*p && n < KB_BUF-1) {
        unsigned int cp;
        if (*p < 0x80) cp = *p++;
        else if ((*p&0xE0)==0xC0 && (p[1]&0xC0)==0x80) {
            cp=((p[0]&0x1F)<<6)|(p[1]&0x3F); p+=2;
        } else if ((*p&0xF0)==0xE0 && (p[1]&0xC0)==0x80 && (p[2]&0xC0)==0x80) {
            cp=((p[0]&15)<<12)|((p[1]&63)<<6)|(p[2]&63); p+=3;
        } else if ((*p&0xF8)==0xF0 && (p[1]&0xC0)==0x80 &&
                   (p[2]&0xC0)==0x80 && (p[3]&0xC0)==0x80) {
            cp=((p[0]&7)<<18)|((p[1]&63)<<12)|((p[2]&63)<<6)|(p[3]&63); p+=4;
        } else { cp=0xFFFD; p++; }
        if (cp>0x10FFFF || (cp>=0xD800 && cp<=0xDFFF)) cp=0xFFFD;
        if (cp<0x10000) chars[n++]=(jchar)cp;
        else if (n+2<=KB_BUF-1) {
            cp-=0x10000;
            chars[n++]=(jchar)(0xD800+(cp>>10));
            chars[n++]=(jchar)(0xDC00+(cp&0x3FF));
        } else break;
    }
    return (*env)->NewString(env, chars, n);
}

static int kb_call_text_method(const char *name, const char *value) {
    int attached = 0, ok = 0;
    JNIEnv *env = kb_get_env(&attached);
    if (!env || !kb_activity || !kb_activity->clazz) return 0;
    if ((*env)->PushLocalFrame(env, 8) != 0) { kb_release_env(attached); return 0; }
    jclass cls = (*env)->GetObjectClass(env, kb_activity->clazz);
    jmethodID method = cls ? (*env)->GetMethodID(env, cls, name, "(Ljava/lang/String;)V") : NULL;
    if (method) {
        jstring text = kb_new_java_string(env, value);
        if (text) {
            (*env)->CallVoidMethod(env, kb_activity->clazz, method, text);
            ok = !(*env)->ExceptionCheck(env);
        }
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
    kb_release_env(attached);
    return ok;
}

static int kb_call_bool_method(const char *name) {
    int attached = 0, result = 0;
    JNIEnv *env = kb_get_env(&attached);
    if (!env || !kb_activity || !kb_activity->clazz) return 0;
    if ((*env)->PushLocalFrame(env, 4) != 0) { kb_release_env(attached); return 0; }
    jclass cls = (*env)->GetObjectClass(env, kb_activity->clazz);
    jmethodID method = cls ? (*env)->GetMethodID(env, cls, name, "()Z") : NULL;
    if (method) {
        jboolean value = (*env)->CallBooleanMethod(env, kb_activity->clazz, method);
        if (!(*env)->ExceptionCheck(env)) result = value ? 1 : 0;
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
    kb_release_env(attached);
    return result;
}

static int kb_call_void_method(const char *name) {
    int attached = 0, ok = 0;
    JNIEnv *env = kb_get_env(&attached);
    if (!env || !kb_activity || !kb_activity->clazz) return 0;
    if ((*env)->PushLocalFrame(env, 4) != 0) { kb_release_env(attached); return 0; }
    jclass cls = (*env)->GetObjectClass(env, kb_activity->clazz);
    jmethodID method = cls ? (*env)->GetMethodID(env, cls, name, "()V") : NULL;
    if (method) {
        (*env)->CallVoidMethod(env, kb_activity->clazz, method);
        ok = !(*env)->ExceptionCheck(env);
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
    kb_release_env(attached);
    return ok;
}

void ds_set_activity(void *act) { kb_activity = (ANativeActivity *)act; }

/* Единственный источник правды для текста — системный редактор в Java.
 * Любое изменение буфера на стороне игры немедленно зеркалим обратно в него,
 * иначе редактор помнит уже стёртые символы и дописывает их к новому вводу
 * ("Q" + "qwerty" -> "Qqwerty"). */
static void kb_sync_editor(void) {
    char copy[KB_BUF];
    pthread_mutex_lock(&kb_mutex);
    snprintf(copy, sizeof(copy), "%s", kb_text);
    pthread_mutex_unlock(&kb_mutex);
    (void)kb_call_text_method("setGameKeyboardText", copy);
}

/* 1, если текст сейчас ведёт системный EditText (он и получает клавиши). */
int keyboard_uses_editor(void) { return kb_call_bool_method("gameKeyboardActive"); }

void keyboard_show(void) {
    char current[KB_BUF];
    pthread_mutex_lock(&kb_mutex);
    snprintf(current, sizeof(current), "%s", kb_text);
    kb_show = 1;
    pthread_mutex_unlock(&kb_mutex);
    if (!kb_call_text_method("showGameKeyboard", current) && kb_activity)
        ANativeActivity_showSoftInput(kb_activity, ANATIVEACTIVITY_SHOW_SOFT_INPUT_FORCED);
}

void keyboard_hide(void) {
    pthread_mutex_lock(&kb_mutex); kb_show = 0; pthread_mutex_unlock(&kb_mutex);
    if (!kb_call_void_method("hideGameKeyboard") && kb_activity)
        ANativeActivity_hideSoftInput(kb_activity, ANATIVEACTIVITY_HIDE_SOFT_INPUT_IMPLICIT_ONLY);
}

const char *keyboard_get_text(void) {
    char copy[KB_BUF];
    pthread_mutex_lock(&kb_mutex); snprintf(copy, sizeof(copy), "%s", kb_text); pthread_mutex_unlock(&kb_mutex);
    return ds_track_string(ds_strdup(copy));
}

const char *keyboard_get_raw(void) {
    pthread_mutex_lock(&kb_mutex);
    snprintf(kb_raw_snapshot, sizeof(kb_raw_snapshot), "%s", kb_text);
    pthread_mutex_unlock(&kb_mutex);
    return kb_raw_snapshot;
}

void keyboard_clear(void) {
    pthread_mutex_lock(&kb_mutex);
    kb_text[0] = '\0'; kb_len = 0; kb_enter = 0;
    pthread_mutex_unlock(&kb_mutex);
    (void)kb_call_text_method("setGameKeyboardText", "");
}

int keyboard_visible(void) {
    int visible;
    pthread_mutex_lock(&kb_mutex); visible = kb_show; pthread_mutex_unlock(&kb_mutex);
    return visible;
}

int keyboard_enter_pressed(void) {
    int enter;
    pthread_mutex_lock(&kb_mutex); enter = kb_enter; kb_enter = 0; pthread_mutex_unlock(&kb_mutex);
    return enter;
}

static void kb_append_cp_locked(unsigned int cp) {
    char u[5]; int n = 0;
    if (cp < 0x80) { u[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { u[0]=(char)(0xC0|(cp>>6)); u[1]=(char)(0x80|(cp&0x3F)); n=2; }
    else if (cp < 0x10000) { u[0]=(char)(0xE0|(cp>>12)); u[1]=(char)(0x80|((cp>>6)&0x3F)); u[2]=(char)(0x80|(cp&0x3F)); n=3; }
    else if (cp <= 0x10FFFF) { u[0]=(char)(0xF0|(cp>>18)); u[1]=(char)(0x80|((cp>>12)&0x3F)); u[2]=(char)(0x80|((cp>>6)&0x3F)); u[3]=(char)(0x80|(cp&0x3F)); n=4; }
    if (n > 0 && kb_len+n < KB_BUF) {
        memcpy(kb_text+kb_len, u, (size_t)n); kb_len += n; kb_text[kb_len] = '\0';
    }
}

void keyboard_type(const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    if (!p) return;
    pthread_mutex_lock(&kb_mutex);
    while (*p) {
        unsigned int cp;
        if (*p < 0x80) cp = *p++;
        else if ((*p&0xE0)==0xC0 && (p[1]&0xC0)==0x80) { cp=((p[0]&0x1F)<<6)|(p[1]&0x3F); p+=2; }
        else if ((*p&0xF0)==0xE0 && (p[1]&0xC0)==0x80 && (p[2]&0xC0)==0x80) { cp=((p[0]&15)<<12)|((p[1]&63)<<6)|(p[2]&63); p+=3; }
        else if ((*p&0xF8)==0xF0 && (p[1]&0xC0)==0x80 && (p[2]&0xC0)==0x80 && (p[3]&0xC0)==0x80) { cp=((p[0]&7)<<18)|((p[1]&63)<<12)|((p[2]&63)<<6)|(p[3]&63); p+=4; }
        else { p++; continue; }
        if (cp=='\n' || cp=='\r') kb_enter=1; else if (cp>=0x20) kb_append_cp_locked(cp);
    }
    pthread_mutex_unlock(&kb_mutex);
    kb_sync_editor();
}

/* Стираем целый UTF-8 символ, а не один байт. */
void keyboard_backspace(void) {
    pthread_mutex_lock(&kb_mutex);
    while (kb_len > 0) {
        unsigned char c = (unsigned char)kb_text[--kb_len];
        kb_text[kb_len] = '\0';
        if ((c & 0xC0) != 0x80) break;
    }
    pthread_mutex_unlock(&kb_mutex);
    kb_sync_editor();
}

/* Физическая клавиатура: KeyEvent.getUnicodeChar с учётом раскладки. */
static unsigned int kb_unicode(int keycode, int meta) {
    JNIEnv *env = NULL; JavaVM *vm; jclass cls; jmethodID ctor, get_uni;
    jobject ev; jint uni = 0; int attached = 0;
    if (!kb_activity || !kb_activity->vm) return 0;
    vm = kb_activity->vm;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return 0;
        attached = 1;
    }
    if ((*env)->PushLocalFrame(env, 8) != 0) { if (attached) (*vm)->DetachCurrentThread(vm); return 0; }
    cls = (*env)->FindClass(env, "android/view/KeyEvent");
    if (!cls) goto done;
    ctor = (*env)->GetMethodID(env, cls, "<init>", "(II)V");
    get_uni = (*env)->GetMethodID(env, cls, "getUnicodeChar", "(I)I");
    if (!ctor || !get_uni) goto done;
    ev = (*env)->NewObject(env, cls, ctor, (jint)0, (jint)keycode);
    if (!ev || (*env)->ExceptionCheck(env)) goto done;
    uni = (*env)->CallIntMethod(env, ev, get_uni, (jint)meta);
    if ((*env)->ExceptionCheck(env)) uni = 0;
done:
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
    if (attached) (*vm)->DetachCurrentThread(vm);
    return uni > 0 ? (unsigned int)uni : 0;
}

int keyboard_handle_key(int keycode, int action, int meta) {
    (void)action;
    /* Сюда попадают только клавиши, которые НЕ дошли до системного EditText:
     * когда редактор владеет IME, события коммитятся в него и до native-очереди
     * просто не доходят. А если редактор потерял фокус (частая история после
     * ресайза окна под клавиатуру), события приходят сюда — и мы ОБЯЗАНЫ сами
     * дописать их в буфер. Раньше здесь стояло «uses_editor -> return 0», и при
     * застрявшем флаге wantKeyboard каждый символ беззвучно терялся: поле ника
     * не принимало ввод вообще. Стёртый текст не «воскресает»: любой буферный
     * edit зеркалится обратно в редактор через kb_sync_editor(). */
    if (keycode==AKEYCODE_DEL || keycode==AKEYCODE_FORWARD_DEL) { keyboard_backspace(); return 1; }
    if (keycode==AKEYCODE_ENTER || keycode==AKEYCODE_NUMPAD_ENTER || keycode==AKEYCODE_DPAD_CENTER) {
        pthread_mutex_lock(&kb_mutex); kb_enter=1; pthread_mutex_unlock(&kb_mutex); return 1;
    }
    if (keycode==AKEYCODE_SPACE) {
        pthread_mutex_lock(&kb_mutex); kb_append_cp_locked(' '); pthread_mutex_unlock(&kb_mutex); kb_sync_editor(); return 1;
    }
    {
        unsigned int cp = kb_unicode(keycode, meta);
        if (cp>=0x20 && cp!=0x7F) {
            pthread_mutex_lock(&kb_mutex); kb_append_cp_locked(cp); pthread_mutex_unlock(&kb_mutex); kb_sync_editor(); return 1;
        }
    }
    if (keycode>=AKEYCODE_A && keycode<=AKEYCODE_Z) {
        pthread_mutex_lock(&kb_mutex); kb_append_cp_locked((unsigned int)('a'+keycode-AKEYCODE_A)); pthread_mutex_unlock(&kb_mutex); kb_sync_editor(); return 1;
    }
    if (keycode>=AKEYCODE_0 && keycode<=AKEYCODE_9) {
        pthread_mutex_lock(&kb_mutex); kb_append_cp_locked((unsigned int)('0'+keycode-AKEYCODE_0)); pthread_mutex_unlock(&kb_mutex); kb_sync_editor(); return 1;
    }
    if (keycode==AKEYCODE_COMMA || keycode==AKEYCODE_PERIOD || keycode==AKEYCODE_MINUS) {
        unsigned int cp = keycode==AKEYCODE_COMMA ? ',' : keycode==AKEYCODE_PERIOD ? '.' : '-';
        pthread_mutex_lock(&kb_mutex); kb_append_cp_locked(cp); pthread_mutex_unlock(&kb_mutex); kb_sync_editor(); return 1;
    }
    return 0;
}

void keyboard_commit_utf8(const char *utf8) { keyboard_type(utf8); }

/* Callbacks from GameActivity's EditText. UTF-16 is converted to valid UTF-8,
 * including surrogate pairs, instead of relying on JNI modified UTF-8. */
JNIEXPORT void JNICALL
Java_com_dimscript_gamedemo_GameActivity_nativeReplaceText(JNIEnv *env, jobject self, jstring value) {
    (void)self;
    pthread_mutex_lock(&kb_mutex);
    kb_text[0]='\0'; kb_len=0;
    if (value) {
        jsize count = (*env)->GetStringLength(env, value);
        const jchar *chars = (*env)->GetStringChars(env, value, NULL);
        if (chars) {
            for (jsize i=0; i<count; i++) {
                unsigned int cp = chars[i];
                if (cp>=0xD800 && cp<=0xDBFF && i+1<count && chars[i+1]>=0xDC00 && chars[i+1]<=0xDFFF) {
                    cp = 0x10000 + ((cp-0xD800)<<10) + (chars[++i]-0xDC00);
                }
                if (cp>=0x20 && cp!=0x7F) kb_append_cp_locked(cp);
            }
            (*env)->ReleaseStringChars(env, value, chars);
        }
    }
    pthread_mutex_unlock(&kb_mutex);
}

JNIEXPORT void JNICALL
Java_com_dimscript_gamedemo_GameActivity_nativeSubmitText(JNIEnv *env, jobject self) {
    (void)env; (void)self;
    pthread_mutex_lock(&kb_mutex); kb_enter=1; pthread_mutex_unlock(&kb_mutex);
}

JNIEXPORT void JNICALL
Java_com_dimscript_gamedemo_GameActivity_nativeKeyboardHidden(JNIEnv *env, jobject self) {
    (void)env; (void)self;
    pthread_mutex_lock(&kb_mutex); kb_show=0; pthread_mutex_unlock(&kb_mutex);
}

#else
/* Десктоп/тесты: простой текстовый буфер без JNI. */
#define KB_BUF 256
static char kb_text[KB_BUF] = {0};
static int kb_len = 0;
static int kb_show = 0;
static int kb_enter = 0;
const char* keyboard_get_text(void){ return ds_track_string(ds_strdup(kb_text)); }
const char* keyboard_get_raw(void){ return kb_text; }
void keyboard_clear(void){ kb_text[0]='\0'; kb_len=0; kb_enter=0; }
int keyboard_visible(void){ return kb_show; }
int keyboard_enter_pressed(void){ int e=kb_enter; kb_enter=0; return e; }
static void kb_append(const char *text){
    if(!text) return;
    for(size_t i=0;text[i] && kb_len+1<KB_BUF-1;i++){
        char c=text[i];
        if(c=='\n'||c=='\r'){ kb_enter=1; continue; }
        kb_text[kb_len++]=c;
    }
    kb_text[kb_len]='\0';
}
void keyboard_type(const char *text){ kb_append(text); }
void keyboard_commit_utf8(const char *utf8){ kb_append(utf8); }
void keyboard_backspace(void){
    while(kb_len>0){
        unsigned char c=(unsigned char)kb_text[--kb_len];
        kb_text[kb_len]='\0';
        if((c & 0xC0) != 0x80) break;
    }
}
int keyboard_handle_key(int keycode, int action, int meta){ (void)keycode; (void)action; (void)meta; return 0; }
int keyboard_uses_editor(void){ return 0; }
void ds_set_activity(void *act){ (void)act; }
void keyboard_show(void){ kb_show=1; }
void keyboard_hide(void){ kb_show=0; }
#endif
