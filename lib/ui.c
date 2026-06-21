/*
 * ui.c  –  SDL2-based on-screen UI voor XboxSaveSync
 *
 * nxdk gebruikt SDL2. SDL1-functies als SDL_SetVideoMode en SDL_Flip
 * bestaan niet meer. We gebruiken:
 *   SDL_CreateWindow / SDL_CreateRenderer / SDL_CreateTexture
 *   SDL_UpdateTexture + SDL_RenderCopy + SDL_RenderPresent
 *
 * Pixels worden getekend in een software framebuffer (g_pixels[]),
 * die elke frame via SDL_UpdateTexture naar de GPU gekopieerd wordt.
 */

#include "ui.h"
#include "../src/config.h"
#include "util.h"

#include <SDL.h>
#include <string.h>
#include <stdio.h>
#include <hal/debug.h>

/* ── Display constants ───────────────────────────────────────────────────── */
#define CHAR_W     8

/* ── Colours (ARGB in host byte order) ──────────────────────────────────── */
#define COL_BG       0xFF0A0A14
#define COL_TITLE    0xFF00C8FF
#define COL_ITEM     0xFFD0D0D0
#define COL_SELECT   0xFF00FF80
#define COL_DIM      0xFF606060
#define COL_BAR_BG   0xFF1E1E2E
#define COL_BAR_FG   0xFF00C864
#define COL_ERROR    0xFFFF4040

/* ── SDL2 globals ────────────────────────────────────────────────────────── */
static SDL_Window   *g_window   = NULL;
static SDL_Renderer *g_renderer = NULL;
static SDL_Texture  *g_texture  = NULL;
static Uint32        g_pixels[SCREEN_W * SCREEN_H];   /* software framebuffer */

static SDL_Joystick *g_joy = NULL;
static Uint8 g_btn[16]      = {0};
static Uint8 g_btn_prev[16] = {0};

/* ── Bitmap font ─────────────────────────────────────────────────────────── */
#include "font8x16.h"

/* ── Pixel helpers ───────────────────────────────────────────────────────── */
static inline void put_pixel(int x, int y, Uint32 col)
{
    if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H)
        g_pixels[y * SCREEN_W + x] = col;
}

static void draw_char(int x, int y, char ch, Uint32 col)
{
    if (ch < 32 || ch > 127) ch = '?';
    const Uint8 *glyph = font8x16_data[(int)(ch - 32)];
    for (int row = 0; row < CHAR_H; row++) {
        Uint8 bits = glyph[row];
        for (int bit = 0; bit < CHAR_W; bit++)
            if (bits & (0x80 >> bit))
                put_pixel(x + bit, y + row, col);
    }
}

void ui_draw_text(int x, int y, const char *s, Uint32 col)
{
    for (; *s; s++, x += CHAR_W)
        draw_char(x, y, *s, col);
}

void ui_fill_rect(int x, int y, int w, int h, Uint32 col)
{
    for (int py = y; py < y + h; py++)
        for (int px = x; px < x + w; px++)
            put_pixel(px, py, col);
}

void ui_clear(void)
{
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
        g_pixels[i] = COL_BG;
}

/* ── Present: kopieer framebuffer naar GPU en toon ──────────────────────── */
void ui_flip(void)
{
    SDL_UpdateTexture(g_texture, NULL, g_pixels, SCREEN_W * sizeof(Uint32));
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
    SDL_RenderPresent(g_renderer);
}

/* ── Controller polling ──────────────────────────────────────────────────── */
static void poll_controller(void)
{
    memcpy(g_btn_prev, g_btn, sizeof(g_btn));
    SDL_JoystickUpdate();
    if (g_joy) {
        /* Buttons A/B/X/Y/Black/White/Start/Back/LT/RT */
        for (int i = 0; i < 10 && i < SDL_JoystickNumButtons(g_joy); i++)
            g_btn[i] = (Uint8)SDL_JoystickGetButton(g_joy, i);

        /* D-pad via HAT */
        Uint8 hat = SDL_JoystickGetHat(g_joy, 0);
        g_btn[DPAD_UP]    = (hat & SDL_HAT_UP)    ? 1 : 0;
        g_btn[DPAD_DOWN]  = (hat & SDL_HAT_DOWN)  ? 1 : 0;
        g_btn[DPAD_LEFT]  = (hat & SDL_HAT_LEFT)  ? 1 : 0;
        g_btn[DPAD_RIGHT] = (hat & SDL_HAT_RIGHT) ? 1 : 0;
    }
}

