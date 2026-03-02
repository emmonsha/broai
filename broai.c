/*
 * broai (ollama): спрашиваю по-русски — получаю bash
 * Провайдер: Ollama (локально, бесплатно, без API-ключей)
 * Модель: qwen2.5:7b
 *
 * Перед запуском:
 *   ollama serve          # в отдельном терминале
 *   ollama pull qwen2.5:7b
 *
 * Компиляция: gcc -o broai broai.c -lcurl -ljson-c -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <curl/curl.h>
#include <json-c/json.h>

#define DEFAULT_URL   "http://localhost:11434/v1/chat/completions"
#define DEFAULT_MODEL "qwen2.5:7b"

/* Переопределить через env при необходимости */
#define API_URL_ENV  "AI_BASH_URL"
#define MODEL_ENV    "AI_BASH_MODEL"

#define MAX_INPUT_LEN 4096
#define MAX_API_KEY_LEN 256
#define MAX_RESPONSE_SIZE (1024 * 1024)
#define LOG_FILE "/tmp/broai_last.log"

#define HISTORY_FILE ".how_history"
#define HISTORY_MAX_ENTRIES 3       /* сколько последних пар question/cmd помним */
#define HISTORY_LINE_MAX 512        /* максимальная длина одной записи в файле */

/* Одна запись истории: вопрос пользователя + команда которую выдал AI */
typedef struct {
    char question[HISTORY_LINE_MAX];
    char command[HISTORY_LINE_MAX];
} HistoryEntry;

/* Контейнер истории */
typedef struct {
    HistoryEntry entries[HISTORY_MAX_ENTRIES];
    int count;
} History;

/* SYSTEM_PROMPT строится динамически при старте — добавляем OS и arch */
static char SYSTEM_PROMPT[512];

static void build_system_prompt(void) {
    struct utsname u;
    const char *os   = "Linux";
    const char *arch = "x86_64";

    if (uname(&u) == 0) {
        os   = u.sysname;   /* "Linux" или "Darwin" */
        arch = u.machine;   /* "x86_64", "arm64", "aarch64"… */
    }

    snprintf(SYSTEM_PROMPT, sizeof(SYSTEM_PROMPT),
        "You are a terminal assistant. The user describes a task in natural language. "
        "Reply with ONLY a bash command — one line, no explanation, no markdown, no backticks. "
        "Multiple commands — use && or |. "
        "OS: %s, arch: %s. "
        "IMPORTANT: use only flags and options that exist on this OS and arch. "
        "If unsure whether a flag exists, prefer the portable POSIX form or omit the flag. "
        "Do NOT invent flags. Do NOT use Linux-only flags on macOS or vice versa.",
        os, arch);
}

