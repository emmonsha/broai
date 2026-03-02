/*
 * broai_core.c — логика приложения
 *
 * Валидация команд, история, спиннер, форматирование вывода, main().
 * Не содержит HTTP-запросов и не знает про провайдеров.
 * Все вызовы AI идут через интерфейс provider.h.
 *
 * Компиляция:
 *   gcc -Wall -Wextra -O2 -o broai broai_core.c provider_ollama.c \
 *       -lcurl -ljson-c -lpthread
 */

#include "broai_core.h"
#include "provider.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>

/* ============================================
 * Forward declarations
 * ============================================ */

static char *escape_string(const char *str);

/* ============================================
 * Коды блокировки + вывод причины
 * ============================================ */

CmdBlockReason validate_command(const char *cmd) {
    if (cmd == NULL || strlen(cmd) == 0) return CMD_TOO_LONG;
    if (strlen(cmd) > MAX_INPUT_LEN)     return CMD_TOO_LONG;

    if (strstr(cmd, "$(") != NULL) return CMD_INJECT;
    if (strstr(cmd, "`")  != NULL) return CMD_INJECT;
    if (strstr(cmd, "${") != NULL) return CMD_INJECT;

    const char *dangerous_redirects[] = {
        ">/etc/", ">>/etc/", "> /etc/",
        ">/boot/", "> /boot/",
        ">/dev/", "> /dev/",
        ">/proc/", "> /proc/",
        ">/sys/", "> /sys/",
        NULL
    };
    for (int i = 0; dangerous_redirects[i] != NULL; i++)
        if (strstr(cmd, dangerous_redirects[i]) != NULL) return CMD_REDIRECT;

    const char *exec_patterns[] = {
        "eval ", "bash -c", "sh -c", "zsh -c",
        "python -c", "python3 -c", "perl -e",
        "ruby -e", "node -e",
        NULL
    };
    for (int i = 0; exec_patterns[i] != NULL; i++)
        if (strstr(cmd, exec_patterns[i]) != NULL) return CMD_EXEC;

    const char *destructive[] = {
        "rm -rf /", "rm -rf /*",
        "dd if=/dev/zero", "dd if=/dev/random",
        "mkfs", "mkswap /dev/",
        ":(){:|:&};:",
        "chmod -R 777 /", "chmod 777 /etc",
        "chown -R",
        "shred /dev/", "wipefs",
        "parted /dev/", "fdisk /dev/",
        "> /dev/sda", "mv /* /dev/null",
        NULL
    };
    for (int i = 0; destructive[i] != NULL; i++)
        if (strstr(cmd, destructive[i]) != NULL) return CMD_DANGEROUS;

    return CMD_OK;
}

void print_block_reason(CmdBlockReason reason, const char *cmd) {
    switch (reason) {
        case CMD_DANGEROUS:
            fprintf(stderr,
                "\033[1;33m⚠  Warning: potentially destructive command blocked\033[0m\n");
            break;
        case CMD_INJECT:
            fprintf(stderr,
                "\033[1;31m✖  Error: dangerous constructs (command substitution)\033[0m\n");
            break;
        case CMD_REDIRECT:
            fprintf(stderr,
                "\033[1;31m✖  Error: dangerous constructs (redirect to system dir)\033[0m\n");
            break;
        case CMD_EXEC:
            fprintf(stderr,
                "\033[1;31m✖  Error: dangerous constructs (exec wrapper)\033[0m\n");
            break;
        case CMD_TOO_LONG:
            fprintf(stderr, "\033[1;31m✖  Error: command is too long\033[0m\n");
            break;
        default:
            break;
    }
    if (cmd != NULL && reason != CMD_TOO_LONG) {
        char *escaped = escape_string(cmd);
        fprintf(stderr, "\033[0;33m   Received: %s\033[0m\n",
                escaped ? escaped : "(null)");
        free(escaped);
    }
}

/* ============================================
 * Вспомогательные функции
 * ============================================ */

char *escape_string(const char *str) {
    if (str == NULL) return NULL;
    size_t len = strlen(str);
    char *escaped = malloc(len * 2 + 1);
    if (escaped == NULL) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '"' || str[i] == '\\' || str[i] == '$' || str[i] == '`')
            escaped[j++] = '\\';
        escaped[j++] = str[i];
    }
    escaped[j] = '\0';
    return escaped;
}

