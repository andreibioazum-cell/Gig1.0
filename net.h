#ifndef NET_H
#define NET_H

/* Онлайн-слой Gig1.0: Firebase Realtime Database поверх обычного REST.
 *
 * Никаких «ивентов» — в базе лежит только то, что реально нужно бою:
 * папка игрока с его координатами, поворотом и здоровьем, и папка пули.
 *
 *   rooms/<комната>/players/<слот>   uid, x, y, angle, hp, alive, seq
 *   rooms/<комната>/bullets/<слот>   x, y, dx, dy, active, shot
 *
 * Слот — 0 или 1: в комнате ровно двое, ты и второй игрок. Свой слот
 * клиент занимает сам при подключении, чужой только читает.
 *
 * Вся сеть живёт в отдельном потоке: игровой цикл никогда не ждёт ответа
 * сервера, он лишь кладёт своё состояние в net_publish* и забирает чужое
 * из net_peer_*. Скрипт видит эти функции как встроенные.
 */

/* Состояние соединения — им удобно рисовать надпись в углу экрана. */
#define NET_OFFLINE     0   /* поток не запущен */
#define NET_CONNECTING  1   /* ищем свободный слот в комнате */
#define NET_WAITING     2   /* слот занят, ждём второго игрока */
#define NET_PLAYING     3   /* оба игрока на связи */
#define NET_ERROR       4   /* сеть недоступна, идут повторные попытки */

/* Подключение к комнате. base_url — корень базы, например
 * "https://cubic-battle-3-default-rtdb.firebaseio.com". Повторный вызов
 * при уже поднятом соединении игнорируется. */
void net_connect(const char *base_url, const char *room);

/* Отключение: поток останавливается, свой слот в базе освобождается. */
void net_disconnect(void);

/* Своё состояние. Вызывайте каждый кадр — в сеть уйдёт последнее. */
void net_publish(double x, double y, double angle, double hp, double alive);
void net_publish_bullet(double x, double y, double dx, double dy,
                        double active, double shot);

/* Состояние соединения и свой слот (-1, пока слот не занят). */
double net_status(void);
double net_online(void);
double net_slot(void);

/* Второй игрок. Пока его нет, net_peer_online() == 0. */
double net_peer_online(void);
double net_peer_x(void);
double net_peer_y(void);
double net_peer_angle(void);
double net_peer_hp(void);
double net_peer_alive(void);

/* Пуля второго игрока. shot — номер выстрела, по нему легко засчитать
 * попадание ровно один раз. */
double net_peer_bullet_active(void);
double net_peer_bullet_x(void);
double net_peer_bullet_y(void);
double net_peer_bullet_dx(void);
double net_peer_bullet_dy(void);
double net_peer_bullet_shot(void);

/* --- Служебное, вызывает хост, а не скрипт --- */

#ifdef __ANDROID__
#include <jni.h>
/* Android-бэкенд ходит в сеть через java.net.HttpURLConnection, поэтому
 * ему нужна JavaVM. Вызывается один раз из android_main. */
void net_set_java_vm(JavaVM *vm);
#endif

/* Полная остановка (перезапуск скрипта, закрытие окна). */
void net_shutdown(void);

#endif
