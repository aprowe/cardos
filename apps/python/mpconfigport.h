/* MicroPython's configuration for the Python app (apps/python/python.c).
 *
 * The embed port's sources in apps/python/mp are generated from this file
 * (tools/make_micropython.sh): change a feature here and generate them again,
 * since the interned strings (genhdr/qstrdefs.generated.h) depend on it. */

#include <port/mpconfigport_common.h>
#undef MICROPY_MPHALPORT_H
#define MICROPY_MPHALPORT_H "cardpy_hal.h"

#define MICROPY_CONFIG_ROM_LEVEL                (MICROPY_CONFIG_ROM_LEVEL_CORE_FEATURES)

#define MICROPY_ENABLE_COMPILER                 (1)
#define MICROPY_ENABLE_GC                       (1)
#define MICROPY_GC_SPLIT_HEAP                   (1)   /* the heap is big-mode blocks */
#define MICROPY_PY_GC                           (1)

/* An app has no assembler of its own for nlr: the toolchain's setjmp spills
 * the register windows with a syscall the kernel's vectors already handle. */
#define MICROPY_NLR_SETJMP                      (1)

#define MICROPY_FLOAT_IMPL                      (MICROPY_FLOAT_IMPL_FLOAT)
#define MICROPY_LONGINT_IMPL                    (MICROPY_LONGINT_IMPL_MPZ)
/* Complex numbers cost ~4 KB of code and kept the app over the loader's
 * 96 KB (CAPP_MAX_CODE, kernel/app/elfload.c): 97484 bytes with them. */
#define MICROPY_PY_BUILTINS_COMPLEX             (0)

#define MICROPY_HELPER_REPL                     (1)
#define MICROPY_REPL_AUTO_INDENT                (1)
#define MICROPY_ENABLE_SOURCE_LINE              (1)
#define MICROPY_ERROR_REPORTING                 (MICROPY_ERROR_REPORTING_NORMAL)
#define MICROPY_KBD_EXCEPTION                   (1)
#define MICROPY_STACK_CHECK                     (1)
/* The app runs on the shell's stack (8 KB, shared with the kernel): Python
 * calls keep their frames on the heap instead of recursing in C. */
#define MICROPY_STACKLESS                       (1)

/* Nothing else runs while Python does: every few hundred bytecodes the app
 * looks for a key held down, and makes it a KeyboardInterrupt. */
void cardpy_poll(void);
#define MICROPY_VM_HOOK_COUNT                   (400)
#define MICROPY_VM_HOOK_INIT                    static unsigned int vm_hook_divisor = MICROPY_VM_HOOK_COUNT;
#define MICROPY_VM_HOOK_POLL                    if (--vm_hook_divisor == 0) { vm_hook_divisor = MICROPY_VM_HOOK_COUNT; cardpy_poll(); }
#define MICROPY_VM_HOOK_LOOP                    MICROPY_VM_HOOK_POLL
#define MICROPY_VM_HOOK_RETURN                  MICROPY_VM_HOOK_POLL

/* Files on the card (cardpy_mod.h): open(), import from the script's
 * folder and /lib, sys.stdout as a stream (print(file=f)), and a finaliser
 * so a file nothing refers to is closed when the heap is collected. */
#define MICROPY_ENABLE_EXTERNAL_IMPORT          (1)
#define MICROPY_PY_SYS_STDFILES                 (1)
#define MICROPY_ENABLE_FINALISER                (1)

/* exit() and quit(), beside sys.exit: SystemExit, which leaves the app. */
extern const struct _mp_obj_fun_builtin_var_t cardpy_exit_obj;
/* input(): typed keys through api->key_wait (API 49), apps/python/python.c */
extern const struct _mp_obj_fun_builtin_var_t cardpy_input_obj;
#define MICROPY_PORT_EXTRA_BUILTINS \
    { MP_ROM_QSTR(MP_QSTR_exit), MP_ROM_PTR(&cardpy_exit_obj) }, \
    { MP_ROM_QSTR(MP_QSTR_quit), MP_ROM_PTR(&cardpy_exit_obj) }, \
    { MP_ROM_QSTR(MP_QSTR_input), MP_ROM_PTR(&cardpy_input_obj) },

#define MICROPY_PY_SYS_PLATFORM                 "cardos"
#define MICROPY_HW_BOARD_NAME                   "Cardputer"
#define MICROPY_HW_MCU_NAME                     "ESP32-S3"
