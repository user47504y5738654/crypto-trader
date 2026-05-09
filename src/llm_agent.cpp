/*
 * llm_agent.cpp — Реализация DeepSeek LLM агента
 * 
 * Два режима:
 *   1. PARSER — преобразование команд пользователя в OrderCommand
 *      (используется в MANUAL режиме)
 *   2. STRATEGIST — автономный анализ рынка и принятие решений
 *      (используется в SEMI_AUTO и FULL_AUTO режимах)
 * 
 * DeepSeek API: https://platform.deepseek.com/api-docs
 * (OpenAI-совместимый: POST /v1/chat/completions)
 */

#include "llm_agent.h"

#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>

#include <curl/curl.h>

// ============================================================================
// CURL callback
// ============================================================================
static size_t llmWriteCb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), total);
    return total;
}

// ============================================================================
// Конструктор
// ============================================================================
LLMAgent::LLMAgent(const std::string& api_key) : m_api_key(api_key) {
    curl_global_init(CURL_GLOBAL_ALL);
    
    if (m_api_key.empty()) {
        const char* env_key = std::getenv("DEEPSEEK_API_KEY");
        if (env_key) {
            m_api_key = env_key;
            std::cout << "  [LLM] Ключ DeepSeek загружен из DEEPSEEK_API_KEY\n";
        } else {
            std::cout << "  [LLM] ВНИМАНИЕ: API ключ DeepSeek не установлен.\n";
            std::cout << "    Используйте --deepseek-key <ключ> или DEEPSEEK_API_KEY\n";
        }
    }
}

// ============================================================================
// Деструктор
// ============================================================================
LLMAgent::~LLMAgent() {
    curl_global_cleanup();
}

// ============================================================================
// Установка API ключа
// ============================================================================
void LLMAgent::setApiKey(const std::string& api_key) {
    m_api_key = api_key;
}

// ============================================================================
// Проверка доступности
// ============================================================================
bool LLMAgent::isAvailable() const {
    return !m_api_key.empty();
}

// ============================================================================
// Формирование системного промпта для ПАРСЕРА
// 
// Задача: преобразовать команду пользователя в строгий JSON.
// Без стратегических решений — только структурирование.
// ============================================================================
std::string LLMAgent::buildParserSystemPrompt() {
    return R"(Ты — торговый парсер для криптовалютной биржи CoinEx.
Твоя задача — преобразовывать команды пользователя в JSON для исполнения.

ПРАВИЛА:
1. Отвечай ТОЛЬКО JSON-объектом, без пояснений и markdown.
2. Все числа — как числа, не строки.
3. symbol — в формате CoinEx: "BTCUSDT", "ETHUSDT" (без "/").
4. market_type всегда "SPOT".
5. Если цена не указана — price_type = "market".
6. Если тейк/стоп не указаны — ставь 0.
7. Если команда непонятна — верни {"action": "unknown", "error": "..."}.

ФОРМАТ ОТВЕТА:
{
    "action": "buy" | "sell",
    "symbol": "ETHUSDT",
    "amount": 0.15,
    "price_type": "market" | "limit",
    "price": 3500.0,
    "take_profit": 10.0,
    "stop_loss": 3.0
}

ПРИМЕРЫ:
Пользователь: "купи 0.15 ETH по рынку, тейк 10%, стоп 3%"
Ответ: {"action":"buy","symbol":"ETHUSDT","amount":0.15,"price_type":"market","price":0,"take_profit":10,"stop_loss":3}

Пользователь: "продай 0.5 BTC по лимиту 65000"
Ответ: {"action":"sell","symbol":"BTCUSDT","amount":0.5,"price_type":"limit","price":65000,"take_profit":0,"stop_loss":0}

Пользователь: "купи SOL на 100 USDT"
Ответ: {"action":"buy","symbol":"SOLUSDT","amount":0.67,"price_type":"market","price":0,"take_profit":0,"stop_loss":0})";
}

