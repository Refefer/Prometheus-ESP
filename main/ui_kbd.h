/*
 * The singleton on-screen keyboard, and the two ways text gets entered.
 *
 * The keyboard occupies 210 of 480 px -- nearly half the panel -- and that
 * single fact drives every configuration layout in this app. Two entry modes,
 * chosen per field:
 *
 *   ui_kbd_edit()   A MODAL FIELD EDITOR for hard fields: URLs, PromQL
 *                   expressions, passwords. It owns y 0..270 with the
 *                   keyboard below. With plain inline focus at this
 *                   resolution the field being edited is frequently
 *                   underneath the keyboard, and the sheet also gives us
 *                   somewhere to put shortcut chips, recents and live
 *                   validation.
 *
 *   ui_kbd_attach() INLINE FOCUS + SHRINK for easy fields: short numerics,
 *                   screen names, display-name overrides. The scroll page
 *                   shrinks to the strip above the keyboard and scrolls the
 *                   field into view, which preserves surrounding context.
 */
#pragma once

#include "lvgl.h"
#include "ui_theme.h"

#include <stddef.h>

typedef enum {
    KB_TEXT = 0,
    KB_NUMBER,
    KB_URL,        /* digits, ':' '/' '.' '-' '_' all on the base layer */
    KB_PROMQL,     /* adds {} [] () " = ! ~ , */
    KB_PASSWORD,
} kb_kind_t;

typedef struct {
    const char *title;      /* sheet header */
    const char *label;      /* field caption */
    const char *value;      /* initial contents */
    const char *hint;       /* one line of dim help under the field */
    kb_kind_t   kind;

    /* NULL-terminated arrays of one-tap insertions and tappable history.
     * For a URL these turn ~40 keystrokes into about 8. */
    const char *const *chips;
    const char *const *recents;

    /* Called on every keystroke, debounced. Fill msg/sev to show a live
     * status line. Optional. */
    void (*validate)(const char *text, void *user, char *msg, size_t cap,
                     severity_t *sev);

    /* text == NULL means the user cancelled. */
    void (*done)(const char *text, void *user);
    void *user;
} ui_kbd_req_t;

/* Creates the keyboard on lv_layer_top(). Call once, under the LVGL lock. */
void ui_kbd_init(void);

/* Inline mode. `page` is the scrollable container to shrink while the
 * keyboard is up; pass NULL to skip the shrink. */
void ui_kbd_attach(lv_obj_t *ta, kb_kind_t kind, lv_obj_t *page);

/* Modal mode. The request is copied, so it may live on the stack -- but the
 * strings it points at must outlive the sheet. */
void ui_kbd_edit(const ui_kbd_req_t *req);

bool ui_kbd_is_open(void);

/* Show a status line on the open sheet (e.g. the result of a Test). */
void ui_kbd_set_status(const char *msg, severity_t sev);
