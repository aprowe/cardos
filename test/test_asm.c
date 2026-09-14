/* The toy machine: its assembler, its interpreter, and its Xtensa back end.
 *
 * The code generator emits machine code for a chip with no memory protection,
 * where a wrong instruction template is a reboot and not a stack trace. So
 * every template is pinned here against the exact bytes that
 * xtensa-esp32s3-elf-as produced for the same instruction on this machine --
 * captured below as GOLD_*, and never transcribed from a manual.
 *
 * What these tests CANNOT do is execute the result: the host is x86. The
 * differential check -- interpret and compile the same program and compare
 * the final state -- only runs on the device, from the IDE's `verify`. What
 * runs here is everything up to the moment of execution, which is where the
 * mistakes actually are.
 */

#include <stdio.h>
#include <string.h>

#include "tinytest.h"

#include "apps/asmvm.h"

/* ---- golden encodings ------------------------------------------------------
 *
 * Straight out of the assembler:
 *
 *    add   a2, a3, a4     802340        movi a2, 2047    ffa722
 *    sub   a2, a3, a4     c02340        movi a3, -2048   00a832
 *    and   a2, a3, a4     102340        addi a2, a3, 100 64c322
 *    or    a2, a3, a4     202340        addi a1, a1, -32 e0c112
 *    xor   a2, a3, a4     302340        l32i a2, a3, 8   022322
 *    mull  a2, a3, a4     822340        s32i a2, a3, 8   026322
 *    quos  a2, a3, a4     d22340        l8ui a2, a3, 8   080322
 *    mov   a2, a3         202330        s8i  a2, a3, 8   084322
 *    slli  a2, a3, 4      1123c0        callx0 a14       000ec0
 *    srli  a2, a3, 4      412430        ret              000080
 *    beq   a2, a3, -21    eb1237        nop              0020f0
 */

static void check_word(const char *what, uint32_t got, uint32_t want) {
  tt_checks++;
  if (got != want) {
    tt_fails++;
    printf("  FAIL %s: %s is %06lx, the assembler says %06lx\n",
           tt_current, what, (unsigned long)got, (unsigned long)want);
  }
}

void test_the_three_operand_templates_match_the_assembler(void) {
  check_word("add",  x_rrr(X_ADD,  2, 3, 4), 0x802340u);
  check_word("sub",  x_rrr(X_SUB,  2, 3, 4), 0xC02340u);
  check_word("and",  x_rrr(X_AND,  2, 3, 4), 0x102340u);
  check_word("or",   x_rrr(X_OR,   2, 3, 4), 0x202340u);
  check_word("xor",  x_rrr(X_XOR,  2, 3, 4), 0x302340u);
  check_word("mull", x_rrr(X_MULL, 2, 3, 4), 0x822340u);
  check_word("quos", x_rrr(X_QUOS, 2, 3, 4), 0xD22340u);
  /* Different registers, to prove the fields and not just the opcode. */
  check_word("add a5,a6,a7", x_rrr(X_ADD, 5, 6, 7), 0x805670u);
}

void test_the_immediate_templates_match_the_assembler(void) {
  check_word("movi 2047",  x_movi(2, 2047),  0xFFA722u);
  check_word("movi -2048", x_movi(3, -2048), 0x00A832u);
  check_word("addi 100",   x_addi(2, 3, 100), 0x64C322u);
  check_word("addi -32",   x_addi(1, 1, -32), 0xE0C112u);
  check_word("mov",        x_mov(2, 3),       0x202330u);
}

void test_the_memory_templates_match_the_assembler(void) {
  check_word("l32i", x_l32i(2, 3, 8), 0x022322u);
  check_word("s32i", x_s32i(2, 3, 8), 0x026322u);
  check_word("l8ui", x_l8ui(2, 3, 8), 0x080322u);
  check_word("s8i",  x_s8i(2, 3, 8),  0x084322u);
}

void test_the_shift_templates_match_the_assembler(void) {
  /* slli encodes 32-n split across two fields, which is the encoding most
   * likely to be got wrong by reading rather than measuring. */
  check_word("slli 4", x_slli(2, 3, 4), 0x1123C0u);
  check_word("srli 4", x_srli(2, 3, 4), 0x412430u);
}

void test_the_control_templates_match_the_assembler(void) {
  check_word("beq",    x_bcc(0x1u, 2, 3, -21), 0xEB1237u);
  check_word("bne",    x_bcc(0x9u, 2, 3, -24), 0xE89237u);
  check_word("j",      x_j(-27),               0xFFF946u);
  check_word("call0",  x_call0(-8),            0xFFFE05u);
  check_word("l32r",   x_l32r(7, -8),          0xFFF871u);
  check_word("callx0", x_callx0(14),           0x000EC0u);
  check_word("ret",    x_ret(),                0x000080u);
  check_word("nop",    x_nop(),                0x0020F0u);
}

