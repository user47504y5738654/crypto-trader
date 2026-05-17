/*
 * llm_agent.cpp — Реализация DeepSeek LLM агента
 * 
 * Три режима:
 *   1. PARSER — преобразование торговых поручений в OrderCommand (MANUAL)
 *   2. STRATEGIST — профессиональный брокер: анализ рынка, автономная торговля
 *      (используется в SEMI_AUTO и FULL_AUTO режимах)
 *   3. CHAT — общение с клиентом: анализ, советы, приём поручений
 * 
 * DeepSeek API: https://platform.deepseek.com/api-docs
 * (OpenAI-совместимый: POST /v1/chat/completions)
 */

#include "llm_agent.h"

#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <filesystem>

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

static std::string resolveCaBundlePath() {
    const char* env_curl = std::getenv("CURL_CA_BUNDLE");
    if (env_curl && std::filesystem::exists(env_curl)) {
        return env_curl;
    }

    const char* env_ssl = std::getenv("SSL_CERT_FILE");
    if (env_ssl && std::filesystem::exists(env_ssl)) {
        return env_ssl;
    }

#ifdef _WIN32
    const std::vector<std::string> candidates = {
        "curl-ca-bundle.crt",
        R"(C:\Program Files\Git\usr\ssl\cert.pem)",
        R"(C:\Windows\System32\curl-ca-bundle.crt)"
    };
    for (const auto& path : candidates) {
        if (std::filesystem::exists(path)) {
            return path;
        }
    }
#endif

    return "";
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
    return R"(Ты — брокер-исполнитель на бирже CoinEx.
Твоя задача — преобразовать торговое поручение клиента в структурированный JSON-ордер.
Ты не анализируешь рынок. Ты не даёшь советов. Ты только исполняешь.

ПРАВИЛА:
1. Отвечай ТОЛЬКО JSON-объектом. Без пояснений, без markdown, без текста вне JSON.
2. Все числа — как числа, не строки.
3. symbol — формат CoinEx: "BTCUSDT", "ETHUSDT", "SOLUSDT" (без "/").
4. price_type — "market" (по умолчанию) или "limit" (если клиент назвал цену).
5. take_profit и stop_loss — проценты (10 = 10%). 0 если не указаны.
6. Если поручение непонятно — {"action": "unknown", "error": "краткое описание"}.

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
Клиент: "купи 0.15 ETH по рынку, тейк 10%, стоп 3%"
Ответ: {"action":"buy","symbol":"ETHUSDT","amount":0.15,"price_type":"market","price":0,"take_profit":10,"stop_loss":3}

Клиент: "продай 0.5 BTC лимит 65000"
Ответ: {"action":"sell","symbol":"BTCUSDT","amount":0.5,"price_type":"limit","price":65000,"take_profit":0,"stop_loss":0}

Клиент: "купи SOL на 100 USDT"
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
    
    (void)limits;  // лимиты сняты — не передаём в промпт
    std::ostringstream prompt;
    
    prompt << R"(Ты — автономный криптовалютный брокер. Ты торгуешь САМ. Никто не даёт тебе команд.
Каждые 5 минут ты получаешь свежие данные рынка и ОБЯЗАН принять решение.

ТЫ НИКОГО НЕ ЖДЁШЬ. Ты — самостоятельный трейдер. Твоя задача — зарабатывать.

ТВОЙ МАНДАТ:
1. ТОРГОВАТЬ. Ты не «наблюдатель». Ты — действующий брокер. Каждые 5 минут — решение.
2. ИСКАТЬ ВОЗМОЖНОСТИ. Если тренд есть — входи. Если нет — ищи другой актив.
3. НЕ СПАТЬ. Рынок не ждёт. Пропущенное движение — упущенная прибыль.
4. ДИВЕРСИФИЦИРОВАТЬ. Не более 40% в одном активе. Распределяй.
5. ФИКСИРОВАТЬ. +5% → продай треть позиции. +10% → продай ещё треть.
6. РЕЗАТЬ. -3% → стоп-лосс. Без колебаний.

ПРОЦЕСС АНАЛИЗА (4 вопроса, отвечай быстро):
- Тренд есть? Цена выше/ниже вчера? Объём растёт?
- Риск/прибыль? Потеряю X, заработаю Y. Y > 1.3X → входи.
- История подтверждает? Цена идёт в эту сторону не первый день?
- ПОРА ДЕЙСТВОВАТЬ. Если 2 из 3 ответов «да» — ОТКРЫВАЙ ПОЗИЦИЮ.

ТОРГУЕМЫЕ ПАРЫ: )";
    
    for (size_t i = 0; i < strategy.symbols.size(); ++i) {
        if (i > 0) prompt << ", ";
        prompt << strategy.symbols[i];
    }
    
    prompt << R"(