// ============================================================================
// Формирование системного промпта для СТРАТЕГА
// 
// Задача: проанализировать рынок и принять торговое решение.
// DeepSeek выступает как автономный трейдер-стратег.
// ============================================================================
std::string LLMAgent::buildStrategistSystemPrompt(
    const StrategyConfig& strategy,
    const RiskLimits& limits) {
    
    std::ostringstream prompt;
    
    prompt << R"(Ты — автономный криптовалютный стратег-трейдер на бирже CoinEx.

ТВОЯ ЗАДАЧА:
Проанализируй текущую рыночную ситуацию и прими взвешенное торговое решение.
Ты можешь: покупать (buy), продавать (sell), держать позицию (hold)
или отменять существующие ордера (cancel).

СТРАТЕГИЯ:
- Название: )" << strategy.name << R"(
- Торгуемые пары: )";
    
    for (size_t i = 0; i < strategy.symbols.size(); ++i) {
        if (i > 0) prompt << ", ";
        prompt << strategy.symbols[i];
    }
    
    prompt << R"(
- Максимальный ордер: $)" << strategy.max_order_usd << R"(
- Дневной лимит убытков: $)" << strategy.daily_loss_limit << R"(
- Максимальная позиция: )" << strategy.max_position_percent << R"(% от портфеля)
- Мин. уверенность для сделки: )" << strategy.min_confidence << R"(
- Тейк-профит по умолчанию: )" << strategy.take_profit_default << R"(%
- Стоп-лосс по умолчанию: )" << strategy.stop_loss_default << R"(%

ГЛОБАЛЬНЫЕ ЛИМИТЫ:
- Макс. ордер: $)" << limits.max_order_usd << R"(
- Дневной лимит убытков: $)" << limits.daily_loss_limit << R"(
- Макс. общая экспозиция: )" << limits.max_total_exposure_pct << R"(%
- Макс. открытых позиций: )" << limits.max_open_positions << R"(

ПРИНЦИПЫ ТОРГОВЛИ:
1. ТРЕНД: покупай на восходящем тренде, продавай на нисходящем.
2. ОБЪЁМЫ: высокий объём подтверждает тренд.
3. ДИВЕРСИФИКАЦИЯ: не ставь всё на одну монету.
4. УПРАВЛЕНИЕ РИСКАМИ: всегда ставь стоп-лосс.
5. НЕ ЖАДНИЧАЙ: фиксируй прибыль частями.
6. ТЕРПЕНИЕ: если нет хорошего сигнала — держи (hold).

ФОРМАТ ОТВЕТА (строгий JSON, без markdown):
{
    "action": "buy" | "sell" | "hold" | "cancel",
    "symbol": "ETHUSDT",
    "amount": 0.15,
    "price_type": "market" | "limit",
    "price": 3500.0,
    "take_profit": 5.0,
    "stop_loss": 3.0,
    "confidence": 0.85,
    "reasoning": "ETH пробил уровень сопротивления на растущих объёмах...",
    "strategy_name": "трендовая",
    "cancel_order_ids": []
}

ПОЛЯ:
- action: "buy" (купить), "sell" (продать), "hold" (ничего не делать), "cancel" (отменить ордера)
- symbol: пара в формате CoinEx ("BTCUSDT", "ETHUSDT")
- amount: количество базовой валюты (число, не строка)
- price_type: "market" (рыночный) или "limit" (лимитный)
- price: цена для лимитного ордера (0 для market)
- take_profit: тейк-профит в процентах (0 если не используется)
- stop_loss: стоп-лосс в процентах
- confidence: твоя уверенность в сделке от 0.0 до 1.0
- reasoning: краткое объяснение решения (1-3 предложения)
- strategy_name: название используемой стратегии
- cancel_order_ids: список ID ордеров для отмены (только если action="cancel")

ВАЖНО:
- Не открывай позицию больше разрешённого лимита.
- Учитывай текущий баланс — не пытайся купить больше, чем доступно USDT.
- Если рынок неопределённый — лучше hold, чем случайная сделка.
- confidence < 0.5 означает слабый сигнал — лучше hold.
- При action="hold" все остальные поля кроме action и reasoning могут быть пустыми.
)";
    
    return prompt.str();
}

