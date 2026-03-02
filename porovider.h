/*
 * provider.h — интерфейс провайдера AI
 *
 * Каждый провайдер реализует три функции:
 *   provider_ask()      — получить bash-команду по вопросу
 *   provider_explain()  — краткое объяснение команды
 *   provider_info()     — подробная справка (флаги, примеры, предупреждения)
 *
 * broai_core.c вызывает только эти три функции и не знает ничего про HTTP/JSON.
 */

#ifndef PROVIDER_H
#define PROVIDER_H

#include "broai_core.h"

/*
 * Получить bash-команду по вопросу на естественном языке.
 * history — контекст последних запросов (может быть NULL).
 * Возвращает строку через malloc, вызывающий обязан освободить.
 * При ошибке возвращает NULL.
 */
char *provider_ask(const char *question, const History *history);

/*
 * Получить краткое объяснение команды (1-3 предложения).
 * Возвращает строку через malloc или NULL при ошибке.
 */
char *provider_explain(const char *cmd);

/*
 * Получить подробную справку по команде.
 * original_question используется для определения языка ответа.
 * Формат ответа: DESCRIPTION / OPTIONS / EXAMPLES / WARNINGS.
 * Возвращает строку через malloc или NULL при ошибке.
 */
char *provider_info(const char *cmd, const char *original_question);

#endif /* PROVIDER_H */