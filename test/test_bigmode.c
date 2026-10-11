/* kernel/app/bigmode.c: big-app mode. An app flagged CAPP_BIG, on screen,
 * may take blocks of the general heap for its lifetime; entering lets go
 * of what the OS can rebuild; leaving frees what the app took. Against a
 * fake heap that counts. */
#include <stdlib.h>
#include <string.h>

#include "tinytest.h"
#include "kernel/app/bigmode.h"

static int n_alloc, n_free, n_drop;
static size_t largest;

static void *f_alloc(size_t n) { n_alloc++; return malloc(n); }
static void f_free(void *p) { if (p) n_free++; free(p); }
static size_t f_largest(void) { return largest; }
static void drop(void) { n_drop++; }

static const BigHeap HEAP = { f_alloc, f_free, f_largest };

static void reset(void) {
  bigmode_set_heap(&HEAP);
  bigmode_reset();                    /* frees what the last test left, then count */
  n_alloc = n_free = n_drop = 0;
  largest = 120 * 1024;
}

static int A, B;                    /* two apps' identities */

void test_bigmode_only_the_big_app_on_screen_gets_memory(void) {
  void *p;
  reset();
  CHECK(!bigmode_active());
  CHECK(bigmode_alloc(&A, 1000) == NULL);       /* nobody entered */
  CHECK_EQ((int)bigmode_room(&A), 0);
  bigmode_enter(&A);
  CHECK(bigmode_active());
  CHECK(bigmode_owner() == &A);
  CHECK(bigmode_alloc(&B, 1000) == NULL);       /* not the big one */
  CHECK(bigmode_alloc(NULL, 1000) == NULL);     /* nor the kernel */
  p = bigmode_alloc(&A, 1000);
  CHECK(p != NULL);
  memset(p, 0x5a, 1000);
  CHECK_EQ(bigmode_blocks(), 1);
}

void test_bigmode_room_keeps_a_reserve(void) {
  reset();
  bigmode_enter(&A);
  CHECK_EQ((int)bigmode_room(&A), (int)(largest - BIG_RESERVE));
  CHECK(bigmode_alloc(&A, largest - BIG_RESERVE + 1) == NULL);   /* past the reserve */
  CHECK(bigmode_alloc(&A, largest - BIG_RESERVE) != NULL);
  largest = BIG_RESERVE / 2;
  CHECK_EQ((int)bigmode_room(&A), 0);
  CHECK(bigmode_alloc(&A, 16) == NULL);
}

void test_bigmode_leaving_frees_everything_it_took(void) {
  int i;
  reset();
  bigmode_enter(&A);
  for (i = 0; i < BIG_BLOCKS; i++) CHECK(bigmode_alloc(&A, 64) != NULL);
  CHECK(bigmode_alloc(&A, 64) == NULL);         /* the table is full */
  CHECK_EQ(n_alloc, BIG_BLOCKS);
  bigmode_release(&B);                          /* someone else closing: nothing */
  CHECK(bigmode_active());
  CHECK_EQ(n_free, 0);
  bigmode_release(&A);
  CHECK(!bigmode_active());
  CHECK_EQ(n_free, BIG_BLOCKS);
  CHECK_EQ(bigmode_blocks(), 0);
  CHECK(bigmode_alloc(&A, 64) == NULL);         /* gone with it */
}

void test_bigmode_entering_lets_go_of_what_the_os_can_rebuild(void) {
  reset();
  CHECK_EQ(bigmode_on_enter(drop), 0);
  CHECK_EQ(bigmode_on_enter(drop), 0);          /* once is enough */
  bigmode_enter(&A);
  CHECK_EQ(n_drop, 1);
  bigmode_enter(&A);                            /* already in: not again */
  CHECK_EQ(n_drop, 1);
  bigmode_release(&A);
  bigmode_enter(&B);                            /* a second big app, later */
  CHECK_EQ(n_drop, 2);
  CHECK(bigmode_owner() == &B);
}

void test_bigmode_a_second_big_app_takes_over(void) {
  void *p;
  reset();
  bigmode_enter(&A);
  p = bigmode_alloc(&A, 128);
  CHECK(p != NULL);
  bigmode_enter(&B);                            /* A was replaced without a release */
  CHECK(bigmode_owner() == &B);
  CHECK_EQ(n_free, 1);                          /* A's blocks went with it */
  CHECK(bigmode_alloc(&A, 64) == NULL);
}
