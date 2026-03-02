#!/bin/bash
#
# broai: installation script
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_NAME="broai"
CONFIG_DIR="$HOME/.broai"
SECRETS_FILE="$CONFIG_DIR/config"

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

# ─── Dependencies ────────────────────────────────────────────────────────────
check_dependencies() {
    log_step "Checking dependencies..."

    local missing=()

    command -v curl  &>/dev/null || missing+=("curl")
    command -v gcc   &>/dev/null || missing+=("gcc")
    pkg-config --exists json-c 2>/dev/null || missing+=("json-c")

    if [ ${#missing[@]} -ne 0 ]; then
        log_warn "Missing: ${missing[*]}"

        if [[ "$OSTYPE" == "darwin"* ]]; then
            log_info "macOS:          brew install ${missing[*]}"
        elif [ -f /etc/debian_version ]; then
            log_info "Debian/Ubuntu:  sudo apt install libcurl4-openssl-dev libjson-c-dev gcc"
        elif [ -f /etc/redhat-release ]; then
            log_info "RHEL/Fedora:    sudo dnf install libcurl-devel json-c-devel gcc"
        else
            log_warn "Please install missing dependencies manually"
        fi

        read -p "Continue anyway? (y/N): " -n 1 -r; echo
        [[ $REPLY =~ ^[Yy]$ ]] || exit 1
    else
        log_info "All dependencies found"
    fi
}

# ─── Provider selection ───────────────────────────────────────────────────────
setup_provider() {
    log_step "Choose AI provider"
    echo
    echo "  1) OpenAI       — gpt-4o-mini     (cloud, API key required)"
    echo "  2) DeepSeek     — deepseek-chat   (cloud, API key required)"
    echo "  3) Qwen         — qwen-plus       (cloud, API key required)"
    echo "  4) Anthropic    — claude-haiku    (cloud, API key required)"
    echo "  5) Ollama       — qwen2.5:7b      (local, no key needed)"
    echo "  6) LM Studio    — local-model     (local, no key needed)"
    echo "  7) Custom URL"
    echo

    read -p "Provider [1-7, default 1]: " -n 1 provider_choice
    echo

    case $provider_choice in
        2)
            AI_PROVIDER="deepseek"
            AI_BASH_URL="https://api.deepseek.com/v1/chat/completions"
            AI_BASH_MODEL="deepseek-chat"
            NEEDS_KEY=1
            KEY_URL="https://platform.deepseek.com/api_keys"
            KEY_VAR="OPENAI_API_KEY"
            ;;
        3)
            AI_PROVIDER="dashscope"
            AI_BASH_URL="https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"
            AI_BASH_MODEL="qwen-plus"
            NEEDS_KEY=1
            KEY_URL="https://dashscope.console.aliyun.com/apiKey"
            KEY_VAR="OPENAI_API_KEY"
            ;;
        4)
            AI_PROVIDER="anthropic"
            AI_BASH_URL="https://api.anthropic.com/v1/messages"
            AI_BASH_MODEL="claude-haiku-4-5-20251001"
            NEEDS_KEY=1
            KEY_URL="https://console.anthropic.com/keys"
            KEY_VAR="OPENAI_API_KEY"
            ;;
        5)
            AI_PROVIDER="ollama"
            AI_BASH_URL="http://localhost:11434/v1/chat/completions"
            AI_BASH_MODEL="qwen2.5:7b"
            NEEDS_KEY=0
            ;;
        6)
            AI_PROVIDER="local"
            AI_BASH_URL="http://localhost:1234/v1/chat/completions"
            AI_BASH_MODEL="local-model"
            NEEDS_KEY=0
            ;;
        7)
            AI_PROVIDER="openai"
            read -p "API URL: " AI_BASH_URL
            read -p "Model:   " AI_BASH_MODEL
            NEEDS_KEY=1
            KEY_URL="your provider's dashboard"
            KEY_VAR="OPENAI_API_KEY"
            ;;
        *)  # default: OpenAI
            AI_PROVIDER="openai"
            AI_BASH_URL="https://api.openai.com/v1/chat/completions"
            AI_BASH_MODEL="gpt-4o-mini"
            NEEDS_KEY=1
            KEY_URL="https://platform.openai.com/api-keys"
            KEY_VAR="OPENAI_API_KEY"
            ;;
    esac

    log_info "Provider: $AI_PROVIDER  |  Model: $AI_BASH_MODEL"
}

