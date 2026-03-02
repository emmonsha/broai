# broai

Терминальный ассистент: спрашиваю по-русски — получаю bash-команду.

Бинарь: `broai`. Алиас: `bro`.

## Зависимости

- libcurl
- json-c

### Установка

**macOS:**
```bash
brew install curl json-c
```

**Ubuntu/Debian:**
```bash
sudo apt install libcurl4-openssl-dev libjson-c-dev
```

## Установка

**Автоматическая установка (рекомендуется):**

```bash
./install.sh
```

Скрипт:
- Проверит зависимости
- Скомпилирует проект
- Настроит API ключ в `~/.broai/config`
- Добавит `how` в PATH
- Обновит `~/.bashrc`

**Ручная установка:**

```bash
make
sudo make install  # устанавливает broai + алиас bro
# или вручную:
export PATH="$PWD:$PATH"
```

## Использование

```bash
export OPENAI_API_KEY="your-api-key"
export AI_BASH_MODEL="gpt-4o-mini"  # опционально
export AI_BASH_URL="https://api.openai.com/v1/chat/completions"  # опционально
export AI_PROVIDER="openai"  # опционально

./bro "найди файлы больше 100мб"
```

## Поддерживаемые провайдеры

### OpenAI (по умолчанию)
```bash
export AI_PROVIDER="openai"
export OPENAI_API_KEY="sk-..."
export AI_BASH_MODEL="gpt-4o-mini"
```

### DeepSeek (OpenAI-compatible)
```bash
export AI_PROVIDER="deepseek"
export OPENAI_API_KEY="sk-..."
export AI_BASH_URL="https://api.deepseek.com/v1/chat/completions"
export AI_BASH_MODEL="deepseek-chat"
```

### Qwen (DashScope / Alibaba)
```bash
export AI_PROVIDER="dashscope"
export OPENAI_API_KEY="sk-..."  # ключ от DashScope
export AI_BASH_URL="https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"
export AI_BASH_MODEL="qwen-plus"
```

### Claude (Anthropic) — через прокси
```bash
export AI_PROVIDER="anthropic"
export OPENAI_API_KEY="sk-ant-..."
export AI_BASH_URL="https://api.anthropic.com/v1/messages"
export AI_BASH_MODEL="claude-sonnet-4-20250514"
```
⚠️ Требуется адаптер формата запроса (Claude использует другой API)

### Ollama (локально, бесплатно)
```bash
export AI_PROVIDER="ollama"
export OPENAI_API_KEY=""  # не нужен
export AI_BASH_URL="http://localhost:11434/v1/chat/completions"
export AI_BASH_MODEL="qwen2.5:7b"  # или любая другая
```

### LM Studio (локально)
```bash
export AI_PROVIDER="local"
export OPENAI_API_KEY=""  # не нужен
export AI_BASH_URL="http://localhost:1234/v1/chat/completions"
export AI_BASH_MODEL="local-model"
```

## Переменные окружения

| Переменная | Описание | По умолчанию |
|------------|----------|--------------|
| `OPENAI_API_KEY` | API ключ OpenAI | (обязательно) |
| `AI_BASH_URL` | URL API | `https://api.openai.com/v1/chat/completions` |
| `AI_BASH_MODEL` | Модель | `gpt-4o-mini` |

## Примеры

```bash
./bro "покажи топ-5 процессов по памяти"
./bro "найди все .log файлы за сегодня"
./bro "освободи место в /tmp"
```

## Безопасность

### Что защищено:
- ✅ API ключ валидируется (только alphanumeric + `-` `_`)
- ✅ Команды проверяются на опасные конструкции (`$()`, backticks, `eval`)
- ✅ Ограничена длина входных данных (4096 символов)
- ✅ Таймауты на HTTP-запросы (30 сек)
- ✅ Файл секретов с правами 600
- ✅ Экранирование вывода

### Меры предосторожности:
1. **Всегда проверяй команду перед выполнением** — подтверждение обязательно
2. **Не используй с недоверенными API** — только официальный OpenAI
3. **Храни ключ в `~/.broai/config`** — не в `.bashrc`
4. **Не передавай секреты в запросе** — они могут уйти в API

### Ограничения:
- Запрещено перенаправление в `/etc/`
- Запрещены `eval`, `bash -c`, подстановка команд
- Максимальная длина API ключа: 256 символов