/*
 * provider_ollama.c — провайдер для Ollama (локальный, бесплатный)
 *
 * Компиляция: gcc -o broai broai_core.c provider_ollama.c -lcurl -ljson-c -lpthread
 *
 * Перед запуском:
 *   ollama serve          # в отдельном терминале
 *   ollama pull qwen2.5:7b
 */

#include "broai_core.h"
#include "provider.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <json-c/json.h>

/* ============================================
 * Настройки Ollama
 * ============================================ */

#define OLLAMA_URL   "http://localhost:11434/v1/chat/completions"
#define OLLAMA_MODEL "qwen2.5:7b"
#define OLLAMA_TIMEOUT_SEC 120L

/* Переопределить URL и модель можно через env (опционально) */
static const char *ollama_url(void) {
    const char *env = getenv("AI_BASH_URL");
    return (env && *env) ? env : OLLAMA_URL;
}

static const char *ollama_model(void) {
    const char *env = getenv("AI_BASH_MODEL");
    return (env && *env) ? env : OLLAMA_MODEL;
}

/* ============================================
 * Промпты
 * ============================================ */

static const char *SYSTEM_PROMPT =
    "You are a terminal assistant. The user describes a task in natural language. "
    "Reply with ONLY a bash command — one line. "
    "No explanations, no markdown, no backticks. "
    "Multiple commands — use && or |. OS: Linux.";

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
    "  (3-6 most useful flags/options of the main utility)\n"
    "EXAMPLES:\n"
    "  <example command>  # <comment>\n"
    "  (2-3 practical examples)\n"
    "WARNINGS: <danger notes if any, or 'none'>";

/* ============================================
 * HTTP — общая функция запроса к Ollama
 * ============================================ */

struct MemoryStruct {
    char  *memory;
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

/*
 * Отправить запрос к Ollama и вернуть сырой JSON-ответ.
 * post_data — тело запроса (JSON строка).
 * Возвращает строку через malloc или NULL при ошибке.
 */
static char *ollama_request(const char *post_data) {
    CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;
    struct MemoryStruct chunk;
    char *result = NULL;

    chunk.memory = malloc(1);
    if (chunk.memory == NULL) return NULL;
    chunk.size = 0;

    curl = curl_easy_init();
    if (!curl) { free(chunk.memory); return NULL; }

    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, ollama_url());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "broai/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, OLLAMA_TIMEOUT_SEC);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    /* Спиннер на время запроса */
    SpinnerState spinner_state;
    pthread_t spinner;
    spinner_start(&spinner_state, &spinner);
    res = curl_easy_perform(curl);
    spinner_stop(&spinner_state, &spinner);

