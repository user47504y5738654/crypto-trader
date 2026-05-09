/*
 * exchange_client.cpp — Реализация клиента CoinEx API v2
 * 
 * Полное соответствие документации https://docs.coinex.com/api/v2/:
 * 
 * Аутентификация (HMAC-SHA256):
 *   prepared_str = METHOD + request_path[?query] + body + timestamp
 *   signed_str = HMAC-SHA256(secret_key, prepared_str) → lowercase hex
 *   Заголовки: X-COINEX-KEY, X-COINEX-SIGN, X-COINEX-TIMESTAMP
 * 
 * Эндпоинты:
 *   GET  /assets/spot/balance       — баланс
 *   GET  /spot/ticker?market=X,Y    — тикеры
 *   GET  /spot/market               — список рынков
 *   POST /spot/order                — создать ордер
 *   GET  /spot/pending-order        — открытые ордера
 *   GET  /spot/finished-order       — завершённые ордера
 *   GET  /spot/order-status         — статус ордера
 *   DELETE /spot/order               — отмена ордеров
 * 
 * Все значения amount/price передаются и принимаются как строки.
 */

#include "exchange_client.h"

#include <iostream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cstring>
#include <random>

#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ============================================================================
// CURL callback
// ============================================================================
static size_t curlWriteCb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), total);
    return total;
}

// ============================================================================
// Текущее время в миллисекундах (UNIX timestamp)
// ============================================================================
static int64_t currentTimeMs() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
}

// ============================================================================
// Безопасный парсинг double из строки
// ============================================================================
double ExchangeClient::parseDouble(const std::string& s) {
    try {
        return std::stod(s);
    } catch (...) {
        return 0.0;
    }
}

// ============================================================================
// Генерация client_id
// ============================================================================
std::string ExchangeClient::generateClientId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    static const char* hex = "0123456789abcdef";
    
    std::string id = "ct_";
    for (int i = 0; i < 16; ++i) {
        id += hex[dis(gen)];
    }
    return id;
}

// ============================================================================
// Конструктор
// ============================================================================
ExchangeClient::ExchangeClient(const std::string& access_id,
                                 const std::string& secret_key)
    : m_access_id(access_id), m_secret_key(secret_key)
{
    curl_global_init(CURL_GLOBAL_ALL);
    
    // Инициализация симулированного баланса для dry-run
    m_sim_balance["USDT"] = {10000.0, 0.0};
    m_sim_balance["BTC"]  = {0.5, 0.0};
    m_sim_balance["ETH"]  = {5.0, 0.0};
    m_sim_balance["SOL"]  = {50.0, 0.0};
}

// ============================================================================
// Деструктор
// ============================================================================
ExchangeClient::~ExchangeClient() {
    curl_global_cleanup();
}

// ============================================================================
// Создание HMAC-SHA256 подписи (CoinEx API v2)
// 
// Формат строки для подписи (без \n!):
//   METHOD + request_path[?query] + body + timestamp
// 
// Пример (GET с query):
//   "GET/spot/pending-order?market=BTCUSDT&market_type=SPOT1700490703564"
// 
// Пример (POST с body):
//   "POST/spot/order{"market":"BTCUSDT",...}1700490703564"
// 
// Результат: lowercase hex, 64 символа
// ============================================================================
std::string ExchangeClient::createSignature(const std::string& method,
                                              const std::string& request_path,
                                              const std::string& body,
                                              const std::string& timestamp) {
    // Собираем строку: METHOD + path + body + timestamp
    std::string prepared = method + request_path + body + timestamp;
    
    // HMAC-SHA256
    unsigned char hash[SHA256_DIGEST_LENGTH];
    unsigned int hash_len = 0;
    
    HMAC(EVP_sha256(),
         m_secret_key.c_str(), static_cast<int>(m_secret_key.length()),
         reinterpret_cast<const unsigned char*>(prepared.c_str()),
         prepared.length(),
         hash, &hash_len);
    
    // Конвертируем в lowercase hex
    std::ostringstream hex;
    for (unsigned int i = 0; i < hash_len; ++i) {
        hex << std::hex << std::setw(2) << std::setfill('0') 
            << static_cast<int>(hash[i]);
    }
    
    return hex.str();  // уже lowercase — std::hex даёт "0a", а не "0A"
}

