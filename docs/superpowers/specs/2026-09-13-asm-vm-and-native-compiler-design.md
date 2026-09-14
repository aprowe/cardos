# Assembly on the device: a VM, a native compiler, and an IDE

**Status:** approved 2026-09-13.

## What this is

A way to write assembly on the Cardputer and run it, two ways: interpreted on
a small virtual machine, or compiled to real Xtensa machine code and executed
natively on the same chip that is running the OS.

It is a toy, deliberately. It is also the front half of sub-project 3, which
`CLAUDE.md` has always described as "a custom language compiled **on the
device** to native Xtensa machine code". Nothing here needs to be thrown away
to get there: the assembler, the IDE and the register monitor are the parts
that a compiler would need anyway, and the code generator is the part that
turns into a compiler back end.

## Why it can work at all

The hard part is already solved, and shipping. `kernel/app/elfload.c` writes
machine code into RAM and jumps to it sixteen times a boot:

```c
code = heap_caps_malloc(code_size, MALLOC_CAP_EXEC);   /* an IRAM pointer */
static uint8_t *writable(uint8_t *exec) {              /* the same SRAM, byte-addressable */
  if (exec && esp_ptr_in_diram_iram(exec))
    return (uint8_t *)esp_ptr_diram_iram_to_dram(exec);
  return exec;
}
```

The ESP32-S3 reaches the same physical SRAM through two address windows: an
instruction window that permits only aligned 32-bit fetches, and a data window
that permits byte access. Code is **written** through the data alias and
**executed** through the instruction alias. `apps/capp.ld` exists because of
that one fact, and it is the same fact this design rests on.

Cache coherency is not a concern: internal SRAM is not cached on this part
(the cache serves external flash and PSRAM, and this board has no PSRAM), and
the loader's daily success is the evidence.

## Encodings, measured

Taken from `xtensa-esp32s3-elf-as` on this machine rather than from a
reference, because a transcription error here is a machine that reboots:

```
movi a2, 2047    ffa722     12-bit signed immediate, -2048..2047
movi a3, -2048   00a832
add  a4, a2, a3  423a       narrow, 2 bytes
sub  a4, a2, a3  c04230     wide, 3 bytes
l32i.n a5, a2, 0 0258
s32i.n a5, a2, 4 1259
addi.n a6, a6, 1 661b
beq  a2, a3, f   eb1237     BRI8: +-128 bytes, about 42 instructions
bne  a2, a3, f   e89237
j    f           fff946     18-bit: +-128 KB
l32r a7, lit     fff871     reads BACKWARDS only
call0 f          fffe05
ret.n            f00d
```

Two consequences shape the code generator:

- **The literal pool goes before the code**, because `l32r` can only read
  backwards. `capp.ld` already does this for the same reason.
- **Only wide forms are emitted**, never the narrow ones, so every instruction
  is three bytes and branch patching is arithmetic rather than a fixed-point
  iteration over shrinking encodings.

## The machine

Twelve registers, `r0`-`r11`, mapped 1:1 onto Xtensa `a2`-`a13`.

| Xtensa | use |
|---|---|
| `a0`, `a1` | return address, stack pointer -- the `call0` ABI's |
| `a2`-`a13` | **`r0`-`r11`, the VM's registers** |
| `a14` | code generator scratch: bounds checks and literals |
| `a15` | base of the data area |

Twelve is not a taste decision. `call0` reserves two registers and the code
generator needs two, and the file divides exactly. **Because the mapping is
1:1, there is no register allocator** -- the hardest part of a compiler simply
does not exist in this design, and that is what makes the whole thing a
weekend's work rather than a term's.

`a14` and `a15` are callee-saved under `call0`, so the prologue saves them and
the epilogue restores them.

### Instructions

Every opcode is chosen because it is one Xtensa instruction. Code generation
is therefore template expansion with the register fields patched in, not
instruction selection.

| VM | Xtensa | notes |
|---|---|---|
| `mov rd, rs` | `mov` | |
| `movi rd, imm` | `movi` | -2048..2047 |
| `lit rd, imm32` | `l32r` | anything wider, from the pool |
| `add/sub/and/or/xor rd, ra, rb` | same | |
| `addi rd, ra, imm` | `addi` | -128..127 |
| `mul rd, ra, rb` | `mull` | |
| `shl/shr rd, ra, imm` | `slli`/`srli` | immediate shifts are one instruction; the variable forms need `SAR` and two |
| `ld/st rd, [ra+imm]` | `l32i`/`s32i` | word |
| `ldb/stb rd, [ra+imm]` | `l8ui`/`s8i` | byte |
| `beq/bne/blt/bge ra, rb, L` | same | inverted-plus-`j` when out of reach |
| `jmp L` | `j` | |
| `call L` / `ret` | `call0` / `ret` | |
| `sys n` | table dispatch / `call0` trampoline | |
| `halt` | return to the monitor | |