// ============================================================================
// Формирование промпта с контекстом рынка
// ============================================================================
std::string LLMAgent::buildMarketContextPrompt(const MarketContext& context) {
    std::ostringstream prompt;
    
    prompt << "=== ТЕКУЩАЯ СИТУАЦИЯ НА РЫНКЕ ===\n\n";
    
    // Цены
    prompt << "ЦЕНЫ:\n";
    for (const auto& [symbol, ticker] : context.tickers) {
        prompt << "  " << symbol << ": $" << ticker.last
               << " (24ч: " << (ticker.change_pct >= 0 ? "+" : "")
               << ticker.change_pct << "%, объём: $" << ticker.value
               << ", макс: $" << ticker.high << ", мин: $" << ticker.low << ")\n";
    }
    prompt << "\n";
    
    // Баланс
    prompt << "БАЛАНС:\n";
    double total_usd = 0.0;
    for (const auto& [ccy, bal] : context.balances) {
        prompt << "  " << ccy << ": " << bal.available;
        if (bal.frozen > 0) prompt << " (+" << bal.frozen << " заморожено)";
        prompt << "\n";
        
        // Оцениваем в USD
        if (ccy == "USDT" || ccy == "USDC") {
            total_usd += bal.total();
        } else {
            std::string symbol = ccy + "USDT";
            if (context.tickers.count(symbol)) {
                total_usd += bal.total() * context.tickers.at(symbol).last;
            }
        }
    }
    prompt << "  Оценочная стоимость портфеля: ~$" << total_usd << "\n\n";
    
    // Позиции
    if (!context.positions.empty()) {
        prompt << "ОТКРЫТЫЕ ПОЗИЦИИ:\n";
        for (const auto& [symbol, amount] : context.positions) {
            prompt << "  " << symbol << ": " << amount;
            if (context.tickers.count(symbol)) {
                double value = amount * context.tickers.at(symbol).last;
                double pct = total_usd > 0 ? (value / total_usd * 100.0) : 0.0;
                prompt << " (~$" << value << ", " << pct << "% портфеля)";
            }
            prompt << "\n";
        }
        prompt << "\n";
    }
    
    // Открытые ордера
    if (!context.open_orders.empty()) {
        prompt << "ОТКРЫТЫЕ ОРДЕРА:\n";
        for (const auto& order : context.open_orders) {
            prompt << "  ID: " << order.order_id
                   << ", заполнено: " << order.filled_amount
                   << " по $" << order.filled_price << "\n";
        }
        prompt << "\n";
    }
    
    // Дневная статистика
    prompt << "ДНЕВНАЯ СТАТИСТИКА:\n";
    prompt << "  Сделок сегодня: " << context.daily_trades << "\n";
    prompt << "  Дневной P&L: $" << context.daily_pnl << "\n";
    
    prompt << "\nТвоё решение:";
    
    return prompt.str();
}

// ============================================================================
// Отправка запроса к DeepSeek API
// ============================================================================
json LLMAgent::sendDeepSeekRequest(const std::string& system_prompt,
                                     const std::string& user_prompt,
                                     int max_tokens,
                                     double temperature) {
    
    if (m_api_key.empty()) {
        throw std::runtime_error("API ключ DeepSeek не установлен");
    }
    
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("CURL: ошибка инициализации");
    }
    
    // Тело запроса
    json request_body = {
        {"model", DeepSeekConfig::MODEL},
        {"messages", json::array({
            {{"role", "system"}, {"content", system_prompt}},
            {{"role", "user"},   {"content", user_prompt}}
        })},
        {"max_tokens", max_tokens},
        {"temperature", temperature},
        {"stream", false}
    };
    
    std::string body_str = request_body.dump();
    std::string response_str;
    
    // Заголовки
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string auth = "Authorization: Bearer " + m_api_key;
    headers = curl_slist_append(headers, auth.c_str());
    
    // Настройка запроса
    curl_easy_setopt(curl, CURLOPT_URL, DeepSeekConfig::API_URL.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_str.length()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, llmWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_str);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    
    CURLcode res = curl_easy_perform(curl);
    
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        throw std::runtime_error("DeepSeek HTTP: " + std::string(curl_easy_strerror(res)));
    }
    
    if (http_code != 200) {
        throw std::runtime_error("DeepSeek API HTTP " + std::to_string(http_code) +
                                 ": " + response_str);
    }
    
    // Парсим ответ
    json resp_json = json::parse(response_str);
    
    std::string content = resp_json["choices"][0]["message"]["content"];
    
    // Удаляем markdown-обёртку ```json ... ``` если есть
    if (content.find("```json") != std::string::npos) {
        size_t start = content.find("{");
        size_t end = content.rfind("}");
        if (start != std::string::npos && end != std::string::npos && end > start) {
            content = content.substr(start, end - start + 1);
        }
    }
    
    // Парсим как JSON
    try {
        return json::parse(content);
    } catch (const json::parse_error& e) {
        throw std::runtime_error("LLM вернул невалидный JSON: " + content);
    }
}

