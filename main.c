#include <curses.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <stdbool.h>
#include <stdarg.h>

/* ===== TYPES ===== */

typedef enum {
    DIFF_EASY   = 30,
    DIFF_MEDIUM = 45,
    DIFF_HARD   = 55
} Difficulty;

typedef enum {
    STATE_MENU = 0,
    STATE_SEED_SELECT,
    STATE_PLAYING,
    STATE_BOT_WATCH,
    STATE_RECORDS,
    STATE_ABOUT,
    STATE_HELP,
    STATE_QUIT
} AppState;

typedef enum {
    CELL_EMPTY  = 0,
    CELL_GIVEN  = 1,
    CELL_PLAYER = 2,
    CELL_BOT    = 3
} CellType;

typedef enum {
    LOG_INFO  = 0,
    LOG_DEBUG = 1,
    LOG_ERROR = 2
} LogLevel;

typedef struct {
    char name[32];
    int  seconds;
    int  difficulty;
    char date[20];
} Record;

typedef struct RecordNode {
    Record            data;
    struct RecordNode *next;
} RecordNode;

typedef struct {
    int      value;
    CellType type;
    bool     error;
} Cell;

typedef struct {
    Cell       board[9][9];
    Cell       solution[9][9];
    int        elapsed;
    Difficulty difficulty;
    bool       solved;
    char       player_name[32];
} Game;

/*
 * Bot backtracking state:
 *   stack_r / stack_c / stack_v  - per-cell attempt history (up to 81 entries)
 *   stack_top                    - current depth
 *   finished                     - set when no more empty cells or contradiction
 */
typedef struct {
    int  stack_r[81];
    int  stack_c[81];
    int  stack_v[81];   /* value placed at this depth */
    int  stack_try[81]; /* next value to try at this depth (1..9) */
    int  stack_top;
    bool finished;
} BotState;

/* ===== GLOBALS ===== */

static Game        g_game;
static AppState    g_state    = STATE_MENU;
static RecordNode *g_records  = NULL;
static FILE       *g_log      = NULL;
static BotState    g_bot;
static int         g_seed_mask   = 0;
static bool        g_seed_cells[3][3];
static int         g_sel_diff    = 0;
static int         g_menu_sel    = 0;
static int         g_cur_r       = 0;
static int         g_cur_c       = 0;

#define RECORDS_FILE "sudoku_records.dat"
#define LOG_FILE     "sudoku.log"

/* ===== LOGGING ===== */

