/* The app data arena: one fixed block for the data of the app on screen,
 * so code relocated against its address can live in flash. */
#include <string.h>
#include "tinytest.h"
#include "kernel/app/arena.h"
#include "kernel/app/capp.h"

void test_arena_one_holder_at_a_time(void) {
  void *a = arena_claim(20000, 8, "jar.capp"), *b;
  CHECK(a != NULL);
  CHECK(arena_owns(a));
  CHECK_EQ(arena_held(), 1);
  CHECK(strcmp(arena_holder(), "jar.capp") == 0);
  b = arena_claim(100, 4, "todo.capp");
  CHECK(b == NULL);
  arena_release(a);
  CHECK_EQ(arena_held(), 0);
  CHECK(strcmp(arena_holder(), "") == 0);
}

/* Today holds the arena; the apps it asks for sections load beside it on
 * the RAM path. A refused claim must leave Today's hold exactly as it was. */
void test_arena_held_so_a_command_load_falls_back(void) {
  void *today = arena_claim(8548, 4, "today.capp");
  CHECK(today != NULL);
  CHECK(arena_claim(15416, 4, "calendar.capp") == NULL);  /* -> heap, RAM code */
  CHECK(arena_claim(17852, 4, "todo.capp") == NULL);
  CHECK(strcmp(arena_holder(), "today.capp") == 0);
  CHECK_EQ(arena_code_in_flash(0, 1, 0), 0);                /* heap data: never flash */
  arena_release(today);
  CHECK(arena_claim(15416, 4, "calendar.capp") != NULL);    /* free again */
  arena_release((void *)(uintptr_t)arena_addr());
}

void test_arena_refuses_too_big_or_overaligned(void) {
  CHECK(arena_claim(ARENA_SIZE + 4, 4, "big") == NULL);
  CHECK(arena_claim(0, 4, "empty") == NULL);
  CHECK(arena_claim(100, 32, "odd") == NULL);
  CHECK_EQ(arena_held(), 0);
  CHECK(arena_claim(ARENA_SIZE, 16, "exact") != NULL);
  arena_release((void *)(uintptr_t)arena_addr());
}

void test_arena_release_by_a_stranger_is_ignored(void) {
  char other[16];
  void *a = arena_claim(64, 4, "a");
  arena_release(other);
  arena_release(NULL);
  CHECK_EQ(arena_held(), 1);
  arena_release(a);
  CHECK_EQ(arena_held(), 0);
}

void test_arena_claim_is_zeroed(void) {
  uint8_t *a = arena_claim(256, 4, "a");
  int i, zero = 1;
  memset(a, 0xA5, 256);
  arena_release(a);
  a = arena_claim(256, 4, "b");
  for (i = 0; i < 256; i++) if (a[i]) zero = 0;
  CHECK(zero);
  CHECK_EQ((uintptr_t)a % ARENA_ALIGN, 0);
  arena_release(a);
}

void test_arena_code_goes_to_flash_only_when_allowed(void) {
  CHECK_EQ(arena_code_in_flash(1, 1, 0), 1);
  CHECK_EQ(arena_code_in_flash(1, 1, CAPP_FULLSCREEN | CAPP_NEEDS_NET), 1);
  CHECK_EQ(arena_code_in_flash(1, 1, CAPP_CODE_IN_RAM), 0);
  CHECK_EQ(arena_code_in_flash(1, 0, 0), 0);   /* an OTA'd device: old table */
  CHECK_EQ(arena_code_in_flash(0, 1, 0), 0);   /* data on the heap */
}
