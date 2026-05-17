#include <curses.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <stdbool.h>
#include <stdarg.h>

#include "sudoku_types.h"
#include "generate.h"
#include "bot.h"

/* ===== GLOBALS ===== */

Game        g_game;
AppState    g_state    = STATE_MENU;
RecordNode *g_records  = NULL;
FILE       *g_log      = NULL;
BotState    g_bot;
int         g_seed_mask   = 0;
bool        g_seed_cells[3][3];
int         g_sel_diff    = 0;
int         g_menu_sel    = 0;
int         g_cur_r       = 0;
int         g_cur_c       = 0;
int         g_opt_sel     = 0;   /* cursor inside Options screen */
int         g_scale       = 100; /* window scale %: 50 / 75 / 100 / 125 / 150 */

#define RECORDS_FILE "sudoku_records.dat"
#define LOG_FILE     "sudoku.log"

/* ===== LOGGING ===== */

void log_msg(LogLevel lvl, const char *fmt, ...) {
    if (!g_log) return;
    const char *lvl_str[] = {"INFO", "DEBUG", "ERROR"};
    time_t t = time(NULL);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(g_log, "[%s][%s] ", tbuf, lvl_str[lvl]);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fprintf(g_log, "\n");
    fflush(g_log);
}

/* ===== RECORDS ===== */

static void records_push(Record r) {
    RecordNode *node = malloc(sizeof(RecordNode));
    node->data = r;
    node->next = g_records;
    g_records  = node;
    log_msg(LOG_INFO, "Record added: %s %ds diff=%d", r.name, r.seconds, r.difficulty);
}

static void records_load(void) {
    FILE *f = fopen(RECORDS_FILE, "rb");
    if (!f) { log_msg(LOG_INFO, "No records file found, starting fresh."); return; }
    Record r;
    while (fread(&r, sizeof(Record), 1, f) == 1) records_push(r);
    fclose(f);
    log_msg(LOG_INFO, "Records loaded.");
}

static void records_save(void) {
    FILE *f = fopen(RECORDS_FILE, "wb");
    if (!f) { log_msg(LOG_ERROR, "Cannot save records."); return; }
    RecordNode *n = g_records;
    while (n) { fwrite(&n->data, sizeof(Record), 1, f); n = n->next; }
    fclose(f);
    log_msg(LOG_INFO, "Records saved.");
}

static void records_free(void) {
    while (g_records) {
        RecordNode *tmp = g_records;
        g_records = g_records->next;
        free(tmp);
    }
}

/* ===== COLORS ===== */

#define COLOR_GRID     1
#define COLOR_GIVEN    2
#define COLOR_PLAYER   3
#define COLOR_ERROR    4
#define COLOR_SELECTED 5
#define COLOR_BOT      6
#define COLOR_TITLE    7
#define COLOR_BUTTON   8
/* Fire color pairs 9..14 (fg on black bg) */
#define COLOR_FIRE_BASE 9
#define FIRE_LEVELS     6

/* ===== FIRE SIMULATION ===== */

/*
 * FIRE_COLS  : width of each side strip in terminal columns.
 * FIRE_ROWS  : max height the simulation tracks (more than screen height is fine).
 * FIRE_MAX   : maximum heat value (15 = hottest).
 *
 * Left strip  (g_fire_l): wind bias +1 (flames lean right, i.e. inward).
 * Right strip (g_fire_r): wind bias -1 (flames lean left,  i.e. inward).
 * This makes both fires appear to billow toward the centre of the screen.
 */
#define FIRE_COLS  35
#define FIRE_ROWS  120
#define FIRE_MAX   12

/* Fixed base window dimensions (scaled by g_scale in apply_scale / main). */
#define BASE_COLS 120
#define BASE_ROWS  35

static int g_fire_l[FIRE_ROWS][FIRE_COLS];
static int g_fire_r[FIRE_ROWS][FIRE_COLS];

static unsigned int g_fire_rng = 12345u;
static inline int fire_rand(void) {
    g_fire_rng = g_fire_rng * 1664525u + 1013904223u;
    return (int)((g_fire_rng >> 16) & 0x7FFF);
}

static void fire_init(void) {
    for (int c = 0; c < FIRE_COLS; c++) {
        g_fire_l[FIRE_ROWS-1][c] = FIRE_MAX;
        g_fire_r[FIRE_ROWS-1][c] = FIRE_MAX;
    }
}

/*
 * wind_bias: +1 = lean right (left strip), -1 = lean left (right strip).
 * Decay is 0 or 1 only — keeps flames tall and fast.
 */
