#!/bin/bash
#
# how: тесты безопасности
#

# Не используем set -e, чтобы тесты продолжались после ошибок

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOW="$SCRIPT_DIR/how"
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASSED=0
FAILED=0

log_pass() {
    echo -e "${GREEN}[PASS]${NC} $1"
    ((PASSED++))
}

log_fail() {
    echo -e "${RED}[FAIL]${NC} $1"
    ((FAILED++))
}

log_info() {
    echo -e "${YELLOW}[INFO]${NC} $1"
}

# Проверка: слишком длинный API ключ
test_long_api_key() {
    log_info "Тест: слишком длинный API ключ (>256 символов)"

    export OPENAI_API_KEY=$(python3 -c "print('A' * 300)")

    local output=$($HOW "test" 2>&1 || true)

    if echo "$output" | grep -q "недействительный"; then
        log_pass "Длинный ключ отклонён"
    else
        log_fail "Длинный ключ не отклонён"
    fi
}

# Проверка: спецсимволы в API ключе
test_invalid_api_key_chars() {
    log_info "Тест: спецсимволы в API ключе"

    export OPENAI_API_KEY="sk-test;rm -rf /"

    local output=$($HOW "test" 2>&1 || true)

    if echo "$output" | grep -q "недействительный"; then
        log_pass "Ключ со спецсимволами отклонён"
    else
        log_fail "Ключ со спецсимволами не отклонён"
    fi
}

