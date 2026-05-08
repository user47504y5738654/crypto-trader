/*
 * main.cpp — Главная точка входа программы
 * 
 * CryptoTrader — консольный AI-трейдер для биржи CoinEx.
 * Принимает инструкции на естественном языке через DeepSeek API,
 * выполняет валидацию и отправляет ордера на биржу.
 * 
 * Поддерживает 2 режима:
 *   - DRY RUN  (по умолчанию): тестирование без реальных торгов
 *   - LIVE     (флаг --live):   активная торговля с подтверждением
 * 
 * Архитектура:
 *   CLI (main.cpp/cli.cpp)
 *       ↓
 *   LLM Agent (llm_agent.cpp) → DeepSeek API
 *       ↓
 *   Validator (validator.cpp) — проверка рисков
 *       ↓
 *   Exchange Client (exchange_client.cpp) → CoinEx API
 *       ↓
 *   Audit (audit.cpp) — логирование в SQLite + JSONL
 * 
 * Автор: Разработчик
 * Версия: 1.0.0
 */

#include "cli.h"
#include "config.h"
#include "llm_agent.h"
#include "validator.h"
#include "exchange_client.h"
#include "audit.h"

#include <iostream>
#include <memory>
#include <csignal>

// ============================================================================
// Глобальные указатели для graceful shutdown
// ============================================================================
static std::unique_ptr<CLI> g_cli = nullptr;

// ============================================================================
// Обработчик сигналов (Ctrl+C, завершение процесса)
// ============================================================================
void signalHandler(int signal) {
    std::cout << "\n[!] Получен сигнал " << signal << ". Завершение работы...\n";
    if (g_cli) {
        g_cli->shutdown();
    }
    exit(0);
}

// ============================================================================
// Вывод справки
// ============================================================================
void printHelp() {
    std::cout << R"(
╔══════════════════════════════════════════════════════════════╗
║             CryptoTrader — AI-трейдер v1.0                  ║
║         Консольный AI-агент для биржи CoinEx                ║
╚══════════════════════════════════════════════════════════════╝

Использование:
  ./crypto_trader [опции]

Опции:
  --live              Включить режим реальной торговли
  --config <файл>     Загрузить конфигурацию из файла
  --key <key>         API ключ CoinEx
  --secret <secret>   Секретный ключ CoinEx
  --deepseek-key <k>  API ключ DeepSeek
  --dry-run           Режим симуляции (по умолчанию)
  --help              Показать эту справку
  --version           Показать версию

Пример:
  ./crypto_trader                          # Запуск в dry-run режиме
  ./crypto_trader --live                   # Реальная торговля
  ./crypto_trader --live --key XXX --secret YYY

Команды (внутри программы):
  > купи 0.15 ETH по рынку, тейк 10%, стоп 3%
  > продай 0.5 BTC, лимит 65000, тейк 5%
  > баланс
  > статус
  > история
  > лимиты
  > помощь
  > выход

ВНИМАНИЕ: Режим LIVE использует реальные средства!
По умолчанию включён режим DRY RUN (симуляция).
)" << std::endl;
}

// ============================================================================
// Парсинг аргументов командной строки
// ============================================================================
std::shared_ptr<AppConfig> parseArgs(int argc, char* argv[]) {
    auto config = std::make_shared<AppConfig>();
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--help") {
            printHelp();
            exit(0);
        } else if (arg == "--version") {
            std::cout << "CryptoTrader v" << APP_VERSION << std::endl;
            exit(0);
        } else if (arg == "--live") {
            config->mode = TradingMode::LIVE;
        } else if (arg == "--dry-run") {
            config->mode = TradingMode::DRY_RUN;
        } else if (arg == "--config" && i + 1 < argc) {
            config->config_file = argv[++i];
        } else if (arg == "--key" && i + 1 < argc) {
            config->api_key = argv[++i];
        } else if (arg == "--secret" && i + 1 < argc) {
            config->api_secret = argv[++i];
        } else if (arg == "--deepseek-key" && i + 1 < argc) {
            config->deepseek_key = argv[++i];
        }
    }
    
    return config;
}

// ============================================================================
// Точка входа
// ============================================================================
int main(int argc, char* argv[]) {
    // Устанавливаем обработчик сигналов
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    setlocale(LC_ALL, "ru_RU.UTF-8");
    
    std::cout << APP_NAME << " v" << APP_VERSION << std::endl;
    std::cout << "Режим: анализ инструкций через DeepSeek + исполнение на CoinEx\n" << std::endl;
    
    // Парсим аргументы
    auto config = parseArgs(argc, argv);
    
    if (config->mode == TradingMode::DRY_RUN) {
        std::cout << "⚪ Режим: DRY RUN (симуляция без реальной торговли)\n";
        std::cout << "   Чтобы переключиться в LIVE, используйте флаг --live\n\n";
    } else {
        std::cout << "🔴 Режим: LIVE (реальная торговля!)\n";
        std::cout << "   ⚠ ВНИМАНИЕ: будут использованы реальные средства!\n\n";
        
        // Запрашиваем подтверждение
        std::cout << "Подтвердите переход в LIVE режим (yes/no): ";
        std::string confirmation;
        std::getline(std::cin, confirmation);
        
        if (confirmation != "yes" && confirmation != "y") {
            std::cout << "Отмена. Переключение в DRY RUN режим.\n";
            config->mode = TradingMode::DRY_RUN;
        }
    }
    
    // Создаём компоненты приложения
    try {
        // Инициализация аудита (SQLite + JSONL)
        auto audit = std::make_shared<AuditLogger>("crypto_trader.db", "trades.jsonl");
        audit->logSystemEvent("Запуск приложения", 
            config->mode == TradingMode::LIVE ? "LIVE" : "DRY_RUN");
        
        // Инициализация клиента биржи
        auto exchange = std::make_shared<ExchangeClient>(
            config->api_key, config->api_secret);
        
        // Инициализация валидатора
        auto validator = std::make_shared<Validator>();
        validator->loadLimits();  // Загружаем лимиты из SQLite
        
        // Инициализация LLM агента
        auto llm_agent = std::make_shared<LLMAgent>(config->deepseek_key);
        
        // Создаём и запускаем CLI
        g_cli = std::make_unique<CLI>(config, llm_agent, validator, exchange, audit);
        g_cli->run();
        
    } catch (const std::exception& e) {
        std::cerr << "[КРИТИЧЕСКАЯ ОШИБКА] " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "Программа завершена. Спасибо за использование!\n";
    return 0;
}