// ============================================================================
// Валидация JSON-схемы ответа парсера
// ============================================================================
bool LLMAgent::validateParserSchema(const json& data) {
    if (!data.contains("action") || !data["action"].is_string()) return false;
    
    std::string action = data["action"];
    if (action != "buy" && action != "sell" && action != "unknown") return false;
    
    if (action == "unknown") return true;  // ok, обработаем отдельно
    
    if (!data.contains("symbol") || !data["symbol"].is_string()) return false;
    if (!data.contains("amount") || !data["amount"].is_number()) return false;
    if (!data.contains("price_type") || !data["price_type"].is_string()) return false;
    
    std::string pt = data["price_type"];
    if (pt != "market" && pt != "limit") return false;
    
    return true;
}

// ============================================================================
// Валидация JSON-схемы ответа стратега
// ============================================================================
bool LLMAgent::validateStrategistSchema(const json& data) {
    if (!data.contains("action") || !data["action"].is_string()) return false;
    
    std::string action = data["action"];
    if (action != "buy" && action != "sell" && action != "hold" && action != "cancel") {
        return false;
    }
    
    // Для hold достаточно action
    if (action == "hold") return true;
    
    // Для cancel нужен cancel_order_ids
    if (action == "cancel") {
        return data.contains("cancel_order_ids") && data["cancel_order_ids"].is_array();
    }
    
    // Для buy/sell нужны все поля
    if (!data.contains("symbol")) return false;
    if (!data.contains("amount")) return false;
    if (!data.contains("price_type")) return false;
    if (!data.contains("confidence")) return false;
    
    return true;
}

// ============================================================================
// Парсинг ответа в OrderCommand
// ============================================================================
OrderCommand LLMAgent::parseOrderCommand(const json& response) {
    OrderCommand cmd;
    
    cmd.action      = response.value("action", "");
    cmd.symbol      = response.value("symbol", "");
    cmd.market_type = CoinExConfig::MARKET_TYPE_SPOT;
    cmd.amount      = response.value("amount", 0.0);
    cmd.price_type  = response.value("price_type", "market");
    cmd.price       = response.value("price", 0.0);
    cmd.take_profit = response.value("take_profit", 0.0);
    cmd.stop_loss   = response.value("stop_loss", 0.0);
    cmd.client_id   = "ct_parser_";
    
    return cmd;
}

// ============================================================================
// Парсинг ответа в StrategistDecision
// ============================================================================
StrategistDecision LLMAgent::parseStrategistDecision(const json& response) {
    StrategistDecision decision;
    
    decision.action        = response.value("action", "hold");
    decision.symbol        = response.value("symbol", "");
    decision.amount        = response.value("amount", 0.0);
    decision.price_type    = response.value("price_type", "market");
    decision.price         = response.value("price", 0.0);
    decision.take_profit   = response.value("take_profit", 0.0);
    decision.stop_loss     = response.value("stop_loss", 0.0);
    decision.confidence    = response.value("confidence", 0.0);
    decision.reasoning     = response.value("reasoning", "");
    decision.strategy_name = response.value("strategy_name", "");
    
    // Ордера на отмену
    if (response.contains("cancel_order_ids") && response["cancel_order_ids"].is_array()) {
        for (const auto& id : response["cancel_order_ids"]) {
            decision.cancel_order_ids.push_back(id.get<std::string>());
        }
    }
    
    // Определяем, нужно ли действовать
    if (decision.action == "buy" || decision.action == "sell" || decision.action == "cancel") {
        decision.should_act = true;
    }
    
    return decision;
}

