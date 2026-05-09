/*
 * cli.cpp — Реализация CLI-оболочки
 * 
 * Три режима:
 *   MANUAL    — пользователь → DeepSeek-парсер → валидация → CoinEx
 *   SEMI_AUTO — цикл стратега: анализ → предложение → подтверждение → CoinEx
 *   FULL_AUTO — цикл стратега: анализ → автоисполнение (с лимитами)
 */

#include "cli.h"
#include "utils.h"

#include <iostream>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <ctime>
#include <chrono>
#include <cctype>

// ============================================================================
// Конструктор
// ============================================================================
CLI::CLI(std::shared_ptr<AppConfig> config,
         std::shared_ptr<LLMAgent> llm,
         std::shared_ptr<Validator> validator,
         std::shared_ptr<ExchangeClient> exchange,
         std::shared_ptr<AuditLogger> audit)
    : m_config(config), m_llm(llm), m_validator(validator),
      m_exchange(exchange), m_audit(audit)
{
    // Настройка стратегии по умолчанию
    m_strategy.name = "default";
    
    std::cout << Color::GREEN << "╔══════════════════════════════════════╗\n";
    std::cout << "║  CryptoTrader v2.0 — AI-Стратег      ║\n";
    std::cout << "║  DeepSeek + CoinEx API v2            ║\n";
    std::cout << "║  'помощь' — список команд            ║\n";
    std::cout << "╚══════════════════════════════════════╝\n" << Color::RESET;
}

CLI::~CLI() {
    shutdown();
    if (m_audit) m_audit->logSystemEvent("shutdown", "ok");
}

// ============================================================================
// Запуск
// ============================================================================
void CLI::run() {
    std::string input;
    
    // Если запущен в режиме стратега — стартуем цикл
    if (m_config->strat_mode == StrategistMode::SEMI_AUTO) {
        switchToSemiAuto();
    } else if (m_config->strat_mode == StrategistMode::FULL_AUTO) {
        switchToFullAuto();
    }
    
    while (m_running) {
        printPrompt();
        
        if (!std::getline(std::cin, input)) break;
        if (input.empty()) continue;
        
        if (m_history.empty() || m_history.back() != input) {
            m_history.push_back(input);
        }
        m_history_pos = m_history.size();
        
        processCommand(input);
    }
}

void CLI::shutdown() {
    m_running = false;
    stopStrategistLoop();
}

// ============================================================================
// Приглашение
// ============================================================================
void CLI::printPrompt() {
    std::string mode;
    if (m_config->mode == TradingMode::LIVE) {
        mode = Color::RED + "LIVE" + Color::RESET;
    } else {
        mode = Color::CYAN + "DRY" + Color::RESET;
    }
    
    std::string strat;
    switch (m_config->strat_mode) {
        case StrategistMode::MANUAL:   strat = ""; break;
        case StrategistMode::SEMI_AUTO: strat = Color::YELLOW + " [Стратег-Советник]" + Color::RESET; break;
        case StrategistMode::FULL_AUTO: strat = Color::RED + " [Стратег-Авто]" + Color::RESET; break;
    }
    
    std::cout << Color::GREEN << "crypto" << Color::RESET
              << "[" << mode << "]" << strat << "> ";
    std::cout.flush();
}

