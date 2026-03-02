#!/bin/bash
#
# broai: установка с Ollama (локально, бесплатно, без API-ключей)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_DIR="$HOME/.broai"
CONFIG_FILE="$CONFIG_DIR/config"
MODEL="qwen2.5:7b"
PREFIX="/usr/local/bin"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

log_info()  { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_step()  { echo -e "${CYAN}${BOLD}==> $1${NC}"; }

echo
echo -e "${CYAN}${BOLD}  broai — Ollama edition${NC}"
echo -e "${CYAN}  Local AI, no API keys, free forever${NC}"
echo

# ─── Dependencies ─────────────────────────────────────────────────────────────
log_step "Checking dependencies..."

missing=()
command -v gcc    &>/dev/null || missing+=("gcc")
command -v curl   &>/dev/null || missing+=("curl")
pkg-config --exists json-c 2>/dev/null || missing+=("json-c")

if [ ${#missing[@]} -ne 0 ]; then
    log_warn "Missing: ${missing[*]}"
    if [[ "$OSTYPE" == "darwin"* ]]; then
        log_info "macOS:         brew install ${missing[*]}"
    elif [ -f /etc/debian_version ]; then
        log_info "Debian/Ubuntu: sudo apt install libcurl4-openssl-dev libjson-c-dev gcc"
    elif [ -f /etc/redhat-release ]; then
        log_info "RHEL/Fedora:   sudo dnf install libcurl-devel json-c-devel gcc"
    fi
    read -p "Continue anyway? (y/N): " -n 1 -r; echo
    [[ $REPLY =~ ^[Yy]$ ]] || exit 1
else
    log_info "All dependencies found"
fi

# ─── Ollama ───────────────────────────────────────────────────────────────────
log_step "Checking Ollama..."

if ! command -v ollama &>/dev/null; then
    log_error "Ollama not found!"
    echo
    echo -e "  Install Ollama first:"
    echo -e "  ${BOLD}  macOS:  brew install ollama${NC}"
    echo -e "  ${BOLD}  Linux:  curl -fsSL https://ollama.ai/install.sh | sh${NC}"
    echo
    exit 1
fi
log_info "Ollama found: $(ollama --version 2>/dev/null || echo 'ok')"

log_step "Checking model $MODEL..."
if ollama list 2>/dev/null | grep -q "$MODEL"; then
    log_info "Model already installed"
else
    # ─── Resource check ───────────────────────────────────────────────────────
    MODEL_SIZE_GB=5      # размер модели на диске с запасом (реально ~4.7GB)
    MODEL_SIZE_MB=5120
    RAM_REQUIRED_GB=9    # RAM для работы с запасом (реально ~8GB)
    RAM_REQUIRED_MB=9216

    log_step "Checking system resources..."

    # Свободное место на диске
    OLLAMA_MODELS_DIR="${OLLAMA_MODELS:-$HOME/.ollama/models}"
    CHECK_DIR="$OLLAMA_MODELS_DIR"
    [ -d "$CHECK_DIR" ] || CHECK_DIR="$HOME"

    if [[ "$OSTYPE" == "darwin"* ]]; then
        DISK_FREE_MB=$(df -m "$CHECK_DIR" | awk 'NR==2 {print $4}')
    else
        DISK_FREE_MB=$(df -m "$CHECK_DIR" | awk 'NR==2 {print $4}')
    fi

    # Общий и свободный RAM
    if [[ "$OSTYPE" == "darwin"* ]]; then
        RAM_TOTAL_MB=$(( $(sysctl -n hw.memsize) / 1024 / 1024 ))
        # vm_stat возвращает страницы по 4096 байт
        RAM_FREE_PAGES=$(vm_stat | awk '/Pages free/ {gsub(/\./, "", $3); print $3}')
        RAM_INACTIVE_PAGES=$(vm_stat | awk '/Pages inactive/ {gsub(/\./, "", $3); print $3}')
        RAM_FREE_MB=$(( (RAM_FREE_PAGES + RAM_INACTIVE_PAGES) * 4096 / 1024 / 1024 ))
    else
        RAM_TOTAL_MB=$(awk '/MemTotal/ {print int($2/1024)}' /proc/meminfo)
        RAM_FREE_MB=$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo)
    fi

    DISK_FREE_GB=$(( DISK_FREE_MB / 1024 ))
    RAM_TOTAL_GB=$(( RAM_TOTAL_MB / 1024 ))
    RAM_FREE_GB=$(( RAM_FREE_MB / 1024 ))

    echo
    echo -e "  Model:  ${BOLD}$MODEL${NC}  (~${MODEL_SIZE_GB}GB download)"
    echo
    echo -e "  Disk space:"
    if [ "$DISK_FREE_MB" -ge "$MODEL_SIZE_MB" ]; then
        echo -e "    Free:     ${GREEN}${DISK_FREE_GB}GB${NC}  (need ${MODEL_SIZE_GB}GB)  ✓"
    else
        echo -e "    Free:     ${RED}${DISK_FREE_GB}GB${NC}  (need ${MODEL_SIZE_GB}GB)  ✗"
        echo
        log_error "Not enough disk space. Free up at least ${MODEL_SIZE_GB}GB and retry."
        exit 1
    fi

    echo -e "  RAM:"
    echo -e "    Total:    ${RAM_TOTAL_GB}GB"
    if [ "$RAM_FREE_MB" -ge "$RAM_REQUIRED_MB" ]; then
        echo -e "    Available:${GREEN}${RAM_FREE_GB}GB${NC}  (need ${RAM_REQUIRED_GB}GB)  ✓"
    elif [ "$RAM_TOTAL_MB" -ge "$RAM_REQUIRED_MB" ]; then
        echo -e "    Available:${YELLOW}${RAM_FREE_GB}GB${NC}  (need ${RAM_REQUIRED_GB}GB)  ⚠"
        echo
        log_warn "Low available RAM. Close other apps before running broai."
        log_warn "The model may run slowly or cause system slowdown."
    else
        echo -e "    Available:${RED}${RAM_FREE_GB}GB${NC}  (need ${RAM_REQUIRED_GB}GB)  ✗"
        echo
        log_warn "Your system has only ${RAM_TOTAL_GB}GB RAM total."
        log_warn "The model requires ~8GB RAM — performance may be poor."
        read -p "  Continue anyway? (y/N): " -n 1 -r; echo
        [[ $REPLY =~ ^[Yy]$ ]] || exit 0
    fi
    echo

    log_warn "Pulling $MODEL (~${MODEL_SIZE_GB}GB) — this may take several minutes..."
    ollama pull "$MODEL"
    log_info "Model ready"
fi

# ─── Config ───────────────────────────────────────────────────────────────────
log_step "Saving configuration..."

mkdir -p "$CONFIG_DIR"
chmod 700 "$CONFIG_DIR"

if [ ! -f "$CONFIG_FILE" ]; then
    touch "$CONFIG_FILE"
    chmod 600 "$CONFIG_FILE"
else
    perms=$(stat -c "%a" "$CONFIG_FILE" 2>/dev/null || stat -f "%A" "$CONFIG_FILE" 2>/dev/null)
    [ "$perms" != "600" ] && chmod 600 "$CONFIG_FILE" && \
        log_warn "Fixed permissions: $perms → 600"
fi

# Удаляем старые записи broai (оставляем остальное)
tmp=$(mktemp); chmod 600 "$tmp"
grep -v -E "(AI_PROVIDER|AI_BASH_URL|AI_BASH_MODEL)" "$CONFIG_FILE" > "$tmp" 2>/dev/null || true
mv "$tmp" "$CONFIG_FILE"
chmod 600 "$CONFIG_FILE"

# Ollama не нужен AI_PROVIDER — URL и модель достаточно
cat >> "$CONFIG_FILE" << CONF

# broai: Ollama
export AI_BASH_URL="http://localhost:11434/v1/chat/completions"
export AI_BASH_MODEL="$MODEL"
CONF

log_info "Saved to $CONFIG_FILE"

# ─── Shell ────────────────────────────────────────────────────────────────────
log_step "Configuring shell..."

secrets_line='[ -f ~/.broai/config ] && source ~/.broai/config'
user_shell=$(basename "$SHELL")

case "$user_shell" in
    zsh)  profile="$HOME/$( [[ "$OSTYPE" == darwin* ]] && echo .zprofile || echo .zshrc )" ;;
    bash) profile="$HOME/$( [[ "$OSTYPE" == darwin* ]] && echo .bash_profile || echo .bashrc )" ;;
    fish) profile="$HOME/.config/fish/config.fish" ;;
    *)    profile="$HOME/.bashrc" ;;