// ============================================================================
// Отправка HTTP-запроса к CoinEx API v2
// ============================================================================
std::string ExchangeClient::httpRequest(const std::string& method,
                                          const std::string& path,
                                          const std::string& body,
                                          const std::string& query_string) {
    
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("CURL: ошибка инициализации");
    }
    
    // Полный URL
    std::string url = CoinExConfig::BASE_URL + path;
    if (!query_string.empty()) {
        url += "?" + query_string;
    }
    
    // Временная метка
    std::string timestamp = std::to_string(currentTimeMs());
    
    // request_path для подписи: путь + query (если есть)
    std::string req_path = path;
    if (!query_string.empty()) {
        req_path += "?" + query_string;
    }
    
    // Создаём подпись
    std::string signature = createSignature(method, req_path, body, timestamp);
    
    // Заголовки аутентификации
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    std::string key_header = "X-COINEX-KEY: " + m_access_id;
    std::string sign_header = "X-COINEX-SIGN: " + signature;
    std::string ts_header = "X-COINEX-TIMESTAMP: " + timestamp;
    
    headers = curl_slist_append(headers, key_header.c_str());
    headers = curl_slist_append(headers, sign_header.c_str());
    headers = curl_slist_append(headers, ts_header.c_str());
    
    // Настройка CURL
    std::string response;
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    
    // Метод и тело
    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.length()));
    } else if (method == "DELETE") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    } else if (method == "PUT") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    }
    // GET — по умолчанию
    
    CURLcode res = curl_easy_perform(curl);
    
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        throw std::runtime_error("HTTP ошибка: " + std::string(curl_easy_strerror(res)));
    }
    
    if (http_code != 200) {
        throw std::runtime_error("CoinEx API: HTTP " + std::to_string(http_code) + 
                                 " — " + response);
    }
    
    // Проверяем код ответа API
    try {
        auto j = json::parse(response);
        if (j.contains("code") && j["code"].get<int>() != 0) {
            std::string msg = j.value("message", "неизвестная ошибка");
            throw std::runtime_error("CoinEx API error: " + msg);
        }
    } catch (const json::parse_error&) {
        // Ответ не JSON — возвращаем как есть (может быть для не-JSON эндпоинтов)
    }
    
    return response;
}

// ============================================================================
// Получение баланса спотового аккаунта
// GET /assets/spot/balance
// ============================================================================
std::map<std::string, Balance> ExchangeClient::getSpotBalance() {
    // Dry-run: симулированный баланс
    if (m_access_id.empty()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_sim_balance;
    }
    
    std::string response = httpRequest("GET", CoinExConfig::SPOT_BALANCE);
    auto j = json::parse(response);
    
    std::map<std::string, Balance> result;
    
    if (j.contains("data") && j["data"].is_array()) {
        for (const auto& item : j["data"]) {
            Balance bal;
            bal.available = parseDouble(item.value("available", "0"));
            bal.frozen    = parseDouble(item.value("frozen", "0"));
            result[item.value("ccy", "")] = bal;
        }
    }
    
    return result;
}