// ============================================================================
// Обработка команд
// ============================================================================
void CLI::processCommand(const std::string& input) {
    std::string lower = StrUtil::toLower(input);
    
    if (lower == "выход" || lower == "exit" || lower == "quit") {
        m_running = false;
    }
    else if (lower == "помощь" || lower == "help" || lower == "?") {
        printHelp();
    }
    else if (lower == "баланс" || lower == "balance") {
        printBalance();
    }
    else if (lower == "статус" || lower == "status") {
        printStatus();
    }
    else if (lower == "история" || lower == "history") {
        printHistory();
    }
    else if (lower == "лимиты" || lower == "limits") {
        printLimits();
    }
    else if (lower == "очистить" || lower == "clear") {
        std::cout << "\033[2J\033[1;1H";
    }
    else if (lower == "стратег" || lower == "strategist") {
        // Включаем полуавтомат
        if (m_config->strat_mode == StrategistMode::MANUAL) {
            switchToSemiAuto();
        } else {
            std::cout << Color::YELLOW << "Стратег уже активен. 'стоп' для остановки.\n" << Color::RESET;
        }
    }
    else if (lower == "авто" || lower == "auto") {
        switchToFullAuto();
    }
    else if (lower == "ручной" || lower == "manual") {
        switchToManual();
    }
    else if (lower == "стоп" || lower == "stop") {
        if (m_strategist_running) {
            stopStrategistLoop();
            std::cout << Color::GREEN << "Стратег остановлен. Ручной режим.\n" << Color::RESET;
        } else {
            std::cout << "Стратег не запущен.\n";
        }
    }
    else if (lower.find("change-balance") == 0 || lower.find("изменить-баланс") == 0) {
        processChangeBalance(input);
    }
    else if (StrUtil::containsAny(lower, {"купи","продай","buy","sell"})) {
        processTradingCommand(input);
    }
    else {
        // Всё остальное → чат с DeepSeek (работает в любом режиме, включая авто)
        processChat(input);
    }
}

// ============================================================================
// Справка
// ============================================================================
void CLI::printHelp() {
    std::cout << Color::CYAN << "\n╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║              🤖  CRYPTOTRADER v2.0  —  AI-СТРАТЕГ       ║\n";
    std::cout << "║              DeepSeek + CoinEx API v2                    ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n" << Color::RESET;
    
    std::cout << "\n" << Color::BOLD << Color::GREEN << "💬  ЧАТ С DeepSeek" << Color::RESET;
    std::cout << Color::DIM << "  (любое сообщение → AI отвечает)\n" << Color::RESET;
    std::cout << "  Просто пиши — AI поймёт. Примеры:\n";
    std::cout << "    " << Color::CYAN << "привет" << Color::RESET << "                                          — поздороваться\n";
    std::cout << "    " << Color::CYAN << "какой рынок сейчас?" << Color::RESET << "                            — спросить про рынок\n";
    std::cout << "    " << Color::CYAN << "что думаешь про BTC?" << Color::RESET << "                           — анализ монеты\n";
    std::cout << "    " << Color::CYAN << "как работает стоп-лосс?" << Color::RESET << "                       — обучение\n";
    std::cout << "    " << Color::CYAN << "купи 0.15 ETH по рынку, тейк 10%, стоп 3%" << Color::RESET << "    — торговля через чат\n\n";
    
    std::cout << Color::BOLD << Color::YELLOW << "📋  КОМАНДЫ" << Color::RESET << "\n\n";
    
    std::cout << "  " << Color::BOLD << "Торговля (быстрые команды):\n" << Color::RESET;
    std::cout << "    " << Color::GREEN << "купи" << Color::RESET << " / " << Color::GREEN << "buy" << Color::RESET
              << "        — покупка (напр: купи 0.15 ETH по рынку)\n";
    std::cout << "    " << Color::RED << "продай" << Color::RESET << " / " << Color::RED << "sell" << Color::RESET
              << "     — продажа (напр: продай 0.5 BTC лимит 65000)\n\n";
    
    std::cout << "  " << Color::BOLD << "Режимы стратега:\n" << Color::RESET;
    std::cout << "    " << Color::YELLOW << "стратег" << Color::RESET << " / strategist"
              << "  — авто-стратег: AI анализирует рынок и торгует сам\n";
    std::cout << "    " << Color::RED << "авто" << Color::RESET << " / auto"
              << "           — полный автомат: AI торгует сам. Можно давать инструкции в чате!\n";
    std::cout << "    " << Color::GREEN << "ручной" << Color::RESET << " / manual"
              << "       — вернуться в ручной режим\n";
    std::cout << "    " << Color::CYAN << "стоп" << Color::RESET << " / stop"
              << "           — остановить стратега\n\n";
    
    std::cout << "  " << Color::BOLD << "Информация:\n" << Color::RESET;
    std::cout << "    " << Color::BLUE << "баланс" << Color::RESET << " / balance"
              << "      — баланс и стоимость портфеля\n";
    std::cout << "    " << Color::MAGENTA << "статус" << Color::RESET << " / status"
              << "      — статус системы, режимы, статистика\n";
    std::cout << "    " << Color::YELLOW << "история" << Color::RESET << " / history"
              << "    — последние 15 ордеров\n";
    std::cout << "    " << Color::CYAN << "лимиты" << Color::RESET << " / limits"
              << "      — лимиты безопасности и параметры стратегии\n\n";
    
    std::cout << "  " << Color::BOLD << "Прочее:\n" << Color::RESET;
    std::cout << "    " << "change-balance 1.9eth 5btc" << "  — изменить баланс dry-run\n";
    std::cout << "    " << "помощь / help / ?" << "     — этот экран\n";
    std::cout << "    " << "очистить / clear" << "       — очистить терминал\n";
    std::cout << "    " << "выход / exit / quit" << "    — завершить программу\n";
    
    std::cout << "\n" << Color::DIM << "💡 Совет: " << Color::RESET
              << Color::DIM << "В ручном режиме любое сообщение, не похожее на команду, "
              << "отправляется DeepSeek как чат. AI понимает русский и английский.\n" << Color::RESET;
}