static void fire_step_buf(int buf[FIRE_ROWS][FIRE_COLS], int wind_bias) {
    /* Re-ignite bottom row with full heat, slight flicker */
    for (int c = 0; c < FIRE_COLS; c++) {
        int v = FIRE_MAX - (fire_rand() % 2);
        buf[FIRE_ROWS-1][c] = (v < 0) ? 0 : v;
    }
    /* Propagate upward with wind bias */
    for (int r = 0; r < FIRE_ROWS-1; r++) {
        for (int c = 0; c < FIRE_COLS; c++) {
            /* Biased horizontal sample: 0, bias, or 2*bias clamped */
            int rn = fire_rand() % 3;   /* 0, 1, 2 */
            int dc = (rn == 0) ? 0 : (rn == 1) ? wind_bias : wind_bias * 2;
            int sc = c + dc;
            if (sc < 0) sc = 0;
            if (sc >= FIRE_COLS) sc = FIRE_COLS-1;
            int heat  = buf[r+1][sc];
            int decay = fire_rand() % 2; /* 0 or 1 — gentle decay = taller fire */
            heat -= decay;
            buf[r][c] = (heat < 0) ? 0 : heat;
        }
    }
}

static void fire_step(void) {
    fire_step_buf(g_fire_l, +1); /* left strip: lean right (inward) */
    fire_step_buf(g_fire_r, -1); /* right strip: lean left (inward) */
}

/*
 * Render one fire strip at screen column screen_x.
 * safe_top: rows above this y are NOT drawn (protects hint line and content).
 * mirror:   if true, render columns right-to-left (right strip reads outward).
 *
 * Heat -> glyph + colour:
 *   1-3   '.' dim red
 *   4-6   '*' red bold
 *   7-9   '*' bright-red bold
 *  10-11  '#' yellow bold
 *  12-13  '#' bright-yellow bold
 *  14+    '@' white bold  (core)
 */
static void draw_fire_buf(int buf[FIRE_ROWS][FIRE_COLS],
                          int screen_x, int rows, int safe_top, bool mirror) {
    /* Bottom of fire aligns with rows-2 (row rows-1 is the hint line) */
    int base_y = rows - 2;
    for (int r = FIRE_ROWS-1; r >= 0; r--) {
        int sy = base_y - (FIRE_ROWS-1 - r);
        if (sy < safe_top || sy < 0 || sy >= rows) continue;
        for (int c = 0; c < FIRE_COLS; c++) {
            int heat = buf[r][c];
            if (heat <= 0) continue;
            int pair;
            chtype glyph;
            if      (heat <= 3)  { pair = COLOR_FIRE_BASE+0; glyph = '.'; }
            else if (heat <= 6)  { pair = COLOR_FIRE_BASE+1; glyph = '*'; }
            else if (heat <= 9)  { pair = COLOR_FIRE_BASE+2; glyph = '*'; }
            else if (heat <= 11) { pair = COLOR_FIRE_BASE+3; glyph = '#'; }
            else if (heat <= 13) { pair = COLOR_FIRE_BASE+4; glyph = '#'; }
            else                 { pair = COLOR_FIRE_BASE+5; glyph = '@'; }
            int attr = COLOR_PAIR(pair) | A_BOLD;
            attron(attr);
            /* mirror: right strip columns are reversed so col 0 is outermost */
            int sc = mirror ? (FIRE_COLS-1 - c) : c;
            mvaddch(sy, screen_x + sc, glyph);
            attroff(attr);
        }
    }
}

static void init_colors(void) {
    start_color();
    use_default_colors();
    init_pair(COLOR_GRID,     COLOR_WHITE,   COLOR_BLACK);
    init_pair(COLOR_GIVEN,    COLOR_CYAN,    COLOR_BLACK);
    init_pair(COLOR_PLAYER,   COLOR_GREEN,   COLOR_BLACK);
    init_pair(COLOR_ERROR,    COLOR_RED,     COLOR_BLACK);
    init_pair(COLOR_SELECTED, COLOR_BLACK,   COLOR_YELLOW);
    init_pair(COLOR_BOT,      COLOR_MAGENTA, COLOR_BLACK);
    init_pair(COLOR_TITLE,    COLOR_YELLOW,  COLOR_BLACK);
    init_pair(COLOR_BUTTON,   COLOR_BLACK,   COLOR_WHITE);
    /* Fire pairs */
    init_pair(COLOR_FIRE_BASE+0, COLOR_RED,    COLOR_BLACK);
    init_pair(COLOR_FIRE_BASE+1, COLOR_RED,    COLOR_BLACK);
    init_pair(COLOR_FIRE_BASE+2, COLOR_RED,    COLOR_BLACK);
    init_pair(COLOR_FIRE_BASE+3, COLOR_YELLOW, COLOR_BLACK);
    init_pair(COLOR_FIRE_BASE+4, COLOR_YELLOW, COLOR_BLACK);
    init_pair(COLOR_FIRE_BASE+5, COLOR_WHITE,  COLOR_BLACK);
}

/* ===== BOX DRAWING ===== */

