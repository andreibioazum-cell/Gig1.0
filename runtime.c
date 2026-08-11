#include "runtime.h"

#include <stdarg.h>
#include <stdio.h>

#define DS_ERROR_MESSAGE_SIZE 1024

Joy joy = {0};
int screen_w = 0;
int screen_h = 0;
double dt = 0.0;

static jmp_buf ds_error_jump;
static int ds_error_handler_active = 0;
static int ds_has_error = 0;
static int ds_restart_requested = 0;
static char ds_last_error[DS_ERROR_MESSAGE_SIZE] = {0};

/* Все строки, созданные на лету (конкатенация, число в строку), живут в
 * этом пуле и освобождаются разом при перезапуске скрипта. */
typedef struct DSStringNode DSStringNode;
struct DSStringNode {
    DSStringNode *next;
    char *string;
};
static DSStringNode *ds_strings = NULL;

void ds_runtime_error(const char *format, ...) {
    va_list args;
    va_list copy;
    va_start(args, format);
    va_copy(copy, args);
    vsnprintf(ds_last_error, sizeof(ds_last_error), format, copy);
    va_end(copy);
    __android_log_vprint(ANDROID_LOG_ERROR, "DimScript", format, args);
    va_end(args);
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

const char *ds_runtime_error_message(void) {
    return ds_last_error[0] ? ds_last_error : "unknown DimScript runtime error";
}

int ds_script_has_error(void) { return ds_has_error; }

void ds_clear_runtime_error(void) {
    ds_has_error = 0;
    ds_last_error[0] = '\0';
}

void ds_request_script_restart(void) { ds_restart_requested = 1; }
int ds_script_restart_requested(void) { return ds_restart_requested; }
void ds_clear_script_restart(void) { ds_restart_requested = 0; }

static char *ds_strdup(const char *string) {
    size_t length;
    char *copy;
    if (!string) string = "";
    length = strlen(string) + 1;
    copy = (char *)malloc(length);
    if (copy) memcpy(copy, string, length);
    return copy;
}

static char *ds_track_string(char *string) {
    DSStringNode *node;
    if (!string) {
        ds_runtime_error("out of memory while creating a string");
        return NULL;
    }
    node = (DSStringNode *)malloc(sizeof(*node));
    if (!node) {
        free(string);
        ds_runtime_error("out of memory while tracking a string");
        return NULL;
    }
    node->string = string;
    node->next = ds_strings;
    ds_strings = node;
    return string;
}

char *ds_num_to_string(double number) {
    char buffer[96];
    if (snprintf(buffer, sizeof(buffer), "%g", number) < 0) return NULL;
    return ds_track_string(ds_strdup(buffer));
}

void ds_string_pool_reset(void) {
    DSStringNode *node = ds_strings;
    while (node) {
        DSStringNode *next = node->next;
        free(node->string);
        free(node);
        node = next;
    }
    ds_strings = NULL;
}

char *ds_concat(const char *left, const char *right) {
    size_t la = left ? strlen(left) : 0;
    size_t lb = right ? strlen(right) : 0;
    char *out = (char *)malloc(la + lb + 1);
    if (!out) return ds_track_string(ds_strdup(""));
    if (la) memcpy(out, left, la);
    if (lb) memcpy(out + lb, right, lb);
    out[la + lb] = '\0';
    return ds_track_string(out);
}

/* =========================================================================
 *  Networking — built-in MQTT client for online multiplayer.
 *
 *  Both players connect to a free public MQTT broker
 *  (broker.hivemq.com, port 1883).  They subscribe to a room-specific
 *  topic and exchange game data via MQTT publish/subscribe.
 *  No VPS or separate server process is needed.
 * ========================================================================= */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <time.h>

/* ---- globals visible to DimScript ---- */
double net_state      = 0;
double net_am_host    = 0;
double net_peer_x     = 0;
double net_peer_y     = 0;
double net_peer_angle = 0;
double net_peer_bx    = 0;
double net_peer_by    = 0;
double net_peer_bdx   = 0;
double net_peer_bdy   = 0;
double net_peer_fire  = 0;
double net_peer_hit   = 0;

/* ---- internal ---- */
#define MQ_BUF  4096

static int mq_sock  = -1;
static int mq_step  = 0;
/* 0=off  1=tcp_connecting  2=wait_connack
   3=wait_suback  4=subscribed(wait_peer)  5=peer_ready */
static char mq_cid[20];
static char mq_topic[64];
static uint8_t mq_rbuf[MQ_BUF];
static int mq_rpos = 0;
static uint32_t mq_tick = 0;
static struct sockaddr_in mq_addr;

static void mq_nb(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* ---- MQTT packet helpers ---- */

static int mq_enc_len(uint8_t *b, int len) {
    int i = 0;
    do {
        uint8_t v = (uint8_t)(len % 128);
        len /= 128;
        if (len > 0) v |= 0x80;
        b[i++] = v;
    } while (len > 0);
    return i;
}

static int mq_dec_len(const uint8_t *b, int *p, int mx) {
    int mul = 1, val = 0;
    for (;;) {
        uint8_t v;
        if (*p >= mx) return -1;
        v = b[(*p)++];
        val += (v & 0x7F) * mul;
        if (!(v & 0x80)) break;
        mul *= 128;
    }
    return val;
}

static void mq_raw(const uint8_t *d, int n) {
    if (mq_sock >= 0 && n > 0) (void)send(mq_sock, d, (size_t)n, 0);
}

static void mq_send_connect(void) {
    uint8_t buf[128];
    int p = 0, cl = (int)strlen(mq_cid), rem = 10 + 2 + cl;
    buf[p++] = 0x10;                        /* CONNECT */
    p += mq_enc_len(buf + p, rem);
    buf[p++]=0; buf[p++]=4;                 /* protocol name "MQTT" */
    buf[p++]='M'; buf[p++]='Q';
    buf[p++]='T'; buf[p++]='T';
    buf[p++]=0x04;                           /* level 4 */
    buf[p++]=0x02;                           /* clean session */
    buf[p++]=0; buf[p++]=0x3C;               /* keepalive 60 s */
    buf[p++]=(uint8_t)(cl>>8);              /* client id */
    buf[p++]=(uint8_t)(cl);
    memcpy(buf+p, mq_cid, (size_t)cl); p+=cl;
    mq_raw(buf, p);
}

static void mq_send_subscribe(void) {
    uint8_t buf[128];
    int p = 0, tl = (int)strlen(mq_topic), rem = 2 + 2 + tl + 1;
    buf[p++]=0x82;                            /* SUBSCRIBE */
    p += mq_enc_len(buf + p, rem);
    buf[p++]=0; buf[p++]=1;                   /* packet id */
    buf[p++]=(uint8_t)(tl>>8);
    buf[p++]=(uint8_t)(tl);
    memcpy(buf+p, mq_topic, (size_t)tl); p+=tl;
    buf[p++]=0;                               /* QoS 0 */
    mq_raw(buf, p);
}

static void mq_send_pub(const char *payload) {
    uint8_t buf[512];
    int p = 0, tl = (int)strlen(mq_topic);
    int dl = (int)strlen(payload), rem = 2 + tl + dl;
    buf[p++]=0x30;                            /* PUBLISH QoS 0 */
    p += mq_enc_len(buf + p, rem);
    buf[p++]=(uint8_t)(tl>>8);
    buf[p++]=(uint8_t)(tl);
    memcpy(buf+p, mq_topic, (size_t)tl); p+=tl;
    memcpy(buf+p, payload,  (size_t)dl); p+=dl;
    mq_raw(buf, p);
}

static void mq_send_ping(void) {
    uint8_t buf[2] = {0xC0, 0};
    mq_raw(buf, 2);
}

/* handle one complete MQTT packet */
static void mq_handle(const uint8_t *pkt, int len) {
    if (len < 2) return;
    uint8_t type = pkt[0] & 0xF0;

    if (type == 0x20 && len >= 4) {                /* CONNACK */
        if (pkt[3] == 0) {
            mq_step = 3;
            mq_send_subscribe();
            ds_log("mqtt: CONNACK ok, subscribing to %s", mq_topic);
        } else {
            ds_log("mqtt: CONNACK error %d", pkt[3]);
            net_close();
        }
    }
    else if (type == 0x90) {                        /* SUBACK */
        mq_step  = 4;
        net_state = 3;
        ds_log("mqtt: SUBACK, waiting for peer");
        /* announce ourselves */
        {   char m[64]; snprintf(m, sizeof m, "%s:J", mq_cid);
            mq_send_pub(m);
        }
    }
    else if (type == 0x30) {                        /* PUBLISH */
        int pos = 1;
        int rem = mq_dec_len(pkt, &pos, len);
        if (rem < 0 || pos + rem > len) return;
        /* topic */
        if (pos + 2 > len) return;
        int tl = (pkt[pos]<<8)|pkt[pos+1]; pos += 2;
        if (pos + tl > len) return; pos += tl;
        /* payload */
        int pl = rem - 2 - tl;
        if (pl <= 0 || pos + pl > len) return;
        char pay[256];
        int n = pl < 255 ? pl : 255;
        memcpy(pay, pkt + pos, (size_t)n); pay[n] = '\0';

        char *sep = strchr(pay, ':');
        if (!sep) return;
        *sep = '\0';
        char *sender = pay;
        char *msg    = sep + 1;

        if (strcmp(sender, mq_cid) == 0) return;    /* ignore self */

        if (strcmp(msg, "J") == 0) {
            net_am_host = (strcmp(mq_cid, sender) < 0) ? 1 : 0;
            mq_step  = 5;
            net_state = 4;
            ds_log("mqtt: peer %s joined, I am %s", sender,
                   net_am_host ? "HOST" : "GUEST");
        }
        else if (strcmp(msg, "L") == 0) {
            mq_step = 4; net_state = 3;
            ds_log("mqtt: peer left");
        }
        else if (msg[0]=='P' && msg[1]==':') {
            float px,py,pa;
            if (sscanf(msg+2, "%f,%f,%f", &px,&py,&pa) == 3) {
                net_peer_x = px; net_peer_y = py; net_peer_angle = pa;
            }
        }
        else if (msg[0]=='B' && msg[1]==':') {
            float bx,by,bdx,bdy;
            if (sscanf(msg+2, "%f,%f,%f,%f", &bx,&by,&bdx,&bdy) == 4) {
                net_peer_bx=bx; net_peer_by=by;
                net_peer_bdx=bdx; net_peer_bdy=bdy;
                net_peer_fire = 1;
            }
        }
        else if (msg[0]=='H' && msg[1]==':') {
            net_peer_hit = 1;
        }
    }
}

/* ---- public API (same names so DimScript code is unchanged) ---- */

void net_connect(const char *host, double port) {
    struct hostent *he;
    int p = port > 0 ? (int)port : 1883;
    int fl;

    if (mq_sock >= 0) net_close();

    srand((unsigned)time(NULL) ^ (unsigned)clock());
    snprintf(mq_cid, sizeof mq_cid, "ds%d", rand() % 100000);

    memset(&mq_addr, 0, sizeof mq_addr);
    mq_addr.sin_family = AF_INET;
    mq_addr.sin_port   = htons((uint16_t)p);
    if (inet_pton(AF_INET, host, &mq_addr.sin_addr) != 1) {
        he = gethostbyname(host);
        if (!he || !he->h_addr_list[0]) {
            ds_log("mqtt: cannot resolve '%s'", host); return;
        }
        memcpy(&mq_addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    }

    mq_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (mq_sock < 0) { ds_log("mqtt: socket()"); return; }
    fl = fcntl(mq_sock, F_GETFL, 0);
    if (fl >= 0) fcntl(mq_sock, F_SETFL, fl | O_NONBLOCK);

    connect(mq_sock, (struct sockaddr *)&mq_addr, sizeof mq_addr);
    mq_step      = 1;
    net_state     = 1;
    net_am_host   = 0;
    net_peer_fire = 0;
    net_peer_hit  = 0;
    mq_rpos       = 0;
    mq_tick       = 0;
    ds_log("mqtt: connecting to %s:%d …", host, p);
}

void net_close(void) {
    if (mq_sock >= 0) {
        if (mq_step >= 4) {
            char m[64]; snprintf(m, sizeof m, "%s:L", mq_cid);
            mq_send_pub(m);
        }
        close(mq_sock);
        mq_sock = -1;
    }
    mq_step   = 0;
    net_state = 0;
    net_am_host = 0;
    mq_rpos = 0;
}

void net_create(const char *name) {
    if (!name) return;
    snprintf(mq_topic, sizeof mq_topic, "ds_game/%s", name);
    /* will subscribe once CONNACK arrives */
}

void net_join(const char *name) { net_create(name); }

void net_send_pos(double x, double y, double angle) {
    char m[128];
    if (mq_sock < 0 || mq_step < 5) return;
    snprintf(m, sizeof m, "%s:P:%.1f,%.1f,%.3f", mq_cid, x, y, angle);
    mq_send_pub(m);
}

void net_send_bullet(double x, double y, double dx, double dy) {
    char m[128];
    if (mq_sock < 0 || mq_step < 5) return;
    snprintf(m, sizeof m, "%s:B:%.1f,%.1f,%.4f,%.4f", mq_cid, x, y, dx, dy);
    mq_send_pub(m);
}

void net_send_hit(void) {
    char m[64];
    if (mq_sock < 0 || mq_step < 5) return;
    snprintf(m, sizeof m, "%s:H:1", mq_cid);
    mq_send_pub(m);
}

void net_poll(void) {
    uint8_t tmp[1024];
    ssize_t nr;
    fd_set wfds, rfds;
    struct timeval tv;
    int err; socklen_t elen;

    net_peer_fire = 0;
    net_peer_hit  = 0;
    if (mq_sock < 0) return;

    /* step 1: TCP connect completing */
    if (mq_step == 1) {
        FD_ZERO(&wfds); FD_SET(mq_sock, &wfds);
        tv.tv_sec = 0; tv.tv_usec = 0;
        if (select(mq_sock + 1, NULL, &wfds, NULL, &tv) > 0) {
            elen = sizeof(err);
            getsockopt(mq_sock, SOL_SOCKET, SO_ERROR, &err, &elen);
            if (err) {
                ds_log("mqtt: tcp failed (%s)", strerror(err));
                close(mq_sock); mq_sock = -1;
                net_state = 0; mq_step = 0; return;
            }
            mq_step = 2;
            mq_send_connect();
            ds_log("mqtt: TCP ok, CONNECT sent");
        }
        return;
    }

    /* read bytes */
    for (;;) {
        FD_ZERO(&rfds); FD_SET(mq_sock, &rfds);
        tv.tv_sec = 0; tv.tv_usec = 0;
        if (select(mq_sock + 1, &rfds, NULL, NULL, &tv) <= 0) break;
        nr = recv(mq_sock, tmp, sizeof tmp, 0);
        if (nr <= 0) {
            if (nr == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                ds_log("mqtt: connection lost");
                close(mq_sock); mq_sock = -1;
                net_state = 0; mq_step = 0;
            }
            break;
        }
        if (mq_rpos + (int)nr <= MQ_BUF) {
            memcpy(mq_rbuf + mq_rpos, tmp, (size_t)nr);
            mq_rpos += (int)nr;
        }
    }

    /* parse complete packets */
    while (mq_rpos >= 2) {
        int pos = 1;
        int rem = mq_dec_len(mq_rbuf, &pos, mq_rpos);
        if (rem < 0) break;
        int total = pos + rem;
        if (total > mq_rpos) break;
        mq_handle(mq_rbuf, total);
        memmove(mq_rbuf, mq_rbuf + total, (size_t)(mq_rpos - total));
        mq_rpos -= total;
    }

    /* keep-alive ping every ~25 s */
    mq_tick++;
    if (mq_step >= 4 && mq_tick >= 1500) {
        mq_send_ping();
        mq_tick = 0;
    }

    /* re-announce while waiting for peer */
    if (mq_step == 4 && (mq_tick & 63) == 0) {
        char m[64]; snprintf(m, sizeof m, "%s:J", mq_cid);
        mq_send_pub(m);
    }
}