// ============================================================================
// Баланс
// ============================================================================
void CLI::printBalance() {
    std::cout << Color::BLUE << "\n📊 Баланс:\n" << Color::RESET;
    
    try {
        auto ctx = m_exchange->getMarketContext();
        auto& balances = ctx.balances;
        
        double total = 0.0;
        
        for (const auto& [ccy, bal] : balances) {
            if (bal.total() < 0.0001) continue;
            
            double usd_value = 0.0;
            if (ccy == "USDT" || ccy == "USDC") {
                usd_value = bal.total();
            } else {
                auto it = ctx.tickers.find(ccy + "USDT");
                if (it != ctx.tickers.end()) {
                    usd_value = bal.total() * it->second.last;
                }
            }
            total += usd_value;
            
            std::cout << "  " << std::left << std::setw(6) << ccy
                      << ": " << Format::crypto(bal.available);
            if (bal.frozen > 0) std::cout << " (+" << Format::crypto(bal.frozen) << " зам.)";
            if (usd_value > 0) std::cout << "  ~" << Format::usdt(usd_value);
            std::cout << "\n";
        }
        
        std::cout << "  ─────────────────────────────\n";
        std::cout << "  Портфель: ~" << Format::usdt(total) << "\n";
        
    } catch (const std::exception& e) {
        std::cout << Color::RED << "Ошибка: " << e.what() << Color::RESET << "\n";
    }
}

// ============================================================================
// Статус
// ============================================================================
void CLI::printStatus() {
    std::cout << Color::MAGENTA << "\nℹ️  Статус:\n" << Color::RESET;
    
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char tb[64]; std::strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    
    std::cout << "  Время:    " << tb << "\n";
    std::cout << "  Версия:   " << APP_VERSION << "\n";
    std::cout << "  Режим:    " << (m_config->mode == TradingMode::LIVE ? "LIVE 🔴" : "DRY-RUN ⚪") << "\n";
    
    std::string sm;
    switch (m_config->strat_mode) {
        case StrategistMode::MANUAL:   sm = "Ручной"; break;
        case StrategistMode::SEMI_AUTO: sm = "Авто-стратег"; break;
        case StrategistMode::FULL_AUTO: sm = "Автомат (стратег торгует)"; break;
    }
    std::cout << "  Стратег:  " << sm << "\n";
    std::cout << "  API:      " << (m_exchange->isConnected() ? "OK ✅" : "Ошибка ❌") << "\n";
    std::cout << "  DeepSeek: " << (m_llm->isAvailable() ? "OK ✅" : "Нет ключа ⚠") << "\n";
    
    if (m_config->mode == TradingMode::DRY_RUN) {
        auto stats = m_exchange->getDryRunStats();
        std::cout << "\n  Dry-Run: " << stats.total_trades << " сделок, "
                  << "комиссий: $" << Format::price(stats.total_fees) << "\n";
    }
    
    std::cout << "  Дневной P&L: $" << m_validator->getDailyPnL() << "\n";
    std::cout << "  Сделок сегодня: " << m_validator->getDailyTrades() << "\n";
}

