#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "drivers/char/ioqueue.h"

extern struct TTY_IOQUEUE keyboard_ioq;

#define KBD_MOD_SHIFT 1
#define KBD_MOD_CTRL 2
#define KBD_MOD_ALT 4

typedef void (*kbd_gui_hook_t)(uint8_t scancode, int pressed, uint8_t mods);

void keyboard_init(void);
void keyboard_flush_pending(void);
void keyboard_handler(void);
void keyboard_set_gui_hook(kbd_gui_hook_t hook);
char keyboard_translate(uint8_t scancode, int shift);

#endif