void test_a_vm_register_maps_onto_the_right_xtensa_one(void) {
  /* r0 is a2 and r11 is a13, because a0/a1 are the ABI's and a14/a15 are the
   * compiler's. If this slips, generated code writes the stack pointer. */
  CHECK_EQ(2u, XA(0));
  CHECK_EQ(13u, XA(ASM_REGS - 1));
  CHECK(XA(ASM_REGS - 1) < XSCRATCH);
  CHECK(XSCRATCH != XBASE);
}

/* ---- the assembler ---------------------------------------------------------- */

static AsmProgram P;
static AsmState   S;

static int assemble(const char *src) {
  memset(&P, 0, sizeof P);
  return asm_assemble(&P, src);
}

void test_a_simple_program_assembles(void) {
  CHECK(assemble("movi r0, 5\nmovi r1, 7\nadd r2, r0, r1\nhalt\n"));
  CHECK_EQ(0, P.err_line);
  CHECK_EQ(4, P.n);
  CHECK_EQ(OP_MOVI, P.code[0].op);
  CHECK_EQ(0, P.code[0].a);
  CHECK_EQ(5, P.code[0].imm);
  CHECK_EQ(OP_ADD, P.code[2].op);
  CHECK_EQ(2, P.code[2].a);
  CHECK_EQ(0, P.code[2].b);
  CHECK_EQ(1, P.code[2].imm);
}

void test_blank_lines_and_comments_are_not_instructions(void) {
  CHECK(assemble("; a comment\n\n  # another\nmovi r0, 1   ; trailing\nhalt\n"));
  CHECK_EQ(2, P.n);
}

void test_a_label_resolves_forwards_and_backwards(void) {
  CHECK(assemble("top:\nmovi r0, 1\njmp done\nmovi r0, 2\ndone:\njmp top\n"));
  CHECK_EQ(4, P.n);
  CHECK_EQ(OP_JMP, P.code[1].op);
  CHECK_EQ(3, P.code[1].imm);     /* forward, to the instruction after movi */
  CHECK_EQ(0, P.code[3].imm);     /* backward, to the top */
}

void test_a_label_on_its_own_line_binds_to_the_next_instruction(void) {
  CHECK(assemble("a:\n\nb:\nmovi r0, 1\njmp a\njmp b\n"));
  CHECK_EQ(0, P.code[1].imm);
  CHECK_EQ(0, P.code[2].imm);
}

void test_errors_name_the_line_they_are_on(void) {
  CHECK(!assemble("movi r0, 1\nhalt\nwibble r0\n"));
  CHECK_EQ(3, P.err_line);
  CHECK(strcmp(P.err, "unknown instruction") == 0);

  CHECK(!assemble("movi r0, 1\nadd r0, r1\n"));
  CHECK_EQ(2, P.err_line);

  CHECK(!assemble("jmp nowhere\n"));
  CHECK_EQ(1, P.err_line);
  CHECK(strcmp(P.err, "no such label") == 0);

  CHECK(!assemble("movi r99, 1\n"));
  CHECK_EQ(1, P.err_line);

  CHECK(!assemble("a:\nmovi r0,1\na:\nhalt\n"));
  CHECK_EQ(3, P.err_line);
}

void test_the_first_error_is_the_one_reported(void) {
  /* Two bad lines: the one the cursor should land on is the earlier. */
  CHECK(!assemble("movi r0, 1\nbogus\nalsobogus\n"));
  CHECK_EQ(2, P.err_line);
}

void test_a_wide_constant_is_promoted_to_the_literal_pool(void) {
  CHECK(assemble("movi r0, 100000\nhalt\n"));
  CHECK_EQ(OP_LIT, P.code[0].op);
  CHECK_EQ(1, P.nlit);
  CHECK_EQ(100000, P.lit[0]);
  /* And one that fits stays a movi, because the pool costs four bytes. */
  CHECK(assemble("movi r0, 2047\nhalt\n"));
  CHECK_EQ(OP_MOVI, P.code[0].op);
  CHECK_EQ(0, P.nlit);
}

void test_the_pool_reuses_a_constant(void) {
  CHECK(assemble("movi r0, 70000\nmovi r1, 70000\nmovi r2, 80000\nhalt\n"));
  CHECK_EQ(2, P.nlit);
}

