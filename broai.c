/*
 * broai_qwen: спрашиваю по-русски — получаю bash
 * Только Ollama + Qwen, без API ключа
 * Компиляция: gcc -o bro broai_qwen.c -lcurl -ljson-c -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/wait.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <curl/curl.h>
#include <json-c/json.h>

#define OLLAMA_URL   "http://localhost:11434/v1/chat/completions"
#define QWEN_MODEL   "qwen2.5:7b"

#define MAX_INPUT_LEN    4096
#define MAX_RESPONSE     (1024 * 1024)
#define MAX_HELP_LEN     8192
#define LOG_FILE         "/tmp/broai_last.log"

#define HISTORY_FILE         ".how_history"
#define HISTORY_MAX_ENTRIES  3
#define HISTORY_LINE_MAX     512

/* ── ANSI цвета ──────────────────────────────────────────────────────────── */
#define CLR_RESET   "\033[0m"
#define CLR_RED     "\033[1;31m"
#define CLR_YELLOW  "\033[1;33m"
#define CLR_GREEN   "\033[0;32m"
#define CLR_CYAN    "\033[0;36m"
#define CLR_BCYAN   "\033[1;36m"
#define CLR_WHITE   "\033[1;37m"
#define CLR_GRAY    "\033[0;90m"

#define ERR(msg)        fprintf(stderr, CLR_RED    "Error: "   CLR_RESET msg "\n")
#define ERRF(fmt, ...)  fprintf(stderr, CLR_RED    "Error: "   CLR_RESET fmt "\n", __VA_ARGS__)
#define WARN(msg)       fprintf(stderr, CLR_YELLOW "Warning: " CLR_RESET msg "\n")

/* ── История ─────────────────────────────────────────────────────────────── */
typedef struct {
    char question[HISTORY_LINE_MAX];
    char command[HISTORY_LINE_MAX];
} HistoryEntry;

typedef struct {
    HistoryEntry entries[HISTORY_MAX_ENTRIES];
    int count;
} History;

/* ── Глобальный кеш help-текста для explain ─────────────────────────────── */
static char *g_last_help_text = NULL;
static char  g_last_utility[64] = {0};

/* ── Промпты ─────────────────────────────────────────────────────────────── */
static const char *SYSTEM_PROMPT_FMT =
    "You are a terminal assistant. The user describes a task in natural language. "
    "Reply with ONLY a bash command — one line, no explanations, no markdown, no backticks. "
    "Do NOT use backslash-escaping of spaces — use quotes around paths with spaces instead. "
    "Use ONLY flags and options that exist in the provided command help output. "
    "The user is running %s — generate commands compatible with this OS and version. "
    "Multiple commands — use && or |.";

static const char *EXPLAIN_PROMPT_FMT =
    "You are a terminal assistant. "
    "The user originally asked: \"%s\" "
    "You must respond in the SAME language as that question (if Russian — answer in Russian, if English — in English). "
    "Explain the following bash command using EXACTLY this structure — no extra text, no markdown:\n"
    "ABOUT: <one sentence: what this command does, like whatis output>\n"
    "EXPLAIN: <2-3 sentences explaining how the command works in this specific invocation>\n"
    "FLAGS:\n"
    "<flag>  <what it does>\n"
    "<flag>  <what it does>\n"
    "EXAMPLES:\n"
    "<example command>  # <comment>\n"
    "<example command>  # <comment>\n"
    "Use plain text only. No backticks, no bullet points, no dashes before flags.";

/* ── HTTP буфер ──────────────────────────────────────────────────────────── */
struct Buf { char *data; size_t size; };

static size_t write_cb(void *ptr, size_t size, size_t n, void *userp) {
    size_t real = size * n;
    struct Buf *b = userp;
    if (b->size + real > MAX_RESPONSE) return 0;
    char *tmp = realloc(b->data, b->size + real + 1);
    if (!tmp) return 0;
    b->data = tmp;
    memcpy(b->data + b->size, ptr, real);
    b->size += real;
    b->data[b->size] = 0;
    return real;
}

/* ── Spinner ─────────────────────────────────────────────────────────────── */
typedef struct { volatile int running; } SpinnerState;