int btn_pressed(int b)
{
    return g_btn[b] && !g_btn_prev[b];
}

/* ── Scroll repeat helper (DPAD up/down met acceleratie + wraparound) ─────── */
/* ScrollState / prototypes staan in ui.h zodat backup.c en restore.c
   deze ook kunnen gebruiken (elk met hun eigen losse ScrollState-instantie). */

/* Acceleratie-curve: hoe langer ingedrukt, hoe kleiner het interval.
   Pas de drempels/snelheden aan naar smaak. */
static Uint32 scroll_repeat_interval(Uint32 held_ms)
{
    if (held_ms > 2000) return 33;   /* ~30/s  na 2s   */
    if (held_ms > 1000) return 66;   /* ~15/s  na 1s   */
    return 100;                       /* ~10/s  basis   */
}

/* Retourneert: -1 = omhoog, +1 = omlaag, 0 = geen scroll deze frame.
   count = aantal items in de lijst (voor wraparound). */
int scroll_update(ScrollState *st, int count)
{
    Uint32 now = SDL_GetTicks();
    int delta = 0;

    if (count <= 0) return 0;

    /* DOWN */
    if (btn_pressed(DPAD_DOWN)) {
        delta = +1;
        st->held_since_down = now;
        st->next_repeat_down = now + 300; /* initiele delay */
    } else if (g_btn[DPAD_DOWN] && now >= st->next_repeat_down) {
        delta = +1;
        Uint32 held = now - st->held_since_down;
        st->next_repeat_down = now + scroll_repeat_interval(held);
    }

    /* UP (alleen als DOWN deze frame niks deed, voorkomt dubbele input bij overlap) */
    if (delta == 0) {
        if (btn_pressed(DPAD_UP)) {
            delta = -1;
            st->held_since_up = now;
            st->next_repeat_up = now + 300;
        } else if (g_btn[DPAD_UP] && now >= st->next_repeat_up) {
            delta = -1;
            Uint32 held = now - st->held_since_up;
            st->next_repeat_up = now + scroll_repeat_interval(held);
        }
    }

    return delta;
}

/* Past delta toe op sel met wraparound over [0, count-1] */
void scroll_apply(int *sel, int delta, int count)
{
    if (delta == 0 || count <= 0) return;
    *sel = (*sel + delta + count) % count;
}

/* ── Init ────────────────────────────────────────────────────────────────── */
int ui_init(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) < 0) return -1;

    g_window = SDL_CreateWindow(
        "XboxSaveSync",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        SCREEN_W, SCREEN_H, 0);
    if (!g_window) return -1;

    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    if (!g_renderer) return -1;

    /* ARGB8888 texture als render target voor de software framebuffer */
    g_texture = SDL_CreateTexture(
        g_renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        SCREEN_W, SCREEN_H);
    if (!g_texture) return -1;

    SDL_ShowCursor(SDL_DISABLE);

    if (SDL_NumJoysticks() > 0) {
        g_joy = SDL_JoystickOpen(0);
        SDL_JoystickEventState(SDL_ENABLE);
    }
    return 0;
}

void ui_shutdown(void)
{
    ui_message_nowait("SaveSyncX", "Shutting down...");
    if (g_joy)      SDL_JoystickClose(g_joy);
    if (g_texture)  SDL_DestroyTexture(g_texture);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window)   SDL_DestroyWindow(g_window);
    SDL_Quit();
}


void ui_pump_events(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) { /* handled by caller */ }
    }
    poll_controller();
}

/* ── Main menu ───────────────────────────────────────────────────────────── */
static const char *MENU_LABELS[] = {
    "  Backup   save games  ->  WebDAV",
    "  Restore  save games  <-  WebDAV",
    "  Download save games  <-  Github",
    "  Settings",
    "  Credits",
    "  Quit",
};
static const MenuID MENU_IDS[] = {
    MENU_BACKUP, MENU_RESTORE, MENU_DOWNLOAD, MENU_SETTINGS, MENU_CREDITS, MENU_EXIT
};
#define MENU_COUNT 6