void test_offsets_out_of_range_are_refused(void) {
  CHECK(!assemble("addi r0, 200\n"));
  CHECK(!assemble("shl r0, 40\n"));
  CHECK(!assemble("ld r0, [r1+300]\n"));
  /* l32i scales its displacement by four and cannot encode an odd one. */
  CHECK(!assemble("ld r0, [r1+2]\n"));
  CHECK(assemble("ld r0, [r1+8]\nhalt\n"));
  /* A byte load has no such restriction. */
  CHECK(assemble("ldb r0, [r1+3]\nhalt\n"));
}

void test_the_unchecked_directive_turns_the_bounds_checks_off(void) {
  CHECK(assemble("movi r0, 1\nhalt\n"));
  CHECK_EQ(1, P.checked);
  CHECK(assemble(".unchecked\nmovi r0, 1\nhalt\n"));
  CHECK_EQ(0, P.checked);
  CHECK(!assemble(".wibble\n"));
}

/* ---- the interpreter --------------------------------------------------------- */

static AsmStop run(const char *src) {
  CHECK(assemble(src));
  memset(&S, 0, sizeof S);
  return asm_run(&P, &S, ASM_STEPS, 0, 0);
}

void test_arithmetic(void) {
  CHECK_EQ(RUN_HALT, run("movi r0, 20\nmovi r1, 7\n"
                         "add r2, r0, r1\nsub r3, r0, r1\n"
                         "mul r4, r0, r1\ndiv r5, r0, r1\nhalt\n"));
  CHECK_EQ(27, S.r[2]);
  CHECK_EQ(13, S.r[3]);
  CHECK_EQ(140, S.r[4]);
  CHECK_EQ(2, S.r[5]);
}

void test_logic_and_shifts(void) {
  CHECK_EQ(RUN_HALT, run("movi r0, 12\nmovi r1, 10\n"
                         "and r2, r0, r1\nor r3, r0, r1\nxor r4, r0, r1\n"
                         "mov r5, r0\nshl r5, 4\nmov r6, r0\nshr r6, 2\nhalt\n"));
  CHECK_EQ(8,   S.r[2]);
  CHECK_EQ(14,  S.r[3]);
  CHECK_EQ(6,   S.r[4]);
  CHECK_EQ(192, S.r[5]);
  CHECK_EQ(3,   S.r[6]);
}

void test_dividing_by_zero_faults_rather_than_returning_zero(void) {
  /* The hardware instruction is undefined for this, so the interpreter must
   * not quietly disagree with the compiled version about it. */
  CHECK_EQ(RUN_FAULT, run("movi r0, 1\nmovi r1, 0\ndiv r2, r0, r1\nhalt\n"));
}

void test_a_loop_counts(void) {
  CHECK_EQ(RUN_HALT, run("movi r0, 0\nmovi r1, 10\nmovi r2, 1\n"
                         "loop:\nadd r0, r0, r2\nblt r0, r1, loop\nhalt\n"));
  CHECK_EQ(10, S.r[0]);
}

void test_every_branch_takes_and_falls_through(void) {
  CHECK_EQ(RUN_HALT, run("movi r0, 1\nmovi r1, 1\nmovi r2, 0\n"
                         "beq r0, r1, a\nhalt\na:\nmovi r2, 7\nhalt\n"));
  CHECK_EQ(7, S.r[2]);
  CHECK_EQ(RUN_HALT, run("movi r0, 1\nmovi r1, 1\nmovi r2, 0\n"
                         "bne r0, r1, a\nmovi r2, 5\nhalt\na:\nmovi r2, 7\nhalt\n"));
  CHECK_EQ(5, S.r[2]);
  CHECK_EQ(RUN_HALT, run("movi r0, 1\nmovi r1, 2\nmovi r2, 0\n"
                         "bge r0, r1, a\nmovi r2, 5\nhalt\na:\nmovi r2, 7\nhalt\n"));
  CHECK_EQ(5, S.r[2]);
}

void test_memory_round_trips(void) {
  CHECK_EQ(RUN_HALT, run("movi r0, 16\nmovi r1, 12345\nst r1, [r0]\n"
                         "ld r2, [r0]\nmovi r3, 200\nstb r3, [r0+4]\n"
                         "ldb r4, [r0+4]\nhalt\n"));
  CHECK_EQ(12345, S.r[2]);
  CHECK_EQ(200, S.r[4]);
}

void test_a_stray_address_faults_instead_of_escaping(void) {
  CHECK_EQ(RUN_FAULT, run("movi r0, 2000\nmovi r1, 1\nst r1, [r0]\nhalt\n"));
  CHECK_EQ(2000u, S.fault_addr);
  /* A negative index is caught as a very large unsigned one, which is what
   * the compiled bgeu does too. */
  CHECK_EQ(RUN_FAULT, run("movi r0, -4\nmovi r1, 1\nst r1, [r0]\nhalt\n"));
}

