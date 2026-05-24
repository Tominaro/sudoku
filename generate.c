#include "generate.h"
#include "sudoku_types.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

/*
 * generate.c -- Sudoku puzzle generation.
 *
 * Counters logged to sudoku.log after each generate_sudoku() call:
 *
 *   fill_calls      : recursive calls inside fill_board_seeded()
 *                     (each call = one cell placement attempt or backtrack)
 *   fill_is_valid   : is_valid() calls inside fill_board_seeded()
 *   fill_placements : successful digit placements (board[r][c] = v)
 *   fill_backtracks : backtrack events (board[r][c] = 0)
 *
 *   removal_candidates : cells tried for removal (loop iterations)
 *   removal_count_calls: count_solutions() top-level calls (one per candidate)
 *   cs_is_valid        : is_valid() calls inside all count_solutions() runs
 *   cs_nodes           : recursive nodes explored inside all count_solutions()
 *   removed            : cells successfully removed (unique-solution confirmed)
 *
 * These counters let you empirically measure the cost of each phase
 * and compare against the theoretical O(9^d) bound from the report.
 */

/* ===== EXTERN GLOBALS (defined in main.c) ===== */

extern Game  g_game;
extern int   g_seed_mask;
extern FILE *g_log;

void log_msg(int lvl, const char *fmt, ...);

/* ===== GENERATION COUNTERS ===== */

typedef struct {
    long fill_calls;       /* fill_board_seeded() recursive invocations    */
    long fill_is_valid;    /* is_valid() calls inside fill phase            */
    long fill_placements;  /* successful digit placements                   */
    long fill_backtracks;  /* backtrack events (cell reset to 0)            */

    long removal_candidates; /* cells tried for removal                    */
    long removal_count_calls;/* count_solutions() invocations (top-level)  */
    long cs_nodes;           /* recursive nodes inside all count_solutions  */
    long cs_is_valid;        /* is_valid() calls inside count_solutions     */
    long removed;            /* cells actually removed                      */
} GenCounters;

/* Active counters during a generate_sudoku() call; zeroed at start. */
static GenCounters gc;

/* ===== GENERATOR NAMES ===== */

const char *generator_name(GeneratorType g) {
    switch (g) {
        case GEN_LCG:       return "LCG + Fisher-Yates";
        case GEN_XORSHIFT:  return "Xorshift32 + F-Y";
        case GEN_QUADRATIC: return "Quadratic residue";
        case GEN_FIBONACCI: return "Fibonacci hashing";
        default:            return "Unknown";
    }
}

/* ===== IS_VALID ===== */

bool is_valid(int board[9][9], int r, int c, int v) {
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

/* ===== ALGORITHM 1: LCG + Fisher-Yates ===== */

void digit_order_lcg(int seed_mask, int pos, int out[9]) {
    unsigned int s = (unsigned int)(seed_mask ^
        ((unsigned int)pos * 6364136223846793005u + 1442695040888963407u));
    s = s * 1664525u + 1013904223u;
    int shift = (int)(s % 9);
    for (int i = 0; i < 9; i++)
        out[i] = ((i + shift) % 9) + 1;
    for (int i = 8; i > 0; i--) {
        s = s * 1664525u + 1013904223u;
        int j = (int)(s % (unsigned int)(i + 1));
        int tmp = out[i]; out[i] = out[j]; out[j] = tmp;
    }
}

/* ===== ALGORITHM 2: Xorshift32 + Fisher-Yates ===== */

void digit_order_xorshift(int seed_mask, int pos, int out[9]) {
    unsigned int s = (unsigned int)seed_mask * 2246822519u
                   ^ (unsigned int)(pos + 1)  * 3266489917u;
    s ^= s >> 16;
    s *= 2246822519u;
    s ^= s >> 13;
    if (s == 0) s = 0xdeadbeef;
    for (int i = 0; i < 9; i++) out[i] = i + 1;
    for (int i = 8; i > 0; i--) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        int j = (int)(s % (unsigned int)(i + 1));
        int tmp = out[i]; out[i] = out[j]; out[j] = tmp;
    }
}

/* ===== ALGORITHM 3: Quadratic-residue permutation ===== */