MenuID ui_main_menu(void)
{
    int sel = 0;
    static ScrollState scroll = {0};
    while (1) {
        ui_pump_events();

        int delta = scroll_update(&scroll, MENU_COUNT);
        scroll_apply(&sel, delta, MENU_COUNT);

        if (btn_pressed(BTN_A) || btn_pressed(BTN_START))
            return MENU_IDS[sel];

        //if (btn_pressed(DPAD_DOWN)) sel = (sel + 1) % MENU_COUNT;
        //if (btn_pressed(DPAD_UP))   sel = (sel + MENU_COUNT - 1) % MENU_COUNT;
        //if (btn_pressed(BTN_A) || btn_pressed(BTN_START))
        //    return MENU_IDS[sel];

        ui_clear();
        ui_draw_text(MARGIN_X, MARGIN_Y, "  SaveSyncX  v1.0", COL_TITLE);
        ui_draw_text(MARGIN_X, MARGIN_Y + CHAR_H,
                  "  TDATA / UDATA  <->  WebDAV", COL_DIM);

        /* Separator */
        ui_fill_rect(MARGIN_X, MARGIN_Y + CHAR_H * 3, SCREEN_W - MARGIN_X*2, 1,
                  COL_DIM);

        for (int i = 0; i < MENU_COUNT; i++) {
            Uint32 col = (i == sel) ? COL_SELECT : COL_ITEM;
            int y = MARGIN_Y + CHAR_H * (5 + i * 2);
            if (i == sel)
                ui_fill_rect(MARGIN_X - 4, y - 2,
                          SCREEN_W - MARGIN_X * 2 + 8, CHAR_H + 4,
                          0xFF001428);
            ui_draw_text(MARGIN_X, y, MENU_LABELS[i], col);
        }

        ui_draw_text(MARGIN_X, SCREEN_H - MARGIN_Y - CHAR_H,
                  "A=Select  Up/Down=Navigate", COL_DIM);
        ui_flip();
        SDL_Delay(16);
    }
}

/* ── Confirm dialog ──────────────────────────────────────────────────────── */
int ui_confirm(const char *message)
{
    int sel = 0;  /* 0=Yes 1=No */
    while (1) {
        ui_pump_events();
        if (btn_pressed(DPAD_LEFT) || btn_pressed(DPAD_RIGHT))
            sel ^= 1;
        if (btn_pressed(BTN_A) || btn_pressed(BTN_START))
            return (sel == 0) ? 1 : 0;
        if (btn_pressed(BTN_B))
            return 0;

        ui_clear();
        ui_draw_text(MARGIN_X, SCREEN_H / 2 - CHAR_H * 2, message, COL_ITEM);

        /* Yes / No buttons */
        for (int i = 0; i < 2; i++) {
            const char *label = (i == 0) ? "  Yes  " : "  No  ";
            int x = SCREEN_W / 2 - 60 + i * 80;
            int y = SCREEN_H / 2 + CHAR_H;
            Uint32 col = (sel == i) ? COL_SELECT : COL_DIM;
            if (sel == i)
                ui_fill_rect(x - 2, y - 2, CHAR_W * 7 + 4, CHAR_H + 4, 0xFF001428);
            ui_draw_text(x, y, label, col);
        }
        ui_draw_text(MARGIN_X, SCREEN_H - MARGIN_Y - CHAR_H,
                  "A=Confirm  B=Cancel", COL_DIM);
        ui_flip();
        SDL_Delay(16);
    }
}