struct MemoryStruct {
    char *memory;
    size_t size;
};

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    if (mem->size + realsize > MAX_RESPONSE_SIZE) {
        fprintf(stderr, "Error: response size limit exceeded\n");
        return 0;
    }

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (ptr == NULL) {
        fprintf(stderr, "Error: failed to allocate memory\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

static char *get_env_or_default(const char *env_var, const char *default_val) {
    char *val = getenv(env_var);
    return (val != NULL && strlen(val) > 0) ? val : (char *)default_val;
}

/* Forward declaration */
static char *escape_string(const char *str);
static char *parse_openai_response(const char *json_str);
static char *unescape_cmd(const char *str);

/* Коды причин блокировки команды */
typedef enum {
    CMD_OK            = 0,  /* команда безопасна */
    CMD_DANGEROUS     = 1,  /* деструктивная команда (rm -rf /, dd if=/dev/zero...) */
    CMD_INJECT        = 2,  /* инъекция: $(), ``, ${} */
    CMD_REDIRECT      = 3,  /* перенаправление в системные директории */
    CMD_EXEC          = 4,  /* eval, bash -c, sh -c и т.п. */
    CMD_TOO_LONG      = 5,  /* превышена длина */
} CmdBlockReason;

/* Forward declaration — определение ниже */
static char *escape_string(const char *str);

/* Проверка команды на опасные конструкции.
 * Возвращает CMD_OK если команда безопасна, иначе — причину блокировки. */
static CmdBlockReason validate_command(const char *cmd) {
    if (cmd == NULL || strlen(cmd) == 0) return CMD_TOO_LONG;
    if (strlen(cmd) > MAX_INPUT_LEN) return CMD_TOO_LONG;

    /* Запрет на подстановку команд и переменных */
    if (strstr(cmd, "$(") != NULL) return CMD_INJECT;
    if (strstr(cmd, "`")  != NULL) return CMD_INJECT;
    if (strstr(cmd, "${") != NULL) return CMD_INJECT;

    /* Запрет на перенаправление в системные директории */
    const char *dangerous_redirects[] = {
        ">/etc/", ">>/etc/", "> /etc/",
        ">/boot/", "> /boot/",
        ">/dev/", "> /dev/",
        ">/proc/", "> /proc/",
        ">/sys/", "> /sys/",
        NULL
    };
    for (int i = 0; dangerous_redirects[i] != NULL; i++) {
        if (strstr(cmd, dangerous_redirects[i]) != NULL) return CMD_REDIRECT;
    }

    /* Запрет на выполнение через eval/bash -c */
    const char *exec_patterns[] = {
        "eval ", "bash -c", "sh -c", "zsh -c",
        "python -c", "python3 -c", "perl -e",
        "ruby -e", "node -e",
        NULL
    };
    for (int i = 0; exec_patterns[i] != NULL; i++) {
        if (strstr(cmd, exec_patterns[i]) != NULL) return CMD_EXEC;
    }

    /* Блок-лист деструктивных команд */
    const char *destructive[] = {
        "rm -rf /",
        "rm -rf /*",
        "dd if=/dev/zero",
        "dd if=/dev/random",
        "mkfs",
        "mkswap /dev/",
        ":(){:|:&};:",
        "chmod -R 777 /",
        "chmod 777 /etc",
        "chown -R",
        "shred /dev/",
        "wipefs",
        "parted /dev/",
        "fdisk /dev/",
        "> /dev/sda",
        "mv /* /dev/null",
        NULL
    };
    for (int i = 0; destructive[i] != NULL; i++) {
        if (strstr(cmd, destructive[i]) != NULL) return CMD_DANGEROUS;
    }

    return CMD_OK;
}

/* Печатает причину блокировки команды с цветовым выделением */
static void print_block_reason(CmdBlockReason reason, const char *cmd) {
    switch (reason) {
        case CMD_DANGEROUS:
            fprintf(stderr, "\033[1;33m⚠  Warning: potentially destructive command blocked\033[0m\n");
            break;
        case CMD_INJECT:
            fprintf(stderr, "\033[1;31m✖  Error: command contains dangerous constructs (command substitution)\033[0m\n");
            break;
        case CMD_REDIRECT:
            fprintf(stderr, "\033[1;31m✖  Error: command contains dangerous constructs (redirect to system dir)\033[0m\n");
            break;
        case CMD_EXEC:
            fprintf(stderr, "\033[1;31m✖  Error: command contains dangerous constructs (exec wrapper)\033[0m\n");
            break;
        case CMD_TOO_LONG:
            fprintf(stderr, "\033[1;31m✖  Error: command is too long\033[0m\n");
            break;
        default:
            break;
    }
    if (cmd != NULL && reason != CMD_TOO_LONG) {
        char *escaped = escape_string(cmd);
        fprintf(stderr, "\033[0;33m   Received: %s\033[0m\n", escaped ? escaped : "(null)");
        free(escaped);
    }
}

/* Экранирование строки для безопасного вывода.
 * Возвращает строку выделенную через malloc — вызывающий обязан освободить её. */
static char *escape_string(const char *str) {
    if (str == NULL) return NULL;

    size_t len = strlen(str);
    /* В худшем случае каждый символ удваивается + нулевой байт */
    char *escaped = malloc(len * 2 + 1);
    if (escaped == NULL) {
        fprintf(stderr, "Error: failed to allocate memory for escape_string\n");
        return NULL;
    }

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '"' || str[i] == '\\' || str[i] == '$' || str[i] == '`') {
            escaped[j++] = '\\';
        }
        escaped[j++] = str[i];
    }
    escaped[j] = '\0';
    return escaped;
}

/* Добавляет записи истории в массив messages */
static void add_history_to_messages(struct json_object *messages,
                                    const History *h,
                                    int include_system_role) {
    for (int i = 0; i < h->count; i++) {
        /* Вопрос пользователя из истории */
        struct json_object *u = json_object_new_object();
        json_object_object_add(u, "role", json_object_new_string("user"));
        json_object_object_add(u, "content", json_object_new_string(h->entries[i].question));
        json_object_array_add(messages, u);

        /* Ответ ассистента из истории */
        struct json_object *a = json_object_new_object();
        json_object_object_add(a, "role",
            json_object_new_string(include_system_role ? "assistant" : "assistant"));
        json_object_object_add(a, "content", json_object_new_string(h->entries[i].command));
        json_object_array_add(messages, a);
    }
}

/* Формирование запроса для Ollama (OpenAI-compatible API) */
static char *build_openai_request(const char *question, const char *model,
                                  const History *h) {
    struct json_object *root = json_object_new_object();
    struct json_object *messages = json_object_new_array();

    /* System prompt */
    struct json_object *sys_msg = json_object_new_object();
    json_object_object_add(sys_msg, "role", json_object_new_string("system"));
    json_object_object_add(sys_msg, "content", json_object_new_string(SYSTEM_PROMPT));
    json_object_array_add(messages, sys_msg);

    /* Добавляем историю как контекст */
    add_history_to_messages(messages, h, 1);

    /* Текущий вопрос пользователя */
    struct json_object *user_msg = json_object_new_object();
    json_object_object_add(user_msg, "role", json_object_new_string("user"));
    json_object_object_add(user_msg, "content", json_object_new_string(question));
    json_object_array_add(messages, user_msg);

    json_object_object_add(root, "model", json_object_new_string(model));
    json_object_object_add(root, "temperature", json_object_new_int(0));
    json_object_object_add(root, "max_tokens", json_object_new_int(1024));
    json_object_object_add(root, "messages", messages);

    /* Отключаем режим рассуждений Qwen3 (think=false) */
    json_object_object_add(root, "think", json_object_new_boolean(0));

    char *result = strdup(json_object_get_string(root));
    json_object_put(root);
    return result;
}