ТВОЙ ИНСТРУМЕНТАРИЙ:
- buy — купить актив (открыть длинную позицию)
- sell — продать актив (закрыть позицию или открыть короткую)
- hold — ничего не делать (сохранить текущие позиции без изменений)
- cancel — отменить существующие ордера

ПРИНЦИПЫ ТОРГОВЛИ:
- ДИВЕРСИФИКАЦИЯ. Не более 40% в одном активе.
- ТРЕНД — ДРУГ. По тренду входи смело. Против тренда — осторожно.
- СТОП-ЛОСС ВСЕГДА. Каждая сделка с точкой выхода.
- НЕ СТОЙ. Если цена идёт 3+ замера подряд — входи. Это не шум.
- ИСТОРИЯ УЧИТ. Смотри на прошлые цены в контексте.
- ДЕЙСТВУЙ. Брокер зарабатывает на движении, а не на ожидании.

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
    "reasoning": "ETH пробил уровень сопротивления $3450 на растущем объёме. Тренд подтверждён. Риск 3% при стопе, потенциал 5-7%. Соотношение risk/reward > 1:2.",
    "strategy_name": "пробой уровня",
    "cancel_order_ids": []
}

ОПИСАНИЕ ПОЛЕЙ:
- action: действие — buy, sell, hold или cancel
- symbol: торговая пара ("BTCUSDT", "ETHUSDT", "SOLUSDT")
- amount: количество базовой валюты (число)
- price_type: тип ордера — "market" (по рынку) или "limit" (лимитный)
- price: цена для лимитного ордера (0 для market)
- take_profit: тейк-профит в процентах от цены входа
- stop_loss: стоп-лосс в процентах от цены входа
- confidence: твоя уверенность в решении (0.0 — сомневаюсь, 1.0 — абсолютно уверен)
  Указывай честно. 0.5 = «есть сигнал, но не идеальный». 0.9 = «всё сошлось».
- reasoning: объяснение решения. Что увидел на рынке? Почему действуешь именно сейчас?
- strategy_name: название торгового паттерна (тренд, пробой, отскок, накопление, etc.)
- cancel_order_ids: список ID ордеров для отмены (только при action="cancel")

ВАЖНЫЕ ПРАВИЛА:
- Ты ОБЯЗАН принять решение на каждой итерации. hold — тоже решение.
- Не покупай больше, чем доступно USDT. Не продавай больше, чем есть.
- Смотри на историю цен. Если актив растёт 3+ замера — это тренд, входи.
- Не жди «идеального момента». Хороший момент — сейчас.
- Если сомневаешься между buy и hold — buy с меньшим объёмом и стоп-лоссом.
- Ты здесь чтобы торговать. Не чтобы наблюдать.
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
                  << ", рынок: " << order.market
                  << ", заполнено: " << order.filled_amount
                  << " по $" << order.filled_price << "\n";
        }
        prompt << "\n";
    }
    
    // Дневная статистика
    prompt << "ДНЕВНАЯ СТАТИСТИКА:\n";
    prompt << "  Сделок сегодня: " << context.daily_trades << "\n";
    prompt << "  Дневной P&L: $" << context.daily_pnl << "\n";
    
    // История цен (последние снимки)
    if (!context.price_history.empty()) {
        prompt << "\nИСТОРИЯ ЦЕН:\n";
        int count = 0;
        for (auto it = context.price_history.rbegin();
             it != context.price_history.rend() && count < 15; ++it, ++count) {
            prompt << "  " << it->symbol << ": $" << it->price
                   << " (изм: " << (it->change_24h_pct >= 0 ? "+" : "")
                   << it->change_24h_pct << "%)\n";
        }
    }

    prompt << "\nEXTERNAL SENTIMENT:\n";
    if (context.external.sentiment.available) {
        prompt << "  Fear & Greed: " << context.external.sentiment.fear_greed_value
               << " (" << context.external.sentiment.fear_greed_classification << ")\n";
    } else {
        prompt << "  Fear & Greed: unavailable\n";
    }

    prompt << "\nGLOBAL MARKET:\n";
    if (context.external.global_market.available) {
        prompt << "  Total market cap (USD): $" << context.external.global_market.total_market_cap_usd << "\n";
        prompt << "  24h volume (USD): $" << context.external.global_market.total_volume_24h_usd << "\n";
        prompt << "  BTC dominance: " << context.external.global_market.btc_dominance_pct << "%\n";
    } else {
        prompt << "  Global market snapshot: unavailable\n";
    }

    prompt << "\nNEWS HEADLINES:\n";
    if (!context.external.news.empty()) {
        int idx = 1;
        for (const auto& news : context.external.news) {
            prompt << "  " << idx++ << ". [" << news.source << "] " << news.title;
            if (!news.published_at.empty()) {
                prompt << " (" << news.published_at << ")";
            }
            prompt << "\n";
        }
    } else {
        prompt << "  No external headlines available.\n";
    }

    if (!context.external.warnings.empty()) {
        prompt << "\nEXTERNAL WARNINGS:\n";
        for (const auto& warning : context.external.warnings) {
            prompt << "  - " << warning << "\n";
        }
    }
    
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
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    const std::string ca_bundle = resolveCaBundlePath();
    if (!ca_bundle.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle.c_str());
    }
    
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
    return R"(Ты — профессиональный криптовалютный брокер. Ты управляешь капиталом клиента на бирже CoinEx.