static void log_msg(LogLevel lvl, const char *fmt, ...) {
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

/* ===== SUDOKU CORE ===== */

static bool is_valid(int board[9][9], int r, int c, int v) {
    for (int i = 0; i < 9; i++) {
        if (board[r][i] == v) return false;
        if (board[i][c] == v) return false;
    }
    int br = (r / 3) * 3, bc = (c / 3) * 3;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            if (board[br+i][bc+j] == v) return false;
    return true;
}

/*
 * Deterministic digit order derived from seed_mask.
 * For each position (0..8) we produce a digit 1..9 by rotating the
 * sequence [1..9] by a shift that depends on seed_mask and the position
 * index, then XOR-scrambling within that rotation.
 *
 * The important property: given the same seed_mask the order is always
 * identical, and different masks produce (mostly) different orders.
 */
static void seed_digit_order(int seed_mask, int pos, int out[9]) {
    /* LCG-style scramble seeded purely by mask + pos */
    unsigned int s = (unsigned int)(seed_mask ^ (pos * 6364136223846793005u + 1442695040888963407u));
    s = s * 1664525u + 1013904223u;
    int shift = (int)(s % 9);

    for (int i = 0; i < 9; i++)
        out[i] = ((i + shift) % 9) + 1;  /* values 1..9, rotated */

    /* Fisher-Yates with the same LCG */
    for (int i = 8; i > 0; i--) {
        s = s * 1664525u + 1013904223u;
        int j = (int)(s % (unsigned int)(i + 1));
        int tmp = out[i]; out[i] = out[j]; out[j] = tmp;
    }
}

/*
 * Fill the board using backtracking.
 * Digit order at each cell is determined by seed_digit_order so that the
 * same seed always yields the same full board.
 */
static bool fill_board_seeded(int board[9][9], int seed_mask, int pos) {
    if (pos == 81) return true;
    int r = pos / 9, c = pos % 9;
    if (board[r][c] != 0) return fill_board_seeded(board, seed_mask, pos + 1);

    int order[9];
    seed_digit_order(seed_mask, pos, order);

    for (int i = 0; i < 9; i++) {
        int v = order[i];
        if (is_valid(board, r, c, v)) {
            board[r][c] = v;
            if (fill_board_seeded(board, seed_mask, pos + 1)) return true;
            board[r][c] = 0;
        }
    }
    return false;
}

/*
 * Count solutions (capped at limit) for uniqueness check.
 * Uses straightforward sequential backtracking (no randomness needed here).
 */
static int count_solutions(int board[9][9], int limit) {
    int r = -1, c = -1;
    for (int i = 0; i < 9 && r == -1; i++)
        for (int j = 0; j < 9 && r == -1; j++)
            if (board[i][j] == 0) { r = i; c = j; }
    if (r == -1) return 1;
    int cnt = 0;
    for (int v = 1; v <= 9 && cnt < limit; v++) {
        if (is_valid(board, r, c, v)) {
            board[r][c] = v;
            cnt += count_solutions(board, limit - cnt);
            board[r][c] = 0;
        }
    }
    return cnt;
}

/*
 * Deterministic cell removal order derived from seed_mask.
 * We fill positions[0..80] with 0..80 then shuffle using the seed.
 */
static void seed_removal_order(int seed_mask, int positions[81]) {
    for (int i = 0; i < 81; i++) positions[i] = i;
    unsigned int s = (unsigned int)(seed_mask * 22695477u + 1u);
    for (int i = 80; i > 0; i--) {
        s = s * 1664525u + 1013904223u;
        int j = (int)(s % (unsigned int)(i + 1));
        int tmp = positions[i]; positions[i] = positions[j]; positions[j] = tmp;
    }
}

/*
 * generate_sudoku:
 *   - Uses g_seed_mask and g_game.difficulty.
 *   - Same seed + same difficulty => same puzzle, every time.
 */
static void generate_sudoku(void) {
    int board[9][9];
    memset(board, 0, sizeof(board));

    /* Step 1: fill a complete valid board deterministically from seed */
    fill_board_seeded(board, g_seed_mask, 0);

    /* Step 2: store solution */
    for (int i = 0; i < 9; i++)
        for (int j = 0; j < 9; j++) {
            g_game.solution[i][j].value = board[i][j];
            g_game.solution[i][j].type  = CELL_GIVEN;
            g_game.solution[i][j].error = false;
        }

    /* Step 3: remove cells in seed-deterministic order while keeping uniqueness */
    int to_remove = (int)g_game.difficulty;
    int positions[81];
    seed_removal_order(g_seed_mask, positions);

    int removed = 0;
    for (int k = 0; k < 81 && removed < to_remove; k++) {
        int r = positions[k] / 9, c = positions[k] % 9;
        int backup = board[r][c];
        board[r][c] = 0;
        int tmp[9][9];
        memcpy(tmp, board, sizeof(board));
        if (count_solutions(tmp, 2) == 1) {
            removed++;
        } else {
            board[r][c] = backup;
        }
    }

    /* Step 4: populate the game board */
    for (int i = 0; i < 9; i++)
        for (int j = 0; j < 9; j++) {
            g_game.board[i][j].value = board[i][j];
            g_game.board[i][j].type  = (board[i][j] != 0) ? CELL_GIVEN : CELL_EMPTY;
            g_game.board[i][j].error = false;
        }

    g_game.solved  = false;
    g_game.elapsed = 0;
    log_msg(LOG_INFO, "Sudoku generated: seed=%d difficulty=%d removed=%d",
            g_seed_mask, (int)g_game.difficulty, removed);
}

/* ===== BOARD VALIDATION ===== */

static void validate_board(void) {
    for (int r = 0; r < 9; r++)
        for (int c = 0; c < 9; c++)
            g_game.board[r][c].error = false;

    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 9; c++) {
            int v = g_game.board[r][c].value;
            if (v == 0) continue;
            for (int k = 0; k < 9; k++) {
                if (k != c && g_game.board[r][k].value == v)
                    g_game.board[r][c].error = true;
                if (k != r && g_game.board[k][c].value == v)
                    g_game.board[r][c].error = true;
            }
            int br = (r/3)*3, bc = (c/3)*3;
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++)
                    if ((br+i != r || bc+j != c) && g_game.board[br+i][bc+j].value == v)
                        g_game.board[r][c].error = true;
        }
    }
}