/* Парсинг ответа Ollama (OpenAI-compatible): choices[0].message.content
 * Fallback на reasoning — Qwen3 иногда пишет ответ туда. */
static char *parse_openai_response(const char *json_str) {
    struct json_object *response_json = json_tokener_parse(json_str);
    if (response_json == NULL) return NULL;

    char *cmd = NULL;
    struct json_object *choices;

    if (json_object_object_get_ex(response_json, "choices", &choices)) {
        struct json_object *first_choice = json_object_array_get_idx(choices, 0);
        if (first_choice != NULL) {
            struct json_object *message;
            if (json_object_object_get_ex(first_choice, "message", &message)) {
                struct json_object *content;
                /* Сначала пробуем content */
                if (json_object_object_get_ex(message, "content", &content)) {
                    const char *content_str = json_object_get_string(content);
                    if (content_str != NULL && strlen(content_str) > 0) {
                        cmd = strdup(content_str);
                    }
                }
                /* Если content пустой — пробуем извлечь команду из reasoning.
                 * Qwen3 через Ollama игнорирует think=false и пишет ответ
                 * в конце reasoning поля. Берём последнюю непустую строку. */
                if (cmd == NULL || strlen(cmd) == 0) {
                    free(cmd);
                    cmd = NULL;
                    struct json_object *reasoning;
                    if (json_object_object_get_ex(message, "reasoning", &reasoning)) {
                        const char *r = json_object_get_string(reasoning);
                        if (r != NULL && strlen(r) > 0) {
                            /* Ищем последнюю непустую строку в reasoning */
                            char *r_copy = strdup(r);
                            if (r_copy != NULL) {
                                char *last_line = NULL;
                                char *line = strtok(r_copy, "\n");
                                while (line != NULL) {
                                    /* Пропускаем пустые строки и строки без команды */
                                    size_t len = strlen(line);
                                    /* Убираем trailing пробелы */
                                    while (len > 0 && (line[len-1] == ' ' ||
                                           line[len-1] == '\n')) len--;
                                    if (len > 0) last_line = line;
                                    line = strtok(NULL, "\n");
                                }
                                if (last_line != NULL) {
                                    /* Обрезаем до длины len */
                                    size_t llen = strlen(last_line);
                                    while (llen > 0 && (last_line[llen-1] == ' ' ||
                                           last_line[llen-1] == '\n')) llen--;
                                    cmd = malloc(llen + 1);
                                    if (cmd != NULL) {
                                        memcpy(cmd, last_line, llen);
                                        cmd[llen] = '\0';
                                    }
                                }
                                free(r_copy);
                            }
                        }
                    }
                }
            }
        }
    }

    json_object_put(response_json);
    return cmd;
}

/* ============================================
 * EXPLAIN — объяснение команды
 * ============================================ */

static const char *EXPLAIN_PROMPT =
    "You are a terminal assistant. The user gives you a bash command. "
    "Explain what it does — briefly, in the same language the user wrote in, 1-3 sentences. "
    "No markdown, no formatting, plain text only.";

static const char *INFO_PROMPT =
    "You are a terminal assistant. The user asked a question in their language, "
    "and you generated a bash command for them. "
    "Now explain the command in the SAME language as the user's original question. "
    "Output EXACTLY this structure (no markdown, no backticks, plain text):\n"
    "DESCRIPTION: <1-2 sentence description of what this command does>\n"
    "OPTIONS:\n"
    "  <flag>  <what it does>\n"
    "  <flag>  <what it does>\n"
    "  (3-6 most useful flags/options of the main utility)\n"
    "EXAMPLES:\n"
    "  <example command>  # <comment>\n"
    "  <example command>  # <comment>\n"
    "  (2-3 practical examples)\n"
    "WARNINGS: <danger notes if any, or 'none'>";

/* Запрашивает у AI объяснение команды.
 * Возвращает строку через malloc или NULL при ошибке. */
typedef struct {
    volatile int running;  /* флаг остановки: 0 = стоп */
} SpinnerState;