# Вспомогательная функция: запустить mock HTTP сервер и вернуть команду от "API"
# Использует nc (netcat) для одноразового ответа
run_with_mock_api() {
    local mock_cmd="$1"
    local port=18765

    # Формируем OpenAI-compatible ответ
    local response_body='{"choices":[{"message":{"content":"'"$mock_cmd"'","role":"assistant"}}],"model":"mock"}'
    local response_len=${#response_body}

    # Запускаем nc как одноразовый HTTP сервер в фоне
    {
        printf "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s"             "$response_len" "$response_body"
    } | nc -l -p $port -q 1 > /dev/null 2>&1 &

    local nc_pid=$!
    sleep 0.2  # дать nc время запуститься

    # Запускаем how с моком
    local output
    output=$(AI_PROVIDER="openai"              OPENAI_API_KEY="test-key-123"              AI_BASH_URL="http://127.0.0.1:${port}/v1/chat/completions"              AI_BASH_MODEL="mock"              echo "" | $HOW "test" 2>&1 || true)

    kill $nc_pid 2>/dev/null || true
    echo "$output"
}

# Проверка: команда с подстановкой $(...)
test_command_substitution() {
    log_info "Тест: команда с подстановкой \$(...)"

    local output
    output=$(run_with_mock_api 'echo "$(cat /etc/passwd)"')

    if echo "$output" | grep -q "опасные конструкции"; then
        log_pass "Подстановка команд заблокирована приложением"
    else
        log_fail "Подстановка команд не заблокирована. Вывод: $output"
    fi
}

# Проверка: команда с backticks
test_backticks() {
    log_info "Тест: команда с backticks"

    local output
    output=$(run_with_mock_api "echo \`cat /etc/passwd\`")

    if echo "$output" | grep -q "опасные конструкции"; then
        log_pass "Backticks заблокированы приложением"
    else
        log_fail "Backticks не заблокированы. Вывод: $output"
    fi
}

# Проверка: перенаправление в /etc/
test_redirect_to_etc() {
    log_info "Тест: перенаправление в /etc/"

    local output
    output=$(run_with_mock_api "echo hack > /etc/passwd")

    if echo "$output" | grep -q "опасные конструкции"; then
        log_pass "Перенаправление в /etc/ заблокировано приложением"
    else
        log_fail "Перенаправление в /etc/ не заблокировано. Вывод: $output"
    fi
}

# Проверка: eval команда
test_eval_command() {
    log_info "Тест: команда eval"

    local output
    output=$(run_with_mock_api "eval \"rm -rf /\"")

    if echo "$output" | grep -q "опасные конструкции"; then
        log_pass "Команда eval заблокирована приложением"
    else
        log_fail "Команда eval не заблокирована. Вывод: $output"
    fi
}

# Проверка: bash -c
test_bash_c() {
    log_info "Тест: команда bash -c"

    local output
    output=$(run_with_mock_api "bash -c \"rm -rf /\"")

    if echo "$output" | grep -q "опасные конструкции"; then
        log_pass "Команда bash -c заблокирована приложением"
    else
        log_fail "Команда bash -c не заблокирована. Вывод: $output"
    fi
}

# Проверка: деструктивная команда rm -rf /
test_destructive_rm() {
    log_info "Тест: деструктивная команда rm -rf /"

    local output
    output=$(run_with_mock_api "rm -rf /")

    if echo "$output" | grep -qE "опасные конструкции|деструктивная"; then
        log_pass "rm -rf / заблокирована приложением"
    else
        log_fail "rm -rf / не заблокирована. Вывод: $output"
    fi
}

# Проверка: деструктивная команда dd
test_destructive_dd() {
    log_info "Тест: деструктивная команда dd if=/dev/zero"

    local output
    output=$(run_with_mock_api "dd if=/dev/zero of=/dev/sda")

    if echo "$output" | grep -qE "опасные конструкции|деструктивная"; then
        log_pass "dd if=/dev/zero заблокирована приложением"
    else
        log_fail "dd if=/dev/zero не заблокирована. Вывод: $output"
    fi
}

# Проверка: слишком длинный ввод
test_long_input() {
    log_info "Тест: слишком длинный ввод (>4096 символов)"

    export OPENAI_API_KEY="test-key-123"

    local long_input=$(python3 -c "print('A' * 5000)")
    local output=$($HOW "$long_input" 2>&1 || true)

    if echo "$output" | grep -q "слишком длинный"; then
        log_pass "Длинный ввод отклонён"
    else
        log_fail "Длинный ввод не отклонён"
    fi
}

# Проверка: файл секретов имеет правильные права
test_secrets_permissions() {
    log_info "Тест: права на файл секретов"
    
    if [ -f "$HOME/.bash_secrets" ]; then
        local perms=$(stat -f "%Lp" "$HOME/.bash_secrets" 2>/dev/null || stat -c "%a" "$HOME/.bash_secrets" 2>/dev/null)
        
        if [ "$perms" = "600" ]; then
            log_pass "Файл секретов имеет права 600"
        else
            log_fail "Файл секретов имеет права $perms (ожидалось 600)"
        fi
    else
        log_info "Файл секретов не найден (пропущено)"
    fi
}

# Проверка: .gitignore существует
test_gitignore() {
    log_info "Тест: наличие .gitignore"
    
    if [ -f "$SCRIPT_DIR/.gitignore" ]; then
        if grep -q ".env" "$SCRIPT_DIR/.gitignore"; then
            log_pass ".gitignore содержит .env"
        else
            log_fail ".gitignore не содержит .env"
        fi
    else
        log_fail ".gitignore не найден"
    fi
}

# Проверка: временный файл не остаётся
test_no_temp_files() {
    log_info "Тест: отсутствие временных файлов"
    
    if ls /tmp/tmp.* 1>/dev/null 2>&1; then
        log_fail "Найдены временные файлы в /tmp"
    else
        log_pass "Временные файлы не найдены"
    fi
}

# Запуск всех тестов
main() {
    echo
    echo "=========================================="
    echo "how: тесты безопасности"
    echo "=========================================="
    echo

    # Проверка наличия бинаря
    if [ ! -f "$HOW" ]; then
        echo -e "${RED}[ERROR]${NC} Бинарь не найден: $HOW"
        echo "Выполните: make"
        exit 1
    fi

    # Проверка наличия nc (netcat) для mock-тестов
    NC_AVAILABLE=false
    if command -v nc &>/dev/null; then
        NC_AVAILABLE=true
        log_info "nc (netcat) найден — mock-тесты будут запущены"
    else
        log_info "nc (netcat) не найден — mock-тесты пропущены (установите netcat)"
    fi

    # Функциональные тесты через mock API (проверяют реальное поведение приложения)
    if [ "$NC_AVAILABLE" = true ]; then
        test_command_substitution
        test_backticks
        test_redirect_to_etc
        test_eval_command
        test_bash_c
        test_destructive_rm
        test_destructive_dd
    fi

    # Тесты с окружением
    test_long_api_key
    test_invalid_api_key_chars
    test_long_input

    # Тесты файловой системы
    test_secrets_permissions
    test_gitignore
    test_no_temp_files
    
    echo
    echo "=========================================="
    echo "Результаты"
    echo "=========================================="
    echo -e "${GREEN}Пройдено:${NC} $PASSED"
    echo -e "${RED}Провалено:${NC} $FAILED"
    echo
    
    if [ $FAILED -eq 0 ]; then
        echo -e "${GREEN}Все тесты пройдены!${NC}"
        exit 0
    else
        echo -e "${RED}Некоторые тесты провалены!${NC}"
        exit 1
    fi
}

main "$@"