// ============================================================================
// История
// ============================================================================
void CLI::printHistory() {
    std::cout << Color::YELLOW << "\n📜 История:\n" << Color::RESET;
    
    auto orders = m_audit->getRecentOrders(15);
    if (orders.empty()) {
        std::cout << "  Пусто.\n";
        return;
    }
    
    for (const auto& o : orders) {
        std::string color = (o.status_str == "FILLED") ? Color::GREEN : Color::YELLOW;
        std::cout << "  " << o.time.substr(11, 8) << " | "
                  << std::setw(4) << o.side << " | "
                  << std::setw(10) << o.symbol << " | "
                  << Format::crypto(o.amount) << " | "
                  << color << o.status_str << Color::RESET;
        if (!o.reason.empty()) {
            std::cout << " | " << o.reason.substr(0, 60);
        }
        std::cout << "\n";
    }
}

// ============================================================================
// Лимиты
// ============================================================================
void CLI::printLimits() {
    std::cout << Color::BLUE << "\n🔒 Ограничения:\n" << Color::RESET;
    std::cout << "  Все лимиты сняты — торговля без ограничений.\n";
    std::cout << "  Единственное ограничение: доступный баланс.\n";
    
    std::cout << "\n📋 Стратегия \"" << m_strategy.name << "\":\n";
    std::cout << "  Пары:                ";
    for (size_t i = 0; i < m_strategy.symbols.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << m_strategy.symbols[i];
    }
    std::cout << "\n";
    std::cout << "  Интервал анализа:    " << m_strategy.analysis_interval_sec << "с\n";
    std::cout << "  Тейк/стоп:           +" << m_strategy.take_profit_default
              << "% / -" << m_strategy.stop_loss_default << "%\n";
    std::cout << "  Макс. сделок/день:   " << m_strategy.max_daily_trades << "\n";
}

// ============================================================================
// Чат с DeepSeek (свободное общение)
// ============================================================================
void CLI::processChat(const std::string& input) {
    if (!m_llm->isAvailable()) {
        std::cout << Color::YELLOW << "Нужен ключ DeepSeek для чата. "
                  << "Установите DEEPSEEK_API_KEY.\n" << Color::RESET;
        std::cout << Color::DIM << "Команды без AI: баланс, статус, история, лимиты, помощь, выход\n"
                  << Color::RESET;
        return;
    }
    
    std::cout << Color::CYAN << "\n💬 " << Color::RESET;
    
    try {
        auto context = m_exchange->getMarketContext();
        auto response = m_llm->chat(input, context);
        
        // Вывод ответа
        std::cout << Color::GREEN << "AI: " << Color::RESET << response.message << "\n";
        
        // Если есть торговая команда — исполняем
        if (response.wants_to_trade) {
            auto& cmd = response.trade;
            
            std::cout << "\n" << Color::CYAN << "⚡ Обнаружена торговая команда: "
                      << cmd.action << " " << cmd.amount << " " << cmd.symbol << "\n" << Color::RESET;
            
            auto validation = m_validator->validateOrder(cmd, context);
            if (!validation.is_valid) {
                std::cout << Color::RED << "❌ " << validation.error_message << "\n" << Color::RESET;
                m_audit->logOrder(cmd, "REJECTED", validation.error_message);
                return;
            }
            
            for (auto& w : validation.warnings) {
                std::cout << Color::YELLOW << "⚠ " << w << "\n" << Color::RESET;
            }
            
            if (m_config->mode == TradingMode::DRY_RUN) {
                auto result = m_exchange->simulateOrder(cmd);
                printOrderPreview(cmd);
                std::cout << Color::GREEN << "  [DRY] ID: " << result.order_id << Color::RESET << "\n";
                m_audit->logOrder(cmd, "DRY_RUN", "чат-команда", result.order_id);
                return;
            }
            
            // LIVE: автоисполнение
            auto result = executeOrder(cmd);
            if (result.status == OrderStatus::FILLED) {
                std::cout << Color::GREEN << "✅ Исполнено! ID: " << result.order_id << "\n" << Color::RESET;
                m_validator->registerTrade(result.filled_value * 0.001);
            } else {
                std::cout << Color::RED << "❌ " << result.error_msg << "\n" << Color::RESET;
            }
        }
        
    } catch (const std::exception& e) {
        std::cout << Color::RED << "Ошибка чата: " << e.what() << "\n" << Color::RESET;
        m_audit->logError("chat:" + input, e.what());
    }
}

