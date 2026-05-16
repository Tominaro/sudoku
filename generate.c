#include "generate.h"
#include "sudoku_types.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/*
 * generate.c -- Sudoku puzzle generation.
 *
 * Four fully deterministic digit-order algorithms are implemented.
 * All shared state lives in main.c (g_game, g_seed_mask, g_log).
 */

/* ===== EXTERN GLOBALS (defined in main.c) ===== */

extern Game  g_game;
extern int   g_seed_mask;
extern FILE *g_log;

void log_msg(int lvl, const char *fmt, ...);

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

/* ===== ALGORITHM 1: LCG + Fisher-Yates (original) =====
 *
 * A 32-bit LCG seeded by (seed_mask XOR pos-mixed constant) produces a
 * shift; after rotating [1..9] by that shift a second Fisher-Yates pass
 * shuffles with the same LCG stream.  The combination is fast and has
 * reasonable avalanche for small seeds.
 */
void digit_order_lcg(int seed_mask, int pos, int out[9]) {
    unsigned int s = (unsigned int)(seed_mask ^
        ((unsigned int)pos * 6364136223846793005u + 1442695040888963407u));
    s = s * 1664525u + 1013904223u;
    int shift = (int)(s % 9);

    for (int i = 0; i < 9; i++)
        out[i] = ((i + shift) % 9) + 1;          /* rotate 1..9 */

    /* Fisher-Yates with the same LCG stream */
    for (int i = 8; i > 0; i--) {
        s = s * 1664525u + 1013904223u;
        int j = (int)(s % (unsigned int)(i + 1));
        int tmp = out[i]; out[i] = out[j]; out[j] = tmp;
    }
}

/* ===== ALGORITHM 2: Xorshift32 + Fisher-Yates =====
 *
 * xorshift32 has a period of 2^32-1 and much better bit-mixing than a
 * plain LCG.  The seed is derived from (seed_mask, pos) via two mixing
 * steps so that even seed=0 never collapses the state to 0.
 * A standard Fisher-Yates pass then permutes [1..9].
 */
void digit_order_xorshift(int seed_mask, int pos, int out[9]) {
    /* Mix seed_mask and pos into a non-zero 32-bit state */
    unsigned int s = (unsigned int)seed_mask * 2246822519u
                   ^ (unsigned int)(pos + 1)  * 3266489917u;
    s ^= s >> 16;
    s *= 2246822519u;
    s ^= s >> 13;
    if (s == 0) s = 0xdeadbeef;   /* xorshift must not start at 0 */

    for (int i = 0; i < 9; i++) out[i] = i + 1;  /* [1..9] */

    for (int i = 8; i > 0; i--) {
        /* xorshift32 step */
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        int j = (int)(s % (unsigned int)(i + 1));
        int tmp = out[i]; out[i] = out[j]; out[j] = tmp;
    }
}

/* ===== ALGORITHM 3: Quadratic-residue permutation =====
 *
 * A quadratic permutation polynomial over Z/9Z maps index k ->
 *   (a*k^2 + b*k + c) mod 9
 * where the coefficients a, b, c are derived from the seed and pos.
 * If the mapping is not a bijection (gcd(a,9) != 1 etc.) we fall back
 * to the natural order for that position -- the backtracker corrects
 * conflicts anyway and the seed still determines which digits are tried
 * first.  No swap pass is required; the permutation is computed directly.
 *
 * Why quadratic?  It is a textbook method (used in hash-table probing)
 * and gives good dispersion without needing a shuffle loop.
 */
void digit_order_quadratic(int seed_mask, int pos, int out[9]) {
    /* Derive three small coefficients from the seed */
    unsigned int h = (unsigned int)seed_mask * 2654435761u
                   ^ (unsigned int)(pos * 40503u + 12345u);
    h ^= h >> 16;

    int a = (int)( h        & 0xFF) % 9;
    int b = (int)((h >>  8) & 0xFF) % 9;
    int c = (int)((h >> 16) & 0xFF) % 9;

    /* Build permutation: slot[i] = (a*i*i + b*i + c) mod 9, value i+1 */
    bool used[9] = {false};
    int  cnt = 0;

    for (int i = 0; i < 9; i++) {
        int slot = (a * i * i + b * i + c) % 9;
        /* Linear probe to resolve collisions */
        while (used[slot]) slot = (slot + 1) % 9;
        used[slot] = true;
        out[slot]  = i + 1;
        cnt++;
    }
    /* cnt == 9 always; used[] guarantees full coverage */
    (void)cnt;
}

/* ===== ALGORITHM 4: Fibonacci hashing permutation =====
 *
 * Fibonacci hashing multiplies by the 32-bit Fibonacci/golden-ratio
 * constant (2654435769) and right-shifts to extract a bucket index.
 * Each of the 9 digits is mapped to a slot in [0..8]; linear probing
 * resolves collisions.  The result is a permutation of [1..9] where the
 * ordering reflects the golden-ratio structure of the hash function.
 *
 * This is distinct from both LCG and xorshift because no shuffle loop
 * is used at all -- the permutation emerges purely from the hash.
 */
void digit_order_fibonacci(int seed_mask, int pos, int out[9]) {
    /* Per-position key: mix seed_mask and pos with a large prime */
    unsigned int base = (unsigned int)seed_mask * 2246822519u
                      ^ (unsigned int)(pos + 1)  * 3266489917u;
    base ^= base >> 15;

    bool used[9] = {false};

    for (int i = 0; i < 9; i++) {
        /* Fibonacci hash of (base + i): multiply by phi-derived constant */
        unsigned int h = (base + (unsigned int)i * 2654435769u);
        h ^= h >> 16;
        int slot = (int)(h % 9u);
        /* Linear probe */
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

/* ===== FILL_BOARD_SEEDED ===== */

bool fill_board_seeded(int board[9][9], int seed_mask, int pos, GeneratorType g) {
    if (pos == 81) return true;
    int r = pos / 9, c = pos % 9;
    if (board[r][c] != 0) return fill_board_seeded(board, seed_mask, pos + 1, g);

    int order[9];
    seed_digit_order(g, seed_mask, pos, order);

    for (int i = 0; i < 9; i++) {
        int v = order[i];
        if (is_valid(board, r, c, v)) {
            board[r][c] = v;
            if (fill_board_seeded(board, seed_mask, pos + 1, g)) return true;
            board[r][c] = 0;
        }
    }
    return false;
}

/* ===== COUNT_SOLUTIONS ===== */

int count_solutions(int board[9][9], int limit) {
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
    int board[9][9];
    memset(board, 0, sizeof(board));

    fill_board_seeded(board, g_seed_mask, 0, g_game.generator);

    for (int i = 0; i < 9; i++)
        for (int j = 0; j < 9; j++) {
            g_game.solution[i][j].value = board[i][j];
            g_game.solution[i][j].type  = CELL_GIVEN;
            g_game.solution[i][j].error = false;
        }

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

    for (int i = 0; i < 9; i++)
        for (int j = 0; j < 9; j++) {
            g_game.board[i][j].value = board[i][j];
            g_game.board[i][j].type  = (board[i][j] != 0) ? CELL_GIVEN : CELL_EMPTY;
            g_game.board[i][j].error = false;
        }

    g_game.solved  = false;
    g_game.elapsed = 0;
    log_msg(LOG_INFO, "Sudoku generated: seed=%d difficulty=%d removed=%d generator=%d",
            g_seed_mask, (int)g_game.difficulty, removed, (int)g_game.generator);
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
