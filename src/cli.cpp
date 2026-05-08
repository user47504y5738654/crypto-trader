/*
 * cli.cpp — Реализация CLI-оболочки
 * 
 * Обрабатывает пользовательский ввод, маршрутизирует команды
 * и отображает результаты в удобном виде.
 */

#include "cli.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <ctime>

// ============================================================================
// Конструктор
// ============================================================================
CLI::CLI(std::shared_ptr<AppConfig> config,
         std::shared_ptr<LLMAgent> llm,
         std::shared_ptr<Validator> validator,
         std::shared_ptr<ExchangeClient> exchange,
         std::shared_ptr<AuditLogger> audit)
    : m_config(config)
    , m_llm(llm)
    , m_validator(validator)
    , m_exchange(exchange)
    , m_audit(audit)
{
    // Приветственное сообщение
    std::cout << Color::GREEN << "╔══════════════════════════════════════╗\n";
    std::cout << "║  Система инициализирована           ║\n";
    std::cout << "║  Введите 'помощь' для списка команд  ║\n";
    std::cout << "╚══════════════════════════════════════╝\n" << Color::RESET;
}

// ============================================================================
// Деструктор
// ============================================================================
CLI::~CLI() {
    if (m_audit) {
        m_audit->logSystemEvent("Завершение приложения", "shutdown");
    }
}

// ============================================================================
// Запуск основного цикла
// ============================================================================
void CLI::run() {
    std::string input;
    
    while (m_running) {
        printPrompt();
        
        if (!std::getline(std::cin, input)) {
            // EOF (Ctrl+D) или ошибка
            break;
        }
        
        // Пропускаем пустые строки
        if (input.empty()) continue;
        
        // Добавляем в историю
        if (m_history.empty() || m_history.back() != input) {
            m_history.push_back(input);
        }
        m_history_pos = m_history.size();
        
        // Обрабатываем команду
        processCommand(input);
    }
}

// ============================================================================
// Остановка
// ============================================================================
void CLI::shutdown() {
    m_running = false;
}

// ============================================================================
// Вывод приглашения
// ============================================================================
void CLI::printPrompt() {
    std::string mode_str = (m_config->mode == TradingMode::LIVE) 
        ? Color::RED + "LIVE" + Color::RESET 
        : Color::CYAN + "DRY-RUN" + Color::RESET;
    
    std::cout << Color::GREEN << "crypto>" << Color::RESET;
    std::cout << " [" << mode_str << "] ";
    std::cout.flush();
}

// ============================================================================
// Обработка команды
// ============================================================================
void CLI::processCommand(const std::string& input) {
    std::string lower_input = input;
    std::transform(lower_input.begin(), lower_input.end(), lower_input.begin(), ::tolower);
    
    // Определяем команду
    if (lower_input == "выход" || lower_input == "exit" || lower_input == "quit") {
        std::cout << "Завершение работы...\n";
        m_running = false;
        
    } else if (lower_input == "помощь" || lower_input == "help" || lower_input == "?") {
        printHelp();
        
    } else if (lower_input == "баланс" || lower_input == "balance") {
        printBalance();
        
    } else if (lower_input == "статус" || lower_input == "status") {
        printStatus();
        
    } else if (lower_input == "история" || lower_input == "history") {
        printHistory();
        
    } else if (lower_input == "лимиты" || lower_input == "limits") {
        printLimits();
        
    } else if (lower_input == "очистить" || lower_input == "clear") {
        std::cout << "\033[2J\033[1;1H";  // Очистка экрана
        
    } else if (lower_input.find("купи") != std::string::npos ||
               lower_input.find("продай") != std::string::npos ||
               lower_input.find("buy") != std::string::npos ||
               lower_input.find("sell") != std::string::npos) {
        // Торговая команда на естественном языке
        processTradingCommand(input);
        
    } else {
        std::cout << Color::YELLOW << "Неизвестная команда. Введите 'помощь' для списка команд.\n" << Color::RESET;
    }
}