# ─── API key setup ────────────────────────────────────────────────────────────
setup_api_key() {
    if [ "$NEEDS_KEY" -eq 0 ]; then
        log_info "Local provider — no API key required"
        API_KEY=""
        return 0
    fi

    echo
    log_step "API key required"
    echo
    echo -e "  ${BOLD}broai needs an API key to talk to ${AI_PROVIDER}.${NC}"
    echo
    echo -e "  ${CYAN}Get your key here:${NC}"
    echo -e "  ${BOLD}  $KEY_URL${NC}"
    echo
    echo -e "  The key will be stored in ${BOLD}~/.broai/config${NC} (chmod 600)"
    echo -e "  and sourced automatically — it never appears in shell history."
    echo

    # Check if key already exists
    if grep -q "$KEY_VAR" "$SECRETS_FILE" 2>/dev/null; then
        log_warn "API key already set in $SECRETS_FILE"
        read -p "  Replace it? (y/N): " -n 1 -r; echo
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            API_KEY=""
            return 0
        fi
    fi

    # Read key securely (no echo, no history)
    read -s -p "  Paste API key: " API_KEY
    echo

    if [ -z "$API_KEY" ]; then
        echo
        log_warn "No key entered — skipping."
        echo -e "  You can add it later manually:"
        echo
        echo -e "  ${BOLD}echo 'export OPENAI_API_KEY=\"sk-...\"' >> ~/.broai/config${NC}"
        echo
        API_KEY=""
    else
        log_info "Key received (not shown)"
    fi
}

# ─── Save config ──────────────────────────────────────────────────────────────
save_config() {
    log_step "Saving configuration..."

    if [ ! -f "$SECRETS_FILE" ]; then
        mkdir -p "$CONFIG_DIR"
        chmod 700 "$CONFIG_DIR"
        touch "$SECRETS_FILE"
        chmod 600 "$SECRETS_FILE"
        log_info "Created $SECRETS_FILE (chmod 600)"
    else
        # Проверяем права существующего файла
        local perms
        perms=$(stat -c "%a" "$SECRETS_FILE" 2>/dev/null || stat -f "%A" "$SECRETS_FILE" 2>/dev/null)
        if [ "$perms" != "600" ]; then
            log_warn "Fixing permissions on $SECRETS_FILE ($perms → 600)"
            chmod 600 "$SECRETS_FILE"
        fi
    fi

    # Remove old broai entries
    local tmp_file
    tmp_file=$(mktemp)
    chmod 600 "$tmp_file"
    grep -v -E "(OPENAI_API_KEY|AI_PROVIDER|AI_BASH_URL|AI_BASH_MODEL)" \
        "$SECRETS_FILE" > "$tmp_file" 2>/dev/null || true
    mv "$tmp_file" "$SECRETS_FILE"

    # Write new entries
    {
        echo "export AI_PROVIDER=\"$AI_PROVIDER\""
        echo "export AI_BASH_URL=\"$AI_BASH_URL\""
        echo "export AI_BASH_MODEL=\"$AI_BASH_MODEL\""
        [ -n "$API_KEY" ] && echo "export OPENAI_API_KEY=\"$API_KEY\""
    } >> "$SECRETS_FILE"

    log_info "Saved to $SECRETS_FILE"
}

# ─── Shell config ─────────────────────────────────────────────────────────────
setup_shell() {
    log_step "Configuring shell..."

    local secrets_line='[ -f ~/.broai/config ] && source ~/.broai/config'
    local profile

    # Определяем по реальному shell пользователя, а не по ОС
    local user_shell
    user_shell=$(basename "$SHELL")

    case "$user_shell" in
        zsh)  # На macOS терминал открывает login shell → читает .zprofile, не .zshrc
              if [[ "$OSTYPE" == "darwin"* ]]; then
                  profile="$HOME/.zprofile"
              else
                  profile="$HOME/.zshrc"
              fi
              ;;
        bash) # На macOS bash тоже login shell → .bash_profile
              if [[ "$OSTYPE" == "darwin"* ]]; then
                  profile="$HOME/.bash_profile"
              else
                  profile="$HOME/.bashrc"
              fi
              ;;
        fish) profile="$HOME/.config/fish/config.fish" ;;
        *)    profile="$HOME/.bashrc" ;;
    esac

    log_info "Shell: $user_shell -> $profile"

    if grep -q "broai/config" "$profile" 2>/dev/null; then
        log_info "Already configured in $profile"
    else
        printf '\n# broai\n%s\n' "$secrets_line" >> "$profile"
        log_info "Added to $profile"
    fi
}