// ============================================================================
// Изменение симулированного баланса (dry-run)
// ============================================================================
void CLI::processChangeBalance(const std::string& input) {
    if (m_config->mode != TradingMode::DRY_RUN) {
        std::cout << Color::YELLOW << "Команда только для DRY-RUN режима.\n" << Color::RESET;
        return;
    }
    
    // Парсим токены вида "1.9eth", "5btc", "3sol", "10000usdt"
    auto parts = StrUtil::split(input, ' ');
    int changed = 0;
    
    for (size_t i = 1; i < parts.size(); ++i) {
        std::string token = StrUtil::toLower(StrUtil::trim(parts[i]));
        if (token.empty()) continue;
        
        // Извлекаем число и валюту
        double amount = 0.0;
        std::string ccy;
        
        // Ищем, где заканчивается число
        size_t num_end = 0;
        bool has_dot = false;
        for (size_t j = 0; j < token.size(); ++j) {
            char c = token[j];
            if (isdigit(c) || (c == '.' && !has_dot)) {
                if (c == '.') has_dot = true;
                num_end = j + 1;
            } else {
                break;
            }
        }
        
        if (num_end == 0) continue;
        
        amount = std::stod(token.substr(0, num_end));
        ccy = token.substr(num_end);
        
        // Нормализуем название валюты
        if (ccy == "btc")       ccy = "BTC";
        else if (ccy == "eth")  ccy = "ETH";
        else if (ccy == "sol")  ccy = "SOL";
        else if (ccy == "usdt") ccy = "USDT";
        // прочие валюты — как есть (уже lowercase)
        
        // Устанавливаем
        m_exchange->setSimBalance(ccy, amount);
        std::cout << Color::GREEN << "  " << ccy << " → " << Format::crypto(amount) 
                  << Color::RESET << "\n";
        changed++;
    }
    
    if (changed == 0) {
        std::cout << Color::YELLOW << "Использование: change-balance <сумма><валюта> ...\n";
        std::cout << "Пример: change-balance 1.9eth 5btc 3sol 10000usdt\n" << Color::RESET;
    } else {
        std::cout << Color::GREEN << "Баланс обновлён (" << changed << " валют).\n" << Color::RESET;
    }
}

