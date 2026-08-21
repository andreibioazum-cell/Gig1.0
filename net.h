#ifndef NET_H
#define NET_H
#ifdef __ANDROID__
#include <jni.h>
#endif

#define NET_SLOTS 4
#define NET_OFFLINE 0
#define NET_CONNECTING 1
#define NET_PLAYING 3
#define NET_ERROR 4
#define NET_LOGIN_IDLE 0
#define NET_LOGIN_OK 2
#define NET_LOGIN_BAD_NICK 5
#define NET_LOGIN_WRONG_PASS 6
#define NET_LOGIN_BAD_PASS 7

#ifdef __ANDROID__
void net_set_java_vm(JavaVM *vm);
#endif

/* Подключение и аккаунты (вход / регистрация). */
void net_connect(const char *url, const char *room);
void net_disconnect(void);
void net_set_data_path(const char *path);
void net_autologin(const char *url);
double net_auth(const char *url, const char *nick, const char *pass);
double net_set_nick(const char *nick);
void net_logout(void);
double net_login_status(void);
const char *net_login_nick(void);
const char *net_login_pass(void);

/* Лидерборд по кубкам */
void net_leaderboard_fetch(const char *url);
double net_leaderboard_status(void);
double net_leaderboard_count(void);
const char *net_leaderboard_nick(double idx);
double net_leaderboard_cups(double idx);

/* Состояние игроков. Удар — постоянное событие со счётчиком punch:
 * если счётчик изменился, удар нельзя потерять между двумя опросами сети.
 * Так же передаётся и суператака «Подарок»: счётчик gift плюс точка броска
 * и направление, по которым соперник повторяет полёт подарка у себя. */
void net_publish(double x, double y, double angle, double hp, double alive, double freeze);
void net_publish_punch(double x, double y, double dx, double dy, double punch);
void net_publish_gift(double x, double y, double dx, double dy, double gift);
void net_set_class(double cls);
double net_status(void);
double net_slot(void);
double net_count(void);
double net_event(void); /* 0 — ивента нет, 1 — «диско», 2 — снегопад */
void net_event_fetch(const char *url); /* перечитать номер ивента из Firebase */
void net_event_set(const char *url, double value); /* запустить ивент для всех */
double net_player_online(double slot);
double net_player_x(double slot);
double net_player_y(double slot);
double net_player_angle(double slot);
double net_player_hp(double slot);
double net_player_alive(double slot);
const char *net_player_nick(double slot);
double net_player_punch_x(double slot);
double net_player_punch_y(double slot);
double net_player_punch_dx(double slot);
double net_player_punch_dy(double slot);
double net_player_punch(double slot);
double net_player_class(double slot);
double net_player_freeze(double slot);
double net_player_gift(double slot);
double net_player_gift_x(double slot);
double net_player_gift_y(double slot);
double net_player_gift_dx(double slot);
double net_player_gift_dy(double slot);

/* Прогресс игрока: кубки, леденцы, выбранный класс и купленные классы. */
double net_load_cups(void);
double net_load_candies(void);
double net_load_class(void);
double net_load_azum(void);
double net_load_santa(void);
double net_load_level(void);
double net_load_levels_unlocked(void);
void net_save_progress(double cups, double candies, double cls, double azum, double santa,
                       double level, double levels_unlocked);

/* Чат читается реже боевого состояния, поэтому не тормозит движение. */
void net_chat_send(const char *text);
void net_chat_trim(double keep); /* оставить последние keep, остальное удалить */
double net_chat_count(void);
const char *net_chat_text(double idx);
const char *net_chat_uid(double idx);

#endif
