# broai — ollama branch

Терминальный ассистент на базе локальной модели **qwen2.5:7b** через Ollama.

Бесплатно. Работает офлайн. Без API-ключей.

## Зависимости

**macOS:**
```bash
brew install ollama curl json-c
```

**Ubuntu/Debian:**
```bash
sudo apt install libcurl4-openssl-dev libjson-c-dev
curl -fsSL https://ollama.ai/install.sh | sh
```

## Установка

```bash
# 1. Скачать и запустить модель (один раз)
ollama pull qwen2.5:7b

# 2. Собрать
make

# 3. Установить глобально (опционально)
sudo make install
```

## Использование

```bash
# В отдельном терминале (или в фоне)
ollama serve

# Использование
bro "найди файлы больше 100мб"
bro -e "покажи топ процессов по памяти"   # с объяснением
bro -i "удали старые логи"                 # с подробной справкой
```

## Команды в меню

```
Execute? [y/N/e/i]
  y — выполнить
  N — отмена
  e — открыть в $EDITOR для правки
  i — подробная справка (флаги, примеры, предупреждения)
```

## Изменить модель (опционально)

```bash
ollama pull llama3.2:3b
export AI_BASH_MODEL="llama3.2:3b"
```

## Структура файлов

```
broai_core.c       — логика: валидация, история, UI, main()
broai_core.h       — общие типы и объявления
provider.h         — интерфейс провайдера (ask/explain/info)
provider_ollama.c  — реализация для Ollama
Makefile
```

## Git-ветки

- `main` — поддержка всех провайдеров (OpenAI, Ollama, DeepSeek, Anthropic...)
- `ollama` — только Ollama, без API-ключей, для быстрого старта