// ============================================================================
// Торговая команда (MANUAL)
// ============================================================================
void CLI::processTradingCommand(const std::string& input) {
    std::cout << Color::CYAN << "\n🤖 Анализ через DeepSeek...\n" << Color::RESET;
    
    try {
        auto context = m_exchange->getMarketContext();
        OrderCommand cmd = m_llm->parseCommand(input, context);
        
        if (cmd.action.empty()) {
            std::cout << Color::RED << "❌ Не удалось распознать команду.\n" << Color::RESET;
            return;
        }
        
        std::cout << Color::GREEN << "✅ Распознано: " << cmd.action << " "
                  << cmd.amount << " " << cmd.symbol;
        if (cmd.price_type == "limit") std::cout << " по $" << cmd.price;
        std::cout << "\n" << Color::RESET;
        
        auto validation = m_validator->validateOrder(cmd, context);
        if (!validation.is_valid) {
            std::cout << Color::RED << "❌ Валидация: " << validation.error_message << "\n" << Color::RESET;
            m_audit->logOrder(cmd, "REJECTED", validation.error_message);
            return;
        }
        
        for (auto& w : validation.warnings) {
            std::cout << Color::YELLOW << "⚠ " << w << "\n" << Color::RESET;
        }
        
        // Dry-run: симуляция с обновлением баланса
        if (m_config->mode == TradingMode::DRY_RUN) {
            auto result = m_exchange->simulateOrder(cmd);
            printOrderPreview(cmd);
            std::cout << Color::GREEN << "  [DRY] ID: " << result.order_id << Color::RESET << "\n";
            m_audit->logOrder(cmd, "DRY_RUN", "симуляция", result.order_id);
            return;
        }
        
        // Live: автоисполнение
        auto result = executeOrder(cmd);
        if (result.status == OrderStatus::FILLED) {
            std::cout << Color::GREEN << "✅ Исполнено! ID: " << result.order_id << "\n" << Color::RESET;
            m_validator->registerTrade(result.filled_value * 0.001);  // ~P&L оценка
        } else {
            std::cout << Color::RED << "❌ " << result.error_msg << "\n" << Color::RESET;
        }
        
    } catch (const std::exception& e) {
        std::cout << Color::RED << "❌ Ошибка: " << e.what() << "\n" << Color::RESET;
        m_audit->logError(input, e.what());
    }
}

// ============================================================================
// Предпросмотр ордера
// ============================================================================
void CLI::printOrderPreview(const OrderCommand& cmd) {
    std::cout << Color::CYAN << "\n╔══════════════════════════════════════════════╗\n";
    std::cout << "║        ПРЕДПРОСМОТР (DRY-RUN)                ║\n";
    std::cout << "╚══════════════════════════════════════════════╝\n" << Color::RESET;
    
    std::cout << "  " << (cmd.action == "buy" ? "Покупка" : "Продажа")
              << " " << cmd.amount << " " << cmd.symbol << "\n";
    std::cout << "  Тип: " << (cmd.price_type == "market" ? "Рыночный" : "Лимитный") << "\n";
    
    double fee = cmd.amount * CoinExConfig::DEFAULT_FEE_RATE;
    std::cout << "  Комиссия: ~" << fee << "\n";
    
    if (cmd.take_profit > 0) std::cout << "  Тейк: +" << cmd.take_profit << "%\n";
    if (cmd.stop_loss > 0)   std::cout << "  Стоп: -" << cmd.stop_loss << "%\n";
    
    std::cout << Color::GREEN << "\n  ✅ Проверено. Торгов не было.\n" << Color::RESET;
}

// ============================================================================
// Исполнение ордера
// ============================================================================
OrderResult CLI::executeOrder(const OrderCommand& cmd) {
    std::cout << Color::YELLOW << "📤 Отправка на CoinEx...\n" << Color::RESET;
    
    try {
        auto result = m_exchange->placeOrder(cmd);
        m_audit->logOrder(cmd,
            result.status == OrderStatus::FILLED ? "FILLED" : "FAILED",
            result.error_msg, result.order_id);
        return result;
    } catch (const std::exception& e) {
        OrderResult err;
        err.status = OrderStatus::REJECTED;
        err.error_msg = e.what();
        m_audit->logOrder(cmd, "ERROR", e.what());
        return err;
    }
}

// ============================================================================
// СТРАТЕГ: переключение режимов
// ============================================================================
void CLI::switchToSemiAuto() {
    if (!m_llm->isAvailable()) {
        std::cout << Color::RED << "Нужен ключ DeepSeek. Установите DEEPSEEK_API_KEY.\n" << Color::RESET;
        return;
    }
    
    m_config->strat_mode = StrategistMode::SEMI_AUTO;
    std::cout << Color::YELLOW << "\n🔄 Авто-стратег запущен.\n";
    std::cout << "DeepSeek анализирует рынок и торгует самостоятельно.\n";
    std::cout << "'стоп' — остановить, 'статус' — сводка, любой текст — чат с AI.\n" << Color::RESET;
    
    startStrategistLoop();
}

