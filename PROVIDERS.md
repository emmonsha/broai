# Провайдеры ИИ для ai-bash

## Быстрая настройка

### Ollama (бесплатно, локально) ⭐ Рекомендуется
```bash
# 1. Установи Ollama
brew install ollama  # macOS
# или https://ollama.ai

# 2. Скачай модель
ollama pull qwen2.5:7b

# 3. Настрой ai-bash
export AI_PROVIDER="ollama"
export AI_BASH_URL="http://localhost:11434/v1/chat/completions"
export AI_BASH_MODEL="qwen2.5:7b"
export OPENAI_API_KEY=""  # не нужен

# 4. Запусти
ollama serve &
./ai_bash "найди файлы больше 100мб"
```

### DeepSeek (дешёвый облачный)
```bash
export AI_PROVIDER="deepseek"
export OPENAI_API_KEY="sk-..."  # ключ от deepseek.com
export AI_BASH_URL="https://api.deepseek.com/v1/chat/completions"
export AI_BASH_MODEL="deepseek-chat"
```

### Qwen через DashScope (Alibaba)
```bash
export AI_PROVIDER="dashscope"
export OPENAI_API_KEY="sk-..."  # ключ от dashscope.aliyun.com
export AI_BASH_URL="https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"
export AI_BASH_MODEL="qwen-plus"
```

### OpenAI (оригинал)
```bash
export AI_PROVIDER="openai"
export OPENAI_API_KEY="sk-..."
export AI_BASH_URL="https://api.openai.com/v1/chat/completions"
export AI_BASH_MODEL="gpt-4o-mini"
```

### LM Studio (локально, любая модель)
```bash
# 1. Установи LM Studio
# https://lmstudio.ai

# 2. Загрузи модель и запусти сервер

# 3. Настрой ai-bash
export AI_PROVIDER="local"
export AI_BASH_URL="http://localhost:1234/v1/chat/completions"
export AI_BASH_MODEL="local-model"
export OPENAI_API_KEY=""  # не нужен
```

## Сравнение провайдеров

| Провайдер | Цена | Скорость | Качество | Приватность |
|-----------|------|----------|----------|-------------|
| Ollama | Бесплатно | ⚡⚡⚡ | ⭐⭐⭐ | 🔒 Локально |
| LM Studio | Бесплатно | ⚡⚡⚡ | ⭐⭐⭐ | 🔒 Локально |
| DeepSeek | $ | ⚡⚡ | ⭐⭐⭐⭐ | ☁️ Облако |
| Qwen | $$ | ⚡⚡ | ⭐⭐⭐⭐ | ☁️ Облако |
| OpenAI | $$$ | ⚡⚡ | ⭐⭐⭐⭐⭐ | ☁️ Облако |

## Переменные окружения

| Переменная | Описание | Пример |
|------------|----------|--------|
| `AI_PROVIDER` | Провайдер | `openai`, `ollama`, `deepseek` |
| `OPENAI_API_KEY` | API ключ | `sk-...` |
| `AI_BASH_URL` | URL API | `https://api.openai.com/...` |
| `AI_BASH_MODEL` | Модель | `gpt-4o-mini`, `qwen2.5:7b` |

## Примечания

### Claude (Anthropic)
⚠️ **Не поддерживается напрямую** — Claude использует другой формат API (не OpenAI-compatible).

Для использования Claude потребуется:
1. Прокси-сервер (например, [litellm](https://github.com/BerriAI/litellm))
2. Или адаптер в коде ai_bash.c

```bash
# Через LiteLLM прокси
ollama pull claude-3-sonnet  # если доступно
export AI_BASH_URL="http://localhost:4000/v1/chat/completions"
```