// ============================================================================
// Получение тикеров
// GET /spot/ticker?market=BTCUSDT,ETHUSDT
// 
// Если symbols пуст — возвращает ВСЕ тикеры.
// ============================================================================
std::map<std::string, Ticker> ExchangeClient::getTickers(
    const std::vector<std::string>& symbols) {
    
    // Dry-run: симулированные цены
    if (m_access_id.empty()) {
        std::map<std::string, Ticker> sim;
        for (const auto& s : {"BTCUSDT", "ETHUSDT", "SOLUSDT"}) {
            Ticker t;
            t.market = s;
            t.last   = (s == std::string("BTCUSDT")) ? 65000.0 :
                       (s == std::string("ETHUSDT")) ? 3500.0 : 150.0;
            t.open   = t.last * 0.995;
            t.high   = t.last * 1.02;
            t.low    = t.last * 0.98;
            t.change_pct = (t.last - t.open) / t.open * 100.0;
            sim[s] = t;
        }
        return sim;
    }
    
    // Формируем query: market=BTCUSDT,ETHUSDT
    std::string query;
    if (!symbols.empty()) {
        query = "market=";
        for (size_t i = 0; i < symbols.size(); ++i) {
            if (i > 0) query += ",";
            query += symbols[i];
        }
    }
    
    std::string response = httpRequest("GET", CoinExConfig::SPOT_TICKER, "", query);
    auto j = json::parse(response);
    
    std::map<std::string, Ticker> result;
    
    if (j.contains("data") && j["data"].is_array()) {
        for (const auto& item : j["data"]) {
            Ticker t;
            t.market  = item.value("market", "");
            t.last    = parseDouble(item.value("last", "0"));
            t.open    = parseDouble(item.value("open", "0"));
            t.high    = parseDouble(item.value("high", "0"));
            t.low     = parseDouble(item.value("low", "0"));
            t.volume  = parseDouble(item.value("volume", "0"));
            t.value   = parseDouble(item.value("value", "0"));
            
            if (t.open > 0) {
                t.change_pct = (t.last - t.open) / t.open * 100.0;
            }
            
            result[t.market] = t;
        }
    }
    
    return result;
}

// ============================================================================
// Получение списка рынков
// GET /spot/market
// ============================================================================
std::vector<std::string> ExchangeClient::getMarketList() {
    // Dry-run: список по умолчанию
    if (m_access_id.empty()) {
        return {"BTCUSDT", "ETHUSDT", "SOLUSDT"};
    }
    
    std::string response = httpRequest("GET", CoinExConfig::SPOT_MARKET);
    auto j = json::parse(response);
    
    std::vector<std::string> markets;
    
    if (j.contains("data") && j["data"].is_array()) {
        for (const auto& item : j["data"]) {
            if (item.contains("market")) {
                markets.push_back(item["market"].get<std::string>());
            } else if (item.is_string()) {
                markets.push_back(item.get<std::string>());
            }
        }
    }
    
    return markets;
}

// ============================================================================
// Получение открытых ордеров
// GET /spot/pending-order?market=X&market_type=SPOT&side=Y
// ============================================================================
std::vector<OrderResult> ExchangeClient::getPendingOrders(
    const std::string& market, const std::string& side) {
    
    if (m_access_id.empty()) return {};
    
    std::string query = "market_type=" + CoinExConfig::MARKET_TYPE_SPOT;
    if (!market.empty()) query += "&market=" + market;
    if (!side.empty())   query += "&side=" + side;
    
    std::string response = httpRequest("GET", CoinExConfig::SPOT_PENDING_ORDERS, "", query);
    auto j = json::parse(response);
    
    std::vector<OrderResult> results;
    
    if (j.contains("data") && j["data"].is_array()) {
        for (const auto& item : j["data"]) {
            OrderResult r;
            r.order_id      = item.value("order_id", "");
            r.filled_amount = parseDouble(item.value("filled_amount", "0"));
            r.filled_price  = parseDouble(item.value("last_filled_price", "0"));
            r.filled_value  = parseDouble(item.value("filled_value", "0"));
            r.base_fee      = parseDouble(item.value("base_fee", "0"));
            r.quote_fee     = parseDouble(item.value("quote_fee", "0"));
            
            std::string status = item.value("status", "");
            if (status == "filled")           r.status = OrderStatus::FILLED;
            else if (status == "part_filled") r.status = OrderStatus::PARTIALLY_FILLED;
            else                               r.status = OrderStatus::PENDING;
            
            results.push_back(r);
        }
    }
    
    return results;
}