void CLI::switchToFullAuto() {
    if (!m_llm->isAvailable()) {
        std::cout << Color::RED << "Нужен ключ DeepSeek.\n" << Color::RESET;
        return;
    }
    
    m_config->strat_mode = StrategistMode::FULL_AUTO;
    std::cout << Color::RED << "\n🤖 АВТО-СТРАТЕГ АКТИВЕН!\n";
    std::cout << "DeepSeek торгует самостоятельно. Вы можете давать инструкции в чате.\n";
    std::cout << "'стоп' — остановить, 'статус' — сводка, любой текст — чат с AI.\n" << Color::RESET;
    
    startStrategistLoop();
}

void CLI::switchToManual() {
    stopStrategistLoop();
    m_config->strat_mode = StrategistMode::MANUAL;
    std::cout << Color::GREEN << "Ручной режим.\n" << Color::RESET;
}

// ============================================================================
// Цикл стратега
// ============================================================================
void CLI::startStrategistLoop() {
    if (m_strategist_running) return;
    
    m_strategist_running = true;
    m_strategist_thread = std::make_unique<std::thread>([this]() {
        while (m_strategist_running && m_running) {
            strategistIteration();
            
            // Ждём интервал
            for (int i = 0; i < m_strategy.analysis_interval_sec && m_strategist_running; ++i) {
                SysUtil::sleepMs(1000);
            }
        }
    });
    
    m_audit->logSystemEvent("strategist_start",
        m_config->strat_mode == StrategistMode::SEMI_AUTO ? "semi_auto" : "full_auto");
}

void CLI::stopStrategistLoop() {
    m_strategist_running = false;
    if (m_strategist_thread && m_strategist_thread->joinable()) {
        m_strategist_thread->join();
        m_strategist_thread.reset();
    }
    m_audit->logSystemEvent("strategist_stop", "ok");
}

// ============================================================================
// Одна итерация стратега
// ============================================================================
void CLI::strategistIteration() {
    try {
        // Шаг 1: Получаем контекст рынка
        auto context = m_exchange->getMarketContext();
        context.daily_pnl = m_validator->getDailyPnL();
        context.daily_trades = m_validator->getDailyTrades();
        
        // Шаг 2: Запрашиваем решение у DeepSeek
        auto limits = m_validator->getLimits();
        auto decision = m_llm->analyzeMarket(context, m_strategy, limits);
        
        // Шаг 3: Логируем
        m_audit->logStrategistDecision(decision, "analyzed");
        
        // Шаг 4: Если hold — пропускаем
        if (!decision.should_act) {
            if (!decision.reasoning.empty()) {
                std::cout << Color::DIM << "  [Стратег] " << decision.reasoning 
                          << Color::RESET << "\n";
            }
            return;
        }
        
        // Шаг 5: Показываем решение
        printStrategistDecision(decision);
        
        // Шаг 6: Валидация
        auto validation = m_validator->validateStrategistDecision(
            decision, context, m_strategy);
        
        if (!validation.is_valid) {
            std::cout << Color::RED << "  ❌ Валидация: " << validation.error_message 
                      << Color::RESET << "\n";
            m_audit->logStrategistDecision(decision, "rejected");
            return;
        }
        
        for (auto& w : validation.warnings) {
            std::cout << Color::YELLOW << "  ⚠ " << w << Color::RESET << "\n";
        }
        
        // Шаг 7: Автоисполнение (без подтверждений)
        bool ok = executeStrategistDecision(decision);
        if (ok) {
            m_audit->logStrategistDecision(decision, "executed");
        }
        
    } catch (const std::exception& e) {
        std::cerr << Color::RED << "  [Стратег] Ошибка: " << e.what() << Color::RESET << "\n";
        m_audit->logError("strategist_iteration", e.what());
        m_validator->registerError();
    }
}