static void *spinner_thread(void *arg) {
    SpinnerState *state = (SpinnerState *)arg;
    const char *frames[] = { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" };
    int nframes = 10;
    int i = 0;

    /* Скрываем курсор */
    fprintf(stderr, "\033[?25l");

    while (state->running) {
        fprintf(stderr, "\r  %s  thinking... ", frames[i % nframes]);
        fflush(stderr);
        i++;
        usleep(80000);  /* 80ms между кадрами */
    }

    /* Очищаем строку спиннера и восстанавливаем курсор */
    fprintf(stderr, "\r\033[K\033[?25h");
    fflush(stderr);

    return NULL;
}

static void spinner_start(SpinnerState *state, pthread_t *thread) {
    state->running = 1;
    pthread_create(thread, NULL, spinner_thread, state);
}

static void spinner_stop(SpinnerState *state, pthread_t *thread) {
    state->running = 0;
    pthread_join(*thread, NULL);
}

static char *get_history_path(void) {
    const char *home = getenv("HOME");
    if (home == NULL) return NULL;

    size_t len = strlen(home) + strlen(HISTORY_FILE) + 2;
    char *path = malloc(len);
    if (path == NULL) return NULL;
    snprintf(path, len, "%s/%s", home, HISTORY_FILE);
    return path;
}

/* Загружает последние HISTORY_MAX_ENTRIES записей из файла истории.
 * Формат файла: чередующиеся строки Q:<вопрос> и A:<команда> */
static History load_history(void) {
    History h = {0};
    char *path = get_history_path();
    if (path == NULL) return h;

    FILE *f = fopen(path, "r");
    free(path);
    if (f == NULL) return h;

    /* Read all entries into temp array */
    HistoryEntry all[64];
    int total = 0;
    char line[HISTORY_LINE_MAX];
    int waiting_answer = 0;

    while (fgets(line, sizeof(line), f) && total < 64) {
        /* Убираем trailing newline */
        size_t ln = strlen(line);
        if (ln > 0 && line[ln - 1] == '\n') line[ln - 1] = '\0';

        if (strncmp(line, "Q:", 2) == 0) {
            strncpy(all[total].question, line + 2, HISTORY_LINE_MAX - 1);
            all[total].question[HISTORY_LINE_MAX - 1] = '\0';
            waiting_answer = 1;
        } else if (strncmp(line, "A:", 2) == 0 && waiting_answer) {
            strncpy(all[total].command, line + 2, HISTORY_LINE_MAX - 1);
            all[total].command[HISTORY_LINE_MAX - 1] = '\0';
            total++;
            waiting_answer = 0;
        }
    }
    fclose(f);

    /* Берём последние HISTORY_MAX_ENTRIES записей */
    int start = total > HISTORY_MAX_ENTRIES ? total - HISTORY_MAX_ENTRIES : 0;
    h.count = total - start;
    for (int i = 0; i < h.count; i++) {
        h.entries[i] = all[start + i];
    }
    return h;
}

/* Сохраняет новую запись в файл истории.
 * Оставляет не более HISTORY_MAX_ENTRIES * 4 строк в файле. */
static void save_history(const char *question, const char *command) {
    if (question == NULL || command == NULL) return;

    char *path = get_history_path();
    if (path == NULL) return;

    /* Читаем существующее содержимое */
    FILE *f = fopen(path, "a");
    if (f == NULL) {
        free(path);
        return;
    }

    /* Убираем переносы строк из вопроса и команды */
    char q[HISTORY_LINE_MAX], a[HISTORY_LINE_MAX];
    strncpy(q, question, HISTORY_LINE_MAX - 1);
    q[HISTORY_LINE_MAX - 1] = '\0';
    strncpy(a, command, HISTORY_LINE_MAX - 1);
    a[HISTORY_LINE_MAX - 1] = '\0';

    for (char *p = q; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    for (char *p = a; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';

    fprintf(f, "Q:%s\nA:%s\n", q, a);
    fclose(f);
    free(path);
}

/* Удаляет markdown-обёртку из ответа LLM.
 * Примеры того что очищается:
 *   ```bash\nfind / -size +100M\n```  -> find / -size +100M
 *   `find / -size +100M`               -> find / -size +100M
 *   ```\nfind / -size +100M\n```      -> find / -size +100M
 * Возвращает новую строку через malloc — вызывающий обязан освободить её. */
static char *strip_markdown(const char *str) {
    if (str == NULL) return NULL;

    const char *p = str;

    /* Убираем <think>...</think> блок — Qwen3 и другие reasoning модели
     * добавляют внутренние рассуждения перед ответом */
    const char *think_open  = "<think>";
    const char *think_close = "</think>";
    if (strncmp(p, think_open, 7) == 0) {
        const char *end_think = strstr(p, think_close);
        if (end_think != NULL) {
            p = end_think + strlen(think_close);
        }
    }

    /* Пропускаем leading пробелы и переносы */
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    /* Убираем блок ```bash\n...\n``` или ```\n...\n``` */
    if (strncmp(p, "```", 3) == 0) {
        p += 3;
        /* Пропускаем опциональный язык (bash, sh, shell) */
        while (*p && *p != '\n') p++;
        /* Пропускаем перенос строки после открывающего ``` */
        if (*p == '\n') p++;

        /* Ищем закрывающий ``` */
        const char *end = strstr(p, "```");
        size_t len;
        if (end != NULL) {
            /* Убираем trailing перенос перед ``` */
            const char *content_end = end;
            while (content_end > p &&
                   (*(content_end - 1) == '\n' || *(content_end - 1) == '\r')) {
                content_end--;
            }
            len = (size_t)(content_end - p);
        } else {
            /* Закрывающий ``` не найден — берём всё что есть */
            len = strlen(p);
        }

        char *result = malloc(len + 1);
        if (result == NULL) return NULL;
        memcpy(result, p, len);
        result[len] = '\0';
        return result;
    }

    /* Убираем одиночные обратные кавычки: `команда` */
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

    /* Нет markdown — возвращаем копию как есть */
    return strdup(p);
}

/* Убирает escape-последовательности которые LLM добавляет в команды:
 *   \\  →  \      (двойной слеш → одинарный)
 *   \"  →  "
 *   \'  →  '
 * Возвращает новую строку через malloc — вызывающий обязан освободить. */
static char *unescape_cmd(const char *str) {
    if (str == NULL) return NULL;

    size_t len = strlen(str);
    char *out = malloc(len + 1);
    if (out == NULL) return NULL;

    size_t i = 0, j = 0;
    while (i < len) {
        if (str[i] == '\\' && i + 1 < len) {
            char next = str[i + 1];
            if (next == '\\' || next == '"' || next == '\'') {
                out[j++] = next;   /* заменяем \\ / \" / \' на сам символ */
                i += 2;
                continue;
            }
        }
        out[j++] = str[i++];
    }
    out[j] = '\0';
    return out;
}

/* Удаляет пробелы по краям строки.
 * Возвращает новую строку через malloc — вызывающий обязан освободить её.
 * (Старая версия возвращала указатель внутрь оригинальной строки,
 * что приводило к неверному free() оригинального указателя.) */
static char *trim_whitespace(const char *str) {
    if (str == NULL) return NULL;

    /* Пропускаем пробелы в начале */
    while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r') str++;

    if (*str == '\0') return strdup("");

    /* Находим конец без хвостовых пробелов */
    const char *end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end--;

    /* Копируем результат в новую строку */
    size_t len = (size_t)(end - str + 1);
    char *result = malloc(len + 1);
    if (result == NULL) {
        fprintf(stderr, "Error: failed to allocate memory for trim_whitespace\n");
        return NULL;
    }
    memcpy(result, str, len);
    result[len] = '\0';
    return result;
}

/* Открывает команду в $EDITOR для редактирования.
 * Writes cmd to a temp file, opens editor,
 * читает результат обратно.
 * Возвращает новую строку через malloc или NULL при ошибке. */
static char *edit_command(const char *cmd) {
    /* Временный файл */
    char tmp_path[] = "/tmp/how_edit_XXXXXX";
    int fd = mkstemp(tmp_path);
    if (fd == -1) {
        fprintf(stderr, "Error: failed to create temp file\n");
        return NULL;
    }

    /* Write command to temp file */
    size_t len = strlen(cmd);
    if (write(fd, cmd, len) != (ssize_t)len ||
        write(fd, "\n", 1) != 1) {
        fprintf(stderr, "Error: failed to write command\n");
        close(fd);
        unlink(tmp_path);
        return NULL;
    }
    close(fd);

    /* Detect editor: $EDITOR → $VISUAL → nano → vi */
    char *editor = getenv("EDITOR");
    if (editor == NULL || strlen(editor) == 0)
        editor = getenv("VISUAL");
    if (editor == NULL || strlen(editor) == 0)
        editor = "nano";

    /* Open editor */
    char edit_cmd[512];
    snprintf(edit_cmd, sizeof(edit_cmd), "%s %s", editor, tmp_path);
    int ret = system(edit_cmd);
    if (ret != 0) {
        fprintf(stderr, "Error: editor exited with code %d\n", ret);
        unlink(tmp_path);
        return NULL;
    }

    /* Read edited file */
    FILE *f = fopen(tmp_path, "r");
    unlink(tmp_path);  /* удаляем сразу после открытия */
    if (f == NULL) {
        fprintf(stderr, "Error: failed to read temp file\n");
        return NULL;
    }

    char buf[MAX_INPUT_LEN];
    size_t nread = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);

    if (nread == 0) {
        fprintf(stderr, "Error: file is empty after editing\n");
        return NULL;
    }
    buf[nread] = '\0';

    /* Strip trailing newline left by editor */
    char *result = trim_whitespace(buf);
    return result;
}

static void log_request(const char *question, const char *request_json,
                        long http_code, const char *response_raw,
                        const char *parsed_cmd) {
    FILE *f = fopen(LOG_FILE, "w");
    if (f == NULL) return;

    /* Время запроса */
    time_t now = time(NULL);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", localtime(&now));

    fprintf(f, "========================================\n");
    fprintf(f, "broai: last request\n");
    fprintf(f, "Time: %s\n", timebuf);
    fprintf(f, "========================================\n\n");

    fprintf(f, "--- User question ---\n");
    fprintf(f, "%s\n\n", question ? question : "(null)");

    fprintf(f, "--- API request (JSON) ---\n");
    fprintf(f, "%s\n\n", request_json ? request_json : "(null)");

    fprintf(f, "--- HTTP response code ---\n");
    fprintf(f, "%ld\n\n", http_code);

    fprintf(f, "--- Raw API response ---\n");
    fprintf(f, "%s\n\n", response_raw ? response_raw : "(null)");

    fprintf(f, "--- Parsed command ---\n");
    fprintf(f, "%s\n", parsed_cmd ? parsed_cmd : "(null)");

    fclose(f);
}

/* ============================================
 * СПИННЕР — крутится пока AI думает
 * ============================================ */

static char *explain_command(const char *cmd) {
    CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;
    struct MemoryStruct chunk;
    char *result = NULL;

    chunk.memory = malloc(1);
    if (chunk.memory == NULL) return NULL;
    chunk.size = 0;

    char *api_url = get_env_or_default(API_URL_ENV, DEFAULT_URL);
    char *model   = get_env_or_default(MODEL_ENV, DEFAULT_MODEL);

    struct json_object *root = json_object_new_object();
    struct json_object *messages = json_object_new_array();

    struct json_object *sys_msg = json_object_new_object();
    json_object_object_add(sys_msg, "role", json_object_new_string("system"));
    json_object_object_add(sys_msg, "content", json_object_new_string(EXPLAIN_PROMPT));
    json_object_array_add(messages, sys_msg);

    struct json_object *user_msg = json_object_new_object();
    json_object_object_add(user_msg, "role", json_object_new_string("user"));
    json_object_object_add(user_msg, "content", json_object_new_string(cmd));
    json_object_array_add(messages, user_msg);

    json_object_object_add(root, "model", json_object_new_string(model));
    json_object_object_add(root, "temperature", json_object_new_int(0));
    json_object_object_add(root, "max_tokens", json_object_new_int(300));
    json_object_object_add(root, "messages", messages);
    json_object_object_add(root, "think", json_object_new_boolean(0));

    char *post_data = strdup(json_object_get_string(root));
    json_object_put(root);

    curl = curl_easy_init();
    if (!curl) { free(chunk.memory); free(post_data); return NULL; }

    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, api_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "broai-ollama/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    SpinnerState spinner_state;
    pthread_t spinner;
    spinner_start(&spinner_state, &spinner);
    res = curl_easy_perform(curl);
    spinner_stop(&spinner_state, &spinner);

    if (res == CURLE_OK) {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code == 200)
            result = parse_openai_response(chunk.memory);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(post_data);
    free(chunk.memory);
    return result;
}

/* ============================================
 * INFO — подробная справка по команде
 * ============================================ */

/* Запрашивает у AI подробную справку: описание, флаги, примеры, предупреждения.
 * Использует INFO_PROMPT. Возвращает строку через malloc или NULL при ошибке. */
static char *info_command(const char *cmd, const char *original_question) {
    CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;
    struct MemoryStruct chunk;
    char *result = NULL;

    chunk.memory = malloc(1);
    if (chunk.memory == NULL) return NULL;
    chunk.size = 0;

    char *api_url = get_env_or_default(API_URL_ENV, DEFAULT_URL);
    char *model   = get_env_or_default(MODEL_ENV, DEFAULT_MODEL);

    struct json_object *root = json_object_new_object();
    struct json_object *messages = json_object_new_array();

    struct json_object *sys_msg = json_object_new_object();
    json_object_object_add(sys_msg, "role", json_object_new_string("system"));
    json_object_object_add(sys_msg, "content", json_object_new_string(INFO_PROMPT));
    json_object_array_add(messages, sys_msg);

    struct json_object *user_msg = json_object_new_object();
    json_object_object_add(user_msg, "role", json_object_new_string("user"));

    /* Передаём оригинальный вопрос + команду, чтобы AI определил язык по вопросу */
    char user_content[MAX_INPUT_LEN * 2];
    if (original_question != NULL && strlen(original_question) > 0) {
        snprintf(user_content, sizeof(user_content),
                 "User question: %s\nCommand: %s", original_question, cmd);
    } else {
        snprintf(user_content, sizeof(user_content), "%s", cmd);
    }
    json_object_object_add(user_msg, "content", json_object_new_string(user_content));
    json_object_array_add(messages, user_msg);

    json_object_object_add(root, "model", json_object_new_string(model));
    json_object_object_add(root, "temperature", json_object_new_int(0));
    json_object_object_add(root, "max_tokens", json_object_new_int(600));
    json_object_object_add(root, "messages", messages);
    json_object_object_add(root, "think", json_object_new_boolean(0));

    char *post_data = strdup(json_object_get_string(root));
    json_object_put(root);

    curl = curl_easy_init();
    if (!curl) { free(chunk.memory); free(post_data); return NULL; }

    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, api_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "broai-ollama/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    SpinnerState spinner_state;
    pthread_t spinner;
    spinner_start(&spinner_state, &spinner);
    res = curl_easy_perform(curl);
    spinner_stop(&spinner_state, &spinner);

    if (res == CURLE_OK) {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code == 200)
            result = parse_openai_response(chunk.memory);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(post_data);
    free(chunk.memory);
    return result;
}

/* Выводит подробную справку по команде с красивым форматированием */
static void print_info(const char *cmd, const char *original_question) {
    printf("\n  \033[1;35m📖  Getting info...\033[0m");
    fflush(stdout);
    char *info = info_command(cmd, original_question);
    if (info == NULL) {
        printf("\n  \033[0;33m(info unavailable)\033[0m\n");
        return;
    }

    printf("\n");
    /* Построчный вывод с отступом и цветовым выделением секций */
    char *info_copy = strdup(info);
    free(info);
    if (info_copy == NULL) return;

    char *line = strtok(info_copy, "\n");
    while (line != NULL) {
        /* Убираем trailing пробелы */
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
            const char *warn_text = line + 9;
            while (*warn_text == ' ') warn_text++;
            if (strcmp(warn_text, "none") != 0 && strcmp(warn_text, "нет") != 0 &&
                strlen(warn_text) > 0) {
                printf("\n  \033[1;31m⚠  WARNINGS\033[0m  %s\n", warn_text);
            }
        } else if (strlen(line) > 0) {
            /* Строки с примерами содержат #-комментарий — подсвечиваем */
            char *hash = strchr(line, '#');
            if (hash != NULL) {
                /* Выводим команду + комментарий разными цветами */
                size_t cmd_len = (size_t)(hash - line);
                printf("  \033[0;32m%.*s\033[0;90m%s\033[0m\n",
                       (int)cmd_len, line, hash);
            } else {
                printf("  \033[0;36m%s\033[0m\n", line);
            }
        }
        line = strtok(NULL, "\n");
    }
    free(info_copy);
    printf("\n");
}

/* ============================================
 * ЛОГИРОВАНИЕ — /tmp/how_last.log
 * ============================================ */


static char *ask(const char *question) {
    CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;
    struct MemoryStruct chunk;
    char *post_data;
    char *cmd = NULL;

    chunk.memory = malloc(1);
    if (chunk.memory == NULL) {
        fprintf(stderr, "Error: failed to allocate memory\n");
        return NULL;
    }
    chunk.size = 0;

    char *api_url = get_env_or_default(API_URL_ENV, DEFAULT_URL);
    char *model   = get_env_or_default(MODEL_ENV, DEFAULT_MODEL);

    /* Загружаем историю предыдущих запросов */
    History history = load_history();
    post_data = build_openai_request(question, model, &history);

    if (post_data == NULL) {
        fprintf(stderr, "Error: failed to build request\n");
        free(chunk.memory);
        return NULL;
    }

    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Error: failed to initialize CURL\n");
        free(chunk.memory);
        free(post_data);
        return NULL;
    }

    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, api_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "broai-ollama/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    /* Запускаем спиннер на время HTTP запроса */
    SpinnerState spinner_state;
    pthread_t spinner;
    spinner_start(&spinner_state, &spinner);

    res = curl_easy_perform(curl);

    spinner_stop(&spinner_state, &spinner);

    if (res != CURLE_OK) {
        fprintf(stderr, "Error: cannot reach Ollama — %s\n", curl_easy_strerror(res));
        fprintf(stderr, "Make sure Ollama is running: ollama serve\n");
        free(chunk.memory);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(post_data);
        return NULL;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        fprintf(stderr, "Error: Ollama returned HTTP %ld\n", http_code);
        if (chunk.size > 0) fprintf(stderr, "Response: %s\n", chunk.memory);
        free(chunk.memory);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(post_data);
        return NULL;
    }

    cmd = parse_openai_response(chunk.memory);
    if (cmd == NULL) fprintf(stderr, "Error: failed to parse response\n");

    log_request(question, post_data, http_code, chunk.memory, cmd);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(post_data);
    free(chunk.memory);

    return cmd;
}