static void *spinner_thread(void *arg) {
    SpinnerState *s = arg;
    const char *frames[] = {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
    int i = 0;
    fprintf(stderr, "\033[?25l");
    while (s->running) {
        fprintf(stderr, "\r  %s  thinking... ", frames[i++ % 10]);
        fflush(stderr);
        usleep(80000);
    }
    fprintf(stderr, "\r\033[K\033[?25h");
    fflush(stderr);
    return NULL;
}

static void spinner_start(SpinnerState *s, pthread_t *t) {
    s->running = 1;
    pthread_create(t, NULL, spinner_thread, s);
}

static void spinner_stop(SpinnerState *s, pthread_t *t) {
    s->running = 0;
    pthread_join(*t, NULL);
}

/* ── OS ──────────────────────────────────────────────────────────────────── */
static const char *get_os_name(void) {
    static char os_buf[64] = {0};
    if (os_buf[0]) return os_buf;
    FILE *fp = popen("uname -s 2>/dev/null", "r");
    if (fp) {
        if (fgets(os_buf, sizeof(os_buf), fp)) {
            size_t n = strlen(os_buf);
            if (n > 0 && os_buf[n-1] == '\n') os_buf[n-1] = '\0';
        }
        pclose(fp);
    }
    if (!os_buf[0]) strncpy(os_buf, "Linux", sizeof(os_buf)-1);
    return os_buf;
}

static char *build_system_prompt(void) {
    const char *os = get_os_name();
    size_t len = strlen(SYSTEM_PROMPT_FMT) + strlen(os) + 1;
    char *buf = malloc(len);
    if (!buf) return NULL;
    snprintf(buf, len, SYSTEM_PROMPT_FMT, os);
    return buf;
}

/* ── Безопасность ────────────────────────────────────────────────────────── */
static int validate_command(const char *cmd) {
    if (!cmd || !*cmd || strlen(cmd) > MAX_INPUT_LEN) return 0;

    if (strstr(cmd, "$(") || strstr(cmd, "`") || strstr(cmd, "${")) return 0;

    const char *bad_redirects[] = {
        ">/etc/", ">>/etc/", "> /etc/", ">/boot/", ">/dev/",
        ">/proc/", ">/sys/", NULL
    };
    for (int i = 0; bad_redirects[i]; i++)
        if (strstr(cmd, bad_redirects[i])) return 0;

    const char *exec_patterns[] = {
        "eval ", "bash -c", "sh -c", "zsh -c",
        "python -c", "python3 -c", "perl -e", "ruby -e", "node -e", NULL
    };
    for (int i = 0; exec_patterns[i]; i++)
        if (strstr(cmd, exec_patterns[i])) return 0;

    const char *destructive[] = {
        "rm -rf /", "rm -rf /*", "dd if=/dev/zero", "dd if=/dev/random",
        "mkfs", "mkswap /dev/", ":(){:|:&};:", "chmod -R 777 /",
        "chmod 777 /etc", "chown -R", "shred /dev/", "wipefs",
        "parted /dev/", "fdisk /dev/", "> /dev/sda", "mv /* /dev/null", NULL
    };
    for (int i = 0; destructive[i]; i++) {
        if (strstr(cmd, destructive[i])) {
            WARN("potentially destructive command blocked");
            return 0;
        }
    }
    return 1;
}

static char *escape_string(const char *str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char *out = malloc(len * 2 + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '"' || str[i] == '\\' || str[i] == '$' || str[i] == '`')
            out[j++] = '\\';
        out[j++] = str[i];
    }
    out[j] = '\0';
    return out;
}

/* ── Строковые утилиты ───────────────────────────────────────────────────── */
static char *trim_whitespace(const char *str) {
    if (!str) return NULL;
    while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r') str++;
    if (!*str) return strdup("");
    const char *end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end--;
    size_t len = (size_t)(end - str + 1);
    char *r = malloc(len + 1);
    if (!r) return NULL;
    memcpy(r, str, len);
    r[len] = '\0';
    return r;
}

static char *strip_markdown(const char *str) {
    if (!str) return NULL;
    const char *p = str;

    /* <think>...</think> */
    if (strncmp(p, "<think>", 7) == 0) {
        const char *end = strstr(p, "</think>");
        if (end) p = end + 8;
    }
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    /* ```bash\n...\n``` */
    if (strncmp(p, "```", 3) == 0) {
        p += 3;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        const char *end = strstr(p, "```");
        const char *ce = end ? end : p + strlen(p);
        while (ce > p && (*(ce-1) == '\n' || *(ce-1) == '\r')) ce--;
        size_t len = (size_t)(ce - p);
        char *r = malloc(len + 1);
        if (!r) return NULL;
        memcpy(r, p, len);
        r[len] = '\0';
        return r;
    }

    /* `команда` */
    if (*p == '`') {
        p++;
        const char *end = strchr(p, '`');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char *r = malloc(len + 1);
        if (!r) return NULL;
        memcpy(r, p, len);
        r[len] = '\0';
        return r;
    }

    return strdup(p);
}

static char *unescape_shell_spaces(const char *str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char *r = malloc(len + 1);
    if (!r) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '\\' && i + 1 < len && str[i+1] == ' ') continue;
        r[j++] = str[i];
    }
    r[j] = '\0';
    return r;
}

/* ── История ─────────────────────────────────────────────────────────────── */
static char *get_history_path(void) {
    const char *home = getenv("HOME");
    if (!home) return NULL;
    size_t len = strlen(home) + strlen(HISTORY_FILE) + 2;
    char *path = malloc(len);
    if (!path) return NULL;
    snprintf(path, len, "%s/%s", home, HISTORY_FILE);
    return path;
}