// ============================================================================
// Справка
// ============================================================================
void CLI::printHelp() {
    std::cout << Color::CYAN << "\n╔══════════════════════════════════════════════════╗\n";
    std::cout << "║              СПРАВКА ПО КОМАНДАМ              ║\n";
    std::cout << "╚══════════════════════════════════════════════════╝\n" << Color::RESET;
    
    std::cout << "\n📝 " << Color::BOLD << "Торговые инструкции (на русском):\n" << Color::RESET;
    std::cout << "  \"купи 0.15 ETH по рынку, тейк 10%, стоп 3%\"\n";
    std::cout << "  \"продай 0.5 BTC, лимит 65000 USDT\"\n";
    std::cout << "  \"buy 100 USDT of SOL, take profit 5%\"\n";
    
    std::cout << "\n⚙️ " << Color::BOLD << "Системные команды:\n" << Color::RESET;
    std::cout << "  баланс / balance       — показать баланс\n";
    std::cout << "  статус / status        — статус системы\n";
    std::cout << "  история / history      — история ордеров\n";
    std::cout << "  лимиты / limits        — текущие лимиты\n";
    std::cout << "  помощь / help / ?      — эта справка\n";
    std::cout << "  очистить / clear       — очистить экран\n";
    std::cout << "  выход / exit / quit    — выход\n";
    
    std::cout << "\n" << Color::DIM << "Совет: Используйте стрелки вверх/вниз для истории команд.\n" << Color::RESET;
}

// ============================================================================
// Вывод баланса
// ============================================================================
void CLI::printBalance() {
    std::cout << Color::BLUE << "\n📊 Запрос баланса...\n" << Color::RESET;
    
    try {
        auto balance = m_exchange->getBalance();
        
        std::cout << Color::BOLD << "Баланс:\n" << Color::RESET;
        std::cout << "┌──────────┬──────────────┬────────────────┐\n";
        std::cout << "│ Валюта   │ Доступно      │ Заморожено     │\n";
        std::cout << "├──────────┼──────────────┼────────────────┤\n";
        
        for (const auto& [currency, bal] : balance) {
            if (bal.available > 0.0001 || bal.frozen > 0.0001) {
                std::cout << "│ " << std::left << std::setw(8) << currency << " │ ";
                std::cout << std::right << std::setw(12) << std::fixed << std::setprecision(4) << bal.available << " │ ";
                std::cout << std::setw(14) << bal.frozen << " │\n";
            }
        }
        
        std::cout << "└──────────┴──────────────┴────────────────┘\n";
        
    } catch (const std::exception& e) {
        std::cout << Color::RED << "Ошибка получения баланса: " << e.what() << Color::RESET << "\n";
    }
}

// ============================================================================
// Статус системы
// ============================================================================
void CLI::printStatus() {
    std::cout << Color::MAGENTA << "\nℹ️  Статус системы:\n" << Color::RESET;
    
    std::string mode_str = (m_config->mode == TradingMode::LIVE) 
        ? Color::RED + "LIVE (реальная торговля)" + Color::RESET
        : Color::CYAN + "DRY RUN (симуляция)" + Color::RESET;
    
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    char time_str[100];
    std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", std::localtime(&now_time));
    
    std::cout << "  Режим:      " << mode_str << "\n";
    std::cout << "  Время:      " << time_str << "\n";
    std::cout << "  Версия:     " << APP_VERSION << "\n";
    std::cout << "  Статус API: " << (m_exchange->isConnected() ? Color::GREEN + "OK" : Color::RED + "Ошибка") << Color::RESET << "\n";
    
    // Если dry-run, показываем статистику симуляции
    if (m_config->mode == TradingMode::DRY_RUN) {
        auto stats = m_exchange->getDryRunStats();
        std::cout << "\n📈 Статистика симуляции:\n";
        std::cout << "  Всего сделок:  " << stats.total_trades << "\n";
        std::cout << "  Успешных:      " << stats.successful_trades << "\n";
        std::cout << "  Прибыль:       " << Color::GREEN << "+" << stats.total_profit << " USDT" << Color::RESET << "\n";
    }
}

