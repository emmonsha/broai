#!/bin/bash
#
# broai: installation script (multi-provider)
# For Ollama (local, no API key): ./setup_ollama.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_DIR="$HOME/.broai"
CONFIG_FILE="$CONFIG_DIR/config"
PREFIX="/usr/local/bin"

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

# ─── Header ───────────────────────────────────────────────────────────────────
echo
echo -e "${CYAN}${BOLD}  broai — AI terminal assistant${NC}"
echo -e "${CYAN}  For Ollama (local, no API key): ./setup_ollama.sh${NC}"
echo

# ─── What will be installed ───────────────────────────────────────────────────
echo -e "  The following will be set up:"
echo -e "    Binary:  ${BOLD}$PREFIX/broai${NC}  +  ${BOLD}$PREFIX/bro${NC}"
echo -e "    Config:  ${BOLD}~/.broai/config${NC}  (chmod 600)"
echo -e "    Shell:   source line added to your shell profile"
echo
read -p "  Proceed? [y/N] " -n 1 -r; echo
echo
[[ $REPLY =~ ^[Yy]$ ]] || exit 0

# ─── Dependencies ─────────────────────────────────────────────────────────────
log_step "Checking dependencies..."

missing=()
command -v gcc  &>/dev/null || missing+=("gcc")
command -v curl &>/dev/null || missing+=("curl")
pkg-config --exists json-c 2>/dev/null || missing+=("json-c")

if [ ${#missing[@]} -ne 0 ]; then
    log_warn "Missing: ${missing[*]}"
    if [[ "$OSTYPE" == "darwin"* ]]; then
        log_info "macOS:         brew install ${missing[*]}"
    elif [ -f /etc/debian_version ]; then
        log_info "Debian/Ubuntu: sudo apt install libcurl4-openssl-dev libjson-c-dev gcc"
    elif [ -f /etc/redhat-release ]; then
        log_info "RHEL/Fedora:   sudo dnf install libcurl-devel json-c-devel gcc"
    else
        log_warn "Please install missing dependencies manually"
    fi
    read -p "  Continue anyway? (y/N): " -n 1 -r; echo
    [[ $REPLY =~ ^[Yy]$ ]] || exit 1
else
    log_info "All dependencies found"
fi

# ─── Compile ──────────────────────────────────────────────────────────────────
if [ "${SKIP_COMPILE}" = "1" ]; then
    log_step "Compiling..."
    log_info "Skipping — already built by make"
else
    log_step "Compiling..."
    cd "$SCRIPT_DIR"

    # Определяем CFLAGS (macOS Homebrew json-c)
    EXTRA_CFLAGS=""
    if [[ "$OSTYPE" == "darwin"* ]]; then
        JSONC_PREFIX=$(brew --prefix json-c 2>/dev/null || true)
        [ -n "$JSONC_PREFIX" ] && EXTRA_CFLAGS="-I$JSONC_PREFIX/include -L$JSONC_PREFIX/lib"
    fi

    rm -f broai
    if gcc -Wall -Wextra -O2 $EXTRA_CFLAGS -o broai broai.c -lcurl -ljson-c -lpthread; then
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
    log_warn "sudo install failed — adding project dir to PATH instead"
    FALLBACK_PROFILE="$HOME/$( [[ "$OSTYPE" == darwin* ]] && echo .zshrc || echo .bashrc )"
    grep -q "$SCRIPT_DIR" "$FALLBACK_PROFILE" 2>/dev/null || \
        printf '\nexport PATH="%s:$PATH"\n' "$SCRIPT_DIR" >> "$FALLBACK_PROFILE"
    log_info "Added $SCRIPT_DIR to PATH in $FALLBACK_PROFILE"
fi

# ─── Config ───────────────────────────────────────────────────────────────────
log_step "Setting up config..."

mkdir -p "$CONFIG_DIR" && chmod 700 "$CONFIG_DIR"
if [ ! -f "$CONFIG_FILE" ]; then
    touch "$CONFIG_FILE" && chmod 600 "$CONFIG_FILE"
    log_info "Created $CONFIG_FILE (chmod 600)"
else
    perms=$(stat -c "%a" "$CONFIG_FILE" 2>/dev/null || stat -f "%A" "$CONFIG_FILE" 2>/dev/null)
    [ "$perms" != "600" ] && chmod 600 "$CONFIG_FILE" && \
        log_warn "Fixed permissions: $perms → 600"
    log_info "$CONFIG_FILE already exists — preserved"
fi

# ─── Shell profile ────────────────────────────────────────────────────────────
log_step "Configuring shell..."

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
    printf '\n# broai\n[ -f ~/.broai/config ] && source ~/.broai/config\n' >> "$profile"
    log_info "Added to $profile"
fi

# ─── gitignore ────────────────────────────────────────────────────────────────
if [ ! -f "$SCRIPT_DIR/.gitignore" ]; then
    printf '.env\n*.o\nbroai\nbro\n' > "$SCRIPT_DIR/.gitignore"
fi

# ─── Done ─────────────────────────────────────────────────────────────────────
echo
echo -e "${GREEN}${BOLD}========================================${NC}"
echo -e "${GREEN}${BOLD}  broai installed successfully!         ${NC}"
echo -e "${GREEN}${BOLD}========================================${NC}"
echo
# echo -e "  Set your API key in ${BOLD}~/.broai/config${NC}:"
# echo -e "  ${BOLD}  echo 'export OPENAI_API_KEY=\"sk-...\"' >> ~/.broai/config${NC}"
# echo
# echo -e "  Apply config now:"
# echo -e "  ${BOLD}  source ~/.broai/config${NC}"
# echo
echo -e "  Usage:"
echo -e "  ${BOLD}  bro \"find files larger than 100mb\"${NC}"
echo -e "  ${BOLD}  bro -e \"show disk usage\"${NC}        # with explanation"
echo -e "  ${BOLD}  bro -i \"remove old logs\"${NC}        # with detailed info"
echo -e "  ${BOLD}  bro -e -i \"compress directory\"${NC}  # both"
echo
echo -e "  Debug log: ${BOLD}/tmp/broai_last.log${NC}"
echo