    if (res != CURLE_OK) {
        fprintf(stderr, "Error: cannot reach Ollama — %s\n", curl_easy_strerror(res));
        fprintf(stderr, "Make sure Ollama is running: ollama serve\n");
    } else {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code == 200) {
            result = strdup(chunk.memory);
        } else {
            fprintf(stderr, "Error: Ollama returned HTTP %ld\n", http_code);
            if (chunk.size > 0)
                fprintf(stderr, "Response: %s\n", chunk.memory);
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(chunk.memory);
    return result;
}

/* ============================================
 * JSON — построение запроса
 * ============================================ */

static void add_history_to_messages(struct json_object *messages, const History *h) {
    for (int i = 0; i < h->count; i++) {
        struct json_object *u = json_object_new_object();
        json_object_object_add(u, "role", json_object_new_string("user"));
        json_object_object_add(u, "content",
            json_object_new_string(h->entries[i].question));
        json_object_array_add(messages, u);

        struct json_object *a = json_object_new_object();
        json_object_object_add(a, "role", json_object_new_string("assistant"));
        json_object_object_add(a, "content",
            json_object_new_string(h->entries[i].command));
        json_object_array_add(messages, a);
    }
}

static char *build_request(const char *system_prompt, const char *user_content,
                            const History *history, int max_tokens) {
    struct json_object *root     = json_object_new_object();
    struct json_object *messages = json_object_new_array();

    /* System message */
    struct json_object *sys = json_object_new_object();
    json_object_object_add(sys, "role",    json_object_new_string("system"));
    json_object_object_add(sys, "content", json_object_new_string(system_prompt));
    json_object_array_add(messages, sys);

    /* История (только для основного запроса) */
    if (history != NULL)
        add_history_to_messages(messages, history);

    /* User message */
    struct json_object *usr = json_object_new_object();
    json_object_object_add(usr, "role",    json_object_new_string("user"));
    json_object_object_add(usr, "content", json_object_new_string(user_content));
    json_object_array_add(messages, usr);

    json_object_object_add(root, "model",       json_object_new_string(ollama_model()));
    json_object_object_add(root, "temperature", json_object_new_int(0));
    json_object_object_add(root, "max_tokens",  json_object_new_int(max_tokens));
    json_object_object_add(root, "messages",    messages);
    /* Отключаем режим рассуждений Qwen3 — нам нужна только команда */
    json_object_object_add(root, "think",       json_object_new_boolean(0));

    char *result = strdup(json_object_get_string(root));
    json_object_put(root);
    return result;
}

/* ============================================
 * JSON — парсинг ответа
 * ============================================ */

/* Извлекает текст из choices[0].message.content.
 * Fallback на reasoning (Qwen3 иногда пишет ответ туда). */
static char *parse_response(const char *json_str) {
    struct json_object *root = json_tokener_parse(json_str);
    if (root == NULL) return NULL;

    char *result = NULL;
    struct json_object *choices;

    if (json_object_object_get_ex(root, "choices", &choices)) {
        struct json_object *choice = json_object_array_get_idx(choices, 0);
        if (choice != NULL) {
            struct json_object *message;
            if (json_object_object_get_ex(choice, "message", &message)) {

                /* Сначала content */
                struct json_object *content;
                if (json_object_object_get_ex(message, "content", &content)) {
                    const char *s = json_object_get_string(content);
                    if (s && *s) result = strdup(s);
                }

                /* Fallback: reasoning (Qwen3 ignore think=false) */
                if (result == NULL || *result == '\0') {
                    free(result);
                    result = NULL;
                    struct json_object *reasoning;
                    if (json_object_object_get_ex(message, "reasoning", &reasoning)) {
                        const char *r = json_object_get_string(reasoning);
                        if (r && *r) {
                            /* Берём последнюю непустую строку */
                            char *copy = strdup(r);
                            if (copy) {
                                char *last = NULL;
                                char *line = strtok(copy, "\n");
                                while (line) {
                                    size_t len = strlen(line);
                                    while (len > 0 && (line[len-1]==' '||line[len-1]=='\r')) len--;
                                    if (len > 0) last = line;
                                    line = strtok(NULL, "\n");
                                }
                                if (last) result = strdup(last);
                                free(copy);
                            }
                        }
                    }
                }
            }
        }
    }

    json_object_put(root);
    return result;
}

/* ============================================
 * Реализация интерфейса provider.h
 * ============================================ */

char *provider_ask(const char *question, const History *history) {
    char *post_data = build_request(SYSTEM_PROMPT, question, history, 1024);
    if (post_data == NULL) return NULL;

    char *raw = ollama_request(post_data);
    free(post_data);
    if (raw == NULL) return NULL;

    char *result = parse_response(raw);
    free(raw);
    return result;
}

char *provider_explain(const char *cmd) {
    char *post_data = build_request(EXPLAIN_PROMPT, cmd, NULL, 300);
    if (post_data == NULL) return NULL;

    char *raw = ollama_request(post_data);
    free(post_data);
    if (raw == NULL) return NULL;

    char *result = parse_response(raw);
    free(raw);
    return result;
}

char *provider_info(const char *cmd, const char *original_question) {
    /* Передаём вопрос + команду — AI определит язык ответа по вопросу */
    char user_content[MAX_INPUT_LEN * 2];
    if (original_question && *original_question) {
        snprintf(user_content, sizeof(user_content),
                 "User question: %s\nCommand: %s", original_question, cmd);
    } else {
        snprintf(user_content, sizeof(user_content), "%s", cmd);
    }

    char *post_data = build_request(INFO_PROMPT, user_content, NULL, 600);
    if (post_data == NULL) return NULL;

    char *raw = ollama_request(post_data);
    free(post_data);
    if (raw == NULL) return NULL;

    char *result = parse_response(raw);
    free(raw);
    return result;
}