/* Возвращает путь к файлу истории (~/.how_history) */
int main(int argc, char *argv[]) {
    char *question;
    char *cmd;
    char input[16];  /* UTF-8: д/Д = 2 байта + \n + \0 — 16 байт достаточно */

    /* Инициализируем system prompt с OS и архитектурой текущей системы */
    build_system_prompt();

    if (argc < 2) {
        printf("Usage: bro [-e] [-i] \"find files larger than 100mb\"\n");
        printf("  -e  explain the command before executing\n");
        printf("  -i  show detailed info (description, options, examples)\n");
        return 1;
    }

    /* Парсинг флагов -e и -i (можно комбинировать: -e -i или -i -e) */
    int explain_mode = 0;
    int info_mode = 0;
    int arg_start = 1;

    while (arg_start < argc && argv[arg_start][0] == '-') {
        if (strcmp(argv[arg_start], "-e") == 0) {
            explain_mode = 1;
            arg_start++;
        } else if (strcmp(argv[arg_start], "-i") == 0) {
            info_mode = 1;
            arg_start++;
        } else {
            /* Неизвестный флаг — прекращаем парсинг флагов */
            break;
        }
    }

    if (arg_start >= argc) {
        printf("Usage: bro [-e] [-i] \"find files larger than 100mb\"\n");
        printf("  -e  explain the command before executing\n");
        printf("  -i  show detailed info (description, options, examples)\n");
        return 1;
    }

    /* Ограничение на размер входных данных */
    size_t total_len = 0;
    for (int i = arg_start; i < argc; i++) {
        total_len += strlen(argv[i]) + 1;
    }
    
    if (total_len > MAX_INPUT_LEN) {
        fprintf(stderr, "Error: input too long (max %d characters)\n", MAX_INPUT_LEN);
        return 1;
    }

    question = malloc(total_len + 1);
    if (question == NULL) {
        fprintf(stderr, "Error: failed to allocate memory\n");
        return 1;
    }
    question[0] = '\0';
    for (int i = arg_start; i < argc; i++) {
        strcat(question, argv[i]);
        if (i < argc - 1) strcat(question, " ");
    }

    /* Сохраняем копию вопроса для истории — question освобождается после ask() */
    char *original_question = strdup(question);

    cmd = ask(question);
    free(question);

    if (cmd == NULL) {
        fprintf(stderr, "Error: failed to get API response\n");
        free(original_question);
        return 1;
    }

    /* Очищаем markdown-обёртку если LLM вернул ```bash ... ``` */
    char *stripped = strip_markdown(cmd);
    free(cmd);
    cmd = stripped;

    if (cmd == NULL) {
        fprintf(stderr, "Error: failed to process API response\n");
        free(original_question);
        return 1;
    }

    /* trim_whitespace возвращает новую строку через malloc —
     * освобождаем оригинальный и переназначаем */
    char *trimmed = trim_whitespace(cmd);
    free(cmd);
    cmd = trimmed;

    if (cmd == NULL) {
        fprintf(stderr, "Error: failed to process API response\n");
        free(original_question);
        return 1;
    }

    /* Убираем экранирующие слеши которые LLM добавляет в вывод:
     * \\ → \, \" → ", \' → ' */
    char *unescaped = unescape_cmd(cmd);
    free(cmd);
    cmd = unescaped;

    if (cmd == NULL) {
        fprintf(stderr, "Error: failed to process API response\n");
        free(original_question);
        return 1;
    }
    
    /* Валидация — проверяем опасность команды, но не блокируем сразу:
     * сначала показываем explain/info, потом решаем что делать */
    CmdBlockReason block_reason = validate_command(cmd);

    /* Explain mode — показываем краткое объяснение до любых предупреждений */
    if (explain_mode) {
        printf("\n  \033[1;36m?  Getting explanation...\033[0m");
        fflush(stdout);
        char *explanation = explain_command(cmd);
        if (explanation != NULL) {
            char *exp_trimmed = trim_whitespace(explanation);
            free(explanation);
            printf("\n  \033[0;36m💡 %s\033[0m\n", exp_trimmed ? exp_trimmed : "");
            free(exp_trimmed);
        } else {
            printf("\n  \033[0;33m(explanation unavailable)\033[0m\n");
        }
    }

    /* Info mode — показываем подробную справку до любых предупреждений */
    if (info_mode) {
        print_info(cmd, original_question);
    }

    /* Цикл подтверждения — повторяется если пользователь выбрал edit или info */
    while (1) {
        char *escaped = escape_string(cmd);
        printf("\n  \033[1;33m➜  %s\033[0m\n\n", escaped ? escaped : cmd);
        free(escaped);

        if (block_reason != CMD_OK) {
            /* Опасная команда — показываем причину и урезанное меню без y */
            print_block_reason(block_reason, NULL);
            printf("  \033[2m(execution blocked — edit to make it safe)\033[0m\n\n");
            printf("Action? [N/e/i] (e=edit, i=info) ");
        } else {
            printf("Execute? [y/N/e/i] (e=edit, i=info) ");
        }

        if (fgets(input, sizeof(input), stdin) == NULL) break;

        unsigned char b0 = (unsigned char)input[0];
        unsigned char b1 = (unsigned char)input[1];

        /* i/I — показать подробную справку и продолжить цикл */
        if (b0 == 'i' || b0 == 'I') {
            print_info(cmd, original_question);
            continue;
        }

        /* e — открыть в $EDITOR */
        if (b0 == 'e' || b0 == 'E') {
            char *edited = edit_command(cmd);
            if (edited == NULL) {
                fprintf(stderr, "Edit failed, command unchanged\n");
                continue;
            }

            /* Валидируем отредактированную команду */
            CmdBlockReason edited_reason = validate_command(edited);
            if (edited_reason != CMD_OK) {
                print_block_reason(edited_reason, edited);
                free(edited);
                continue;
            }

            /* Команда после редактирования стала безопасной */
            free(cmd);
            cmd = edited;
            block_reason = CMD_OK;
            continue;
        }

        /* y/Y/д/Д — выполнить (только если команда безопасна) */
        if (block_reason != CMD_OK) {
            /* Молча игнорируем y для опасных команд — меню покажется снова */
            if (b0 == '\n' || b0 == 'n' || b0 == 'N') break;
            continue;
        }

        int confirm_ascii = (b0 == 'y' || b0 == 'Y');
        /* д = 0xD0 0xB4, Д = 0xD0 0x94 в UTF-8 */
        int confirm_d_lower = (b0 == 0xD0 && b1 == 0xB4);
        int confirm_d_upper = (b0 == 0xD0 && b1 == 0x94);

        if (confirm_ascii || confirm_d_lower || confirm_d_upper) {
            int ret = system(cmd);
            save_history(original_question, cmd);
            free(cmd);
            free(original_question);
            if (ret == -1) return 1;
            return WEXITSTATUS(ret);
        }

        /* Любой другой ввод — отмена */
        break;
    }

    free(cmd);
    free(original_question);
    return 0;
}