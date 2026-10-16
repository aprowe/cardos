/* The Python app's HAL header (MICROPY_MPHALPORT_H): what MicroPython's
 * core asks of a port beyond py/mphal.h's defaults. */
#ifndef CARDPY_HAL_H
#define CARDPY_HAL_H

#define mp_hal_pin_obj_t

/* A key held down is the interrupt (cardpy_poll), whatever the character. */
static inline void mp_hal_set_interrupt_char(int c) { (void)c; }

#endif
