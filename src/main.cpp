/*
 * main.cpp — Точка входа
 * 
 * CryptoTrader v2.0 — AI-Стратег
 * DeepSeek + CoinEx API v2
 * 
 * Режимы:
 *   --dry-run              Симуляция (по умолчанию)
 *   --live                 Реальная торговля
 *   --semi-auto            Стратег-советник (DeepSeek предлагает, вы подтверждаете)
 *   --auto                 Авто-стратег (DeepSeek торгует сам)
 *   --manual               Ручной режим (по умолчанию)
 */

#include "cli.h"
#include "config.h"
#include "llm_agent.h"
#include "validator.h"
#include "exchange_client.h"
#include "audit.h"
#include "utils.h"

#include <iostream>
#include <memory>
#include <csignal>

// ============================================================================
static std::unique_ptr<CLI> g_cli;

void signalHandler(int sig) {
    std::cout << "\n[!] Сигнал " << sig << ". Завершение...\n";
    if (g_cli) g_cli->shutdown();
    exit(0);
}

// ============================================================================
void printHelp() {
    std::cout << R"(
╔══════════════════════════════════════════════════════════════╗
║          CryptoTrader v2.0 — AI-Стратег                     ║
║          DeepSeek + CoinEx API v2                           ║
╚══════════════════════════════════════════════════════════════╝

Использование:
  ./crypto_trader [опции]

Опции:
  --live                   Реальная торговля
  --dry-run                Симуляция (по умолчанию)
  --semi-auto              Стратег-советник
  --auto                   Авто-стратег
  --manual                 Ручной режим (по умолчанию)
  --key <access_id>        CoinEx API Key (Access ID)
  --secret <secret_key>    CoinEx Secret Key
  --deepseek-key <key>     DeepSeek API Key
  --help                   Справка
  --version                Версия

Переменные окружения:
  DEEPSEEK_API_KEY         Ключ DeepSeek
  COINEX_API_KEY           CoinEx Access ID
  COINEX_API_SECRET        CoinEx Secret Key

Примеры:
  ./crypto_trader                                    # DRY-RUN, ручной
  ./crypto_trader --live --manual                    # LIVE, ручной
  ./crypto_trader --semi-auto                        # Стратег-советник
  ./crypto_trader --live --auto                      # Авто-стратег (осторожно!)
)" << std::endl;
}

// ============================================================================
std::shared_ptr<AppConfig> parseArgs(int argc, char* argv[]) {
    auto config = std::make_shared<AppConfig>();
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--help") {
            printHelp();
            exit(0);
        }
        else if (arg == "--version") {
            std::cout << "CryptoTrader v" << APP_VERSION << "\n";
            exit(0);
        }
        else if (arg == "--live") {
            config->mode = TradingMode::LIVE;
        }
        else if (arg == "--dry-run") {
            config->mode = TradingMode::DRY_RUN;
        }
        else if (arg == "--semi-auto" || arg == "--semi") {
            config->strat_mode = StrategistMode::SEMI_AUTO;
        }
        else if (arg == "--auto") {
            config->strat_mode = StrategistMode::FULL_AUTO;
        }
        else if (arg == "--manual") {
            config->strat_mode = StrategistMode::MANUAL;
        }
        else if (arg == "--key" && i + 1 < argc) {
            config->access_id = argv[++i];
        }
        else if (arg == "--secret" && i + 1 < argc) {
            config->secret_key = argv[++i];
        }
        else if (arg == "--deepseek-key" && i + 1 < argc) {
            config->deepseek_key = argv[++i];
        }
        else if (arg == "--config" && i + 1 < argc) {
            config->config_file = argv[++i];
        }
    }
    
    // Переменные окружения (приоритет ниже, чем аргументы CLI)
    if (config->access_id.empty()) {
        config->access_id = SysUtil::getEnv("COINEX_API_KEY");
    }
    if (config->secret_key.empty()) {
        config->secret_key = SysUtil::getEnv("COINEX_API_SECRET");
    }
    if (config->deepseek_key.empty()) {
        config->deepseek_key = SysUtil::getEnv("DEEPSEEK_API_KEY");
    }
    
    return config;
}

// ============================================================================
int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    std::cout << APP_NAME << " v" << APP_VERSION << "\n\n";
    
    auto config = parseArgs(argc, argv);
    
    // Вывод режимов
    if (config->mode == TradingMode::DRY_RUN) {
        std::cout << "⚪ DRY-RUN (симуляция)\n";
    } else {
        std::cout << "🔴 LIVE — реальная торговля. Сделки исполняются автоматически.\n";
    }
    
    std::string sm;
    switch (config->strat_mode) {
        case StrategistMode::MANUAL:   sm = "Ручной"; break;
        case StrategistMode::SEMI_AUTO: sm = "Стратег-Советник"; break;
        case StrategistMode::FULL_AUTO: sm = "Авто-Стратег"; break;
    }
    std::cout << "Режим стратега: " << sm << "\n\n";
    
    try {
        // Аудит
        auto audit = std::make_shared<AuditLogger>(
            "crypto_trader.db", "trades.jsonl");
        audit->logSystemEvent("startup",
            (config->mode == TradingMode::LIVE ? "LIVE" : "DRY") +
            std::string(":") + sm);
        
        // Биржа
        auto exchange = std::make_shared<ExchangeClient>(
            config->access_id, config->secret_key);
        
        // Валидатор
        auto validator = std::make_shared<Validator>();
        validator->loadLimits();
        
        // DeepSeek
        auto llm = std::make_shared<LLMAgent>(config->deepseek_key);
        
        // CLI
        g_cli = std::make_unique<CLI>(config, llm, validator, exchange, audit);
        g_cli->run();
        
    } catch (const std::exception& e) {
        std::cerr << "[КРИТИЧЕСКАЯ ОШИБКА] " << e.what() << "\n";
        return 1;
    }
    
    std::cout << "Завершено.\n";
    return 0;
}