/* ── Progress screen ─────────────────────────────────────────────────────── */
int ui_progress(const TransferState *st)
{
    ui_pump_events();
    if (st->cancellable && btn_pressed(BTN_B)) return -1;

    ui_clear();
    ui_draw_text(MARGIN_X, MARGIN_Y, "  Transfer in progress...", COL_TITLE);

    /* File count */
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "Files: %d / %d",
             st->files_done, st->files_total);
    ui_draw_text(MARGIN_X, MARGIN_Y + CHAR_H * 2, tmp, COL_ITEM);

    /* Bytes */
    char done_str[32], total_str[32];
    util_format_bytes(st->bytes_done, done_str, sizeof(done_str));
    util_format_bytes(st->bytes_total, total_str, sizeof(total_str));
    snprintf(tmp, sizeof(tmp), "Bytes: %s / %s", done_str, total_str);
    ui_draw_text(MARGIN_X, MARGIN_Y + CHAR_H * 3, tmp, COL_ITEM);

    /* Progress bar */
    int bar_x = MARGIN_X, bar_y = MARGIN_Y + CHAR_H * 5;
    int bar_w  = SCREEN_W - MARGIN_X * 2, bar_h = 12;
    ui_fill_rect(bar_x, bar_y, bar_w, bar_h, COL_BAR_BG);
    if (st->bytes_total > 0) {
        int fill = (int)((double)st->bytes_done / st->bytes_total * bar_w);
        if (fill > bar_w) fill = bar_w;
        ui_fill_rect(bar_x, bar_y, fill, bar_h, COL_BAR_FG);
    }

    /* Current file */
    ui_draw_text(MARGIN_X, MARGIN_Y + CHAR_H * 7, st->current_file, COL_DIM);

    /* Status */
    ui_draw_text(MARGIN_X, MARGIN_Y + CHAR_H * 8, st->status_msg, COL_ERROR);

    /* ── Scrolling log: oudste → nieuwste, nieuwste onderaan ── */
    ui_draw_text(MARGIN_X, MARGIN_Y + CHAR_H * 10, "Log:", COL_DIM);
    for (int i = 0; i < TRANSFER_LOG_LINES; i++) {
        int idx = (st->log_next + i) % TRANSFER_LOG_LINES;
        if (st->log[idx][0] == '\0') continue;
        Uint32 col = (i == TRANSFER_LOG_LINES - 1) ? COL_ITEM : COL_DIM;
        ui_draw_text(MARGIN_X, MARGIN_Y + CHAR_H * (11 + i),
                  st->log[idx], col);
    }

    if (st->cancellable)
        ui_draw_text(MARGIN_X, SCREEN_H - MARGIN_Y - CHAR_H,
                  "B = Cancel transfer", COL_DIM);
    else
        ui_draw_text(MARGIN_X, SCREEN_H - MARGIN_Y - CHAR_H,
                  "Please wait...", COL_DIM);
    ui_flip();
    return 0;
}

/* ── Message overlay ─────────────────────────────────────────────────────── */
/* 
*   ui_message_nowait("SaveSyncX", "Initialize network...");
*
*   ui_message_timeout("SaveSyncX", "Connecting...", 1500);
*
*   ui_message("SaveSyncX", "Done");
*/

/* center message */
static int text_width(const char *s)
{
    return strlen(s) * CHAR_W;
}

static int center_x(const char *s)
{
    return (SCREEN_W - text_width(s)) / 2;
}

void ui_message(const char *title, const char *body)
{
    /* Eerst aantal regels tellen */
    int line_count = 1;
    for (const char *p = body; *p; p++) {
        if (*p == '\n') line_count++;
    }

    /* Buffer kopiëren want we gaan strtok gebruiken */
    char body_copy[512];
    strncpy(body_copy, body, sizeof(body_copy) - 1);
    body_copy[sizeof(body_copy) - 1] = '\0';

    /* Verzamel regels in array */
    char *lines[32];
    int idx = 0;
    char *token = strtok(body_copy, "\n");
    while (token && idx < 32) {
        lines[idx++] = token;
        token = strtok(NULL, "\n");
    }
    line_count = idx; /* actual number */

    while (1) {
        ui_pump_events();

        ui_clear();

        /* Titel gecentreerd */
        ui_draw_text(center_x(title),
                     SCREEN_H / 2 - CHAR_H * (line_count / 2 + 2),
                     title, COL_TITLE);

        /* Blok met regels verticaal centreren */
        int block_height = line_count * CHAR_H;
        int start_y = (SCREEN_H - block_height) / 2;

        for (int i = 0; i < line_count; i++) {
            ui_draw_text(center_x(lines[i]),
                         start_y + i * CHAR_H,
                         lines[i], COL_ITEM);
        }

        /* Voettekst */
        ui_draw_text(center_x("Press A to continue"),
                     SCREEN_H - MARGIN_Y - CHAR_H,
                     "Press A to continue", COL_DIM);

        ui_flip();

        if (btn_pressed(BTN_A) || btn_pressed(BTN_B) || btn_pressed(BTN_START))
            break;

        SDL_Delay(16);
    }
}
void ui_message_nowait(const char *title, const char *body)
{
    ui_pump_events();
    ui_clear();

    ui_draw_text(center_x(title),
                 SCREEN_H / 2 - CHAR_H * 2,
                 title, COL_TITLE);

    ui_draw_text(center_x(body),
                 SCREEN_H / 2,
                 body, COL_ITEM);

    ui_flip();
}
void ui_message_timeout(const char *title, const char *body, int ms)
{
    uint32_t start = SDL_GetTicks();

    while (1) {
        ui_pump_events();
        ui_clear();

        ui_draw_text(center_x(title),
                     SCREEN_H / 2 - CHAR_H * 2,
                     title, COL_TITLE);

        ui_draw_text(center_x(body),
                     SCREEN_H / 2,
                     body, COL_ITEM);

        if (SDL_GetTicks() - start >= (uint32_t)ms) {
            break;
        }

        ui_flip();
        SDL_Delay(16);
    }
}
/* ── Settings screen ─────────────────────────────────────────────────────── */
/*
 * Very minimal: cycles through fields with D-Pad Up/Down.
 * For string fields it shows the current value and lets the user
 * edit it char-by-char with Left/Right (cycle A-Z,0-9,...).
 * For integer fields Left/Right decrement/increment.
 * A = save & exit, B = cancel.
 *
 * A proper on-screen keyboard would be nicer but this keeps code compact.
 */

