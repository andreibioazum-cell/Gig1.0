/* Проверка экрана настроек и переключения языка.
 *
 * Тыкаем в реальные экранные координаты и смотрим, что меняется
 * настоящее состояние игры: экран, переменная языка и — главное —
 * текст, который игра собирается нарисовать.
 */

#include <stdio.h>
#include <string.h>

#include "runtime.h"

/* Живут в сгенерированном game.c, читаем через пробник. */
double game_state_value(void);
double lang_value(void);
const char *translate(const char *ru, const char *en);

static int failures = 0;

static void check(const char *what, int ok) {
    if (!ok) failures++;
    printf("%s %s\n", ok ? "  ok  " : "ПРОВАЛ", what);
}

static void frames(int n) {
    int i;
    for (i = 0; i < n; i++) {
        dt = 0.016;
        update();
        draw(NULL);
    }
}

static void tap(float x, float y) {
    touch(x, y, 0, 1);
    frames(2);
    touch(x, y, 1, 1);
    frames(2);
}

/* Переход между экранами занимает ~0.25 с в каждую сторону. */
static void settle(void) { frames(45); }

int main(void) {
    screen_w = 1280;
    screen_h = 720;
    dt = 0.016;
    init(NULL);
    frames(5);

    check("старт в лобби", game_state_value() == 0);
    check("язык по умолчанию — русский", lang_value() == 0);

    /* Лобби -> Настройки (третья кнопка). */
    tap((float)screen_w / 2, (float)screen_h / 2 + 142);
    settle();
    check("открылся экран настроек", game_state_value() == 3);

    /* Жмём English. */
    tap((float)screen_w / 2 + 130, (float)screen_h / 2);
    frames(4);
    check("выбран английский", lang_value() == 1);
    check("подписи стали английскими",
          strcmp(translate("Настройки", "Settings"), "Settings") == 0);

    /* Возврат в лобби — кнопка уже подписана по-английски. */
    tap((float)screen_w / 2, (float)screen_h / 2 + 114);
    settle();
    check("вернулись в лобби", game_state_value() == 0);

    /* Язык должен пережить выход из настроек. */
    check("язык сохранился после выхода", lang_value() == 1);

    /* Заходим в бой и убеждаемся, что игра работает на английском. */
    tap((float)screen_w / 2, (float)screen_h / 2 - 42);
    settle();
    check("бой запускается при английском языке", game_state_value() == 1);
    check("надпись боя переведена",
          strcmp(translate("Огонь", "Fire"), "Fire") == 0);

    /* Назад в лобби, снова в настройки, возвращаем русский. */
    tap((float)screen_w / 2, 36);
    settle();
    check("из боя вернулись в лобби", game_state_value() == 0);

    tap((float)screen_w / 2, (float)screen_h / 2 + 142);
    settle();
    check("настройки открылись повторно", game_state_value() == 3);

    tap((float)screen_w / 2 - 130, (float)screen_h / 2);
    frames(4);
    check("вернули русский", lang_value() == 0);
    check("подписи снова русские",
          strcmp(translate("Настройки", "Settings"), "Настройки") == 0);

    tap((float)screen_w / 2, (float)screen_h / 2 + 114);
    settle();
    check("финальный выход в лобби", game_state_value() == 0);

    if (failures) printf("ОШИБОК: %d\n", failures);
    else printf("настройки и выбор языка работают\n");
    return failures ? 1 : 0;
}