esac

log_info "Shell: $user_shell → $profile"

if grep -q "broai/config" "$profile" 2>/dev/null; then
    log_info "Already configured in $profile"
else
    printf '\n# broai\n%s\n' "$secrets_line" >> "$profile"
    log_info "Added to $profile"
fi

# ─── Compile ──────────────────────────────────────────────────────────────────
log_step "Compiling..."
cd "$SCRIPT_DIR"

if [ "${SKIP_COMPILE}" = "1" ]; then
    log_info "Skipping — already built by make"
else
    EXTRA_CFLAGS=""
    if [[ "$OSTYPE" == "darwin"* ]]; then
        JSONC_PREFIX=$(brew --prefix json-c 2>/dev/null || true)
        [ -n "$JSONC_PREFIX" ] && EXTRA_CFLAGS="-I$JSONC_PREFIX/include -L$JSONC_PREFIX/lib"
    fi

    rm -f broai
    if gcc -Wall -Wextra -O2 $EXTRA_CFLAGS -o broai broai_ollama.c -lcurl -ljson-c -lpthread; then
        log_info "Build successful: $SCRIPT_DIR/broai"
    else
        log_error "Compilation failed"
        exit 1
    fi
fi

# ─── Install binary ───────────────────────────────────────────────────────────
log_step "Installing binary..."