extern AppConfig g_cfg_edit;  /* forward – see bottom of this file */

/* Field descriptors */
typedef enum { FT_STR, FT_INT, FT_BOOL } FieldType;
typedef struct {
    const char *label;
    FieldType   type;
    int         max_len;
    char       *str_val;
    int        *int_val;
} Field;

int ui_settings_screen(void)
{
    /* We need access to the AppConfig – passed in via a small trick:
       main.c calls config_load into a local cfg, then passes a pointer
       to ui_settings_screen which modifies it in-place.
       For simplicity here we use a file-scope pointer set before the call.
       See ui_set_cfg_ptr() below. */
    extern AppConfig *g_settings_cfg;
    if (!g_settings_cfg) return -1;
    AppConfig *cfg = g_settings_cfg;
    
    Field fields[] = {
        { "Host      :", FT_STR,  sizeof(cfg->host)-1,        cfg->host,        NULL },
        { "Port      :", FT_INT,  0,                           NULL,             &cfg->port },
        { "Username  :", FT_STR,  sizeof(cfg->username)-1,    cfg->username,    NULL },
        { "Password  :", FT_STR,  sizeof(cfg->password)-1,    cfg->password,    NULL },
        { "RemotePath:", FT_STR,  sizeof(cfg->remote_base)-1, cfg->remote_base, NULL },
    };
    int nfields = (int)(sizeof(fields) / sizeof(fields[0]));
    int sel = 0;

    /* For string editing: cursor position per field */
    int cursors[5] = {0};
    for (int i = 0; i < nfields; i++)
        if (fields[i].type == FT_STR)
            cursors[i] = (int)strlen(fields[i].str_val);

    static const char *CHARSET =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789"
        "!@#$%^&*()_+-=[]{}|;':\",./<>? \\/";

    while (1) {
        ui_pump_events();

        Field *f = &fields[sel];

        if (btn_pressed(DPAD_DOWN)) sel = (sel + 1) % nfields;
        if (btn_pressed(DPAD_UP))   sel = (sel + nfields - 1) % nfields;

        if (f->type == FT_INT || f->type == FT_BOOL) {
            if (btn_pressed(DPAD_RIGHT)) (*f->int_val)++;
            if (btn_pressed(DPAD_LEFT))  (*f->int_val)--;
            if (f->type == FT_BOOL) {
                if (*f->int_val < 0) *f->int_val = 1;
                if (*f->int_val > 1) *f->int_val = 0;
            }
        } else {
            /* String: Left moves cursor back / cycles char up,
                       Right moves cursor forward / cycles char down */
            int cur = cursors[sel];
            int slen = (int)strlen(f->str_val);
            int csz = (int)strlen(CHARSET);

            if (btn_pressed(BTN_X) && cur > 0) {
                /* Backspace */
                memmove(f->str_val + cur - 1,
                        f->str_val + cur, slen - cur + 1);
                cursors[sel]--;
            }
            /* Cycle current char with Y/B (up/down charset) */
            if (btn_pressed(BTN_Y)) {
                /* insert next char in charset at cursor */
                if (slen < f->max_len) {
                    memmove(f->str_val + cur + 1,
                            f->str_val + cur, slen - cur + 1);
                    f->str_val[cur] = CHARSET[0];
                    cursors[sel]++;
                }
            }
            if (btn_pressed(DPAD_LEFT) && cur > 0)
                cursors[sel]--;
            if (btn_pressed(DPAD_RIGHT) && cur < slen)
                cursors[sel]++;
        }

        if (btn_pressed(BTN_A) || btn_pressed(BTN_START)) return 0;
        if (btn_pressed(BTN_B)) return -1;

        /* ─ Draw ─ */
        ui_clear();
        ui_draw_text(MARGIN_X, MARGIN_Y, "  Settings", COL_TITLE);
        ui_draw_text(MARGIN_X, MARGIN_Y + CHAR_H,
                  "  D-Pad=Navigate  X=Backspace  Y=Insert  A=Save  B=Cancel",
                  COL_DIM);
        ui_fill_rect(MARGIN_X, MARGIN_Y + CHAR_H * 2 + 4,
                  SCREEN_W - MARGIN_X * 2, 1, COL_DIM);

        for (int i = 0; i < nfields; i++) {
            int y = MARGIN_Y + CHAR_H * (4 + i * 2);
            Uint32 col = (i == sel) ? COL_SELECT : COL_ITEM;
            if (i == sel)
                ui_fill_rect(MARGIN_X - 4, y - 2,
                          SCREEN_W - MARGIN_X * 2 + 8, CHAR_H + 4, 0xFF001428);

            char row[128];
            Field *rf = &fields[i];
            if (rf->type == FT_STR) {
                snprintf(row, sizeof(row), "%s %s", rf->label, rf->str_val);
                /* draw cursor */
                if (i == sel) {
                    int cx = MARGIN_X + (12 + 1 + cursors[i]) * CHAR_W;
                    ui_fill_rect(cx, y, CHAR_W, CHAR_H, COL_SELECT);
                }
            } else {
                snprintf(row, sizeof(row), "%s %d %s",
                         rf->label, *rf->int_val,
                         rf->type == FT_BOOL
                            ? (*rf->int_val ? "(on)" : "(off)") : "");
            }
            ui_draw_text(MARGIN_X, y, row, col);
        }
        ui_flip();
        SDL_Delay(16);
    }
}

