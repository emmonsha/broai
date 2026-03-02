CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LIBS    = -lcurl -ljson-c -lpthread
TARGET  = broai
ALIAS   = bro
PREFIX  = /usr/local/bin

# macOS Homebrew paths for json-c
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    ifneq ($(shell brew --prefix json-c 2>/dev/null),)
        JSONC_PREFIX = $(shell brew --prefix json-c)
        CFLAGS += -I$(JSONC_PREFIX)/include
        LIBS   += -L$(JSONC_PREFIX)/lib
    endif
endif

# ─── Build ────────────────────────────────────────────────────────────────────

all: $(TARGET)

$(TARGET): broai.c
	@pkill -x $(TARGET) 2>/dev/null || pkill -x $(ALIAS) 2>/dev/null || true
	@rm -f $(TARGET)
	$(CC) $(CFLAGS) -o $(TARGET) broai.c $(LIBS)

# ollama ветка: только Ollama + qwen2.5:7b, без API-ключей
ollama: broai_ollama.c
	@pkill -x $(TARGET) 2>/dev/null || pkill -x $(ALIAS) 2>/dev/null || true
	@rm -f $(TARGET)
	$(CC) $(CFLAGS) -o $(TARGET) broai_ollama.c $(LIBS)
	@echo "Built: broai (ollama / qwen2.5:7b)"

# ─── Install ──────────────────────────────────────────────────────────────────
# make сначала собирает бинарь, затем передаёт SKIP_COMPILE=1 в скрипт
# чтобы скрипт не вызывал make повторно (рекурсия)

install: $(TARGET)
	@SKIP_COMPILE=1 bash ./install.sh

install-ollama: ollama
	@SKIP_COMPILE=1 bash ./setup_ollama.sh

# Quick install: только бинарь, без скриптов (для CI)
install-bin: $(TARGET)
	sudo cp $(TARGET) $(PREFIX)/$(TARGET)
	sudo ln -sf $(PREFIX)/$(TARGET) $(PREFIX)/$(ALIAS)
	@echo "Installed: $(PREFIX)/$(TARGET)"
	@echo "Alias:     $(PREFIX)/$(ALIAS) -> $(PREFIX)/$(TARGET)"

# ─── Uninstall ────────────────────────────────────────────────────────────────

uninstall:
	@echo ""
	@echo "  Uninstalling broai..."
	@echo ""
	@echo "  The following will be removed:"
	@echo "    Binary:  $(PREFIX)/$(TARGET)"
	@echo "    Symlink: $(PREFIX)/$(ALIAS)"
	@echo "    Config:  ~/.broai/"
	@echo "    History: ~/.how_history"
	@echo "    Log:     /tmp/broai_last.log"
	@echo "    Shell:   broai lines from your shell profile"
	@echo ""
	@read -p "  Proceed? [y/N] " ans && [ "$$ans" = "y" ] || [ "$$ans" = "Y" ] || exit 0
	@echo ""
	@echo "  [1/4] Removing binary..."
	@sudo rm -f $(PREFIX)/$(TARGET) $(PREFIX)/$(ALIAS) && \
	    echo "        removed $(PREFIX)/$(TARGET)" || \
	    echo "        $(PREFIX)/$(TARGET) not found (skipped)"
	@echo ""
	@echo "  [2/4] Removing config..."
	@rm -rf ~/.broai && echo "        removed ~/.broai/" || true
	@echo ""
	@echo "  [3/4] Removing history and logs..."
	@rm -f ~/.how_history && echo "        removed ~/.how_history" || true
	@rm -f /tmp/broai_last.log && echo "        removed /tmp/broai_last.log" || true
	@echo ""
	@echo "  [4/4] Cleaning shell profile..."
	@for profile in ~/.zprofile ~/.zshrc ~/.bash_profile ~/.bashrc; do \
	    if [ -f "$$profile" ] && grep -q "broai" "$$profile" 2>/dev/null; then \
	        cp "$$profile" "$$profile.broai_backup"; \
	        grep -v -E "(# broai|broai/config)" "$$profile.broai_backup" > "$$profile"; \
	        rm -f "$$profile.broai_backup"; \
	        echo "        cleaned $$profile"; \
	    fi; \
	done
	@echo ""
	@echo "  broai removed. Open a new terminal to apply changes."
	@echo ""

# ─── Misc ─────────────────────────────────────────────────────────────────────

clean:
	@pkill -x $(TARGET) 2>/dev/null || true
	rm -f $(TARGET)

.PHONY: all ollama install install-ollama install-bin uninstall clean