void digit_order_quadratic(int seed_mask, int pos, int out[9]) {
    unsigned int h = (unsigned int)seed_mask * 2654435761u
                   ^ (unsigned int)(pos * 40503u + 12345u);
    h ^= h >> 16;
    int a = (int)( h        & 0xFF) % 9;
    int b = (int)((h >>  8) & 0xFF) % 9;
    int c = (int)((h >> 16) & 0xFF) % 9;
    bool used[9] = {false};
    int  cnt = 0;
    for (int i = 0; i < 9; i++) {
        int slot = (a * i * i + b * i + c) % 9;
        while (used[slot]) slot = (slot + 1) % 9;
        used[slot] = true;
        out[slot]  = i + 1;
        cnt++;
    }
    (void)cnt;
}

/* ===== ALGORITHM 4: Fibonacci hashing permutation ===== */

void digit_order_fibonacci(int seed_mask, int pos, int out[9]) {
    unsigned int base = (unsigned int)seed_mask * 2246822519u
                      ^ (unsigned int)(pos + 1)  * 3266489917u;
    base ^= base >> 15;
    bool used[9] = {false};
    for (int i = 0; i < 9; i++) {
        unsigned int h = (base + (unsigned int)i * 2654435769u);
        h ^= h >> 16;
        int slot = (int)(h % 9u);
        while (used[slot]) slot = (slot + 1) % 9;
        used[slot] = true;
        out[slot]  = i + 1;
    }
}

/* ===== DISPATCH ===== */

void seed_digit_order(GeneratorType g, int seed_mask, int pos, int out[9]) {
    switch (g) {
        case GEN_XORSHIFT:  digit_order_xorshift (seed_mask, pos, out); break;
        case GEN_QUADRATIC: digit_order_quadratic(seed_mask, pos, out); break;
        case GEN_FIBONACCI: digit_order_fibonacci (seed_mask, pos, out); break;
        default:            digit_order_lcg       (seed_mask, pos, out); break;
    }
}

/* ===== FILL_BOARD_SEEDED (instrumented) ===== */

bool fill_board_seeded(int board[9][9], int seed_mask, int pos, GeneratorType g) {
    gc.fill_calls++;

    if (pos == 81) return true;
    int r = pos / 9, c = pos % 9;
    if (board[r][c] != 0) return fill_board_seeded(board, seed_mask, pos + 1, g);

    int order[9];
    seed_digit_order(g, seed_mask, pos, order);

    for (int i = 0; i < 9; i++) {
        int v = order[i];
        gc.fill_is_valid++;
        if (is_valid(board, r, c, v)) {
            board[r][c] = v;
            gc.fill_placements++;
            if (fill_board_seeded(board, seed_mask, pos + 1, g)) return true;
            board[r][c] = 0;
            gc.fill_backtracks++;
        }
    }
    return false;
}

/* ===== COUNT_SOLUTIONS (instrumented) ===== */

/*
 * Internal recursive worker — uses a separate counter so the top-level
 * wrapper can count invocations without double-counting.
 */
static int count_solutions_rec(int board[9][9], int limit) {
    gc.cs_nodes++;

    int r = -1, c = -1;
    for (int i = 0; i < 9 && r == -1; i++)
        for (int j = 0; j < 9 && r == -1; j++)
            if (board[i][j] == 0) { r = i; c = j; }
    if (r == -1) return 1;

    int cnt = 0;
    for (int v = 1; v <= 9 && cnt < limit; v++) {
        gc.cs_is_valid++;
        if (is_valid(board, r, c, v)) {
            board[r][c] = v;
            cnt += count_solutions_rec(board, limit - cnt);
            board[r][c] = 0;
        }
    }
    return cnt;
}

int count_solutions(int board[9][9], int limit) {
    gc.removal_count_calls++;
    return count_solutions_rec(board, limit);
}

/* ===== SEED_REMOVAL_ORDER ===== */

void seed_removal_order(int seed_mask, int positions[81]) {
    for (int i = 0; i < 81; i++) positions[i] = i;
    unsigned int s = (unsigned int)(seed_mask * 22695477u + 1u);
    for (int i = 80; i > 0; i--) {
        s = s * 1664525u + 1013904223u;
        int j = (int)(s % (unsigned int)(i + 1));
        int tmp = positions[i]; positions[i] = positions[j]; positions[j] = tmp;
    }
}