/* Pointer setter called from main.c before opening settings */
static AppConfig *g_settings_cfg_storage = NULL;
AppConfig *g_settings_cfg = NULL;

void ui_set_cfg_ptr(AppConfig *cfg)
{
    g_settings_cfg = cfg;
    g_settings_cfg_storage = cfg;
}

/* ── Credits screen ───────────────────────────────────────────────────────── */
void ui_credits_screen(void)
{
    typedef struct { const char *label; const char *value; } Row;
    static const Row rows[] = {
        { "Author",    "vandoeselaar"                          },
        { "Source",    "github.com/vandoeselaar/SaveSyncX"    },
        { "",          ""                                      },
        { "Built with","---"                                   },
        { "nxdk",      "github.com/XboxDev/nxdk"              },
        { "SDL2",      "via nxdk"                             },
        { "lwIP",      "via nxdk"                             },
        { "BearSSL",   "bearssl.org"                          },
        { "",          ""                                      },
        { "References","---"                                   },
        { "Re-signing","feudalnate's resigner, XSavSig005"    },
        { "Game data", "consolemods.org wiki"                 },
        { "",          ""                                      },
        { "License",   "MIT"                                  },
    };
    static const int NROWS = (int)(sizeof(rows) / sizeof(rows[0]));

    while (1) {
        ui_pump_events();
        if (btn_pressed(BTN_B) || btn_pressed(BTN_START)) return;

        ui_clear();

        ui_draw_text(MARGIN_X, MARGIN_Y,
                     "  SaveSyncX  v1.2  --  Credits", COL_TITLE);
        ui_fill_rect(MARGIN_X, MARGIN_Y + CHAR_H + 4,
                     SCREEN_W - MARGIN_X * 2, 1, COL_DIM);

        int y = MARGIN_Y + CHAR_H * 3;
        for (int i = 0; i < NROWS; i++, y += CHAR_H + 2) {
            if (rows[i].label[0] == '\0') continue;

            if (strcmp(rows[i].value, "---") == 0) {
                /* section header */
                ui_draw_text(MARGIN_X, y, rows[i].label, COL_DIM);
                continue;
            }

            char line[128];
            snprintf(line, sizeof(line), "%-12s %s",
                     rows[i].label, rows[i].value);
            ui_draw_text(MARGIN_X, y, line, COL_ITEM);
        }

        /* hint bar */
        ui_draw_text(MARGIN_X, SCREEN_H - MARGIN_Y - CHAR_H,
                     "[B] Back", COL_DIM);
        ui_draw_text(SCREEN_W - MARGIN_X - 14 * CHAR_W,
                     SCREEN_H - MARGIN_Y - CHAR_H,
                     "SaveSyncX  v1.2", COL_DIM);

        ui_flip();
        SDL_Delay(16);
    }
}