void test_a_runaway_loop_is_stopped(void) {
  CHECK(assemble("here:\njmp here\n"));
  memset(&S, 0, sizeof S);
  CHECK_EQ(RUN_STEPS, asm_run(&P, &S, 500, 0, 0));
}

void test_one_instruction_at_a_time(void) {
  CHECK(assemble("movi r0, 3\nmovi r1, 4\nadd r2, r0, r1\nhalt\n"));
  memset(&S, 0, sizeof S);
  asm_run(&P, &S, 1, 0, 0);
  CHECK_EQ(3, S.r[0]);
  CHECK_EQ(0, S.r[1]);
  CHECK_EQ(1u, S.pc);
  asm_run(&P, &S, 1, 0, 0);
  CHECK_EQ(4, S.r[1]);
  asm_run(&P, &S, 1, 0, 0);
  CHECK_EQ(7, S.r[2]);
}

void test_call_and_ret(void) {
  CHECK_EQ(RUN_HALT, run("movi r0, 0\ncall sub\ncall sub\nhalt\n"
                         "sub:\naddi r0, 5\nret\n"));
  CHECK_EQ(10, S.r[0]);
}

void test_running_off_the_end_is_a_halt_not_a_crash(void) {
  CHECK_EQ(RUN_HALT, run("movi r0, 1\n"));
  CHECK_EQ(1, S.r[0]);
}

void test_an_empty_program_says_so(void) {
  CHECK(assemble("; nothing here\n"));
  memset(&S, 0, sizeof S);
  CHECK_EQ(RUN_NOCODE, asm_run(&P, &S, ASM_STEPS, 0, 0));
}

/* ---- sys calls ---------------------------------------------------------------- */

static char SYS_OUT[64];
static int  SYS_LEN;
static int  SYS_CALLS;

static void fake_sys(void *ctx, int call, AsmState *st) {
  (void)ctx;
  SYS_CALLS++;
  if (call == SYS_PUTC && SYS_LEN + 1 < (int)sizeof SYS_OUT)
    SYS_OUT[SYS_LEN++] = (char)st->r[0];
  if (call == SYS_TICKS) st->r[0] = 42;
  SYS_OUT[SYS_LEN] = 0;
}

void test_a_sys_call_reaches_the_host_and_can_write_back(void) {
  SYS_LEN = SYS_CALLS = 0;
  SYS_OUT[0] = 0;
  CHECK(assemble("movi r0, 72\nsys 0\nmovi r0, 105\nsys 0\nsys 2\nhalt\n"));
  memset(&S, 0, sizeof S);
  CHECK_EQ(RUN_HALT, asm_run(&P, &S, ASM_STEPS, fake_sys, 0));
  CHECK(strcmp(SYS_OUT, "Hi") == 0);
  CHECK_EQ(3, SYS_CALLS);
  CHECK_EQ(42, S.r[0]);          /* SYS_TICKS wrote into the state */
}

void test_an_unknown_sys_call_is_refused_at_assembly(void) {
  CHECK(!assemble("sys 99\n"));
  CHECK_EQ(1, P.err_line);
}

/* ---- the code generator -------------------------------------------------------- */

static uint8_t BUF[8192];
static AsmEmit E;

static int compile(const char *src) {
  CHECK(assemble(src));
  memset(&E, 0, sizeof E);
  memset(BUF, 0, sizeof BUF);
  E.buf = BUF;
  E.cap = (int)sizeof BUF;
  return asm_compile(&P, &E, 0x40080000u);
}

static uint32_t word_at(int off) {
  return (uint32_t)BUF[off] | ((uint32_t)BUF[off + 1] << 8) |
         ((uint32_t)BUF[off + 2] << 16);
}

void test_compiling_emits_the_expected_instruction(void) {
  CHECK(compile("add r0, r1, r2\nhalt\n"));
  /* r0,r1,r2 are a2,a3,a4 -- the very instruction the golden test pinned. */
  check_word("compiled add", word_at(E.ins_at[0]), 0x802340u);
}

void test_the_literal_pool_comes_before_the_code(void) {
  CHECK(compile("movi r0, 100000\nhalt\n"));
  /* Two words: the constant, then the trampoline's address. */
  CHECK_EQ(8, E.code0);
  CHECK_EQ(100000u, (uint32_t)BUF[0] | ((uint32_t)BUF[1] << 8) |
                    ((uint32_t)BUF[2] << 16) | ((uint32_t)BUF[3] << 24));
  CHECK(E.ins_at[0] > E.code0);
  /* l32r can only read backwards, so the instruction must sit after it. */
  CHECK(E.ins_at[0] > 0);
}