if sudo cp "$SCRIPT_DIR/broai" "$PREFIX/broai" && \
   sudo ln -sf "$PREFIX/broai" "$PREFIX/bro"; then
    log_info "Installed: $PREFIX/broai"
    log_info "Alias:     $PREFIX/bro → $PREFIX/broai"
else
    log_warn "sudo install failed — adding project dir to PATH"
    grep -q "$SCRIPT_DIR" "$profile" 2>/dev/null || \
        printf '\nexport PATH="%s:$PATH"\n' "$SCRIPT_DIR" >> "$profile"
    log_info "Added $SCRIPT_DIR to PATH in $profile"
fi

# ─── Done ─────────────────────────────────────────────────────────────────────
echo
echo -e "${GREEN}${BOLD}========================================${NC}"
echo -e "${GREEN}${BOLD}  broai (Ollama) installed!             ${NC}"
echo -e "${GREEN}${BOLD}========================================${NC}"
echo
echo -e "  Apply config now:"
echo -e "  ${BOLD}  source $CONFIG_FILE${NC}"
echo
echo -e "  Start Ollama (keep running in background):"
echo -e "  ${BOLD}  ollama serve${NC}"
echo
echo -e "  Usage:"
echo -e "  ${BOLD}  bro \"find files larger than 100mb\"${NC}"
echo -e "  ${BOLD}  bro -e \"show disk usage\"${NC}        # with explanation"
echo -e "  ${BOLD}  bro -i \"remove old logs\"${NC}        # with detailed info"
echo -e "  ${BOLD}  bro -e -i \"compress directory\"${NC}  # both"
echo
echo -e "  Debug log: ${BOLD}/tmp/broai_last.log${NC}"
echo