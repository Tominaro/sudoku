#ifndef SUDOKU_TYPES_H
#define SUDOKU_TYPES_H

#include <stdbool.h>

/* ===== ENUMS ===== */

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
    STATE_OPTIONS,
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

/*
 * GeneratorType — digit-order algorithm used when filling the board.
 *
 *   GEN_LCG      : LCG + Fisher-Yates (original algorithm)
 *   GEN_XORSHIFT : xorshift32 + Fisher-Yates
 *   GEN_QUADRATIC: quadratic-residue permutation (no swap needed)
 *   GEN_FIBONACCI : Fibonacci-hashing permutation
 */
typedef enum {
    GEN_LCG       = 0,
    GEN_XORSHIFT  = 1,
    GEN_QUADRATIC = 2,
    GEN_FIBONACCI = 3,
    GEN_COUNT     = 4
} GeneratorType;

/* ===== STRUCTS ===== */

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
    Cell          board[9][9];
    Cell          solution[9][9];
    int           elapsed;
    Difficulty    difficulty;
    bool          solved;
    char          player_name[32];
    GeneratorType generator;
} Game;


#endif /* SUDOKU_TYPES_H */
