CC = gcc
CFLAGS = -Wall -Wextra -O2
LIBS = -lcurl -ljson-c -lpthread
TARGET = broai
ALIAS = bro
PREFIX = /usr/local/bin

# macOS Homebrew paths for json-c
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    ifneq ($(shell brew --prefix json-c 2>/dev/null),)
        JSONC_PREFIX = $(shell brew --prefix json-c)
        CFLAGS += -I$(JSONC_PREFIX)/include
        LIBS += -L$(JSONC_PREFIX)/lib
    endif
endif

all: $(TARGET)

$(TARGET): broai.c
	@# Убиваем старый процесс если запущен
	@pkill -x $(TARGET) 2>/dev/null || pkill -x $(ALIAS) 2>/dev/null || true
	@# Удаляем старый бинарь чтобы линкер мог перезаписать
	@rm -f $(TARGET)
	$(CC) $(CFLAGS) -o $(TARGET) broai.c $(LIBS)

# Full install — runs install.sh (API key setup, permissions, shell config)
install: $(TARGET)
	@if [ -f ./install.sh ]; then \
		bash ./install.sh; \
	else \
		$(MAKE) install-bin; \
	fi

# Quick install — binary only, no setup wizard
install-bin: $(TARGET)
	sudo cp $(TARGET) $(PREFIX)/$(TARGET)
	sudo ln -sf $(PREFIX)/$(TARGET) $(PREFIX)/$(ALIAS)
	@echo "Installed: $(PREFIX)/$(TARGET)"
	@echo "Alias:     $(PREFIX)/$(ALIAS) -> $(PREFIX)/$(TARGET)"

uninstall:
	sudo rm -f $(PREFIX)/$(TARGET) $(PREFIX)/$(ALIAS)
	@echo "Uninstalled $(TARGET) and $(ALIAS)"

clean:
	@pkill -x $(TARGET) 2>/dev/null || true
	rm -f $(TARGET)

.PHONY: all install install-bin uninstall clean