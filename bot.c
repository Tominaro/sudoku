#include "bot.h"
#include "sudoku_types.h"

#include <stdio.h>

/*
 * bot.c — Step-by-step backtracking bot solver.
 *
 * Accesses shared globals defined in main.c.
 */

/* ===== EXTERN GLOBALS (defined in main.c) ===== */

extern Game     g_game;
extern BotState g_bot;
extern FILE    *g_log;

/* is_valid() lives in generate.c. */
bool is_valid(int board[9][9], int r, int c, int v);

/* log_msg() is defined in main.c. */
void log_msg(int lvl, const char *fmt, ...);

/* ===== BOT_INIT ===== */

void bot_init(void) {
    g_bot.stack_top = 0;
    g_bot.finished  = false;

    g_bot.stack_r[0]   = 0;
    g_bot.stack_c[0]   = 0;
    g_bot.stack_v[0]   = 0;
    g_bot.stack_try[0] = 1;
}

/* ===== BOT_FIND_EMPTY (internal helper) ===== */

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

/* ===== BOT_STEP ===== */

bool bot_step(void) {
    if (g_bot.finished) return false;

    /* Bootstrap: find the very first empty cell if the stack is empty. */
    if (g_bot.stack_top == 0) {
        int r, c;
        if (!bot_find_empty(0, 0, &r, &c)) {
            g_bot.finished = true;
            return false;
        }
        g_bot.stack_r[0]   = r;
        g_bot.stack_c[0]   = c;
        g_bot.stack_v[0]   = 0;
        g_bot.stack_try[0] = 1;
        g_bot.stack_top    = 1;
    }

    while (g_bot.stack_top > 0) {
        int depth = g_bot.stack_top - 1;
        int r     = g_bot.stack_r[depth];
        int c     = g_bot.stack_c[depth];

        /* Erase the value previously placed by the bot at this cell. */
        if (g_game.board[r][c].value != 0 &&
            g_game.board[r][c].type == CELL_BOT) {
            g_game.board[r][c].value = 0;
            g_game.board[r][c].type  = CELL_EMPTY;
        }

        /* Build a plain int board for is_valid(). */
        int tmp[9][9];
        for (int i = 0; i < 9; i++)
            for (int j = 0; j < 9; j++)
                tmp[i][j] = g_game.board[i][j].value;

        bool placed = false;
        for (int v = g_bot.stack_try[depth]; v <= 9; v++) {
            if (is_valid(tmp, r, c, v)) {
                g_game.board[r][c].value = v;
                g_game.board[r][c].type  = CELL_BOT;
                g_bot.stack_v[depth]     = v;
                g_bot.stack_try[depth]   = v + 1;

                log_msg(LOG_DEBUG, "Bot place [%d][%d]=%d depth=%d", r, c, v, depth);

                int nr, nc;
                int next_r = (c + 1 < 9) ? r : r + 1;
                int next_c = (c + 1 < 9) ? c + 1 : 0;
                if (!bot_find_empty(next_r, next_c, &nr, &nc)) {
                    g_bot.finished = true;
                    return true;
                }

                int nd = g_bot.stack_top;
                g_bot.stack_r[nd]   = nr;
                g_bot.stack_c[nd]   = nc;
                g_bot.stack_v[nd]   = 0;
                g_bot.stack_try[nd] = 1;
                g_bot.stack_top++;

                placed = true;
                return true;
            }
        }

        if (!placed) {
            log_msg(LOG_DEBUG, "Bot backtrack at [%d][%d] depth=%d", r, c, depth);
            g_bot.stack_top--;
        }
    }

    g_bot.finished = true;
    return false;
}