/* ===== GENERATE_SUDOKU ===== */

void generate_sudoku(void) {
    /* Reset all counters for this run */
    memset(&gc, 0, sizeof(gc));

    int board[9][9];
    memset(board, 0, sizeof(board));

    /* ---- Phase 1: fill complete board ---- */
    fill_board_seeded(board, g_seed_mask, 0, g_game.generator);

    for (int i = 0; i < 9; i++)
        for (int j = 0; j < 9; j++) {
            g_game.solution[i][j].value = board[i][j];
            g_game.solution[i][j].type  = CELL_GIVEN;
            g_game.solution[i][j].error = false;
        }

    /* ---- Phase 2: remove cells (uniqueness check per candidate) ---- */
    int to_remove = (int)g_game.difficulty;
    int positions[81];
    seed_removal_order(g_seed_mask, positions);

    int removed = 0;
    for (int k = 0; k < 81 && removed < to_remove; k++) {
        gc.removal_candidates++;
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
    gc.removed = removed;

    for (int i = 0; i < 9; i++)
        for (int j = 0; j < 9; j++) {
            g_game.board[i][j].value = board[i][j];
            g_game.board[i][j].type  = (board[i][j] != 0) ? CELL_GIVEN : CELL_EMPTY;
            g_game.board[i][j].error = false;
        }

    g_game.solved  = false;
    g_game.elapsed = 0;

    /* ---- Log summary ---- */
    log_msg(LOG_INFO,
        "=== Generation complete ===");
    log_msg(LOG_INFO,
        "  seed=%d  difficulty=%d  generator=%s",
        g_seed_mask, (int)g_game.difficulty,
        generator_name(g_game.generator));

    log_msg(LOG_INFO,
        "  [Phase 1: fill board]");
    log_msg(LOG_INFO,
        "    fill_calls      = %ld  (recursive invocations of fill_board_seeded)",
        gc.fill_calls);
    log_msg(LOG_INFO,
        "    fill_is_valid   = %ld  (is_valid() calls during fill)",
        gc.fill_is_valid);
    log_msg(LOG_INFO,
        "    fill_placements = %ld  (successful digit placements)",
        gc.fill_placements);
    log_msg(LOG_INFO,
        "    fill_backtracks = %ld  (cells reset to 0 during backtrack)",
        gc.fill_backtracks);

    log_msg(LOG_INFO,
        "  [Phase 2: cell removal / uniqueness checks]");
    log_msg(LOG_INFO,
        "    removal_candidates  = %ld  (cells tried for removal)",
        gc.removal_candidates);
    log_msg(LOG_INFO,
        "    removal_count_calls = %ld  (count_solutions() top-level calls)",
        gc.removal_count_calls);
    log_msg(LOG_INFO,
        "    cs_nodes            = %ld  (recursive nodes in all count_solutions runs)",
        gc.cs_nodes);
    log_msg(LOG_INFO,
        "    cs_is_valid         = %ld  (is_valid() calls inside count_solutions)",
        gc.cs_is_valid);
    log_msg(LOG_INFO,
        "    removed             = %ld  (cells successfully removed)",
        gc.removed);

    long total_is_valid = gc.fill_is_valid + gc.cs_is_valid;
    long total_ops      = gc.fill_calls + gc.cs_nodes;
    log_msg(LOG_INFO,
        "  [Totals]");
    log_msg(LOG_INFO,
        "    total is_valid calls = %ld",
        total_is_valid);
    log_msg(LOG_INFO,
        "    total recursive nodes (fill + removal) = %ld",
        total_ops);
}

/* ===== VALIDATE_BOARD ===== */

void validate_board(void) {
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

/* ===== CHECK_SOLVED ===== */

bool check_solved(void) {
    for (int r = 0; r < 9; r++)
        for (int c = 0; c < 9; c++) {
            if (g_game.board[r][c].value == 0) return false;
            if (g_game.board[r][c].error)      return false;
        }
    return true;
}
