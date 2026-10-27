/* Infrared timings: kernel/sys/irproto.c. */
#include "tinytest.h"
#include "kernel/sys/irproto.h"

/* The 32 bits a frame carries, read back the way a receiver would:
 * least significant first, a long space a 1. */
static uint32_t bits_lsb(const uint32_t *d) {
  uint32_t v = 0;
  int i;
  for (i = 0; i < 32; i++)
    if (d[3 + 2 * i] > 1000) v |= 1u << i;
  return v;
}

static uint32_t sum(const uint32_t *d, int n) {
  int i; uint32_t s = 0;
  for (i = 0; i < n; i++) s += d[i];
  return s;
}

void test_ir_nec_standard_address_sends_inverses(void) {
  uint32_t d[200];
  int n = ir_encode(IR_NEC, 0x00, 0x40, 0, d, 200);   /* 44-key remote: power */
  CHECK_EQ(n, 68);
  CHECK_EQ(d[0], 9000);
  CHECK_EQ(d[1], 4500);
  CHECK_EQ(d[2], 562);
  CHECK(bits_lsb(d) == 0xBF40FF00u);                  /* 00 FF 40 BF on the wire */
  CHECK_EQ(d[66], 562);                               /* the closing mark */
  CHECK_EQ(sum(d, n), IR_NEC_FRAME_US);               /* padded to a frame */
}

void test_ir_nec_extended_address_is_sixteen_bits(void) {
  uint32_t d[200];
  int n = ir_encode(IR_NEC, 0xEF00, 0x03, 0, d, 200); /* 24-key remote: on */
  CHECK_EQ(n, 68);
  CHECK(bits_lsb(d) == 0xFC03EF00u);
}

void test_ir_nec_msb_matches_the_old_printed_form(void) {
  uint32_t a[200], b[200];
  /* 0xF7C03F is how older tools printed the 24-key remote's "on". */
  int i, n = ir_encode(IR_NEC_MSB, 0, 0x00F7C03F, 0, a, 200);
  CHECK_EQ(n, 68);
  CHECK_EQ(ir_encode(IR_NEC, 0xEF00, 0x03, 0, b, 200), 68);
  for (i = 0; i < n; i++) CHECK_EQ(a[i], b[i]);
}

void test_ir_nec_repeats_follow_the_frame(void) {
  uint32_t d[200];
  int n = ir_encode(IR_NEC, 0x00, 0x5C, 2, d, 200);
  CHECK_EQ(n, 68 + 2 * 4);
  CHECK_EQ(d[68], 9000);
  CHECK_EQ(d[69], 2250);
  CHECK_EQ(d[70], 562);
  CHECK_EQ(sum(d + 68, 4), IR_NEC_FRAME_US);
  CHECK_EQ(sum(d, n), 3 * IR_NEC_FRAME_US);
}

void test_ir_refuses_what_it_cannot_send(void) {
  uint32_t d[200];
  CHECK_EQ(ir_encode(99, 0, 0, 0, d, 200), -1);
  CHECK_EQ(ir_encode(IR_NEC, 0x10000, 0, 0, d, 200), -1);
  CHECK_EQ(ir_encode(IR_NEC, 0, 0x100, 0, d, 200), -1);
  CHECK_EQ(ir_encode(IR_NEC, 0, 1, 0, d, 67), -2);
  CHECK_EQ(ir_encode(IR_NEC, 0, 1, 1, d, 70), -2);
  CHECK_EQ(ir_encode(IR_NEC, 0, 1, -1, d, 200), -1);
  CHECK(ir_carrier(IR_NEC) == 38000u);
  CHECK(ir_carrier(7) == 0u);
}