static bool check_solved(void) {
    for (int r = 0; r < 9; r++)
        for (int c = 0; c < 9; c++) {
            if (g_game.board[r][c].value == 0) return false;
            if (g_game.board[r][c].error)      return false;
        }
    return true;
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

    const char *logo[] = {
        "  ____  _   _ ____   ___  _  ___  _   _ ",
        " / ___|| | | |  _ \\ / _ \\| |/ / || | | |",
        " \\___ \\| | | | | | | | | | ' /| || |_| |",
        "  ___) | |_| | |_| | |_| | . \\|__   _|",
        " |____/ \\___/|____/ \\___/|_|\\_\\  |_|  "
    };
    int logo_h = 5, logo_w = 42;
    int ly = rows/2 - 7;
    int lx = (cols - logo_w) / 2;
    attron(COLOR_PAIR(COLOR_TITLE) | A_BOLD);
    for (int i = 0; i < logo_h; i++)
        mvprintw(ly+i, lx, "%s", logo[i]);
    attroff(COLOR_PAIR(COLOR_TITLE) | A_BOLD);

    const char *items[] = {
        "  New game  ",
        "  Bot watch  ",
        "  Records  ",
        "  About  ",
        "  FAQ  ",
        "  QUIT  "
    };
    int n = 6;
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
    mvprintw(rows-1, 2, "arrows: select   Enter: confirm   q: exit");
    refresh();
}

/* ===== SEED SELECTION SCREEN ===== */

