#!/bin/bash
#
# broai: быстрая настройка Ollama
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_DIR="$HOME/.broai"
CONFIG_FILE="$CONFIG_DIR/config"

echo "=========================================="
echo "broai: настройка Ollama"
echo "=========================================="
echo

# Проверка Ollama
if ! command -v ollama &> /dev/null; then
    echo "[ERROR] Ollama не установлен!"
    echo
    echo "Установите Ollama:"
    echo "  macOS:  brew install ollama"
    echo "  Linux:  curl -fsSL https://ollama.ai/install.sh | sh"
    echo
    exit 1
fi

echo "[INFO] Ollama найден"

# Проверка модели
MODEL="qwen2.5:7b"
echo "[INFO] Проверяем модель $MODEL..."

if ! ollama list 2>/dev/null | grep -q "$MODEL"; then
    echo "[WARN] Модель $MODEL не найдена"
    echo "[INFO] Загрузка модели (может занять время)..."
    ollama pull $MODEL
else
    echo "[INFO] Модель уже установлена"
fi

# Настройка конфига
echo "[INFO] Настройка ~/.broai/config..."

mkdir -p "$CONFIG_DIR"
chmod 700 "$CONFIG_DIR"

if [ ! -f "$CONFIG_FILE" ]; then
    touch "$CONFIG_FILE"
    chmod 600 "$CONFIG_FILE"
fi

# Удаляем старые настройки broai
grep -v -E "(AI_PROVIDER|AI_BASH_URL|AI_BASH_MODEL)" "$CONFIG_FILE" > "$CONFIG_FILE.tmp" 2>/dev/null || true
mv "$CONFIG_FILE.tmp" "$CONFIG_FILE"
chmod 600 "$CONFIG_FILE"

# Добавляем настройки Ollama
cat >> "$CONFIG_FILE" << CONF

# broai: Ollama
export AI_PROVIDER="ollama"
export AI_BASH_URL="http://localhost:11434/v1/chat/completions"
export AI_BASH_MODEL="$MODEL"
CONF

echo "[INFO] Настройки сохранены в ~/.broai/config"

# Настройка shell
for profile in "$HOME/.zshrc" "$HOME/.bashrc"; do
    if [ -f "$profile" ]; then
        if ! grep -q "broai/config" "$profile" 2>/dev/null; then
            echo "" >> "$profile"
            echo "# broai" >> "$profile"
            echo '[ -f ~/.broai/config ] && source ~/.broai/config' >> "$profile"
            echo "[INFO] Добавлено в $profile"
        else
            echo "[INFO] Уже настроено в $profile"
        fi
        break
    fi
done

# Компиляция
echo "[INFO] Компиляция..."
cd "$SCRIPT_DIR"
make clean > /dev/null 2>&1 || true
make

echo
echo "=========================================="
echo "Готово!"
echo "=========================================="
echo
echo "Примените настройки прямо сейчас:"
echo "  source ~/.broai/config"
echo
echo "Запустите Ollama (в отдельном терминале):"
echo "  ollama serve"
echo
echo "Использование:"
echo "  bro \"найди файлы больше 100мб\""
echo