// ============================================================================
// Парсер: преобразование команды пользователя в OrderCommand
// ============================================================================
OrderCommand LLMAgent::parseCommand(const std::string& user_input,
                                     const MarketContext& context) {
    
    std::cout << "  [LLM] Формирую промпт...\n";
    
    std::string system_prompt = buildParserSystemPrompt();
    std::string user_prompt = buildMarketContextPrompt(context);
    user_prompt += "\n\nИнструкция пользователя: " + user_input;
    
    std::cout << "  [LLM] Отправляю запрос к DeepSeek (парсер)...\n";
    
    json response;
    bool success = false;
    
    for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
        try {
            response = sendDeepSeekRequest(system_prompt, user_prompt,
                                           DeepSeekConfig::PARSER_MAX_TOKENS,
                                           DeepSeekConfig::PARSER_TEMPERATURE);
            success = true;
            break;
        } catch (const std::exception& e) {
            std::cerr << "  [LLM] Попытка " << attempt << "/" << MAX_RETRIES
                      << ": " << e.what() << "\n";
            if (attempt < MAX_RETRIES) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }
    
    if (!success) {
        throw std::runtime_error("Не удалось получить ответ от DeepSeek после " +
                                 std::to_string(MAX_RETRIES) + " попыток");
    }
    
    if (!validateParserSchema(response)) {
        throw std::runtime_error("Ответ LLM не соответствует JSON-схеме парсера");
    }
    
    OrderCommand cmd = parseOrderCommand(response);
    
    if (cmd.action == "unknown") {
        std::string error = response.value("error", "неизвестная ошибка");
        throw std::runtime_error("LLM не распознал команду: " + error);
    }
    
    std::cout << "  [LLM] Команда распознана: " << cmd.action << " " 
              << cmd.amount << " " << cmd.symbol << "\n";
    
    return cmd;
}

// ============================================================================
// СТРАТЕГ: анализ рынка и принятие решения
// ============================================================================
StrategistDecision LLMAgent::analyzeMarket(const MarketContext& context,
                                             const StrategyConfig& strategy,
                                             const RiskLimits& limits) {
    
    std::string system_prompt = buildStrategistSystemPrompt(strategy, limits);
    std::string user_prompt = buildMarketContextPrompt(context);
    
    json response;
    bool success = false;
    
    for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
        try {
            response = sendDeepSeekRequest(system_prompt, user_prompt,
                                           DeepSeekConfig::STRATEGIST_MAX_TOKENS,
                                           DeepSeekConfig::STRATEGIST_TEMPERATURE);
            success = true;
            break;
        } catch (const std::exception& e) {
            std::cerr << "  [LLM] Стратег: попытка " << attempt << "/" 
                      << MAX_RETRIES << ": " << e.what() << "\n";
            if (attempt < MAX_RETRIES) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }
    }
    
    if (!success) {
        // При ошибке — безопасное решение: держать позицию
        StrategistDecision fallback;
        fallback.action = "hold";
        fallback.reasoning = "Ошибка связи с DeepSeek API";
        return fallback;
    }
    
    if (!validateStrategistSchema(response)) {
        std::cerr << "  [LLM] Ответ стратега не соответствует схеме\n";
        StrategistDecision fallback;
        fallback.action = "hold";
        fallback.reasoning = "Ошибка валидации ответа стратега";
        return fallback;
    }
    
    return parseStrategistDecision(response);
}

