#ifndef BOT_H
#define BOT_H

#include <stdbool.h>

/*
 * bot.h — Step-by-step backtracking bot solver.
 *
 * The bot animates the backtracking algorithm one step per call to bot_step(),
 * making each placement (or erasure) visible on the game board.
 *
 * Shared state accessed:  g_game  (read/write cells),  g_bot  (solver state),
 *                         g_log   (debug logging).
 * All of these are defined in main.c.
 */

/*
 * BotState — backtracking stack kept between bot_step() calls.
 *
 *   stack_r / stack_c  - row/col of the cell at each depth
 *   stack_v            - value last placed at this depth
 *   stack_try          - next digit to attempt when we return here
 *   stack_top          - number of active stack frames
 *   finished           - set when the puzzle is solved or exhausted
 */
typedef struct {
    int  stack_r[81];
    int  stack_c[81];
    int  stack_v[81];
    int  stack_try[81];
    int  stack_top;
    bool finished;
} BotState;

/* Initialise the bot; call once after generate_sudoku(). */
void bot_init(void);

/*
 * Advance the bot by exactly one step.
 * Returns true if the board was modified (a digit was placed or cleared).
 * Sets g_bot.finished when the puzzle is solved or no solution exists.
 */
bool bot_step(void);

#endif /* BOT_H */