char *trim_whitespace(const char *str) {
    if (str == NULL) return NULL;
    while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r') str++;
    if (*str == '\0') return strdup("");
    const char *end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
        end--;
    size_t len = (size_t)(end - str + 1);
    char *result = malloc(len + 1);
    if (result == NULL) return NULL;
    memcpy(result, str, len);
    result[len] = '\0';
    return result;
}

char *strip_markdown(const char *str) {
    if (str == NULL) return NULL;
    const char *p = str;

    /* Убираем <think>...</think> */
    if (strncmp(p, "<think>", 7) == 0) {
        const char *end_think = strstr(p, "</think>");
        if (end_think != NULL) p = end_think + 8;
    }

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    if (strncmp(p, "```", 3) == 0) {
        p += 3;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        const char *end = strstr(p, "```");
        size_t len;
        if (end != NULL) {
            const char *content_end = end;
            while (content_end > p &&
                   (*(content_end-1) == '\n' || *(content_end-1) == '\r'))
                content_end--;
            len = (size_t)(content_end - p);
        } else {
            len = strlen(p);
        }
        char *result = malloc(len + 1);
        if (result == NULL) return NULL;
        memcpy(result, p, len);
        result[len] = '\0';
        return result;
    }

    if (*p == '`') {
        p++;
        const char *end = strchr(p, '`');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char *result = malloc(len + 1);
        if (result == NULL) return NULL;
        memcpy(result, p, len);
        result[len] = '\0';
        return result;
    }

    return strdup(p);
}

char *edit_command(const char *cmd) {
    char tmp_path[] = "/tmp/broai_edit_XXXXXX";
    int fd = mkstemp(tmp_path);
    if (fd == -1) {
        fprintf(stderr, "Error: failed to create temp file\n");
        return NULL;
    }
    size_t len = strlen(cmd);
    if (write(fd, cmd, len) != (ssize_t)len || write(fd, "\n", 1) != 1) {
        fprintf(stderr, "Error: failed to write command\n");
        close(fd);
        unlink(tmp_path);
        return NULL;
    }
    close(fd);

    char *editor = getenv("EDITOR");
    if (editor == NULL || strlen(editor) == 0) editor = getenv("VISUAL");
    if (editor == NULL || strlen(editor) == 0) editor = "nano";

    char edit_cmd[512];
    snprintf(edit_cmd, sizeof(edit_cmd), "%s %s", editor, tmp_path);
    int ret = system(edit_cmd);
    if (ret != 0) {
        fprintf(stderr, "Error: editor exited with code %d\n", ret);
        unlink(tmp_path);
        return NULL;
    }

    FILE *f = fopen(tmp_path, "r");
    unlink(tmp_path);
    if (f == NULL) { fprintf(stderr, "Error: failed to read temp file\n"); return NULL; }

    char buf[MAX_INPUT_LEN];
    size_t nread = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (nread == 0) { fprintf(stderr, "Error: file is empty after editing\n"); return NULL; }
    buf[nread] = '\0';
    return trim_whitespace(buf);
}

/* ============================================
 * Спиннер
 * ============================================ */

