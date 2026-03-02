/*
 * broai_core.h — общие типы, константы и объявления
 */

#ifndef BROAI_CORE_H
#define BROAI_CORE_H

#include <stddef.h>

/* ============================================
 * Константы
 * ============================================ */

#define MAX_INPUT_LEN        4096
#define MAX_API_KEY_LEN      256
#define MAX_RESPONSE_SIZE    (1024 * 1024)
#define LOG_FILE             "/tmp/broai_last.log"

#define HISTORY_FILE         ".how_history"
#define HISTORY_MAX_ENTRIES  3
#define HISTORY_LINE_MAX     512

/* ============================================
 * История запросов
 * ============================================ */

typedef struct {
    char question[HISTORY_LINE_MAX];
    char command[HISTORY_LINE_MAX];
} HistoryEntry;

typedef struct {
    HistoryEntry entries[HISTORY_MAX_ENTRIES];
    int count;
} History;

History load_history(void);
void    save_history(const char *question, const char *command);

/* ============================================
 * Коды блокировки команды
 * ============================================ */

typedef enum {
    CMD_OK        = 0,
    CMD_DANGEROUS = 1,   /* rm -rf /, dd if=/dev/zero ... */
    CMD_INJECT    = 2,   /* $(), ``, ${} */
    CMD_REDIRECT  = 3,   /* редирект в /etc/, /dev/ ... */
    CMD_EXEC      = 4,   /* eval, bash -c ... */
    CMD_TOO_LONG  = 5,
} CmdBlockReason;

CmdBlockReason validate_command(const char *cmd);
void           print_block_reason(CmdBlockReason reason, const char *cmd);

/* ============================================
 * Вспомогательные функции
 * ============================================ */

char *escape_string(const char *str);
char *trim_whitespace(const char *str);
char *strip_markdown(const char *str);
char *edit_command(const char *cmd);
void  print_info_formatted(const char *info_text);

/* ============================================
 * Спиннер
 * ============================================ */

#include <pthread.h>

typedef struct {
    volatile int running;
} SpinnerState;

void spinner_start(SpinnerState *state, pthread_t *thread);
void spinner_stop(SpinnerState *state, pthread_t *thread);

#endif /* BROAI_CORE_H */