// ============================================================================
// Получение завершённых ордеров
// GET /spot/finished-order?market=X&market_type=SPOT&limit=20
// ============================================================================
std::vector<OrderResult> ExchangeClient::getFinishedOrders(
    const std::string& market, const std::string& side, int limit) {
    
    if (m_access_id.empty()) return {};
    
    std::string query = "market_type=" + CoinExConfig::MARKET_TYPE_SPOT +
                        "&limit=" + std::to_string(limit);
    if (!market.empty()) query += "&market=" + market;
    if (!side.empty())   query += "&side=" + side;
    
    std::string response = httpRequest("GET", CoinExConfig::SPOT_FINISHED_ORDERS, "", query);
    auto j = json::parse(response);
    
    std::vector<OrderResult> results;
    
    if (j.contains("data") && j["data"].is_array()) {
        for (const auto& item : j["data"]) {
            OrderResult r;
            r.order_id      = item.value("order_id", "");
            r.filled_amount = parseDouble(item.value("filled_amount", "0"));
            r.filled_price  = parseDouble(item.value("last_filled_price", "0"));
            r.filled_value  = parseDouble(item.value("filled_value", "0"));
            r.base_fee      = parseDouble(item.value("base_fee", "0"));
            r.quote_fee     = parseDouble(item.value("quote_fee", "0"));
            r.created_at    = item.value("created_at", 0LL);
            
            std::string status = item.value("status", "");
            if (status == "filled")           r.status = OrderStatus::FILLED;
            else if (status == "cancelled")   r.status = OrderStatus::CANCELLED;
            else if (status == "rejected")    r.status = OrderStatus::REJECTED;
            else                               r.status = OrderStatus::EXPIRED;
            
            results.push_back(r);
        }
    }
    
    return results;
}

// ============================================================================
// Размещение ордера
// POST /spot/order
// 
// Обязательные поля: market, market_type, side, type, amount
// ============================================================================
OrderResult ExchangeClient::placeOrder(const OrderCommand& cmd) {
    // Dry-run
    if (m_access_id.empty()) {
        return simulateOrder(cmd);
    }
    
    // Формируем тело запроса
    json body = {
        {"market",      cmd.symbol},
        {"market_type", CoinExConfig::MARKET_TYPE_SPOT},
        {"side",        cmd.action},
        {"type",        cmd.price_type},
        {"amount",      std::to_string(cmd.amount)}
    };
    
    // Цена для лимитных ордеров
    if (cmd.price_type == "limit" && cmd.price > 0) {
        body["price"] = std::to_string(cmd.price);
    }
    
    // client_id
    if (!cmd.client_id.empty()) {
        body["client_id"] = cmd.client_id;
    }
    
    std::string body_str = body.dump();
    std::string response = httpRequest("POST", CoinExConfig::SPOT_ORDER, body_str);
    
    auto j = json::parse(response);
    
    OrderResult result;
    
    if (j.contains("data")) {
        const auto& data = j["data"];
        
        result.order_id      = data.value("order_id", "");
        result.filled_amount = parseDouble(data.value("filled_amount", "0"));
        result.filled_price  = parseDouble(data.value("last_filled_price", "0"));
        result.filled_value  = parseDouble(data.value("filled_value", "0"));
        result.base_fee      = parseDouble(data.value("base_fee", "0"));
        result.quote_fee     = parseDouble(data.value("quote_fee", "0"));
        result.created_at    = data.value("created_at", 0LL);
        
        std::string status = data.value("status", "pending");
        if (status == "filled")           result.status = OrderStatus::FILLED;
        else if (status == "part_filled") result.status = OrderStatus::PARTIALLY_FILLED;
        else if (status == "cancelled")   result.status = OrderStatus::CANCELLED;
        else                               result.status = OrderStatus::PENDING;
    } else {
        result.status = OrderStatus::REJECTED;
        result.error_msg = j.value("message", "неизвестная ошибка");
    }
    
    return result;
}

// ============================================================================
// Отмена всех ордеров
// DELETE /spot/order?market=X&market_type=SPOT
// ============================================================================
bool ExchangeClient::cancelAllOrders(const std::string& market) {
    if (m_access_id.empty()) return true;
    
    std::string query = "market_type=" + CoinExConfig::MARKET_TYPE_SPOT;
    if (!market.empty()) query += "&market=" + market;
    
    try {
        httpRequest("DELETE", CoinExConfig::SPOT_ORDER, "", query);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[EXCHANGE] Ошибка отмены ордеров: " << e.what() << "\n";
        return false;
    }
}