// ============================================================================
// История ордеров
// ============================================================================
void CLI::printHistory() {
    std::cout << Color::YELLOW << "\n📜 История ордеров:\n" << Color::RESET;
    
    try {
        auto orders = m_audit->getRecentOrders(20);
        
        if (orders.empty()) {
            std::cout << "  История пуста.\n";
            return;
        }
        
        std::cout << "┌──────────┬────────┬──────────┬────────────┬──────────────┐\n";
        std::cout << "│ Время    │ Тип    │ Пара     │ Объём      │ Статус       │\n";
        std::cout << "├──────────┼────────┼──────────┼────────────┼──────────────┤\n";
        
        for (const auto& order : orders) {
            std::cout << "│ " << std::setw(8) << order.time.substr(11, 8) << " │ ";
            std::cout << std::setw(6) << order.side << " │ ";
            std::cout << std::setw(8) << order.symbol << " │ ";
            std::cout << std::setw(10) << std::fixed << std::setprecision(4) << order.amount << " │ ";
            std::cout << std::setw(12) << order.status_str << " │\n";
        }
        
        std::cout << "└──────────┴────────┴──────────┴────────────┴──────────────┘\n";
        
    } catch (const std::exception& e) {
        std::cout << Color::RED << "Ошибка получения истории: " << e.what() << Color::RESET << "\n";
    }
}

// ============================================================================
// Вывод лимитов
// ============================================================================
void CLI::printLimits() {
    auto limits = m_validator->getLimits();
    
    std::cout << Color::BLUE << "\n🔒 Текущие лимиты безопасности:\n" << Color::RESET;
    std::cout << "  Максимальный ордер:       " << limits.max_order_usd << " USDT\n";
    std::cout << "  Дневной лимит убытка:     " << limits.daily_loss_limit << " USDT\n";
    std::cout << "  Макс. открытых позиций:   " << limits.max_open_positions << "\n";
    std::cout << "  Отклонение от рынка:      ±" << limits.max_market_deviation << "%\n";
    std::cout << "  Circuit Breaker:          " << limits.circuit_breaker_count << " ошибок → " 
              << limits.circuit_breaker_seconds << "с паузы\n";
}

// ============================================================================
// Обработка торговой команды через LLM
// ============================================================================
void CLI::processTradingCommand(const std::string& natural_language_input) {
    std::cout << Color::CYAN << "\n🤖 Анализ инструкции через DeepSeek...\n" << Color::RESET;
    
    try {
        // Шаг 1: Получаем контекст с биржи (баланс, цены)
        auto context = m_exchange->getMarketContext();
        
        // Шаг 2: Отправляем в LLM
        OrderCommand cmd = m_llm->parseCommand(natural_language_input, context);
        
        if (cmd.action.empty()) {
            std::cout << Color::RED << "❌ LLM не смог интерпретировать команду.\n" << Color::RESET;
            return;
        }
        
        std::cout << Color::GREEN << "✅ Команда распознана:\n" << Color::RESET;
        std::cout << "  Действие:   " << cmd.action << "\n";
        std::cout << "  Пара:       " << cmd.symbol << "\n";
        std::cout << "  Объём:      " << cmd.amount << "\n";
        std::cout << "  Тип цены:   " << cmd.price_type << "\n";
        if (cmd.price_type == "limit") {
            std::cout << "  Цена:       " << cmd.price << " USDT\n";
        }
        std::cout << "  Тейк-профит: " << cmd.take_profit << "%\n";
        std::cout << "  Стоп-лосс:  " << cmd.stop_loss << "%\n";
        
        // Шаг 3: Валидация
        std::cout << "\n🔍 Проверка рисков...\n";
        auto validation_result = m_validator->validate(cmd, context);
        
        if (!validation_result.is_valid) {
            std::cout << Color::RED << "❌ Ошибка валидации: " << validation_result.error_message << "\n" << Color::RESET;
            m_audit->logCommand(cmd, "REJECTED", validation_result.error_message);
            return;
        }
        
        std::cout << Color::GREEN << "✅ Валидация пройдена\n" << Color::RESET;
        
        // Шаг 4: Исполнение (в зависимости от режима)
        if (m_config->mode == TradingMode::DRY_RUN) {
            printOrderPreview(cmd);
            m_audit->logCommand(cmd, "DRY_RUN", "Симуляция");
        } else {
            // Запрашиваем подтверждение перед live-торговлей
            std::cout << Color::YELLOW << "\n⚠ Подтвердите ордер (yes/no): " << Color::RESET;
            std::string confirmation;
            std::getline(std::cin, confirmation);
            
            if (confirmation != "yes" && confirmation != "y") {
                std::cout << "Ордер отменён.\n";
                m_audit->logCommand(cmd, "CANCELLED_BY_USER", "Пользователь отменил");
                return;
            }
            
            OrderResult result = executeOrder(cmd);
            
            if (result.status == OrderStatus::FILLED) {
                std::cout << Color::GREEN << "✅ Ордер исполнен! ID: " << result.order_id << "\n" << Color::RESET;
            } else {
                std::cout << Color::RED << "❌ Ошибка: " << result.error_msg << "\n" << Color::RESET;
            }
        }
        
    } catch (const std::exception& e) {
        std::cout << Color::RED << "❌ Критическая ошибка: " << e.what() << "\n" << Color::RESET;
        m_audit->logError(natural_language_input, e.what());
    }
}

