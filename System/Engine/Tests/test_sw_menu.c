/* Unit test for Engine/Runtime/src/sw_menu.c (input filter + menu state).
 * Build: cl /nologo /W3 test_sw_menu.c ../Runtime/src/sw_menu.c  (or run test_sw_menu.bat) */
#include <stdio.h>
#include <stdint.h>
#include <windows.h>
#include "../Runtime/src/sw_menu.h"

static int g_fps = 0;
void sw_fps_set(int f) { g_fps = f; }
int sw_fps_get(void) { return g_fps; }

enum { A = 1 << 0, B = 1 << 1, PLUS = 1 << 10, MINUS = 1 << 11, UP = 1 << 13, DOWN = 1 << 15, RIGHT = 1 << 14, LEFT = 1 << 12 };
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint64_t poll(uint64_t b) { int n; return sw_menu_filter(b, &n); }

int main(void)
{
    /* plain buttons pass through */
    CHECK(poll(A) == A);
    CHECK(poll(0) == 0);

    /* lone Plus: held back first, then passed on after the hold time */
    CHECK(poll(PLUS) == 0);
    Sleep(250);
    CHECK(poll(PLUS) == PLUS);
    CHECK(poll(0) == 0);

    /* quick Plus tap: replayed as a short press */
    CHECK(poll(PLUS) == 0);
    Sleep(20);
    CHECK(poll(0) == PLUS);
    Sleep(150);
    CHECK(poll(0) == 0);

    /* Plus then Minus inside the hold: opens the menu, the game sees nothing */
    CHECK(!sw_menu_is_open());
    CHECK(poll(PLUS) == 0);
    CHECK(poll(PLUS | MINUS) == 0);
    CHECK(sw_menu_is_open());
    CHECK(sw_menu_selected() == SW_MENU_CONTINUE);
    CHECK(poll(0) == 0);

    /* navigation: down to Frame rate, A / right step 30 -> 60 -> 90, left goes back */
    CHECK(poll(DOWN) == 0);
    CHECK(sw_menu_selected() == SW_MENU_FPS);
    CHECK(poll(0) == 0);
    CHECK(poll(A) == 0);
    CHECK(g_fps == 60);
    CHECK(poll(0) == 0);
    CHECK(poll(RIGHT) == 0);
    CHECK(g_fps == 90);
    CHECK(poll(0) == 0);
    CHECK(poll(LEFT) == 0);
    CHECK(g_fps == 60);
    CHECK(poll(0) == 0);

    /* up wraps to Quit; first A arms, second A queues the quit action */
    CHECK(poll(UP) == 0); CHECK(poll(0) == 0);
    CHECK(poll(UP) == 0); CHECK(poll(0) == 0);
    CHECK(sw_menu_selected() == SW_MENU_QUIT);
    CHECK(poll(A) == 0); CHECK(poll(0) == 0);
    CHECK(sw_menu_quit_armed());
    CHECK(sw_menu_take_action() == SW_MENU_ACT_NONE);
    CHECK(poll(A) == 0); CHECK(poll(0) == 0);
    CHECK(sw_menu_take_action() == SW_MENU_ACT_QUIT);

    /* B closes; the held B does not leak to the game until released */
    CHECK(poll(B) == 0);
    CHECK(!sw_menu_is_open());
    CHECK(poll(B) == 0);
    CHECK(poll(0) == 0);
    CHECK(poll(B) == B);
    CHECK(poll(0) == 0);

    /* both in the same poll toggles open, again toggles closed */
    CHECK(poll(PLUS | MINUS) == 0); CHECK(sw_menu_is_open());
    CHECK(poll(0) == 0);
    CHECK(poll(PLUS | MINUS) == 0); CHECK(!sw_menu_is_open());
    CHECK(poll(PLUS | MINUS) == 0);   /* still held: gate */
    CHECK(poll(0) == 0);
    CHECK(poll(A) == A);

    printf(fails ? "%d FAILED\n" : "all menu tests passed\n", fails);
    return fails != 0;
}