static void draw_seed_screen(void) {
    clear();
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int bx = (cols - 50) / 2, by = (rows - 20) / 2;
    draw_box_title(by, bx, 20, 50, " Generation settings ");

    mvprintw(by+2, bx+2, "1. Mark cells in 3x3 (Space/Enter):");
    mvprintw(by+3, bx+4, "   (defines the generation seed)");

    int gy = by+5, gx = bx+15;
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

    /* compute seed mask from checked cells */
    int mask = 0;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            if (g_seed_cells[r][c])
                mask |= (1 << (r*3+c));
    /* if no cells selected, display what will be used (time-based) */
    mvprintw(gy+7, bx+2, "Seed: %d (0x%03X)%s",
             mask, mask, (mask == 0) ? "  [random]" : "");

    const char *dnames[] = {"Easy (30 removed)", "Medium (45 removed)", "Hard (55 removed)"};
    mvprintw(by+14, bx+2, "2. Difficulty:");
    for (int i = 0; i < 3; i++) {
        if (i == g_sel_diff) {
            attron(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
            mvprintw(by+15+i, bx+4, "> %s", dnames[i]);
            attroff(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
        } else {
            mvprintw(by+15+i, bx+4, "  %s", dnames[i]);
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
    mvprintw(0, GX, "SUDOKU  [%s]  Seed: %d  Time: %02d:%02d",
             diff_names[di], g_seed_mask,
             g_game.elapsed/60, g_game.elapsed%60);
    attroff(COLOR_PAIR(COLOR_TITLE) | A_BOLD);

    /* draw grid lines */
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
            if ((cur & A_CHARTEXT) == ACS_VLINE)
                mvaddch(y, x, ACS_PLUS);
            else
                mvaddch(y, x, thick ? ACS_HLINE : '-');
        }
    }

    /* draw cells */
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

    /* legend */
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
        mvprintw(rows-1, 2, "arrows: move   1-9: enter   Del/0: erase   q: menu");
    refresh();
}

/* ===== BOT: SEQUENTIAL BACKTRACKING ===== */

/*
 * Bot solver mirrors a standard backtracking algorithm executed one step at
 * a time so the animation is visible.
 *
 * State machine per bot_step() call:
 *   - Find the first empty cell (row-major order).
 *   - If none: puzzle solved.
 *   - Try next digit (stack_try[depth]) in 1..9.
 *       - Valid: place it, push to stack, advance depth.
 *       - No valid digit found: backtrack — undo last placement, increment
 *         its try counter, continue from that cell again.
 *
 * The bot always starts from the first empty cell in reading order, matching
 * the same sequential logic used in count_solutions().
 */

static void bot_init(void) {
    g_bot.stack_top = 0;
    g_bot.finished  = false;

    /* Push initial state: start scanning from (0,0) with try=1 */
    g_bot.stack_r[0]   = 0;
    g_bot.stack_c[0]   = 0;
    g_bot.stack_v[0]   = 0;
    g_bot.stack_try[0] = 1;
}

/* Find the first empty cell in reading order at or after (r,c). */
static bool bot_find_empty(int start_r, int start_c, int *out_r, int *out_c) {
    for (int r = start_r; r < 9; r++) {
        int cs = (r == start_r) ? start_c : 0;
        for (int c = cs; c < 9; c++) {
            if (g_game.board[r][c].value == 0) {
                *out_r = r; *out_c = c;
                return true;
            }
        }
    }
    return false;
}

/*
 * Execute one bot step.
 * Returns true if the board was modified (a cell was placed or cleared),
 * false if already finished or stuck.
 */
static bool bot_step(void) {
    if (g_bot.finished) return false;

    /* If stack is empty we need to find the very first empty cell */
    if (g_bot.stack_top == 0) {
        int r, c;
        if (!bot_find_empty(0, 0, &r, &c)) {
            /* No empty cells: already solved */
            g_bot.finished = true;
            return false;
        }
        g_bot.stack_r[0]   = r;
        g_bot.stack_c[0]   = c;
        g_bot.stack_v[0]   = 0;
        g_bot.stack_try[0] = 1;
        g_bot.stack_top    = 1;
    }

    /* Work on top of stack */
    while (g_bot.stack_top > 0) {
        int depth = g_bot.stack_top - 1;
        int r     = g_bot.stack_r[depth];
        int c     = g_bot.stack_c[depth];

        /* Erase any previously placed value at this cell */
        if (g_game.board[r][c].value != 0 &&
            g_game.board[r][c].type == CELL_BOT) {
            g_game.board[r][c].value = 0;
            g_game.board[r][c].type  = CELL_EMPTY;
        }

        /* Try digits starting from stack_try[depth] */
        bool placed = false;
        int tmp[9][9];
        for (int i = 0; i < 9; i++)
            for (int j = 0; j < 9; j++)
                tmp[i][j] = g_game.board[i][j].value;

        for (int v = g_bot.stack_try[depth]; v <= 9; v++) {
            if (is_valid(tmp, r, c, v)) {
                /* Place this digit */
                g_game.board[r][c].value = v;
                g_game.board[r][c].type  = CELL_BOT;
                g_bot.stack_v[depth]     = v;
                g_bot.stack_try[depth]   = v + 1; /* next try if we backtrack here */

                log_msg(LOG_DEBUG, "Bot place [%d][%d]=%d depth=%d", r, c, v, depth);

                /* Find next empty cell (scan from next position in reading order) */
                int nr, nc;
                int next_r = (c + 1 < 9) ? r : r + 1;
                int next_c = (c + 1 < 9) ? c + 1 : 0;
                if (!bot_find_empty(next_r, next_c, &nr, &nc)) {
                    /* No more empty cells => solved */
                    g_bot.finished = true;
                    return true;
                }

                /* Push next cell */
                int nd = g_bot.stack_top;
                g_bot.stack_r[nd]   = nr;
                g_bot.stack_c[nd]   = nc;
                g_bot.stack_v[nd]   = 0;
                g_bot.stack_try[nd] = 1;
                g_bot.stack_top++;

                placed = true;
                return true; /* one visual step done */
            }
        }

        if (!placed) {
            /* Backtrack */
            log_msg(LOG_DEBUG, "Bot backtrack at [%d][%d] depth=%d", r, c, depth);
            g_bot.stack_top--;
            /* The erase at the top of the loop will clear this cell next iter */
        }
    }

    /* Stack exhausted without solution (shouldn't happen on valid puzzle) */
    g_bot.finished = true;
    return false;
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
    if (!g_records) mvprintw(6, 4, "(no records)");
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
        "   Arrows Up/Down       - select menu option",
        "   Enter                - confirm",
        "   q                    - exit",
        "",
        " SEED SELECTION:",
        "   Arrows               - move in 3x3 grid",
        "   Space / Enter        - toggle cell (seed bit)",
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
        "   Same seed + same difficulty => identical puzzle.",
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
/*
 * PDCurses on Windows returns '\r' for Enter; ncurses returns '\n'.
 * KEY_ENTER is the numpad Enter. Check all three.
 */
#define IS_ENTER(ch) ((ch) == '\n' || (ch) == '\r' || (ch) == KEY_ENTER)

/* ===== STATE HANDLERS ===== */

static void handle_menu(int ch) {
    int n = 6;
    if (ch == KEY_UP)   g_menu_sel = (g_menu_sel + n - 1) % n;
    if (ch == KEY_DOWN) g_menu_sel = (g_menu_sel + 1) % n;
    if (IS_ENTER(ch)) {
        switch (g_menu_sel) {
            case 0:
                g_cur_r = 0; g_cur_c = 0;
                memset(g_seed_cells, 0, sizeof(g_seed_cells));
                g_state = STATE_SEED_SELECT;
                break;
            case 1:
                g_cur_r = 0; g_cur_c = 0;
                memset(g_seed_cells, 0, sizeof(g_seed_cells));
                strcpy(g_game.player_name, "__BOT__");
                g_state = STATE_SEED_SELECT;
                break;
            case 2: g_state = STATE_RECORDS; break;
            case 3: g_state = STATE_ABOUT;   break;
            case 4: g_state = STATE_HELP;    break;
            case 5: g_state = STATE_QUIT;    break;
        }
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

        /* Build seed mask */
        g_seed_mask = 0;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                if (g_seed_cells[r][c])
                    g_seed_mask |= (1 << (r*3+c));
        if (g_seed_mask == 0)
            g_seed_mask = (int)(time(NULL) & 0x1FF);

        /*
         * srand is only used internally by legacy code; generation now uses
         * seed_digit_order / seed_removal_order which are fully deterministic.
         * We still seed the C rand() for any unrelated future use.
         */
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
        log_msg(LOG_INFO, "Game started: seed=%d diff=%d bot=%d",
                g_seed_mask, (int)g_game.difficulty, bot_mode ? 1 : 0);
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
    printf("  Same seed + same difficulty always gives the same puzzle.\n");
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
            printf("sudoku v0.2  2026\n");
            return 0;
        }
    }

    g_log = fopen(LOG_FILE, "a");
    log_msg(LOG_INFO, "=== sudoku started ===");

    records_load();

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    nodelay(stdscr, FALSE);

    if (has_colors()) init_colors();

    srand((unsigned)time(NULL));
    memset(g_game.player_name, 0, sizeof(g_game.player_name));

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
            case STATE_ABOUT:       draw_about();       break;
            case STATE_HELP:        draw_help();        break;
            default: break;
        }

        /* Input with bot animation */
        if (g_state == STATE_BOT_WATCH) {
            halfdelay(1); /* ~100 ms timeout */
        } else {
            nocbreak();
            cbreak();
        }

        int ch = getch();

        if (g_state == STATE_BOT_WATCH) {
            nocbreak(); cbreak();
            if (ch == 'q' || ch == 'Q') { g_state = STATE_MENU; continue; }
            if (ch == ERR || ch == KEY_RESIZE) {
                /* Advance bot one step per tick (~100 ms) */
                if (!g_bot.finished) {
                    bot_step();
                    if (g_bot.finished || check_solved()) {
                        g_game.solved = true;
                        log_msg(LOG_INFO, "Bot finished solving.");
                        napms(1500);
                        g_state = STATE_MENU;
                    }
                }
            }
            continue;
        }

        if (ch == ERR) continue;

        if ((g_state == STATE_RECORDS || g_state == STATE_ABOUT || g_state == STATE_HELP)
            && (ch == 27 || ch == 'q' || ch == 'Q')) {
            g_state = STATE_MENU;
            continue;
        }

        switch (g_state) {
            case STATE_MENU:        handle_menu(ch); break;
            case STATE_SEED_SELECT: handle_seed(ch); break;
            case STATE_PLAYING:     handle_game(ch); break;
            default: break;
        }

        last_tick = time(NULL);
    }

    endwin();

    records_save();
    records_free();
    log_msg(LOG_INFO, "=== sudoku exited ===");
    if (g_log) fclose(g_log);

    return 0;
}