static void *spinner_thread_fn(void *arg) {
    SpinnerState *state = (SpinnerState *)arg;
    const char *frames[] = {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
    int i = 0;
    fprintf(stderr, "\033[?25l");
    while (state->running) {
        fprintf(stderr, "\r  %s  thinking... ", frames[i++ % 10]);
        fflush(stderr);
        usleep(80000);
    }
    fprintf(stderr, "\r\033[K\033[?25h");
    fflush(stderr);
    return NULL;
}

void spinner_start(SpinnerState *state, pthread_t *thread) {
    state->running = 1;
    pthread_create(thread, NULL, spinner_thread_fn, state);
}

void spinner_stop(SpinnerState *state, pthread_t *thread) {
    state->running = 0;
    pthread_join(*thread, NULL);
}

/* ============================================
 * История
 * ============================================ */

static char *get_history_path(void) {
    const char *home = getenv("HOME");
    if (home == NULL) return NULL;
    size_t len = strlen(home) + strlen(HISTORY_FILE) + 2;
    char *path = malloc(len);
    if (path == NULL) return NULL;
    snprintf(path, len, "%s/%s", home, HISTORY_FILE);
    return path;
}

History load_history(void) {
    History h = {0};
    char *path = get_history_path();
    if (path == NULL) return h;

    FILE *f = fopen(path, "r");
    free(path);
    if (f == NULL) return h;

    HistoryEntry all[64];
    int total = 0, waiting = 0;
    char line[HISTORY_LINE_MAX];

    while (fgets(line, sizeof(line), f) && total < 64) {
        size_t ln = strlen(line);
        if (ln > 0 && line[ln-1] == '\n') line[ln-1] = '\0';

        if (strncmp(line, "Q:", 2) == 0) {
            strncpy(all[total].question, line + 2, HISTORY_LINE_MAX - 1);
            all[total].question[HISTORY_LINE_MAX-1] = '\0';
            waiting = 1;
        } else if (strncmp(line, "A:", 2) == 0 && waiting) {
            strncpy(all[total].command, line + 2, HISTORY_LINE_MAX - 1);
            all[total].command[HISTORY_LINE_MAX-1] = '\0';
            total++;
            waiting = 0;
        }
    }
    fclose(f);

    int start = total > HISTORY_MAX_ENTRIES ? total - HISTORY_MAX_ENTRIES : 0;
    h.count = total - start;
    for (int i = 0; i < h.count; i++)
        h.entries[i] = all[start + i];
    return h;
}

void save_history(const char *question, const char *command) {
    if (question == NULL || command == NULL) return;
    char *path = get_history_path();
    if (path == NULL) return;

    FILE *f = fopen(path, "a");
    if (f == NULL) { free(path); return; }

    char q[HISTORY_LINE_MAX], a[HISTORY_LINE_MAX];
    strncpy(q, question, HISTORY_LINE_MAX - 1); q[HISTORY_LINE_MAX-1] = '\0';
    strncpy(a, command,  HISTORY_LINE_MAX - 1); a[HISTORY_LINE_MAX-1] = '\0';
    for (char *p = q; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    for (char *p = a; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';

    fprintf(f, "Q:%s\nA:%s\n", q, a);
    fclose(f);
    free(path);
}

/* ============================================
 * Форматированный вывод info
 * ============================================ */

void print_info_formatted(const char *info_text) {
    if (info_text == NULL) {
        printf("\n  \033[0;33m(info unavailable)\033[0m\n");
        return;
    }

    printf("\n");
    char *copy = strdup(info_text);
    if (copy == NULL) return;

    char *line = strtok(copy, "\n");
    while (line != NULL) {
        size_t ln = strlen(line);
        while (ln > 0 && (line[ln-1] == ' ' || line[ln-1] == '\r')) ln--;
        line[ln] = '\0';

        if (strncmp(line, "DESCRIPTION:", 12) == 0) {
            printf("  \033[1;37mDESCRIPTION\033[0m%s\n", line + 11);
        } else if (strncmp(line, "OPTIONS:", 8) == 0) {
            printf("\n  \033[1;37mOPTIONS\033[0m\n");
        } else if (strncmp(line, "EXAMPLES:", 9) == 0) {
            printf("\n  \033[1;37mEXAMPLES\033[0m\n");
        } else if (strncmp(line, "WARNINGS:", 9) == 0) {
            const char *w = line + 9;
            while (*w == ' ') w++;
            if (strcmp(w, "none") != 0 && strcmp(w, "нет") != 0 && *w != '\0')
                printf("\n  \033[1;31m⚠  WARNINGS\033[0m  %s\n", w);
        } else if (ln > 0) {
            char *hash = strchr(line, '#');
            if (hash != NULL) {
                printf("  \033[0;32m%.*s\033[0;90m%s\033[0m\n",
                       (int)(hash - line), line, hash);
            } else {
                printf("  \033[0;36m%s\033[0m\n", line);
            }
        }
        line = strtok(NULL, "\n");
    }
    free(copy);
    printf("\n");
}

/* ============================================
 * Вывод info через провайдер
 * ============================================ */

static void show_info(const char *cmd, const char *original_question) {
    printf("\n  \033[1;35m📖  Getting info...\033[0m");
    fflush(stdout);
    char *info = provider_info(cmd, original_question);
    print_info_formatted(info);
    free(info);
}

/* ============================================
 * main
 * ============================================ */

int main(int argc, char *argv[]) {
    char input[16];

    if (argc < 2) {
        printf("Usage: bro [-e] [-i] \"find files larger than 100mb\"\n");
        printf("  -e  explain the command before executing\n");
        printf("  -i  show detailed info (description, options, examples)\n");
        return 1;
    }

    /* Парсинг флагов -e и -i */
    int explain_mode = 0, info_mode = 0, arg_start = 1;
    while (arg_start < argc && argv[arg_start][0] == '-') {
        if      (strcmp(argv[arg_start], "-e") == 0) { explain_mode = 1; arg_start++; }
        else if (strcmp(argv[arg_start], "-i") == 0) { info_mode    = 1; arg_start++; }
        else break;
    }

    if (arg_start >= argc) {
        printf("Usage: bro [-e] [-i] \"question\"\n");
        return 1;
    }

    /* Собираем вопрос из оставшихся аргументов */
    size_t total_len = 0;
    for (int i = arg_start; i < argc; i++) total_len += strlen(argv[i]) + 1;

    if (total_len > MAX_INPUT_LEN) {
        fprintf(stderr, "Error: input too long (max %d characters)\n", MAX_INPUT_LEN);
        return 1;
    }

    char *question = malloc(total_len + 1);
    if (question == NULL) { fprintf(stderr, "Error: failed to allocate memory\n"); return 1; }
    question[0] = '\0';
    for (int i = arg_start; i < argc; i++) {
        strcat(question, argv[i]);
        if (i < argc - 1) strcat(question, " ");
    }

    char *original_question = strdup(question);

    /* Загружаем историю и запрашиваем команду */
    History history = load_history();
    char *cmd = provider_ask(question, &history);
    free(question);

    if (cmd == NULL) {
        fprintf(stderr, "Error: failed to get response\n");
        free(original_question);
        return 1;
    }

    /* Очищаем markdown и пробелы */
    char *tmp = strip_markdown(cmd); free(cmd); cmd = tmp;
    if (cmd == NULL) { fprintf(stderr, "Error: failed to process response\n"); free(original_question); return 1; }
    tmp = trim_whitespace(cmd); free(cmd); cmd = tmp;
    if (cmd == NULL) { fprintf(stderr, "Error: failed to process response\n"); free(original_question); return 1; }

    /* Валидируем до вывода, но не выходим сразу */
    CmdBlockReason block_reason = validate_command(cmd);

    /* Explain — показываем до предупреждений */
    if (explain_mode) {
        printf("\n  \033[1;36m?  Getting explanation...\033[0m");
        fflush(stdout);
        char *explanation = provider_explain(cmd);
        if (explanation != NULL) {
            char *exp = trim_whitespace(explanation); free(explanation);
            printf("\n  \033[0;36m💡 %s\033[0m\n", exp ? exp : "");
            free(exp);
        } else {
            printf("\n  \033[0;33m(explanation unavailable)\033[0m\n");
        }
    }

    /* Info — показываем до предупреждений */
    if (info_mode) {
        show_info(cmd, original_question);
    }

    /* Цикл подтверждения */
    while (1) {
        char *escaped = escape_string(cmd);
        printf("\n  \033[1;33m➜  %s\033[0m\n\n", escaped ? escaped : cmd);
        free(escaped);

        if (block_reason != CMD_OK) {
            print_block_reason(block_reason, NULL);
            printf("  \033[2m(execution blocked — edit to make it safe)\033[0m\n\n");
            printf("Action? [N/e/i] (e=edit, i=info) ");
        } else {
            printf("Execute? [y/N/e/i] (e=edit, i=info) ");
        }

        if (fgets(input, sizeof(input), stdin) == NULL) break;

        unsigned char b0 = (unsigned char)input[0];
        unsigned char b1 = (unsigned char)input[1];

        /* i — подробная справка */
        if (b0 == 'i' || b0 == 'I') {
            show_info(cmd, original_question);
            continue;
        }

        /* e — редактор */
        if (b0 == 'e' || b0 == 'E') {
            char *edited = edit_command(cmd);
            if (edited == NULL) { fprintf(stderr, "Edit failed\n"); continue; }

            CmdBlockReason edited_reason = validate_command(edited);
            if (edited_reason != CMD_OK) {
                print_block_reason(edited_reason, edited);
                free(edited);
                continue;
            }
            free(cmd);
            cmd = edited;
            block_reason = CMD_OK;
            continue;
        }

        /* Для опасных команд y не работает */
        if (block_reason != CMD_OK) {
            if (b0 == '\n' || b0 == 'n' || b0 == 'N') break;
            continue;
        }

        /* y/Y/д/Д — выполнить */
        int confirm = (b0 == 'y' || b0 == 'Y')
                   || (b0 == 0xD0 && b1 == 0xB4)   /* д */
                   || (b0 == 0xD0 && b1 == 0x94);   /* Д */

        if (confirm) {
            int ret = system(cmd);
            save_history(original_question, cmd);
            free(cmd);
            free(original_question);
            return (ret == -1) ? 1 : WEXITSTATUS(ret);
        }

        break;
    }

    free(cmd);
    free(original_question);
    return 0;
}