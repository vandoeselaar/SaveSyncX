#ifndef UI_H
#define UI_H

#include "../src/config.h"
#include <SDL.h>

/* ── Menu IDs ────────────────────────────────────────────────────────────── */
typedef enum {
    MENU_MAIN = 0,
    MENU_BACKUP,
    MENU_RESTORE,
    MENU_DOWNLOAD, 
    MENU_SETTINGS,
    MENU_CREDITS,
    MENU_EXIT,
} MenuID;

/* ── Layout constants ────────────────────────────────────────────────────── */
#define SCREEN_W       640
#define SCREEN_H       480
#define CHAR_H         16
#define MARGIN_X       32
#define MARGIN_Y       24
#define LIST_PAGE_SIZE 18
#define UI_FLAG_WEBDAV_OK  (1 << 0)
#define UI_FLAG_GITHUB_OK  (1 << 1)

/* ── Log buffer size ─────────────────────────────────────────────────────── */
#define TRANSFER_LOG_LINES  8
#define TRANSFER_LOG_WIDTH  60

/* ── Progress state (shared between transfer thread and UI) ─────────────── */
typedef struct {
    volatile int    active;          /* 1 while transfer runs              */
    volatile int    cancellable;     /* 1 = B-knop annuleert, 0 = B genegeerd
                                         (toont ook geen "B = Cancel" hint) */
    volatile size_t bytes_done;
    volatile size_t bytes_total;
    volatile int    files_done;
    volatile int    files_total;
    char            current_file[256];
    char            status_msg[128];
    volatile int    error;           /* non-zero if something went wrong   */
    /* ── Scrolling log buffer ── */
    char            log[TRANSFER_LOG_LINES][TRANSFER_LOG_WIDTH];
    volatile int    log_next;        /* index waar de volgende regel komt  */
} TransferState;

/* Per-lijst scroll/repeat state. Elke aanroeplocatie (do_backup, do_restore,
   ui_main_menu, ...) houdt zijn EIGEN static ScrollState bij -- nooit delen
   tussen verschillende lijsten, anders lekt de repeat-timing van de ene
   lijst naar de andere zodra je tussen schermen wisselt. */
typedef struct {
    Uint32 next_repeat_up;
    Uint32 next_repeat_down;
    Uint32 held_since_up;
    Uint32 held_since_down;
} ScrollState;
/* Verwerkt DPAD up/down met initiele delay (300ms) + acceleratie.
   Retourneert -1 (omhoog), +1 (omlaag) of 0 (geen scroll deze frame).
   count = aantal items in de lijst; bij count <= 0 altijd 0. */
int scroll_update(ScrollState *st, int count);
 
/* Past delta toe op *sel met wraparound binnen [0, count-1]. No-op bij
   delta == 0 of count <= 0. */
void scroll_apply(int *sel, int delta, int count);

/* ── Initialise SDL + font rendering ────────────────────────────────────── */
int  ui_init(void);
void ui_shutdown(void);

/* ── Main event loop ─────────────────────────────────────────────────────── */
/*  Returns the MenuID the user confirmed, or MENU_EXIT to quit.            */
MenuID ui_main_menu(int flags);

/* ── Settings editor (d-pad to navigate, on-screen keyboard for text) ───── */
int ui_settings_screen(void);     /* Returns 0 if saved, -1 if cancelled  */

/* ── Credits screen ──────────────────────────────────────────────────────── */
void ui_credits_screen(void);

/* ── Confirm dialog  ─────────────────────────────────────────────────────── */
/*  message: question text.  Returns 1=Yes, 0=No                            */
int ui_confirm(const char *message);

/* ── Progress screen (call in your transfer thread's idle loop) ─────────── */
/*  Returns 0 to continue, -1 if user pressed B to abort                    */
int ui_progress(const TransferState *state);

/* ── Simple message overlay ──────────────────────────────────────────────── */
void ui_message(const char *title, const char *body);
void ui_message_nowait(const char *title, const char *body);
void ui_message_timeout(const char *title, const char *body, int ms);

/* ── Poll events; call at top of any loop to avoid freezing ─────────────── */
void ui_pump_events(void);

/* Controller button constants (mapped from SDL joystick) */
#define BTN_A       0
#define BTN_B       1
#define BTN_X       2
#define BTN_Y       3
#define BTN_START   6
#define BTN_BACK    7
#define DPAD_UP     10
#define DPAD_DOWN   11
#define DPAD_LEFT   12
#define DPAD_RIGHT  13

void ui_set_cfg_ptr(AppConfig *cfg);

/* Returns 1 if button b was just pressed this frame (edge-detect) */
int btn_pressed(int b);

/* Low-level draw functions (software framebuffer) */
void ui_clear(void);
void ui_flip(void);
void ui_draw_text(int x, int y, const char *s, Uint32 col);
void ui_fill_rect(int x, int y, int w, int h, Uint32 col);

/* Colour constants (ARGB) */
#define UI_COL_BG      0xFF0A0A14
#define UI_COL_TITLE   0xFF00C8FF
#define UI_COL_ITEM    0xFFD0D0D0
#define UI_COL_SELECT  0xFF00FF80
#define UI_COL_DIM     0xFF606060
#define UI_COL_OK      0xFF00C864
#define UI_COL_ERROR   0xFFFF4040
#define UI_COL_SELBG   0xFF001428

#endif /* UI_H */