static void draw_box_title(int y, int x, int h, int w, const char *title) {
    attron(COLOR_PAIR(COLOR_GRID));
    for (int i = 1; i < w-1; i++) {
        mvaddch(y,   x+i, ACS_HLINE);
        mvaddch(y+h, x+i, ACS_HLINE);
    }
    for (int i = 1; i < h; i++) {
        mvaddch(y+i, x,   ACS_VLINE);
        mvaddch(y+i, x+w, ACS_VLINE);
    }
    mvaddch(y,   x,   ACS_ULCORNER);
    mvaddch(y,   x+w, ACS_URCORNER);
    mvaddch(y+h, x,   ACS_LLCORNER);
    mvaddch(y+h, x+w, ACS_LRCORNER);
    attroff(COLOR_PAIR(COLOR_GRID));
    if (title) {
        attron(COLOR_PAIR(COLOR_TITLE) | A_BOLD);
        mvprintw(y, x + (w - (int)strlen(title))/2, "%s", title);
        attroff(COLOR_PAIR(COLOR_TITLE) | A_BOLD);
    }
}

/* ===== MAIN MENU ===== */

static void draw_menu(void) {
    clear();
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    /* Fire strips: each FIRE_COLS wide, capped above the hint line.
     * safe_top keeps flames out of the logo+menu area (upper half).
     * Left strip leans right (mirror=false), right strip leans left (mirror=true). */
    int fire_safe = rows / 2 - 6;  /* flames only in lower half */
    if (fire_safe < 1) fire_safe = 1;
    draw_fire_buf(g_fire_l, 0,                rows, fire_safe, false);
    draw_fire_buf(g_fire_r, cols - FIRE_COLS, rows, fire_safe, true);

    const char *logo[] = {
        "  ____  _   _ ____   ___  _  ___  _   _ ",
        " / ___|| | | |  _ \\ / _ \\| |/ / || | | |",
        " \\___ \\| | | | | | | | | | ' /| || |_| |",
        "  ___) | |_| | |_| | |_| | . \\|__   _|",
        " |____/ \\___/|____/ \\___/|_|\\_\\  |_|  "
    };
    int logo_h = 5, logo_w = 42;
    int ly = rows/2 - 8;
    int lx = (cols - logo_w) / 2;
    attron(COLOR_PAIR(COLOR_TITLE) | A_BOLD);
    for (int i = 0; i < logo_h; i++)
        mvprintw(ly+i, lx, "%s", logo[i]);
    attroff(COLOR_PAIR(COLOR_TITLE) | A_BOLD);

    const char *items[] = {
        "  New game  ",
        "  Bot watch  ",
        "  Records  ",
        "  Options  ",
        "  About  ",
        "  FAQ  ",
        "  QUIT  "
    };
    int n = 7;
    int start_y = ly + logo_h + 2;
    for (int i = 0; i < n; i++) {
        int ix = (cols - (int)strlen(items[i])) / 2;
        if (i == g_menu_sel) {
            attron(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
            mvprintw(start_y + i*2, ix, "%s", items[i]);
            attroff(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
        } else {
            attron(COLOR_PAIR(COLOR_GRID));
            mvprintw(start_y + i*2, ix, "%s", items[i]);
            attroff(COLOR_PAIR(COLOR_GRID));
        }
    }
    mvprintw(rows-1, 2, "Arrows: select   Enter: confirm   q: exit");
    refresh();
}

/* ===== OPTIONS SCREEN ===== */

/*
 * Currently one option: Generator algorithm.
 * Left/Right arrows cycle through GEN_* values for the highlighted row.
 * Esc / q returns to the menu.
 */
static void draw_options(void) {
    clear();
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int bw = 58, bh = 14;
    int bx = (cols - bw) / 2, by = (rows - bh) / 2;
    draw_box_title(by, bx, bh, bw, " Options ");

    /* Row 0: Generator */
    int ry = by + 3;
    if (g_opt_sel == 0) attron(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
    else                attron(COLOR_PAIR(COLOR_GRID));
    mvprintw(ry, bx+3, "Generator:   < %-22s >",
             generator_name(g_game.generator));
    if (g_opt_sel == 0) attroff(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
    else                attroff(COLOR_PAIR(COLOR_GRID));

    const char *desc[GEN_COUNT] = {
        "LCG seed + Fisher-Yates shuffle (default)",
        "Xorshift32 PRNG + Fisher-Yates shuffle",
        "Quadratic-residue slot permutation",
        "Fibonacci (golden-ratio) hash permutation"
    };
    attron(COLOR_PAIR(COLOR_GRID));
    mvprintw(ry + 1, bx+5, "%s", desc[g_game.generator]);
    attroff(COLOR_PAIR(COLOR_GRID));

    /* Row 1: Scale */
    int sy = ry + 4;
    if (g_opt_sel == 1) attron(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
    else                attron(COLOR_PAIR(COLOR_GRID));
    mvprintw(sy, bx+3, "Window scale: < %3d%% >  [50/75/100/125/150]",
             g_scale);
    if (g_opt_sel == 1) attroff(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
    else                attroff(COLOR_PAIR(COLOR_GRID));

    attron(COLOR_PAIR(COLOR_GRID));
    mvprintw(sy + 1, bx+5,
             "Takes effect on next launch (resize_term applied now)");
    attroff(COLOR_PAIR(COLOR_GRID));

    attron(COLOR_PAIR(COLOR_GRID));
    mvprintw(by + bh - 2, bx+3,
             "Up/Down: row   Left/Right: change   Esc/q: back");
    attroff(COLOR_PAIR(COLOR_GRID));

    refresh();
}

/* ===== SEED SELECTION SCREEN ===== */

static void draw_seed_screen(void) {
    clear();
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int bx = (cols - 54) / 2, by = (rows - 22) / 2;
    draw_box_title(by, bx, 22, 54, " Generation settings ");

    /* Generator info line */
    attron(COLOR_PAIR(COLOR_BOT));
    mvprintw(by+2, bx+2, "Generator: %s", generator_name(g_game.generator));
    attroff(COLOR_PAIR(COLOR_BOT));

    mvprintw(by+4, bx+2, "1. Mark cells in 3x3 grid (Space to toggle):");
    mvprintw(by+5, bx+4, "   (defines the generation seed)");

    int gy = by+7, gx = bx+17;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            int y = gy + r*2, x = gx + c*4;
            if (r == g_cur_r && c == g_cur_c)
                attron(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
            else if (g_seed_cells[r][c])
                attron(COLOR_PAIR(COLOR_PLAYER) | A_BOLD);
            else
                attron(COLOR_PAIR(COLOR_GRID));

            mvprintw(y, x, "[%s]", g_seed_cells[r][c] ? "#" : " ");

            if (r == g_cur_r && c == g_cur_c) attroff(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
            else if (g_seed_cells[r][c]) attroff(COLOR_PAIR(COLOR_PLAYER) | A_BOLD);
            else attroff(COLOR_PAIR(COLOR_GRID));
        }
    }

    int mask = 0;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            if (g_seed_cells[r][c])
                mask |= (1 << (r*3+c));
    mvprintw(gy+7, bx+2, "Seed: %d (0x%03X)%s",
             mask, mask, (mask == 0) ? "  [random]" : "");

    const char *dnames[] = {"Easy (30 removed)", "Medium (45 removed)", "Hard (55 removed)"};
    mvprintw(by+16, bx+2, "2. Difficulty:");
    for (int i = 0; i < 3; i++) {
        if (i == g_sel_diff) {
            attron(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
            mvprintw(by+17+i, bx+4, "> %s", dnames[i]);
            attroff(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
        } else {
            mvprintw(by+17+i, bx+4, "  %s", dnames[i]);
        }
    }

    mvprintw(rows-2, 2,
        "Arrows: navigate   Space: toggle   Tab: difficulty   Enter: start   Esc: menu");
    refresh();
}

/* ===== GAME BOARD ===== */

#define GY 2
#define GX 4
#define CW 4
#define CH 2

static void draw_game(bool bot_mode) {
    clear();
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    (void)cols;

    const char *diff_names[] = {"Easy", "Medium", "Hard"};
    int di = (g_game.difficulty == DIFF_EASY) ? 0 :
             (g_game.difficulty == DIFF_MEDIUM) ? 1 : 2;
    attron(COLOR_PAIR(COLOR_TITLE) | A_BOLD);
    mvprintw(0, GX, "SUDOKU  [%s]  Seed: %d  Time: %02d:%02d  Gen: %s",
             diff_names[di], g_seed_mask,
             g_game.elapsed/60, g_game.elapsed%60,
             generator_name(g_game.generator));
    attroff(COLOR_PAIR(COLOR_TITLE) | A_BOLD);

    /* Grid lines */
    for (int i = 0; i <= 9; i++) {
        int x = GX + i * CW;
        for (int j = 0; j <= 9 * CH; j++)
            mvaddch(GY + j, x, (i % 3 == 0) ? ACS_VLINE : '|');
    }
    for (int i = 0; i <= 9; i++) {
        int y = GY + i * CH;
        bool thick = (i % 3 == 0);
        for (int j = 0; j <= 9 * CW; j++) {
            int x = GX + j;
            chtype cur = mvinch(y, x);
            if ((cur & A_CHARTEXT) == (ACS_VLINE & A_CHARTEXT))
                mvaddch(y, x, ACS_PLUS);
            else
                mvaddch(y, x, thick ? ACS_HLINE : '-');
        }
    }

    /* Cells */
    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 9; c++) {
            int y = GY + r * CH + 1;
            int x = GX + c * CW + 2;
            Cell *cell = &g_game.board[r][c];
            bool selected = !bot_mode && (r == g_cur_r && c == g_cur_c);

            if (selected)                        attron(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
            else if (cell->error)                attron(COLOR_PAIR(COLOR_ERROR)    | A_BOLD);
            else if (cell->type == CELL_GIVEN)   attron(COLOR_PAIR(COLOR_GIVEN)   | A_BOLD);
            else if (cell->type == CELL_BOT)     attron(COLOR_PAIR(COLOR_BOT));
            else if (cell->type == CELL_PLAYER)  attron(COLOR_PAIR(COLOR_PLAYER));
            else                                 attron(COLOR_PAIR(COLOR_GRID));

            mvaddch(y, x, (cell->value != 0) ? ('0' + cell->value) : '.');

            attroff(A_BOLD | A_REVERSE
                    | COLOR_PAIR(COLOR_SELECTED) | COLOR_PAIR(COLOR_ERROR)
                    | COLOR_PAIR(COLOR_GIVEN)    | COLOR_PAIR(COLOR_PLAYER)
                    | COLOR_PAIR(COLOR_BOT)      | COLOR_PAIR(COLOR_GRID));
        }
    }

    /* Legend */
    int lx = GX + 9*CW + 4;
    attron(COLOR_PAIR(COLOR_GIVEN) | A_BOLD);
    mvprintw(GY+1, lx, "# Given");
    attroff(COLOR_PAIR(COLOR_GIVEN) | A_BOLD);
    attron(COLOR_PAIR(COLOR_PLAYER));
    mvprintw(GY+3, lx, "# Yours");
    attroff(COLOR_PAIR(COLOR_PLAYER));
    attron(COLOR_PAIR(COLOR_ERROR) | A_BOLD);
    mvprintw(GY+5, lx, "# Error");
    attroff(COLOR_PAIR(COLOR_ERROR) | A_BOLD);
    if (bot_mode) {
        attron(COLOR_PAIR(COLOR_BOT));
        mvprintw(GY+7, lx, "# Bot");
        attroff(COLOR_PAIR(COLOR_BOT));
        attron(COLOR_PAIR(COLOR_GRID));
        mvprintw(GY+9, lx, "depth:%d", g_bot.stack_top);
        attroff(COLOR_PAIR(COLOR_GRID));
    }
    if (g_game.solved) {
        attron(COLOR_PAIR(COLOR_TITLE) | A_BOLD);
        mvprintw(GY+11, lx, "* Solved!");
        attroff(COLOR_PAIR(COLOR_TITLE) | A_BOLD);
    }

    if (bot_mode)
        mvprintw(rows-1, 2, "q: stop watching bot");
    else
        mvprintw(rows-1, 2, "Arrows: move   1-9: enter   Del/0: erase   q: menu");
    refresh();
}

/* ===== RECORDS SCREEN ===== */

static void draw_records(void) {
    clear();
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    draw_box_title(1, 2, rows-3, cols-4, " Records ");

    const char *dnames[] = {"Easy", "Medium", "Hard"};
    mvprintw(3, 4, "%-20s %-10s %-10s %-20s", "Name", "Time", "Difficulty", "Date");
    mvhline(4, 4, ACS_HLINE, cols-8);

    int row = 5;
    RecordNode *n = g_records;
    while (n && row < rows-3) {
        char tstr[16];
        snprintf(tstr, sizeof(tstr), "%02d:%02d", n->data.seconds/60, n->data.seconds%60);
        mvprintw(row++, 4, "%-20s %-10s %-10s %-20s",
                 n->data.name, tstr,
                 (n->data.difficulty < 3) ? dnames[n->data.difficulty] : "?",
                 n->data.date);
        n = n->next;
    }
    if (!g_records) mvprintw(6, 4, "(no records yet)");
    mvprintw(rows-2, 4, "Esc / q: back");
    refresh();
}

/* ===== ABOUT SCREEN ===== */

static void draw_about(void) {
    clear();
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int bx = (cols-60)/2, by = (rows-18)/2;
    draw_box_title(by, bx, 18, 60, " ABOUT ");

    const char *lines[] = {
        "",
        "  Game: SUDOKU (console)",
        "",
        "  Authors: George B.K., Leonid L.A.",
        "  Group: 5131001/50603",
        "  Year: 2026",
        "",
        "  University: SPbPU",
        "       St. Petersburg Polytechnic",
        "",
        "  Esc / q: back",
        "",
    };
    for (int i = 0; i < 12; i++)
        mvprintw(by+1+i, bx+1, "%s", lines[i]);
    refresh();
}

/* ===== HELP SCREEN ===== */

static void draw_help(void) {
    clear();
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int bx = (cols-64)/2, by = 1;
    int bh = rows-3;
    draw_box_title(by, bx, bh, 64, " FAQ ");

    const char *lines[] = {
        "",
        " MAIN MENU:",
        "   Arrows Up/Down       - select menu item",
        "   Enter                - confirm",
        "   q                    - exit",
        "",
        " OPTIONS:",
        "   Arrows Left/Right    - cycle generator algorithm",
        "   Esc / q              - back to menu",
        "",
        " SEED SELECTION:",
        "   Arrows               - move in 3x3 grid",
        "   Space                - toggle seed cell",
        "   Tab                  - change difficulty",
        "   Enter                - generate and start game",
        "   Esc                  - return to menu",
        "",
        " GAME BOARD:",
        "   Arrows               - move cursor",
        "   1-9                  - enter digit",
        "   0 / Del / Backspace  - erase digit",
        "   q                    - return to menu",
        "",
        " BOT WATCHING:",
        "   Bot solves the puzzle step by step (backtracking)",
        "   q                    - stop watching",
        "",
        " SEED:",
        "   Mark cells in the 3x3 grid to set a 9-bit seed.",
        "   Same seed + difficulty + generator => same puzzle.",
        "   Empty grid => random seed based on current time.",
        "",
        " FILES:",
        "   sudoku_records.dat   - saved records",
        "   sudoku.log           - debug log",
        "",
        " Esc / q: back",
    };
    int max_lines = bh - 1;
    for (int i = 0; i < (int)(sizeof(lines)/sizeof(lines[0])) && i < max_lines; i++)
        mvprintw(by+1+i, bx+1, "%s", lines[i]);
    refresh();
}

/* ===== NAME INPUT ===== */

static void input_name(void) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    echo();
    curs_set(1);
    attron(COLOR_PAIR(COLOR_TITLE) | A_BOLD);
    mvprintw(rows/2, (cols-40)/2, "Enter your name for records: ");
    attroff(COLOR_PAIR(COLOR_TITLE) | A_BOLD);
    refresh();
    getnstr(g_game.player_name, 31);
    noecho();
    curs_set(0);
}

/* ===== ENTER KEY MACRO ===== */

#define IS_ENTER(ch) ((ch) == '\n' || (ch) == '\r' || (ch) == KEY_ENTER)

/* ===== STATE HANDLERS ===== */

static void handle_menu(int ch) {
    int n = 7;
    if (ch == KEY_UP)   g_menu_sel = (g_menu_sel + n - 1) % n;
    if (ch == KEY_DOWN) g_menu_sel = (g_menu_sel + 1) % n;
    if (IS_ENTER(ch)) {
        switch (g_menu_sel) {
            case 0: /* New game */
                g_cur_r = 0; g_cur_c = 0;
                memset(g_seed_cells, 0, sizeof(g_seed_cells));
                memset(g_game.player_name, 0, sizeof(g_game.player_name));
                g_state = STATE_SEED_SELECT;
                break;
            case 1: /* Bot watch */
                g_cur_r = 0; g_cur_c = 0;
                memset(g_seed_cells, 0, sizeof(g_seed_cells));
                strcpy(g_game.player_name, "__BOT__");
                g_state = STATE_SEED_SELECT;
                break;
            case 2: g_state = STATE_RECORDS; break;
            case 3: g_state = STATE_OPTIONS; g_opt_sel = 0; break;
            case 4: g_state = STATE_ABOUT;   break;
            case 5: g_state = STATE_HELP;    break;
            case 6: g_state = STATE_QUIT;    break;
        }
    }
}

/* Scale steps: 50, 75, 100, 125, 150 */
static const int SCALE_STEPS[] = {50, 75, 100, 125, 150};
#define SCALE_N 5

static void apply_scale(void) {
    int want_cols = BASE_COLS * g_scale / 100;
    int want_rows = BASE_ROWS * g_scale / 100;
    if (want_cols < 60)  want_cols = 60;
    if (want_rows < 18)  want_rows = 18;
    resize_term(want_rows, want_cols);
    /* Re-init fire buffers to match new height */
    fire_init();
}

static void handle_options(int ch) {
    if (ch == 27 || ch == 'q' || ch == 'Q') {
        g_state = STATE_MENU;
        return;
    }
    if (ch == KEY_UP)   g_opt_sel = (g_opt_sel + 1) % 2;
    if (ch == KEY_DOWN) g_opt_sel = (g_opt_sel + 1) % 2;

    if (g_opt_sel == 0) {
        /* Generator row */
        if (ch == KEY_LEFT)
            g_game.generator = (GeneratorType)
                ((g_game.generator + GEN_COUNT - 1) % GEN_COUNT);
        if (ch == KEY_RIGHT)
            g_game.generator = (GeneratorType)
                ((g_game.generator + 1) % GEN_COUNT);
    } else {
        /* Scale row: find current step, move left/right */
        int idx = 2; /* default 100% */
        for (int i = 0; i < SCALE_N; i++)
            if (SCALE_STEPS[i] == g_scale) { idx = i; break; }
        if (ch == KEY_LEFT  && idx > 0)          idx--;
        if (ch == KEY_RIGHT && idx < SCALE_N-1)  idx++;
        g_scale = SCALE_STEPS[idx];
        apply_scale();
    }
}

static void handle_seed(int ch) {
    if (ch == 27) { g_state = STATE_MENU; return; }

    if (ch == KEY_UP)    g_cur_r = (g_cur_r > 0) ? g_cur_r-1 : g_cur_r;
    if (ch == KEY_DOWN)  g_cur_r = (g_cur_r < 2) ? g_cur_r+1 : g_cur_r;
    if (ch == KEY_LEFT)  g_cur_c = (g_cur_c > 0) ? g_cur_c-1 : g_cur_c;
    if (ch == KEY_RIGHT) g_cur_c = (g_cur_c < 2) ? g_cur_c+1 : g_cur_c;

    if (ch == ' ')
        g_seed_cells[g_cur_r][g_cur_c] = !g_seed_cells[g_cur_r][g_cur_c];

    if (ch == '\t')
        g_sel_diff = (g_sel_diff + 1) % 3;

    if (IS_ENTER(ch)) {
        Difficulty diffs[] = {DIFF_EASY, DIFF_MEDIUM, DIFF_HARD};
        g_game.difficulty = diffs[g_sel_diff];

        g_seed_mask = 0;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                if (g_seed_cells[r][c])
                    g_seed_mask |= (1 << (r*3+c));
        if (g_seed_mask == 0)
            g_seed_mask = (int)(time(NULL) & 0x1FF);

        srand((unsigned)g_seed_mask);

        generate_sudoku();
        g_cur_r = 0; g_cur_c = 0;

        bool bot_mode = (strcmp(g_game.player_name, "__BOT__") == 0);
        if (bot_mode) {
            bot_init();
            g_state = STATE_BOT_WATCH;
        } else {
            g_state = STATE_PLAYING;
        }
        log_msg(LOG_INFO, "Game started: seed=%d diff=%d gen=%d bot=%d",
                g_seed_mask, (int)g_game.difficulty,
                (int)g_game.generator, bot_mode ? 1 : 0);
    }
}

static void handle_game(int ch) {
    if (ch == 'q' || ch == 'Q') { g_state = STATE_MENU; return; }
    if (ch == KEY_UP)    g_cur_r = (g_cur_r > 0) ? g_cur_r-1 : g_cur_r;
    if (ch == KEY_DOWN)  g_cur_r = (g_cur_r < 8) ? g_cur_r+1 : g_cur_r;
    if (ch == KEY_LEFT)  g_cur_c = (g_cur_c > 0) ? g_cur_c-1 : g_cur_c;
    if (ch == KEY_RIGHT) g_cur_c = (g_cur_c < 8) ? g_cur_c+1 : g_cur_c;

    if (ch >= '1' && ch <= '9') {
        Cell *cell = &g_game.board[g_cur_r][g_cur_c];
        if (cell->type != CELL_GIVEN) {
            cell->value = ch - '0';
            cell->type  = CELL_PLAYER;
            validate_board();
            if (check_solved()) {
                g_game.solved = true;
                Record rec;
                strncpy(rec.name, g_game.player_name, 31);
                rec.name[31] = '\0';
                rec.seconds    = g_game.elapsed;
                rec.difficulty = (g_game.difficulty == DIFF_EASY)  ? 0 :
                                 (g_game.difficulty == DIFF_MEDIUM) ? 1 : 2;
                time_t t = time(NULL);
                strftime(rec.date, sizeof(rec.date), "%Y-%m-%d", localtime(&t));
                if (strlen(rec.name) == 0 || strcmp(rec.name, "__BOT__") == 0) {
                    input_name();
                    strncpy(rec.name, g_game.player_name, 31);
                }
                records_push(rec);
                records_save();
                log_msg(LOG_INFO, "Puzzle solved in %ds", g_game.elapsed);
            }
        }
    }

    if (ch == KEY_BACKSPACE || ch == KEY_DC || ch == '0' ||
        ch == 127 || ch == '\b') {
        Cell *cell = &g_game.board[g_cur_r][g_cur_c];
        if (cell->type == CELL_PLAYER) {
            cell->value = 0;
            cell->type  = CELL_EMPTY;
            validate_board();
        }
    }
}

/* ===== CLI HELP ===== */

static void print_help_cli(const char *prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  --help, -h     Show this help and exit\n");
    printf("  --version      Show version and exit\n");
    printf("\nDescription:\n");
    printf("  Console Sudoku with deterministic seed-based generation.\n");
    printf("  Same seed + same difficulty + same generator => same puzzle.\n");
    printf("\nIn-game controls:\n");
    printf("  Arrows          - navigate\n");
    printf("  1-9             - enter digit\n");
    printf("  Del / 0         - erase digit\n");
    printf("  Tab             - switch difficulty (seed screen)\n");
    printf("  Space           - toggle seed cell\n");
    printf("  Enter           - confirm\n");
    printf("  Esc             - go back\n");
    printf("  q               - exit / return to menu\n");
    printf("\nFiles:\n");
    printf("  %-25s - saved records\n", RECORDS_FILE);
    printf("  %-25s - debug log\n",     LOG_FILE);
}

/* ===== MAIN ===== */

int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help_cli(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("sudoku v0.3  2026\n");
            return 0;
        }
    }

    g_log = fopen(LOG_FILE, "a");
    log_msg(LOG_INFO, "=== sudoku started ===");

    records_load();

    /* Default game settings */
    g_game.generator = GEN_LCG;

    /* Apply window size according to g_scale (BASE_COLS/BASE_ROWS defined at file scope). */
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    nodelay(stdscr, FALSE);
    {
        int want_cols = BASE_COLS * g_scale / 100;
        int want_rows = BASE_ROWS * g_scale / 100;
        if (want_cols < 60)  want_cols = 60;
        if (want_rows < 18)  want_rows = 18;
        resize_term(want_rows, want_cols);
    }

    if (has_colors()) init_colors();

    srand((unsigned)time(NULL));
    memset(g_game.player_name, 0, sizeof(g_game.player_name));
    fire_init();

    time_t last_tick = time(NULL);

    while (g_state != STATE_QUIT) {
        /* Timer update */
        if (g_state == STATE_PLAYING && !g_game.solved) {
            time_t now = time(NULL);
            if (now != last_tick) {
                g_game.elapsed += (int)(now - last_tick);
                last_tick = now;
            }
        }

        /* Draw */
        switch (g_state) {
            case STATE_MENU:        draw_menu();        break;
            case STATE_SEED_SELECT: draw_seed_screen(); break;
            case STATE_PLAYING:     draw_game(false);   break;
            case STATE_BOT_WATCH:   draw_game(true);    break;
            case STATE_RECORDS:     draw_records();     break;
            case STATE_OPTIONS:     draw_options();     break;
            case STATE_ABOUT:       draw_about();       break;
            case STATE_HELP:        draw_help();        break;
            default: break;
        }

        /*
         * Input timeout:
         *   BOT_WATCH  — halfdelay(1) ~20 steps/sec (two bot_step() calls
         *                per tick double effective speed without visual blur).
         *   PLAYING    — halfdelay(10) so the timer updates every second.
         *   STATE_MENU — halfdelay(1) for fire animation.
         *   everything else — blocking getch.
         */
        if (g_state == STATE_BOT_WATCH || g_state == STATE_PLAYING ||
            g_state == STATE_MENU) {
            int delay = (g_state == STATE_BOT_WATCH) ? 1 :
                        (g_state == STATE_PLAYING)   ? 10 : 1;
            halfdelay(delay);
        } else {
            nocbreak();
            cbreak();
        }

        int ch = getch();

        /* Restore blocking mode after a timed getch */
        if (g_state == STATE_BOT_WATCH || g_state == STATE_PLAYING ||
            g_state == STATE_MENU) {
            nocbreak();
            cbreak();
        }

        if (g_state == STATE_BOT_WATCH) {
            if (ch == 'q' || ch == 'Q') { g_state = STATE_MENU; continue; }
            if (ch == ERR || ch == KEY_RESIZE) {
                if (!g_bot.finished) {
                    /* Two steps per timeout tick — doubles animation speed. */
                    for (int _s = 0; _s < 2 && !g_bot.finished; _s++) {
                        bot_step();
                        if (g_bot.finished || check_solved()) {
                            g_game.solved = true;
                            log_msg(LOG_INFO, "Bot finished solving.");
                            /* Redraw once so the player sees the completed board. */
                            draw_game(true);
                            /* Show a prompt and wait for Enter or q. */
                            int rows_tmp, cols_tmp;
                            getmaxyx(stdscr, rows_tmp, cols_tmp);
                            attron(COLOR_PAIR(COLOR_TITLE) | A_BOLD);
                            mvprintw(rows_tmp - 1, 2,
                                     "Solved! Press Enter to return to menu...");
                            attroff(COLOR_PAIR(COLOR_TITLE) | A_BOLD);
                            refresh();
                            nocbreak(); cbreak(); nodelay(stdscr, FALSE);
                            int wait_ch;
                            do {
                                wait_ch = getch();
                            } while (!IS_ENTER(wait_ch) &&
                                     wait_ch != 'q' && wait_ch != 'Q' &&
                                     wait_ch != 27);
                            g_state = STATE_MENU;
                            break;
                        }
                    }
                }
            }
            continue;
        }

        /* ERR from halfdelay timeout: update timer (PLAYING) or advance fire (MENU) */
        if (ch == ERR) {
            if (g_state == STATE_MENU) fire_step();
            continue;
        }

        if ((g_state == STATE_RECORDS || g_state == STATE_ABOUT ||
             g_state == STATE_HELP)
            && (ch == 27 || ch == 'q' || ch == 'Q')) {
            g_state = STATE_MENU;
            continue;
        }

        switch (g_state) {
            case STATE_MENU:        handle_menu(ch);    break;
            case STATE_OPTIONS:     handle_options(ch); break;
            case STATE_SEED_SELECT: handle_seed(ch);    break;
            case STATE_PLAYING:     handle_game(ch);    break;
            default: break;
        }
    }

    endwin();

    records_save();
    records_free();
    log_msg(LOG_INFO, "=== sudoku exited ===");
    if (g_log) fclose(g_log);

    return 0;
}