// ============================================================================
// Предпросмотр ордера (dry-run)
// ============================================================================
void CLI::printOrderPreview(const OrderCommand& cmd) {
    std::cout << Color::CYAN << "\n╔══════════════════════════════════════════════╗\n";
    std::cout << "║        ПРЕДПРОСМОТР ОРДЕРА (DRY-RUN)        ║\n";
    std::cout << "╚══════════════════════════════════════════════╝\n" << Color::RESET;
    
    std::cout << "  📋 Детали ордера:\n";
    std::cout << "     " << (cmd.action == "buy" ? "Покупка" : "Продажа") 
              << " " << cmd.amount << " " << cmd.symbol << "\n";
    std::cout << "     Тип: " << (cmd.price_type == "market" ? "Рыночный" : "Лимитный") << "\n";
    
    // Симулируем расчёт комиссии
    double fee_rate = 0.002;  // 0.2% комиссия CoinEx
    double estimated_fee = cmd.amount * fee_rate;
    
    std::cout << "\n  💰 Расчёт комиссии:\n";
    std::cout << "     Ставка: " << (fee_rate * 100) << "%\n";
    std::cout << "     Комиссия: ≈ " << std::fixed << std::setprecision(6) << estimated_fee << " " 
              << (cmd.symbol.find('/') != std::string::npos ? cmd.symbol.substr(0, cmd.symbol.find('/')) : cmd.symbol) << "\n";
    
    if (cmd.take_profit > 0 || cmd.stop_loss > 0) {
        std::cout << "\n  🎯 Уровни:\n";
        if (cmd.take_profit > 0) {
            std::cout << "     Тейк-профит: +" << cmd.take_profit << "%\n";
        }
        if (cmd.stop_loss > 0) {
            std::cout << "     Стоп-лимит:  -" << cmd.stop_loss << "%\n";
        }
    }
    
    std::cout << Color::GREEN << "\n  ✅ Ордер проверен. Реальных торгов не производилось.\n" << Color::RESET;
}

// ============================================================================
// Исполнение ордера (live)
// ============================================================================
OrderResult CLI::executeOrder(const OrderCommand& cmd) {
    std::cout << Color::YELLOW << "📤 Отправка ордера на CoinEx...\n" << Color::RESET;
    
    try {
        OrderResult result = m_exchange->placeOrder(cmd);
        m_audit->logCommand(cmd, result.status == OrderStatus::FILLED ? "FILLED" : "FAILED", 
                           result.error_msg);
        return result;
    } catch (const std::exception& e) {
        OrderResult error_result;
        error_result.status = OrderStatus::REJECTED;
        error_result.error_msg = e.what();
        m_audit->logCommand(cmd, "ERROR", e.what());
        return error_result;
    }
}