static History load_history(void) {
    History h = {0};
    char *path = get_history_path();
    if (!path) return h;
    FILE *f = fopen(path, "r");
    free(path);
    if (!f) return h;

    HistoryEntry all[64];
    int total = 0, waiting = 0;
    char line[HISTORY_LINE_MAX];

    while (fgets(line, sizeof(line), f) && total < 64) {
        size_t ln = strlen(line);
        if (ln > 0 && line[ln-1] == '\n') line[ln-1] = '\0';
        if (strncmp(line, "Q:", 2) == 0) {
            strncpy(all[total].question, line+2, HISTORY_LINE_MAX-1);
            all[total].question[HISTORY_LINE_MAX-1] = '\0';
            waiting = 1;
        } else if (strncmp(line, "A:", 2) == 0 && waiting) {
            strncpy(all[total].command, line+2, HISTORY_LINE_MAX-1);
            all[total].command[HISTORY_LINE_MAX-1] = '\0';
            total++;
            waiting = 0;
        }
    }
    fclose(f);

    int start = total > HISTORY_MAX_ENTRIES ? total - HISTORY_MAX_ENTRIES : 0;
    h.count = total - start;
    for (int i = 0; i < h.count; i++) h.entries[i] = all[start + i];
    return h;
}

static void save_history(const char *question, const char *command) {
    if (!question || !command) return;
    char *path = get_history_path();
    if (!path) return;
    FILE *f = fopen(path, "a");
    if (!f) { free(path); return; }

    char q[HISTORY_LINE_MAX], a[HISTORY_LINE_MAX];
    strncpy(q, question, HISTORY_LINE_MAX-1); q[HISTORY_LINE_MAX-1] = '\0';
    strncpy(a, command,  HISTORY_LINE_MAX-1); a[HISTORY_LINE_MAX-1] = '\0';
    for (char *p = q; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    for (char *p = a; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';

    fprintf(f, "Q:%s\nA:%s\n", q, a);
    fclose(f);
    free(path);
}

/* ── Help context ────────────────────────────────────────────────────────── */
static int cmd_exists(const char *name) {
    if (!name || !*name) return 0;
    for (const char *p = name; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') return 0;
    char shell_cmd[128];
    snprintf(shell_cmd, sizeof(shell_cmd), "which %s >/dev/null 2>&1", name);
    return system(shell_cmd) == 0;
}

static const char *extract_utility_from_question(const char *question) {
    static const struct { const char *kw; const char *cmd; } hints[] = {
        {"файл","find"},{"find","find"},{"файлы","find"},{"files","find"},
        {"поиск","find"},{"search","find"},
        {"cpu","top"},{"нагрузка","top"},{"загрузка","top"},{"monitor","top"},
        {"процесс","ps"},{"process","ps"},{"процессы","ps"},{"processes","ps"},
        {"убить","kill"},{"kill","kill"},{"топ","top"},{"top","top"},
        {"память","free"},{"memory","free"},
        {"диск","df"},{"disk","df"},{"место","df"},{"space","df"},
        {"размер","du"},{"size","du"},
        {"сеть","ss"},{"network","ss"},{"порт","ss"},{"port","ss"},
        {"скачать","curl"},{"download","curl"},
        {"архив","tar"},{"archive","tar"},{"сжать","tar"},{"compress","tar"},
        {"распаковать","tar"},{"extract","tar"},
        {"права","chmod"},{"permission","chmod"},
        {"владелец","chown"},{"owner","chown"},
        {"строки","grep"},{"grep","grep"},{"найди","grep"},
        {"сортировка","sort"},{"sort","sort"},
        {"дата","date"},{"date","date"},{"время","date"},
        {"пользователь","id"},{"user","id"},
        {"скопировать","cp"},{"copy","cp"},
        {"переместить","mv"},{"move","mv"},
        {"удалить","rm"},{"delete","rm"},
        {"содержимое","ls"},{"list","ls"},{"список","ls"},{"показать","ls"},
        {NULL, NULL}
    };

    if (!question) return NULL;

    char lower[MAX_INPUT_LEN];
    strncpy(lower, question, MAX_INPUT_LEN-1);
    lower[MAX_INPUT_LEN-1] = '\0';
    for (char *p = lower; *p; p++) *p = (char)tolower((unsigned char)*p);

    for (int i = 0; hints[i].kw; i++)
        if (strstr(lower, hints[i].kw)) return hints[i].cmd;

    /* fallback: which */
    static char found[64];
    const char *p = question;
    while (*p) {
        while (*p && !isalpha((unsigned char)*p)) p++;
        if (!*p) break;
        size_t wlen = 0;
        const char *ws = p;
        while (*p && (isalnum((unsigned char)*p) || *p=='-' || *p=='_')) { p++; wlen++; }
        if (wlen >= 2 && wlen <= 32) {
            memcpy(found, ws, wlen);
            found[wlen] = '\0';
            if (cmd_exists(found)) return found;
        }
    }
    return NULL;
}

static char *run_and_read(const char *shell_cmd, size_t min_bytes) {
    FILE *fp = popen(shell_cmd, "r");
    if (!fp) return NULL;
    char *buf = malloc(MAX_HELP_LEN + 1);
    if (!buf) { pclose(fp); return NULL; }
    size_t n = fread(buf, 1, MAX_HELP_LEN, fp);
    pclose(fp);
    if (n < min_bytes) { free(buf); return NULL; }
    buf[n] = '\0';
    return buf;
}

static char *get_cmd_help(const char *name) {
    if (!name) return NULL;
    for (const char *p = name; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') return NULL;

    char sc[256];
    char *buf;

    snprintf(sc, sizeof(sc), "%s --help 2>&1 | head -c %d", name, MAX_HELP_LEN);
    if ((buf = run_and_read(sc, 20))) return buf;

    snprintf(sc, sizeof(sc), "MANPAGER=cat man %s 2>/dev/null | col -bx | head -c %d", name, MAX_HELP_LEN);
    if ((buf = run_and_read(sc, 20))) return buf;

    snprintf(sc, sizeof(sc), "man -P cat %s 2>/dev/null | col -bx | head -c %d", name, MAX_HELP_LEN);
    return run_and_read(sc, 20);
}

static char *build_question_with_help(const char *question) {
    const char *utility = extract_utility_from_question(question);
    if (!utility) return strdup(question);

    char *help_text = get_cmd_help(utility);
    if (!help_text) return strdup(question);

    if (strlen(help_text) > 3000) help_text[3000] = '\0';

    size_t total = strlen(question) + strlen(help_text) + 256;
    char *result = malloc(total);
    if (!result) { free(help_text); return strdup(question); }

    snprintf(result, total,
        "%s\n\n"
        "[Context: help output for '%s' on this system — use ONLY these flags/options:\n"
        "---\n%s\n---]",
        question, utility, help_text);

    free(g_last_help_text);
    g_last_help_text = help_text;
    strncpy(g_last_utility, utility, sizeof(g_last_utility)-1);
    g_last_utility[sizeof(g_last_utility)-1] = '\0';

    return result;
}

/* ── Редактор ────────────────────────────────────────────────────────────── */
static char *edit_command(const char *cmd) {
    char tmp_path[] = "/tmp/bro_edit_XXXXXX";
    int fd = mkstemp(tmp_path);
    if (fd == -1) { ERR("failed to create temp file"); return NULL; }

    size_t len = strlen(cmd);
    if (write(fd, cmd, len) != (ssize_t)len || write(fd, "\n", 1) != 1) {
        ERR("failed to write command");
        close(fd); unlink(tmp_path); return NULL;
    }
    close(fd);

    char *editor = getenv("EDITOR");
    if (!editor || !*editor) editor = getenv("VISUAL");
    if (!editor || !*editor) editor = "nano";

    char edit_cmd[512];
    snprintf(edit_cmd, sizeof(edit_cmd), "%s %s", editor, tmp_path);
    int ret = system(edit_cmd);
    if (ret != 0) { ERRF("editor exited with code %d", ret); unlink(tmp_path); return NULL; }

    FILE *f = fopen(tmp_path, "r");
    unlink(tmp_path);
    if (!f) { ERR("failed to read temp file"); return NULL; }

    char buf[MAX_INPUT_LEN];
    size_t nread = fread(buf, 1, sizeof(buf)-1, f);
    fclose(f);
    if (!nread) { ERR("file is empty after editing"); return NULL; }
    buf[nread] = '\0';
    return trim_whitespace(buf);
}

/* ── Лог ─────────────────────────────────────────────────────────────────── */
static void log_request(const char *question, const char *effective_q,
                        const char *request_json, long http_code,
                        const char *response_raw, const char *parsed_cmd) {
    FILE *f = fopen(LOG_FILE, "w");
    if (!f) return;
    time_t now = time(NULL);
    char tb[32];
    strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(f, "========================================\n");
    fprintf(f, "broai_qwen: last request — %s\n", tb);
    fprintf(f, "========================================\n\n");
    fprintf(f, "--- Question ---\n%s\n\n", question ? question : "(null)");
    fprintf(f, "--- OS ---\n%s\n\n", get_os_name());
    fprintf(f, "--- Effective question (with help) ---\n%s\n\n", effective_q ? effective_q : "(same)");
    fprintf(f, "--- Request JSON ---\n%s\n\n", request_json ? request_json : "(null)");
    fprintf(f, "--- HTTP ---\n%ld\n\n", http_code);
    fprintf(f, "--- Raw response ---\n%s\n\n", response_raw ? response_raw : "(null)");
    fprintf(f, "--- Parsed command ---\n%s\n", parsed_cmd ? parsed_cmd : "(null)");
    fclose(f);
}

/* ── Парсинг ответа ──────────────────────────────────────────────────────── */
static char *parse_response(const char *json_str) {
    struct json_object *resp = json_tokener_parse(json_str);
    if (!resp) return NULL;

    char *cmd = NULL;
    struct json_object *choices;
    if (json_object_object_get_ex(resp, "choices", &choices)) {
        struct json_object *c0 = json_object_array_get_idx(choices, 0);
        struct json_object *msg, *content;
        if (c0 && json_object_object_get_ex(c0, "message", &msg)) {
            if (json_object_object_get_ex(msg, "content", &content)) {
                const char *s = json_object_get_string(content);
                if (s && *s) cmd = strdup(s);
            }
            /* Fallback: reasoning (Qwen3 иногда пишет ответ туда) */
            if (!cmd || !*cmd) {
                free(cmd); cmd = NULL;
                struct json_object *reasoning;
                if (json_object_object_get_ex(msg, "reasoning", &reasoning)) {
                    const char *r = json_object_get_string(reasoning);
                    if (r && *r) {
                        char *rc = strdup(r);
                        if (rc) {
                            char *last = NULL, *line = strtok(rc, "\n");
                            while (line) {
                                size_t l = strlen(line);
                                while (l > 0 && (line[l-1]==' '||line[l-1]=='\n')) l--;
                                if (l > 0) last = line;
                                line = strtok(NULL, "\n");
                            }
                            if (last) {
                                size_t l = strlen(last);
                                while (l > 0 && (last[l-1]==' '||last[l-1]=='\n')) l--;
                                cmd = malloc(l+1);
                                if (cmd) { memcpy(cmd, last, l); cmd[l] = '\0'; }
                            }
                            free(rc);
                        }
                    }
                }
            }
        }
    }
    json_object_put(resp);
    return cmd;
}

/* ── HTTP запрос к Ollama ────────────────────────────────────────────────── */
static char *do_request(const char *post_data) {
    struct Buf buf = { malloc(1), 0 };
    CURL *curl = curl_easy_init();
    if (!curl) { free(buf.data); return NULL; }

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,            OLLAMA_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     post_data);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      "broai-qwen/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        120L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    SpinnerState sp; pthread_t thr;
    spinner_start(&sp, &thr);
    CURLcode res = curl_easy_perform(curl);
    spinner_stop(&sp, &thr);

    char *result = NULL;
    if (res == CURLE_OK) {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        if (code == 200) {
            result = strdup(buf.data);
        } else {
            ERRF("HTTP %ld", code);
            if (buf.size > 0)
                fprintf(stderr, CLR_GRAY "  %s" CLR_RESET "\n", buf.data);
        }
    } else {
        ERRF("CURL: %s", curl_easy_strerror(res));
    }

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    free(buf.data);
    return result;
}

/* ── Формирование запроса ────────────────────────────────────────────────── */
static void add_history(struct json_object *messages, const History *h) {
    for (int i = 0; i < h->count; i++) {
        struct json_object *u = json_object_new_object();
        json_object_object_add(u, "role",    json_object_new_string("user"));
        json_object_object_add(u, "content", json_object_new_string(h->entries[i].question));
        json_object_array_add(messages, u);

        struct json_object *a = json_object_new_object();
        json_object_object_add(a, "role",    json_object_new_string("assistant"));
        json_object_object_add(a, "content", json_object_new_string(h->entries[i].command));
        json_object_array_add(messages, a);
    }
}

static char *build_request(const char *question, const History *h) {
    struct json_object *root     = json_object_new_object();
    struct json_object *messages = json_object_new_array();

    char *sys_prompt = build_system_prompt();
    struct json_object *sys = json_object_new_object();
    json_object_object_add(sys, "role",    json_object_new_string("system"));
    json_object_object_add(sys, "content", json_object_new_string(sys_prompt ? sys_prompt : ""));
    json_object_array_add(messages, sys);
    free(sys_prompt);

    add_history(messages, h);

    struct json_object *usr = json_object_new_object();
    json_object_object_add(usr, "role",    json_object_new_string("user"));
    json_object_object_add(usr, "content", json_object_new_string(question));
    json_object_array_add(messages, usr);

    json_object_object_add(root, "model",       json_object_new_string(QWEN_MODEL));
    json_object_object_add(root, "temperature", json_object_new_int(0));
    json_object_object_add(root, "max_tokens",  json_object_new_int(1024));
    json_object_object_add(root, "think",       json_object_new_boolean(0));
    json_object_object_add(root, "messages",    messages);

    char *result = strdup(json_object_get_string(root));
    json_object_put(root);
    return result;
}

/* ── ask ─────────────────────────────────────────────────────────────────── */
static char *ask(const char *question) {
    History history = load_history();

    char *enriched = build_question_with_help(question);
    const char *effective = enriched ? enriched : question;

    char *post_data = build_request(effective, &history);
    if (!post_data) { free(enriched); return NULL; }

    char *raw_resp = do_request(post_data);
    char *cmd = raw_resp ? parse_response(raw_resp) : NULL;
    if (!cmd) ERR("failed to parse API response");

    log_request(question, effective, post_data,
                raw_resp ? 200 : 0, raw_resp, cmd);

    free(enriched);
    free(post_data);
    free(raw_resp);
    return cmd;
}

/* ── explain ─────────────────────────────────────────────────────────────── */
static char *explain_command(const char *cmd, const char *original_question) {
    const char *oq = (original_question && *original_question)
                     ? original_question : "explain this command";
    size_t ep_len = strlen(EXPLAIN_PROMPT_FMT) + strlen(oq) + 4;
    char *explain_prompt = malloc(ep_len);
    if (!explain_prompt) return NULL;
    snprintf(explain_prompt, ep_len, EXPLAIN_PROMPT_FMT, oq);

    char *user_content = NULL;
    if (g_last_help_text && strlen(g_last_help_text) > 10) {
        size_t hlen = strlen(g_last_help_text);
        if (hlen > 4000) hlen = 4000;
        size_t uc_len = strlen(cmd) + hlen + 256;
        user_content = malloc(uc_len);
        if (user_content) {
            char help_trunc[4001];
            memcpy(help_trunc, g_last_help_text, hlen);
            help_trunc[hlen] = '\0';
            snprintf(user_content, uc_len,
                "Command: %s\n\n"
                "[man page for '%s' on this system:\n---\n%s\n---]",
                cmd, g_last_utility[0] ? g_last_utility : "this command", help_trunc);
        }
    }

    struct json_object *root     = json_object_new_object();
    struct json_object *messages = json_object_new_array();

    struct json_object *sys = json_object_new_object();
    json_object_object_add(sys, "role",    json_object_new_string("system"));
    json_object_object_add(sys, "content", json_object_new_string(explain_prompt));
    json_object_array_add(messages, sys);
    free(explain_prompt);

    struct json_object *usr = json_object_new_object();
    json_object_object_add(usr, "role",    json_object_new_string("user"));
    json_object_object_add(usr, "content", json_object_new_string(user_content ? user_content : cmd));
    json_object_array_add(messages, usr);
    free(user_content);

    json_object_object_add(root, "model",       json_object_new_string(QWEN_MODEL));
    json_object_object_add(root, "temperature", json_object_new_int(0));
    json_object_object_add(root, "max_tokens",  json_object_new_int(600));
    json_object_object_add(root, "think",       json_object_new_boolean(0));
    json_object_object_add(root, "messages",    messages);

    char *post_data = strdup(json_object_get_string(root));
    json_object_put(root);

    char *raw = do_request(post_data);
    free(post_data);

    char *result = raw ? parse_response(raw) : NULL;
    free(raw);
    return result;
}

/* ── Вывод объяснения ────────────────────────────────────────────────────── */

/* Убирает trailing пробелы/\r и возвращает длину */
static size_t rtrim_len(const char *s) {
    size_t l = strlen(s);
    while (l > 0 && (s[l-1] == ' ' || s[l-1] == '\r' || s[l-1] == '\t')) l--;
    return l;
}

/* ── Стиль 1: цветные метки с отступом ──────────────────────────────────────
 *
 *   curl -X POST http://example.com
 *   transfers data from or to a server
 *
 *   ● Объяснение
 *     Отправляет POST запрос на указанный URL...
 *
 *   ▸ Флаги
 *     -X POST           метод запроса
 *     -d "data"         тело запроса
 *
 *   $ Примеры
 *     curl -X POST http://...       # простой POST
 */
static void print_style_labels(const char *cmd,
                                const char *about,
                                char **explain, int n_explain,
                                char **flags,   int n_flags,
                                char **examples,int n_examples) {
    printf("\n");
    printf("  " CLR_WHITE "%s" CLR_RESET "\n", cmd);
    if (about)
        printf("  " CLR_GRAY "%s" CLR_RESET "\n", about);
    printf("\n");

    if (n_explain > 0) {
        printf("  " CLR_BCYAN "● Объяснение" CLR_RESET "\n");
        for (int i = 0; i < n_explain; i++)
            printf("    %s\n", explain[i]);
        printf("\n");
    }

    if (n_flags > 0) {
        printf("  " CLR_YELLOW "▸ Флаги" CLR_RESET "\n");
        for (int i = 0; i < n_flags; i++) {
            char *sep = strstr(flags[i], "  ");
            if (!sep) sep = strchr(flags[i], '\t');
            if (sep) {
                *sep = '\0';
                char *fp = flags[i], *dp = sep + 1;
                while (*dp == ' ' || *dp == '\t') dp++;
                printf("    " CLR_YELLOW "%-16s" CLR_RESET "  %s\n", fp, dp);
            } else {
                printf("    " CLR_YELLOW "%s" CLR_RESET "\n", flags[i]);
            }
        }
        printf("\n");
    }

    if (n_examples > 0) {
        printf("  " CLR_GREEN "$ Примеры" CLR_RESET "\n");
        for (int i = 0; i < n_examples; i++) {
            char *comment = strstr(examples[i], "  #");
            if (!comment) comment = strstr(examples[i], " #");
            if (comment) {
                *comment = '\0';
                char *cp = comment + 1;
                while (*cp == ' ') cp++;
                printf("    " CLR_WHITE "%-36s" CLR_GRAY "  %s" CLR_RESET "\n",
                       examples[i], cp);
            } else {
                printf("    " CLR_WHITE "%s" CLR_RESET "\n", examples[i]);
            }
        }
        printf("\n");
    }
}

/* ── Стиль 2: заголовки с подчёркиванием ────────────────────────────────────
 *
 *   curl -X POST http://example.com
 *   ───────────────────────────────
 *   transfers data from or to a server
 *
 *   ОБЪЯСНЕНИЕ
 *   ──────────
 *   Отправляет POST запрос...
 *
 *   ФЛАГИ
 *   ─────
 *   -X POST           метод запроса
 *
 *   ПРИМЕРЫ
 *   ───────
 *   curl -X POST http://...       # простой POST
 */
static void print_uline(const char *clr, int len) {
    printf("  %s", clr);
    for (int i = 0; i < len; i++) printf("─");
    printf(CLR_RESET "\n");
}

static void print_style_underline(const char *cmd,
                                   const char *about,
                                   char **explain, int n_explain,
                                   char **flags,   int n_flags,
                                   char **examples,int n_examples) {
    printf("\n");
    printf("  " CLR_WHITE "%s" CLR_RESET "\n", cmd);
    print_uline(CLR_GRAY, (int)strlen(cmd));
    if (about)
        printf("  " CLR_GRAY "%s" CLR_RESET "\n", about);
    printf("\n");

    if (n_explain > 0) {
        printf("  " CLR_BCYAN "ОБЪЯСНЕНИЕ" CLR_RESET "\n");
        print_uline(CLR_BCYAN, 10);
        for (int i = 0; i < n_explain; i++)
            printf("  %s\n", explain[i]);
        printf("\n");
    }

    if (n_flags > 0) {
        printf("  " CLR_YELLOW "ФЛАГИ" CLR_RESET "\n");
        print_uline(CLR_YELLOW, 5);
        for (int i = 0; i < n_flags; i++) {
            char *sep = strstr(flags[i], "  ");
            if (!sep) sep = strchr(flags[i], '\t');
            if (sep) {
                *sep = '\0';
                char *fp = flags[i], *dp = sep + 1;
                while (*dp == ' ' || *dp == '\t') dp++;
                printf("  " CLR_YELLOW "%-16s" CLR_RESET "  %s\n", fp, dp);
            } else {
                printf("  " CLR_YELLOW "%s" CLR_RESET "\n", flags[i]);
            }
        }
        printf("\n");
    }

    if (n_examples > 0) {
        printf("  " CLR_GREEN "ПРИМЕРЫ" CLR_RESET "\n");
        print_uline(CLR_GREEN, 7);
        for (int i = 0; i < n_examples; i++) {
            char *comment = strstr(examples[i], "  #");
            if (!comment) comment = strstr(examples[i], " #");
            if (comment) {
                *comment = '\0';
                char *cp = comment + 1;
                while (*cp == ' ') cp++;
                printf("  " CLR_WHITE "%-36s" CLR_GRAY "  %s" CLR_RESET "\n",
                       examples[i], cp);
            } else {
                printf("  " CLR_WHITE "%s" CLR_RESET "\n", examples[i]);
            }
        }
        printf("\n");
    }
}

/* ── Парсер + диспетчер стилей ───────────────────────────────────────────── */

/* Стиль задаётся через переменную окружения BRO_STYLE:
 *   BRO_STYLE=1  →  цветные метки (по умолчанию)
 *   BRO_STYLE=2  →  заголовки с подчёркиванием
 */
static void print_explanation(const char *cmd, const char *explanation) {
    char *exp_trimmed = trim_whitespace(explanation);
    if (!exp_trimmed) return;

    typedef enum { SEC_NONE, SEC_ABOUT, SEC_EXPLAIN, SEC_FLAGS, SEC_EXAMPLES } Section;
    Section cur = SEC_NONE;

    char *about          = NULL;
    char *explain_lines[16]; int n_explain  = 0;
    char *flag_lines[32];    int n_flags    = 0;
    char *example_lines[16]; int n_examples = 0;

    char *copy = strdup(exp_trimmed);
    free(exp_trimmed);
    if (!copy) return;

    char *saveptr = NULL;
    char *line = strtok_r(copy, "\n", &saveptr);
    while (line) {
        size_t l = rtrim_len(line);
        if (l == 0) { line = strtok_r(NULL, "\n", &saveptr); continue; }
        char buf[1024];
        if (l >= sizeof(buf)) l = sizeof(buf) - 1;
        memcpy(buf, line, l);
        buf[l] = '\0';

        if (strncmp(buf, "ABOUT:",    6) == 0) {
            cur = SEC_ABOUT;
            char *v = buf + 6; while (*v == ' ') v++;
            if (*v) about = strdup(v);
            line = strtok_r(NULL, "\n", &saveptr); continue;
        }
        if (strncmp(buf, "EXPLAIN:",  8) == 0) {
            cur = SEC_EXPLAIN;
            char *v = buf + 8; while (*v == ' ') v++;
            if (*v && n_explain < 16) explain_lines[n_explain++] = strdup(v);
            line = strtok_r(NULL, "\n", &saveptr); continue;
        }
        if (strncmp(buf, "FLAGS:",    6) == 0) { cur = SEC_FLAGS;    line = strtok_r(NULL, "\n", &saveptr); continue; }
        if (strncmp(buf, "EXAMPLES:", 9) == 0) { cur = SEC_EXAMPLES; line = strtok_r(NULL, "\n", &saveptr); continue; }

        switch (cur) {
            case SEC_EXPLAIN:  if (n_explain  < 16) explain_lines[n_explain++]  = strdup(buf); break;
            case SEC_FLAGS:    if (n_flags    < 32) flag_lines[n_flags++]        = strdup(buf); break;
            case SEC_EXAMPLES: if (n_examples < 16) example_lines[n_examples++]  = strdup(buf); break;
            default: break;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    free(copy);

    /* Выбор стиля */
    int style = 1;
    char *env_style = getenv("BRO_STYLE");
    if (env_style && env_style[0] == '2') style = 2;

    if (style == 2)
        print_style_underline(cmd, about, explain_lines, n_explain,
                              flag_lines, n_flags, example_lines, n_examples);
    else
        print_style_labels(cmd, about, explain_lines, n_explain,
                           flag_lines, n_flags, example_lines, n_examples);

    /* Освобождаем */
    free(about);
    for (int i = 0; i < n_explain;  i++) free(explain_lines[i]);
    for (int i = 0; i < n_flags;    i++) free(flag_lines[i]);
    for (int i = 0; i < n_examples; i++) free(example_lines[i]);
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: bro [-e] \"найди файлы больше 100мб\"\n");
        printf("  -e  explain the command before executing\n");
        return 1;
    }

    int explain_mode = 0, arg_start = 1;
    if (strcmp(argv[1], "-e") == 0) {
        explain_mode = 1;
        arg_start = 2;
        if (argc < 3) {
            printf("Usage: bro -e \"найди файлы больше 100мб\"\n");
            return 1;
        }
    }

    size_t total_len = 0;
    for (int i = arg_start; i < argc; i++) total_len += strlen(argv[i]) + 1;
    if (total_len > MAX_INPUT_LEN) {
        ERRF("input too long (max %d characters)", MAX_INPUT_LEN);
        return 1;
    }

    char *question = malloc(total_len + 1);
    if (!question) { ERR("failed to allocate memory"); return 1; }
    question[0] = '\0';
    for (int i = arg_start; i < argc; i++) {
        strcat(question, argv[i]);
        if (i < argc-1) strcat(question, " ");
    }

    char *original_question = strdup(question);
    char *cmd = ask(question);
    free(question);

    if (!cmd) {
        ERR("failed to get API response");
        free(original_question);
        return 1;
    }

    char *tmp;
    tmp = strip_markdown(cmd);        free(cmd); cmd = tmp;
    tmp = unescape_shell_spaces(cmd); free(cmd); cmd = tmp;
    tmp = trim_whitespace(cmd);       free(cmd); cmd = tmp;

    if (!cmd) {
        ERR("failed to process API response");
        free(original_question);
        return 1;
    }

    /* Опасная команда */
    if (!validate_command(cmd)) {
        fprintf(stderr, CLR_YELLOW "Warning: " CLR_RESET
                "command contains potentially dangerous constructs.\n");
        printf("\n  " CLR_RED "➜  %s" CLR_RESET "\n\n", cmd);
        printf("  Blocked. [i] for explanation, any other key to cancel: ");
        char inp[16] = {0};
        if (fgets(inp, sizeof(inp), stdin) && (inp[0]=='i'||inp[0]=='I')) {
            char *explanation = explain_command(cmd, original_question);
            if (explanation) { print_explanation(cmd, explanation); free(explanation); }
            else printf(CLR_YELLOW "  (explanation unavailable)" CLR_RESET "\n\n");
        }
        free(cmd); free(original_question);
        return 1;
    }

    /* Explain mode */
    if (explain_mode) {
        printf("\n  " CLR_BCYAN "?  Getting explanation..." CLR_RESET);
        fflush(stdout);
        char *explanation = explain_command(cmd, original_question);
        if (explanation) { print_explanation(cmd, explanation); free(explanation); }
        else printf(CLR_YELLOW "  (explanation unavailable)" CLR_RESET "\n\n");
    }

    /* Цикл подтверждения */
    char input[16];
    while (1) {
        printf("\n  " CLR_YELLOW "➜  %s" CLR_RESET "\n\n", cmd);
        printf("Execute? [y/N/e/i] (e=edit, i=info) ");
        if (!fgets(input, sizeof(input), stdin)) break;

        unsigned char b0 = (unsigned char)input[0];
        unsigned char b1 = (unsigned char)input[1];

        if (b0 == 'i' || b0 == 'I') {
            char *explanation = explain_command(cmd, original_question);
            if (explanation) { print_explanation(cmd, explanation); free(explanation); }
            else printf(CLR_YELLOW "  (explanation unavailable)" CLR_RESET "\n\n");
            continue;
        }

        if (b0 == 'e' || b0 == 'E') {
            char *edited = edit_command(cmd);
            if (!edited) { fprintf(stderr, CLR_YELLOW "Warning: " CLR_RESET "edit failed\n"); continue; }
            if (!validate_command(edited)) {
                char *esc = escape_string(edited);
                ERR("edited command contains dangerous constructs");
                fprintf(stderr, "Received: %s\n", esc ? esc : "(null)");
                free(esc); free(edited);
                continue;
            }
            free(cmd); cmd = edited;
            continue;
        }

        /* y/Y/д/Д */
        int yes = (b0=='y'||b0=='Y') ||
                  (b0==0xD0 && (b1==0xB4||b1==0x94));
        if (yes) {
            int ret = system(cmd);
            save_history(original_question, cmd);
            free(cmd); free(original_question);
            return (ret == -1) ? 1 : WEXITSTATUS(ret);
        }

        break;
    }

    free(cmd);
    free(original_question);
    return 0;
}