# ─── Compile ──────────────────────────────────────────────────────────────────
compile() {
    log_step "Compiling..."

    cd "$SCRIPT_DIR"
    make clean &>/dev/null || true

    if make; then
        log_info "Build successful: $SCRIPT_DIR/broai"
    else
        log_error "Compilation failed"
        exit 1
    fi
}

# ─── Install binary ───────────────────────────────────────────────────────────
install_binary() {
    log_step "Installing binary..."

    local prefix="/usr/local/bin"

    if sudo cp "$SCRIPT_DIR/broai" "$prefix/broai" && \
       sudo ln -sf "$prefix/broai" "$prefix/bro"; then
        log_info "Installed: $prefix/broai"
        log_info "Alias:     $prefix/bro -> $prefix/broai"
    else
        log_warn "sudo install failed — adding to PATH instead"
        local profile

        if [[ "$OSTYPE" == "darwin"* ]]; then
            profile="$HOME/.zshrc"
        else
            profile="$HOME/.bashrc"
        fi

        grep -q "$SCRIPT_DIR" "$profile" 2>/dev/null || \
            printf '\nexport PATH="%s:$PATH"\n' "$SCRIPT_DIR" >> "$profile"

        log_info "Added $SCRIPT_DIR to PATH in $profile"
    fi
}

# ─── gitignore ───────────────────────────────────────────────────────────────
create_gitignore() {
    if [ ! -f "$SCRIPT_DIR/.gitignore" ]; then
        printf '.env\n*.o\nbroai\nbro\n' > "$SCRIPT_DIR/.gitignore"
        log_info "Created .gitignore"
    fi
}

# ─── Finish ───────────────────────────────────────────────────────────────────
finish() {
    echo
    echo -e "${GREEN}${BOLD}========================================${NC}"
    echo -e "${GREEN}${BOLD}  broai installed successfully!         ${NC}"
    echo -e "${GREEN}${BOLD}========================================${NC}"
    echo
    echo -e "  Reload your shell:"
    echo -e "  ${BOLD}  source ~/.zshrc${NC}   (or open a new terminal)"
    echo
    echo -e "  ${YELLOW}${BOLD}  ⚡ To apply now in this terminal:${NC}"
    echo -e "  ${BOLD}  source ~/.broai/config${NC}"
    echo
    echo -e "  Usage:"
    echo -e "  ${BOLD}  bro \"find files larger than 100mb\"${NC}"
    echo -e "  ${BOLD}  bro -e \"show disk usage\"${NC}   (with explanation)"
    echo

    if [ "$NEEDS_KEY" -eq 1 ] && [ -z "$API_KEY" ]; then
        echo -e "${YELLOW}${BOLD}  ⚠  API key not set.${NC}"
        echo -e "  Add it to ~/.broai/config when ready:"
        echo
        echo -e "  ${BOLD}  echo 'export OPENAI_API_KEY=\"sk-...\"' >> ~/.broai/config${NC}"
        echo -e "  ${BOLD}  source ~/.broai/config${NC}"
        echo
    fi

    echo -e "  Debug log: ${BOLD}/tmp/broai_last.log${NC}"
    echo
}

# ─── Main ─────────────────────────────────────────────────────────────────────
main() {
    echo
    echo -e "${CYAN}${BOLD}  broai — AI terminal assistant${NC}"
    echo -e "${CYAN}  github.com/yourname/broai${NC}"
    echo

    check_dependencies
    setup_provider
    setup_api_key
    save_config
    setup_shell
    compile
    install_binary
    create_gitignore
    finish
}

main "$@"