// ============================================================================
// Вывод решения стратега
// ============================================================================
void CLI::printStrategistDecision(const StrategistDecision& d) {
    std::cout << "\n" << Color::CYAN << "╔══════════════════════════════════════════╗\n";
    std::cout << "║        РЕШЕНИЕ СТРАТЕГА (DeepSeek)       ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n" << Color::RESET;
    
    std::string action_str;
    if (d.action == "buy")  action_str = Color::GREEN + "ПОКУПКА" + Color::RESET;
    else if (d.action == "sell") action_str = Color::RED + "ПРОДАЖА" + Color::RESET;
    else if (d.action == "cancel") action_str = Color::YELLOW + "ОТМЕНА" + Color::RESET;
    else action_str = "HOLD";
    
    std::cout << "  Действие:    " << action_str << "\n";
    
    if (d.action == "buy" || d.action == "sell") {
        std::cout << "  Пара:        " << d.symbol << "\n";
        std::cout << "  Объём:       " << d.amount << "\n";
        std::cout << "  Тип:         " << d.price_type << "\n";
        if (d.price > 0) std::cout << "  Цена:        $" << d.price << "\n";
        if (d.take_profit > 0) std::cout << "  Тейк:        +" << d.take_profit << "%\n";
        if (d.stop_loss > 0)   std::cout << "  Стоп:        -" << d.stop_loss << "%\n";
    }
    
    if (d.action == "cancel") {
        std::cout << "  Отмена:      " << d.cancel_order_ids.size() << " ордеров\n";
    }
    
    std::cout << "  Уверенность: " << (d.confidence * 100) << "%\n";
    std::cout << "  Стратегия:   " << d.strategy_name << "\n";
    std::cout << "  Причина:     " << d.reasoning << "\n";
}

// ============================================================================
// Исполнение решения стратега
// ============================================================================
bool CLI::executeStrategistDecision(const StrategistDecision& d) {
    try {
        if (d.action == "cancel") {
            // Отмена ордеров
            m_exchange->cancelAllOrders();
            std::cout << Color::GREEN << "  ✅ Ордера отменены.\n" << Color::RESET;
            return true;
        }
        
        if (d.action == "buy" || d.action == "sell") {
            OrderCommand cmd;
            cmd.action      = d.action;
            cmd.symbol      = d.symbol;
            cmd.market_type = CoinExConfig::MARKET_TYPE_SPOT;
            cmd.amount      = d.amount;
            cmd.price_type  = d.price_type;
            cmd.price       = d.price;
            cmd.take_profit = d.take_profit;
            cmd.stop_loss   = d.stop_loss;
            cmd.reason      = d.reasoning;
            
            if (m_config->mode == TradingMode::DRY_RUN) {
                auto result = m_exchange->simulateOrder(cmd);
                std::cout << Color::GREEN << "  ✅ [DRY] " << d.action << " " 
                          << d.amount << " " << d.symbol 
                          << " | ID: " << result.order_id << Color::RESET << "\n";
                m_audit->logOrder(cmd, "DRY_RUN", "стратег", result.order_id);
                return true;
            }
            
            auto result = m_exchange->placeOrder(cmd);
            if (result.status == OrderStatus::FILLED) {
                std::cout << Color::GREEN << "  ✅ Исполнено! ID: " << result.order_id 
                          << Color::RESET << "\n";
                m_audit->logOrder(cmd, "FILLED", "стратег", result.order_id);
                return true;
            } else {
                std::cout << Color::RED << "  ❌ " << result.error_msg << Color::RESET << "\n";
                m_audit->logOrder(cmd, "FAILED", result.error_msg);
                return false;
            }
        }
        
        return false;
        
    } catch (const std::exception& e) {
        std::cerr << Color::RED << "  ❌ Ошибка: " << e.what() << Color::RESET << "\n";
        m_audit->logError("strategist_execute", e.what());
        return false;
    }
}
