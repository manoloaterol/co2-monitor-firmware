#pragma once

#include <stdbool.h>

typedef enum {
    BTN_NONE = 0,
    BTN_LEFT,
    BTN_RIGHT,
    BTN_PUSH,
} btn_event_t;

void btn_init(void);

// Returns the pending event once and consumes it.
btn_event_t btn_take(void);

// How long the centre button must be held to ask for setup mode.
#define BTN_LONG_MS 3000

// How long the centre button must stay open before a press counts as over.
// Longer than a contact bounce, so a bounce on release does not read as a
// second press.
#define BTN_RELEASE_MS 100

typedef enum {
    PUSH_NONE = 0,
    PUSH_SHORT,   // released before BTN_LONG_MS
    PUSH_LONG,    // held for BTN_LONG_MS; reported while still held
} btn_push_t;

// Polls the centre button and reports each press once. Polled rather than
// driven from the interrupt because the interrupt only fires on the rising
// edge and would say nothing about how long the contact stays closed. A short
// press is only known on release, which is what keeps the start of a long
// press from also counting as a short one. Call it once per loop pass.
btn_push_t btn_push_poll(void);