// ============================================================================
// Отмена ордера по ID
// DELETE /spot/order?order_id=X&market=Y&market_type=SPOT
// ============================================================================
bool ExchangeClient::cancelOrder(const std::string& order_id,
                                   const std::string& market) {
    if (m_access_id.empty()) return true;
    
    std::string query = "order_id=" + order_id +
                        "&market=" + market +
                        "&market_type=" + CoinExConfig::MARKET_TYPE_SPOT;
    
    try {
        httpRequest("DELETE", CoinExConfig::SPOT_ORDER, "", query);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[EXCHANGE] Ошибка отмены ордера " << order_id 
                  << ": " << e.what() << "\n";
        return false;
    }
}

// ============================================================================
// Получение контекста рынка (для стратега DeepSeek)
// ============================================================================
MarketContext ExchangeClient::getMarketContext() {
    MarketContext ctx;
    ctx.server_time = currentTimeMs();
    
    try {
        // Получаем тикеры для всех отслеживаемых пар
        std::vector<std::string> symbols = {"BTCUSDT", "ETHUSDT", "SOLUSDT"};
        ctx.tickers = getTickers(symbols);
        
        // Получаем баланс
        ctx.balances = getSpotBalance();
        
        // Получаем открытые ордера
        ctx.open_orders = getPendingOrders();
        
        // Вычисляем позиции на основе баланса
        for (const auto& [ccy, bal] : ctx.balances) {
            if (ccy != "USDT" && bal.total() > 0.0001) {
                ctx.positions[ccy + "USDT"] = bal.total();
            }
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[EXCHANGE] Ошибка получения контекста: " 
                  << e.what() << "\n";
    }
    
    return ctx;
}

// ============================================================================
// Проверка соединения
// ============================================================================
bool ExchangeClient::isConnected() {
    if (m_access_id.empty()) return true;
    
    try {
        httpRequest("GET", CoinExConfig::SPOT_MARKET);
        return true;
    } catch (...) {
        return false;
    }
}

// ============================================================================
// Время сервера
// ============================================================================
int64_t ExchangeClient::getServerTime() {
    return currentTimeMs();
}

// ============================================================================
// Симуляция ордера (dry-run)
// ============================================================================
OrderResult ExchangeClient::simulateOrder(const OrderCommand& cmd) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    OrderResult result;
    result.order_id = "SIM-" + generateClientId();
    result.status = OrderStatus::FILLED;
    result.filled_amount = cmd.amount;
    result.created_at = currentTimeMs();
    
    // Симулированная цена
    double price = 0.0;
    if (cmd.symbol == "BTCUSDT")      price = 65000.0;
    else if (cmd.symbol == "ETHUSDT") price = 3500.0;
    else if (cmd.symbol == "SOLUSDT") price = 150.0;
    else                               price = 100.0;
    
    result.filled_price = price;
    result.filled_value = cmd.amount * price;
    result.base_fee = cmd.amount * CoinExConfig::DEFAULT_FEE_RATE;
    result.quote_fee = result.filled_value * CoinExConfig::DEFAULT_FEE_RATE;
    
    // Обновляем симулированный баланс
    std::string base_ccy = cmd.symbol;
    // Убираем USDT из имени пары для получения базовой валюты
    if (base_ccy.length() > 4) {
        base_ccy = base_ccy.substr(0, base_ccy.length() - 4);
    }
    
    if (cmd.action == "buy") {
        double cost = cmd.amount * price;
        m_sim_balance["USDT"].available -= cost;
        m_sim_balance[base_ccy].available += cmd.amount;
    } else {
        m_sim_balance[base_ccy].available -= cmd.amount;
        m_sim_balance["USDT"].available += cmd.amount * price;
    }
    
    // Статистика
    m_stats.total_trades++;
    m_stats.successful_trades++;
    m_stats.total_fees += result.quote_fee;
    
    return result;
}

// ============================================================================
// Статистика dry-run
// ============================================================================
ExchangeClient::DryRunStats ExchangeClient::getDryRunStats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

// ============================================================================
// Сброс dry-run статистики
// ============================================================================
void ExchangeClient::resetDryRunStats() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stats = DryRunStats{};
}