**No divide.** Whether this part has the Xtensa divide option is not something
to assume; if `quos`/`quou` do not assemble, division is a `sys` call. This is
checked against the toolchain during implementation, not guessed at.

### Memory

One flat data area, sized at assembly time, addressed relative to `a15`. The
VM's memory and the OS's heap are different things and a program cannot name
an address outside its area -- in checked mode by construction, in unchecked
mode by convention.

## Two back ends, one front end

The assembler produces VM bytecode. From there:

**Interpret.** A `switch`. Single-step, breakpoints, and a register display
that updates one instruction at a time -- which is the entire reason a VM is
here at all, because a chip with no debugger and no memory protection cannot
offer that natively.

**Compile.** Walk the same bytecode, emit Xtensa into a `MALLOC_CAP_EXEC`
buffer through the writable alias, jump to it. Literal pool first, wide forms
only, two passes so branch targets are known before they are patched.

The same source runs both ways, which is the point: a loop that takes a second
interpreted finishes instantly compiled, on the same screen, from the same
file.

## Safety

**Checked by default; `unchecked` opts out.**

Compiled code gets one compare-and-branch against a compile-time limit before
each load and store, into a shared fault stub that stops the program and
reports the offending address in the monitor. `a14` holds the scratch, so the
check costs no VM register. An `unchecked` directive drops the checks once a
program works; the monitor always says which mode produced the code, because
"it was fast that time" is not a thing to have to remember.

Unchecked native code can panic and reboot the device. That is stated plainly
in the app's help rather than hidden, and it is why checked is the default.

## Testing, and one honest limit

The best property of this design is the differential test: run a program both
ways, assert the final registers and memory match. **It cannot run on the
host** -- the host is x86 and cannot execute Xtensa. So the testing splits:

- **Host** (`test/test_asm.c`): the assembler, the interpreter, and the code
  generator checked against **golden encodings** -- the exact bytes that
  `xtensa-esp32s3-elf-as` produces for the same instruction, captured in the
  test. This pins every template without hardware.
- **Device**: a `verify` command in the IDE that runs both back ends and
  compares. This is what would actually catch a wrong template, and it is
  cheap because both back ends are already there.

## The IDE

One app, `apps/ide.c`, built from `apps/edit.c`'s editing core -- the line
buffer, cursor, file browser and save path -- and **not** its markdown
renderer, which is 220 lines of dead weight for an assembly file.

Four things the editor does not have:

- **Run** assembles the buffer and runs it, without saving first, because the
  edit-run loop should not go through the card.
- **Errors** put the cursor on the offending line with the message in the
  status bar. An assembler that says "syntax error" and not where is worse
  than no assembler.
- **A console**, a pane at the foot of the screen carrying `sys` output and
  the run's result. Toggleable, because 135 pixels is not many.
- **A monitor**, the twelve registers and a slice of memory, for stepping.

### Structure

The core -- assembler, interpreter, code generator, with no UI in it -- lives
in `apps/asmvm.h` as static functions, included by `apps/ide.c` and directly
by `test/test_asm.c`. `tools/build_apps.py` compiles `apps/*.c` only, so a
header is not built as its own app.

This departs from `test_pinball.c` and `test_calendar.c`, which include the
whole app. It is worth the departure: those tests carry a fake `CardApi` to
satisfy the UI, and there is no reason for a test of an instruction encoder to
need a fake `fill()`. The IDE keeps the established pattern for its own UI
logic.

## Files

- `apps/asmvm.h` -- new. Assembler, interpreter, Xtensa code generator.
- `apps/ide.c` -- new. Editor core from `edit.c`, plus run/errors/console/monitor.
- `test/test_asm.c` -- new. Host tests, including the golden encodings.
- `tools/build_apps.py` -- seed `ide` into `Tools`.
- `tools/make_color_icons.py` -- an icon.

## Cost

Roughly 20-25 KB of flash. The factory partition is at 61.3%, so there is
about 1.1 MB spare. Executable RAM at rest is 165-184 KB, and a compiled toy
program is a few hundred bytes of it.

## What is deliberately not here

- **Emitting a `.capp`.** The code generator could write a real app to
  `/desktop` that the existing loader runs with an icon. It is a hundred lines
  on top of what is here and it is not in this round: compiled code runs in
  place and is gone at reboot, and the program lives in its source file.
- **Recursion beyond one level.** `call0` puts the return address in `a0` and
  nesting needs it saved. One level works with no stack discipline at all;
  deeper needs a frame, which can come later.
- **A register allocator, an optimiser, a linker.** None are needed and each
  would be the tail wagging the dog.