// ============================================================================
// Системный промпт для ЧАТА
// 
// DeepSeek выступает как дружелюбный AI-ассистент по криптотрейдингу.
// Может отвечать на вопросы, анализировать рынок, давать советы.
// Если пользователь хочет торговать — включает торговую команду в JSON-ответ.
// ============================================================================
std::string LLMAgent::buildChatSystemPrompt() {
    return R"(Ты — AI-ассистент криптотрейдера на бирже CoinEx.
Твоё имя — CryptoTrader AI. Ты дружелюбный, полезный и говоришь по-русски.

ТВОИ ВОЗМОЖНОСТИ:
1. Отвечать на вопросы о рынке криптовалют
2. Анализировать текущие цены и тренды
3. Объяснять торговые концепции
4. Принимать торговые инструкции и создавать ордера
5. Давать советы по управлению рисками

ТЕКУЩИЙ РЕЖИМ: ручное управление. Пользователь даёт команды — ты помогаешь.

ПРАВИЛА ОТВЕТА:
- Отвечай строго в JSON-формате.
- Если пользователь просто общается или задаёт вопрос — отвечай текстом.
- Если пользователь явно хочет КУПИТЬ или ПРОДАТЬ — добавь поле trade.
- Будь краток: 1-4 предложения для обычного ответа.
- Используй эмодзи умеренно.

ФОРМАТ ОТВЕТА:
{
    "message": "Твой текстовый ответ здесь...",
    "trade": null
}

Если пользователь хочет торговать (явно сказал "купи", "продай", "buy", "sell"):
{
    "message": "Понял! Создаю ордер на покупку 0.15 ETH по рынку.",
    "trade": {
        "action": "buy",
        "symbol": "ETHUSDT",
        "amount": 0.15,
        "price_type": "market",
        "price": 0,
        "take_profit": 0,
        "stop_loss": 0
    }
}

ПРАВИЛА ДЛЯ ТОРГОВЫХ КОМАНД:
- symbol: всегда в формате CoinEx без "/" — "BTCUSDT", "ETHUSDT", "SOLUSDT"
- amount: количество базовой валюты (НЕ в USDT, если не указано иное)
- Если пользователь сказал "на 100 USDT" — пересчитай amount = 100 / цена
- price_type: "market" (по умолчанию) или "limit" (если указана цена)
- take_profit и stop_loss: проценты (10 = 10%). 0 если не указаны.
- Если команда неясная — trade = null и уточни в message.

ПРИМЕРЫ:
Пользователь: "привет"
Ответ: {"message":"Привет! Я CryptoTrader AI. Могу помочь с торговлей на CoinEx. Спрашивай о рынке или давай торговые команды.","trade":null}

Пользователь: "какой рынок сейчас?"
Ответ: {"message":"Смотрю текущие данные с биржи. Вижу цены в контексте. Задай уточняющий вопрос — по какой паре интересует?","trade":null}

Пользователь: "купи 0.1 ETH по рынку"
Ответ: {"message":"Принято. Создаю рыночный ордер на покупку 0.1 ETHUSDT.","trade":{"action":"buy","symbol":"ETHUSDT","amount":0.1,"price_type":"market","price":0,"take_profit":0,"stop_loss":0}}

Пользователь: "купи BTC на 500 USDT"
Ответ: {"message":"Сейчас BTC ~$65000. На $500 получится ~0.0077 BTC. Создаю рыночный ордер.","trade":{"action":"buy","symbol":"BTCUSDT","amount":0.0077,"price_type":"market","price":0,"take_profit":0,"stop_loss":0}}

Пользователь: "что думаешь про ETH?"
Ответ: {"message":"ETH показывает уверенный рост. Сейчас он на уровне, который стоит отслеживать. Если хочешь — могу помочь с покупкой или установкой стоп-лосса.","trade":null})";
}

// ============================================================================
// Парсинг ответа чата
// ============================================================================
ChatResponse LLMAgent::parseChatResponse(const json& response) {
    ChatResponse cr;
    
    cr.message = response.value("message", "Ответ не получен.");
    
    if (response.contains("trade") && !response["trade"].is_null()) {
        const auto& t = response["trade"];
        cr.wants_to_trade = true;
        cr.trade.action      = t.value("action", "");
        cr.trade.symbol      = t.value("symbol", "");
        cr.trade.market_type = CoinExConfig::MARKET_TYPE_SPOT;
        cr.trade.amount      = t.value("amount", 0.0);
        cr.trade.price_type  = t.value("price_type", "market");
        cr.trade.price       = t.value("price", 0.0);
        cr.trade.take_profit = t.value("take_profit", 0.0);
        cr.trade.stop_loss   = t.value("stop_loss", 0.0);
        cr.trade.reason      = "чат-команда";
    }
    
    return cr;
}

// ============================================================================
// ЧАТ: свободное общение с DeepSeek
// ============================================================================
ChatResponse LLMAgent::chat(const std::string& user_message,
                              const MarketContext& context) {
    
    std::string system_prompt = buildChatSystemPrompt();
    std::string user_prompt = buildMarketContextPrompt(context);
    user_prompt += "\n\nСообщение пользователя: " + user_message;
    
    std::cout << "  [LLM] Чат-запрос к DeepSeek...\n";
    
    json response;
    bool success = false;
    
    for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
        try {
            response = sendDeepSeekRequest(system_prompt, user_prompt,
                                           1500,   // max_tokens для чата
                                           0.7);   // температура — более разговорная
            success = true;
            break;
        } catch (const std::exception& e) {
            std::cerr << "  [LLM] Чат попытка " << attempt << "/" << MAX_RETRIES
                      << ": " << e.what() << "\n";
            if (attempt < MAX_RETRIES) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }
    
    if (!success) {
        ChatResponse fallback;
        fallback.message = "Извини, не могу связаться с DeepSeek. Проверь API ключ.";
        return fallback;
    }
    
    return parseChatResponse(response);
}
