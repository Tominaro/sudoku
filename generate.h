#ifndef GENERATE_H
#define GENERATE_H

#include <stdbool.h>
#include "sudoku_types.h"

/*
 * generate.h -- Sudoku puzzle generation.
 *
 * Four deterministic digit-order algorithms are supported (GeneratorType).
 * Given the same seed_mask, difficulty, and generator the result is always
 * identical.
 */

/* Human-readable name for a generator (ASCII only). */
const char *generator_name(GeneratorType g);

/* Validate whether placing value v at (r,c) is legal on board. */
bool is_valid(int board[9][9], int r, int c, int v);

/*
 * Digit-order functions -- each fills out[9] with a permutation of 1..9
 * that is fully determined by (seed_mask, pos).
 *
 * GEN_LCG      : LCG seeding + Fisher-Yates shuffle
 * GEN_XORSHIFT : xorshift32  + Fisher-Yates shuffle
 * GEN_QUADRATIC: quadratic-residue permutation (no swap pass needed)
 * GEN_FIBONACCI: Fibonacci hashing permutation  (no swap pass needed)
 */
void digit_order_lcg      (int seed_mask, int pos, int out[9]);
void digit_order_xorshift (int seed_mask, int pos, int out[9]);
void digit_order_quadratic(int seed_mask, int pos, int out[9]);
void digit_order_fibonacci (int seed_mask, int pos, int out[9]);

/* Dispatch: call the correct digit-order function for g. */
void seed_digit_order(GeneratorType g, int seed_mask, int pos, int out[9]);

/* Fill board from pos using seeded backtracking with the chosen generator. */
bool fill_board_seeded(int board[9][9], int seed_mask, int pos, GeneratorType g);

/* Count solutions (capped at limit) -- used for uniqueness check. */
int count_solutions(int board[9][9], int limit);

/*
 * Deterministic cell-removal order derived from seed_mask.
 * positions[0..80] is filled with shuffled indices 0..80.
 */
void seed_removal_order(int seed_mask, int positions[81]);

/*
 * Generate a full puzzle into g_game.
 * Uses g_seed_mask, g_game.difficulty, and g_game.generator.
 * Same (seed, difficulty, generator) => same puzzle, every time.
 */
void generate_sudoku(void);

/* Mark conflicting cells in g_game.board. */
void validate_board(void);

/* Return true when every cell is filled without errors. */
bool check_solved(void);

#endif /* GENERATE_H */
