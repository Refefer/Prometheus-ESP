/*
 * Widget factories and the change-diffing setters.
 *
 * The diff helpers are not an optimisation to add later -- they are load
 * bearing. LVGL 8's style setters have no old-value comparison, so an
 * unconditional write invalidates the object and forces a repaint even when
 * nothing changed. With a dozen live tiles repainting into an 800x480 PSRAM
 * framebuffer, that is the difference between a poll costing twelve small
 * rect invalidations and costing a full-screen redraw.
 */
#pragma once

#include "lvgl.h"
#include "ui_theme.h"

/* ---------------------------------------------------------- diffing setters */

void label_set_if_changed(lv_obj_t *l, const char *txt);
void label_set_fmt_if_changed(lv_obj_t *l, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void text_color_if_changed(lv_obj_t *o, lv_color_t c);
void bg_color_if_changed(lv_obj_t *o, lv_color_t c);
void border_color_if_changed(lv_obj_t *o, lv_color_t c);
void opa_if_changed(lv_obj_t *o, lv_opa_t opa);
/* Toggling LV_OBJ_FLAG_HIDDEN unconditionally also invalidates; check first. */
void hidden_if_changed(lv_obj_t *o, bool hidden);

/* ------------------------------------------------------------------ factories */

lv_obj_t *make_panel(lv_obj_t *parent);               /* flat surface, no scroll */
lv_obj_t *make_card(lv_obj_t *parent);                /* rounded tile surface */
lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t colour);
lv_obj_t *make_btn(lv_obj_t *parent, const char *text,
                   lv_event_cb_t cb, void *user_data);
lv_obj_t *make_btn_accent(lv_obj_t *parent, const char *text,
                          lv_event_cb_t cb, void *user_data);
lv_obj_t *make_chip(lv_obj_t *parent, const char *text,
                    lv_event_cb_t cb, void *user_data);
lv_obj_t *make_field(lv_obj_t *parent, const char *value, bool password);
lv_obj_t *make_dropdown(lv_obj_t *parent, const char *options,
                        lv_event_cb_t cb, void *user_data);
lv_obj_t *make_switch(lv_obj_t *parent, bool on, lv_event_cb_t cb, void *user_data);
lv_obj_t *make_divider(lv_obj_t *parent, lv_coord_t w);
/* A small filled circle -- status dots, freshness indicators, page dots. */
lv_obj_t *make_dot(lv_obj_t *parent, lv_coord_t d, lv_color_t colour);

/*
 * Logs any two visible siblings whose rectangles intersect.
 *
 * These sheets are positioned with absolute pixel arithmetic, so a control
 * that grows or moves silently slides under its neighbour -- twice now a
 * button has been half-hidden behind another and only a person looking at the
 * glass noticed. Call it once after building and refreshing a sheet; it is
 * O(n^2) over a few dozen children, which is nothing next to opening one.
 *
 * Hidden children are skipped: rows that deliberately share a slot and show
 * one set at a time are correct, not overlapping.
 */
void ui_check_overlaps(lv_obj_t *root, const char *what);

/* A transient message over everything, auto-dismissed. */
void ui_toast(const char *text, severity_t sev, uint32_t ms);
