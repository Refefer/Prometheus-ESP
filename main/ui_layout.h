/*
 * Every pixel constant in the project lives here.
 *
 * The UI is authored natively for this panel's 800x480 rather than in a
 * scaled design space: there is exactly one board, and esp32flight's
 * UISX/UISY scaling macros exist only because that project supports six
 * panels. A future port is "edit this header", which is the right cost.
 */
#pragma once

/* ------------------------------------------------------------------ screen */

#define SCR_W            800
#define SCR_H            480

#define HEADER_H          40
#define FOOTER_H          26
#define CONTENT_Y        (HEADER_H)                       /*  40 */
#define CONTENT_H        (SCR_H - HEADER_H - FOOTER_H)    /* 414 */
#define FOOTER_Y         (SCR_H - FOOTER_H)               /* 454 */

/* -------------------------------------------------------------- tile grid */

/*
 * 4 cols x 3 rows of 185x128 cells. Spans do the rest: a 2x1 is 380x128 --
 * wide and short, exactly the shape a "number + unit + sparkline" wants --
 * and a 4x3 is 770x404 for a single-metric hero page. One formula, one
 * occupancy bitmap, one packer.
 *
 * Checks: col 3 right edge = 600 + 185 = 785 = SCR_W - GRID_MX.
 *         row 2 bottom     = 321 + 128 = 449, and FOOTER_Y is 454.
 */
#define GRID_COLS          4
#define GRID_ROWS          3
#define GRID_MX           15      /* left/right margin */
#define GRID_MY            5      /* top/bottom margin inside the content area */
#define GRID_GAP          10

#define CELL_W           185      /* (800 - 2*15 - 3*10) / 4 */
#define CELL_H           128      /* (414 - 2*5  - 2*10) / 3 */
#define COL_PITCH        (CELL_W + GRID_GAP)   /* 195 */
#define ROW_PITCH        (CELL_H + GRID_GAP)   /* 138 */

#define TILE_X(c)        (GRID_MX + COL_PITCH * (c))               /* 15,210,405,600 */
#define TILE_Y(r)        (CONTENT_Y + GRID_MY + ROW_PITCH * (r))   /* 45,183,321 */
#define TILE_W(s)        (COL_PITCH * (s) - GRID_GAP)              /* 185,380,575,770 */
#define TILE_H(s)        (ROW_PITCH * (s) - GRID_GAP)              /* 128,266,404 */

#define TILE_SLOTS       (GRID_COLS * GRID_ROWS)   /* the live slot pool: 12 */

/* -------------------------------------------------------- compact row list */

/* LAYOUT_ROWS: 14 rows of 28px inside CONTENT_H (14*28 = 392, + 11 top pad). */
#define ROW_PITCH_COMPACT 28
#define ROWS_PER_PAGE     14

/* ---------------------------------------------------------------- keyboard */

/*
 * The keyboard occupies 210 of 480 px -- nearly half the screen. Every
 * configuration layout is built around EDIT_STRIP_H being all the room
 * available while it is up, which is why hard fields (URLs, PromQL,
 * passwords) get a modal editor rather than inline focus: with inline focus
 * the field being edited is frequently underneath the keyboard.
 */
#define KB_H             210
#define KB_Y             (SCR_H - KB_H)     /* 270 */
#define EDIT_STRIP_H     (SCR_H - KB_H)     /* 270 */

/* ------------------------------------------------------------------ common */

#define PAD_S              6
#define PAD_M             10
#define PAD_L             16

#define RADIUS_TILE        6
#define RADIUS_CTRL        6
#define RADIUS_CHIP       14

/* Touch targets. 9mm is the usual comfort floor; at this panel's 133 DPI
 * that is ~47px, so nothing interactive should be shorter than TOUCH_MIN. */
#define TOUCH_MIN         44
#define BTN_H             44
#define FIELD_H           48
#define LIST_ROW_H        42