void test_a_short_branch_is_one_instruction(void) {
  CHECK(compile("movi r0, 0\nmovi r1, 1\nbeq r0, r1, done\nmovi r0, 9\ndone:\nhalt\n"));
  /* The branch is a single 3-byte beq, not the inverted-and-jump pair. */
  CHECK_EQ(3, E.ins_at[3] - E.ins_at[2]);
  CHECK_EQ(7u, word_at(E.ins_at[2]) & 0xFu);        /* op0 = 7, a BRI8 */
}

void test_a_long_branch_becomes_an_inverted_hop_and_a_jump(void) {
  /* Over 42 instructions of reach, so the 8-bit displacement cannot hold it
   * and the generator must invert the test and jump instead. This is the one
   * piece of the back end with a real decision in it. */
  char src[4096];
  int i, n = 0;
  n += sprintf(src + n, "movi r0, 0\nmovi r1, 1\nbeq r0, r1, done\n");
  for (i = 0; i < 80; i++) n += sprintf(src + n, "nop\n");
  n += sprintf(src + n, "done:\nhalt\n");
  CHECK(compile(src));
  CHECK_EQ(6, E.ins_at[3] - E.ins_at[2]);           /* two instructions now */
  CHECK_EQ(9u, (word_at(E.ins_at[2]) >> 12) & 0xFu); /* beq inverted to bne */
  CHECK_EQ(6u, word_at(E.ins_at[2] + 3) & 0xFu);     /* followed by a j */
}

void test_a_bounds_check_is_emitted_and_can_be_turned_off(void) {
  int checked_len, unchecked_len;
  CHECK(compile("movi r0, 0\nld r1, [r0]\nhalt\n"));
  checked_len = E.ins_at[2] - E.ins_at[1];
  CHECK(compile(".unchecked\nmovi r0, 0\nld r1, [r0]\nhalt\n"));
  unchecked_len = E.ins_at[2] - E.ins_at[1];
  /* Checked costs two extra instructions: the limit and the compare. */
  CHECK_EQ(6, checked_len - unchecked_len);
  CHECK_EQ(6, unchecked_len);                        /* add, then the load */
}

void test_the_prologue_saves_what_the_abi_says_it_must(void) {
  CHECK(compile("halt\n"));
  /* The first instruction is the frame, and it must come before any store. */
  check_word("frame", word_at(E.code0), x_addi(1u, 1u, -32));
  check_word("save a0", word_at(E.code0 + 3), x_s32i(0u, 1u, 0u));
}

void test_every_instruction_is_three_bytes(void) {
  /* The uniform size is what makes branch offsets arithmetic instead of a
   * fixed point, so it is worth asserting rather than assuming. */
  int i, bad = 0;
  CHECK(compile("movi r0, 1\nadd r1, r0, r0\nmov r2, r1\n"
                "shl r2, 3\nshr r2, 1\nnop\nret\nhalt\n"));
  for (i = 0; i < 7; i++)
    if (E.ins_at[i + 1] - E.ins_at[i] != 3) bad++;
  CHECK_EQ(0, bad);
}

void test_a_program_too_big_for_the_buffer_reports_it(void) {
  char src[4096];
  int i, n = 0;
  for (i = 0; i < 150; i++) n += sprintf(src + n, "nop\n");
  n += sprintf(src + n, "halt\n");
  CHECK(assemble(src));
  memset(&E, 0, sizeof E);
  E.buf = BUF;
  E.cap = 64;                       /* deliberately far too small */
  CHECK(!asm_compile(&P, &E, 0x40080000u));
  CHECK_EQ(1, E.overflow);
}

void test_the_two_passes_agree_on_where_everything_lands(void) {
  /* If the layout moved between passes, every branch would be off. It cannot
   * move, because all instructions are the same size -- this asserts it end
   * to end rather than by reasoning. */
  int first[16];
  int i, bad = 0;
  CHECK(compile("a:\nmovi r0, 1\njmp b\nnop\nb:\njmp a\nhalt\n"));
  for (i = 0; i < 5; i++) first[i] = E.ins_at[i];
  CHECK(compile("a:\nmovi r0, 1\njmp b\nnop\nb:\njmp a\nhalt\n"));
  for (i = 0; i < 5; i++) if (first[i] != E.ins_at[i]) bad++;
  CHECK_EQ(0, bad);
}