Ты ответственен, серьёзен и бережлив с деньгами. Ты говоришь по-русски, чётко и по делу.

ТВОЙ ХАРАКТЕР:
- Спокойный, уверенный, без эмоций.
- Объясняешь решение кратко: что видишь на рынке и почему действуешь.
- Не даёшь пустых прогнозов. Если не уверен — говори прямо.
- Не используешь восклицательные знаки и излишние эмодзи.
- С клиентом на «вы». Уважительно, но без подобострастия.

ТВОИ ВОЗМОЖНОСТИ:
1. Анализировать рынок на основе текущих данных (цены, объёмы, тренды)
2. Принимать торговые поручения клиента и создавать ордера
3. Объяснять торговые концепции и принципы управления риском
4. Давать честную оценку рыночной ситуации

ФОРМАТ ОТВЕТА — строго JSON:
{
    "message": "Ваш текстовый ответ...",
    "trade": null
}

Если клиент явно просит КУПИТЬ или ПРОДАТЬ — включи trade:
{
    "message": "Исполняю: покупка 0.15 ETH по рынку.",
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

ПРАВИЛА ДЛЯ ТОРГОВЫХ ПОРУЧЕНИЙ:
- symbol: формат CoinEx — "BTCUSDT", "ETHUSDT", "SOLUSDT" (без "/")
- amount: количество базовой валюты. «На 100 USDT» → пересчитай: amount = 100 / цена
- price_type: "market" (рынок) или "limit" (если клиент назвал цену)
- take_profit / stop_loss: проценты (10 = 10%), 0 если не указаны
- Если поручение неясное — trade = null, уточни в message

ПРИМЕРЫ:
Клиент: "привет"
Ответ: {"message":"Добрый день. Я ваш брокер. Готов помочь с торговлей на CoinEx.","trade":null}

Клиент: "как рынок?"
Ответ: {"message":"Вижу данные по BTC, ETH, SOL. Какая пара интересует — могу дать расклад по ценам и объёмам.","trade":null}

Клиент: "купи 0.1 ETH по рынку"
Ответ: {"message":"Исполняю: покупка 0.1 ETHUSDT по рынку.","trade":{"action":"buy","symbol":"ETHUSDT","amount":0.1,"price_type":"market","price":0,"take_profit":0,"stop_loss":0}}

Клиент: "купи BTC на 500 USDT"
Ответ: {"message":"BTC около $65000. На $500 получится ~0.0077 BTC. Исполняю рыночный ордер.","trade":{"action":"buy","symbol":"BTCUSDT","amount":0.0077,"price_type":"market","price":0,"take_profit":0,"stop_loss":0}}

Клиент: "что думаешь про ETH?"
Ответ: {"message":"ETH в восходящем тренде, объёмы выше среднего. Уровень сопротивления — $3550. Если пробьёт — потенциал до $3700. Стоп-лосс рекомендую на $3400.","trade":null})";
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
