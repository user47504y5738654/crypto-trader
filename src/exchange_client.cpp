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
#include <cstdlib>
#include <regex>
#include <set>
#include <cctype>
#include <filesystem>

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

static void applyTlsOptions(CURL* curl, const std::string& ca_bundle) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (!ca_bundle.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle.c_str());
    }
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

std::string ExchangeClient::trim(const std::string& input) {
    size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) {
        ++start;
    }

    if (start == input.size()) {
        return "";
    }

    size_t end = input.size() - 1;
    while (end > start && std::isspace(static_cast<unsigned char>(input[end]))) {
        --end;
    }
    return input.substr(start, end - start + 1);
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
void ExchangeClient::setDryRun(bool dry) {
    m_dry_run = dry;
}

void ExchangeClient::setSimBalance(const std::string& currency, double amount) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sim_balance[currency] = {amount, 0.0};
}

void ExchangeClient::setCMCKey(const std::string& key) {
    m_cmc_key = key;
}

std::vector<PriceSnapshot> ExchangeClient::getPriceHistory() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_price_history;
}

// ============================================================================
// CoinMarketCap API — получение реальных цен
// ============================================================================
std::map<std::string, Ticker> ExchangeClient::fetchCMCPrices(
    const std::vector<std::string>& symbols) {
    
    std::map<std::string, Ticker> result;
    
    if (m_cmc_key.empty()) return result;
    
    // Формируем список символов: "BTC,ETH,SOL"
    std::string sym_list;
    for (size_t i = 0; i < symbols.size(); ++i) {
        if (i > 0) sym_list += ",";
        // BTCUSDT → BTC
        std::string s = symbols[i];
        if (s.length() > 4 && s.substr(s.length() - 4) == "USDT") {
            s = s.substr(0, s.length() - 4);
        }
        sym_list += s;
    }
    
    CURL* curl = curl_easy_init();
    if (!curl) return result;
    
    std::string url = CMCConfig::API_URL + "?symbol=" + sym_list + "&convert=USD";
    std::string response;
    
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    std::string key_hdr = "X-CMC_PRO_API_KEY: " + m_cmc_key;
    headers = curl_slist_append(headers, key_hdr.c_str());
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    const std::string ca_bundle = resolveCaBundlePath();
    applyTlsOptions(curl, ca_bundle);
    
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK || http_code != 200) {
        std::cerr << "[CMC] HTTP " << http_code << ": " << 
            (res != CURLE_OK ? curl_easy_strerror(res) : "OK") << "\n";
        return result;
    }
    
    try {
        auto j = json::parse(response);
        if (!j.contains("data")) return result;
        
        int64_t now = currentTimeMs();
        
        for (const auto& [coin, data] : j["data"].items()) {
            if (!data.contains("quote") || !data["quote"].contains("USD")) continue;
            
            const auto& usd = data["quote"]["USD"];
            std::string market = coin + "USDT";
            
            Ticker t;
            t.market  = market;
            t.last    = parseDouble(std::to_string(usd["price"].get<double>()));
            t.volume  = parseDouble(std::to_string(usd["volume_24h"].get<double>()));
            t.change_pct = parseDouble(std::to_string(usd["percent_change_24h"].get<double>()));
            t.open    = t.last / (1.0 + t.change_pct / 100.0);
            t.high    = t.last * 1.01;
            t.low     = t.last * 0.99;
            t.value   = t.volume * t.last;
            
            result[market] = t;
            
            // Сохраняем в историю
            PriceSnapshot snap;
            snap.timestamp = now;
            snap.symbol = market;
            snap.price = t.last;
            snap.volume_24h = t.volume;
            snap.change_24h_pct = t.change_pct;
            
            std::lock_guard<std::mutex> lock(m_mutex);
            m_price_history.push_back(snap);
            if (m_price_history.size() > 100) {
                m_price_history.erase(m_price_history.begin());
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[CMC] Parse error: " << e.what() << "\n";
    }
    
    return result;
}

// ============================================================================
// Создание HMAC-SHA256 подписи (CoinEx API v2)
// 
// Формат строки для подписи (без \n!):
//   METHOD + request_path[?query] + body + timestamp
// 
// Пример (GET с query):
//   "GET/v2/spot/pending-order?market=BTCUSDT&market_type=SPOT1700490703564"
// 
// Пример (POST с body):
//   "POST/v2/spot/order{"market":"BTCUSDT",...}1700490703564"
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
    
    // request_path для подписи: /v2 + путь + query (CoinEx требует полный путь)
    std::string req_path = "/v2" + path;
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
    const std::string ca_bundle = resolveCaBundlePath();
    applyTlsOptions(curl, ca_bundle);
    
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

std::string ExchangeClient::httpRequestAbsolute(const std::string& url,
                                                 const std::vector<std::string>& headers) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("CURL init failed for external request");
    }

    std::string response;
    struct curl_slist* curl_headers = nullptr;

    bool has_user_agent = false;
    bool has_accept = false;
    for (const auto& header : headers) {
        std::string lower = header;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.rfind("user-agent:", 0) == 0) has_user_agent = true;
        if (lower.rfind("accept:", 0) == 0) has_accept = true;
        curl_headers = curl_slist_append(curl_headers, header.c_str());
    }

    if (!has_accept) {
        curl_headers = curl_slist_append(curl_headers,
            "Accept: application/json, application/rss+xml, application/xml;q=0.9, */*;q=0.8");
    }
    if (!has_user_agent) {
        curl_headers = curl_slist_append(curl_headers,
            "User-Agent: CryptoTrader/2.0 (+https://github.com)");
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curl_headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    const std::string ca_bundle = resolveCaBundlePath();
    applyTlsOptions(curl, ca_bundle);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(curl_headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error(std::string("External HTTP error: ") + curl_easy_strerror(res));
    }
    if (http_code != 200) {
        throw std::runtime_error("External HTTP status: " + std::to_string(http_code));
    }
    return response;
}

// ============================================================================
// Получение баланса спотового аккаунта
// GET /assets/spot/balance
// ============================================================================
std::map<std::string, Balance> ExchangeClient::getSpotBalance() {
    // Dry-run: симулированный баланс
    if (m_dry_run || m_access_id.empty()) {
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
    
    // Dry-run: CMC если есть ключ, иначе симуляция
    if (m_dry_run || m_access_id.empty()) {
        // Пробуем CoinMarketCap для реальных цен
        if (!m_cmc_key.empty()) {
            auto cmc = fetchCMCPrices(symbols);
            if (!cmc.empty()) return cmc;
        }
        // Fallback: симулированные цены
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
    if (m_dry_run || m_access_id.empty()) {
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
    
    if (m_dry_run || m_access_id.empty()) return {};
    
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
            r.market        = item.value("market", market);
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
    
    if (m_dry_run || m_access_id.empty()) return {};
    
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
            r.market        = item.value("market", market);
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
    if (m_dry_run || m_access_id.empty()) {
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
    result.market = cmd.symbol;
    
    if (j.contains("data")) {
        const auto& data = j["data"];
        
        result.order_id      = data.value("order_id", "");
        result.market        = data.value("market", cmd.symbol);
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
    if (m_dry_run || m_access_id.empty()) return true;
    
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
    if (m_dry_run || m_access_id.empty()) return true;
    
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
ExternalSentiment ExchangeClient::fetchFearGreed(std::vector<std::string>& warnings) {
    ExternalSentiment sentiment;
    try {
        const std::string response = httpRequestAbsolute("https://api.alternative.me/fng/?limit=1");
        auto j = json::parse(response);
        if (!j.contains("data") || !j["data"].is_array() || j["data"].empty()) {
            warnings.push_back("fear_greed: unexpected response");
            return sentiment;
        }

        const auto& item = j["data"][0];
        sentiment.fear_greed_value = std::stoi(item.value("value", "0"));
        sentiment.fear_greed_classification = item.value("value_classification", "");
        sentiment.updated_at = item.value("timestamp", "");
        sentiment.available = true;
    } catch (const std::exception& e) {
        warnings.push_back(std::string("fear_greed: ") + e.what());
    }
    return sentiment;
}

ExternalGlobalMarket ExchangeClient::fetchGlobalMarket(std::vector<std::string>& warnings) {
    ExternalGlobalMarket market;
    try {
        const std::string response = httpRequestAbsolute("https://api.coingecko.com/api/v3/global");
        auto j = json::parse(response);
        if (!j.contains("data")) {
            warnings.push_back("coingecko_global: missing data");
            return market;
        }

        const auto& data = j["data"];
        if (data.contains("total_market_cap") && data["total_market_cap"].contains("usd")) {
            market.total_market_cap_usd = data["total_market_cap"]["usd"].get<double>();
        }
        if (data.contains("total_volume") && data["total_volume"].contains("usd")) {
            market.total_volume_24h_usd = data["total_volume"]["usd"].get<double>();
        }
        if (data.contains("market_cap_percentage") && data["market_cap_percentage"].contains("btc")) {
            market.btc_dominance_pct = data["market_cap_percentage"]["btc"].get<double>();
        }

        market.updated_at = std::to_string(currentTimeMs());
        market.available = true;
    } catch (const std::exception& e) {
        warnings.push_back(std::string("coingecko_global: ") + e.what());
    }
    return market;
}

std::vector<ExternalNewsItem> ExchangeClient::fetchNewsHeadlines(
    std::vector<std::string>& warnings, int max_items) {
    std::vector<ExternalNewsItem> result;
    std::set<std::string> seen_titles;

    const std::vector<std::pair<std::string, std::string>> feeds = {
        {"CoinDesk", "https://www.coindesk.com/arc/outboundfeeds/rss/"},
        {"Cointelegraph", "https://cointelegraph.com/rss"}
    };

    auto extract_tag = [](const std::string& block, const std::string& tag) -> std::string {
        std::regex rx("<" + tag + R"((?:\s[^>]*)?>([\s\S]*?)</)" + tag + ">", std::regex::icase);
        std::smatch match;
        if (!std::regex_search(block, match, rx)) {
            return "";
        }
        return ExchangeClient::trim(match[1].str());
    };

    for (const auto& [source, url] : feeds) {
        if (static_cast<int>(result.size()) >= max_items) {
            break;
        }

        try {
            const std::string rss = httpRequestAbsolute(url);
            std::regex item_rx(R"(<item\b[^>]*>([\s\S]*?)</item>)", std::regex::icase);
            auto begin = std::sregex_iterator(rss.begin(), rss.end(), item_rx);
            auto end = std::sregex_iterator();

            for (auto it = begin; it != end && static_cast<int>(result.size()) < max_items; ++it) {
                const std::string item_xml = (*it)[1].str();
                ExternalNewsItem item;
                item.source = source;
                item.title = extract_tag(item_xml, "title");
                item.published_at = extract_tag(item_xml, "pubDate");
                item.link = extract_tag(item_xml, "link");

                if (item.title.empty() || seen_titles.count(item.title)) {
                    continue;
                }
                seen_titles.insert(item.title);
                result.push_back(item);
            }
        } catch (const std::exception& e) {
            warnings.push_back(source + std::string("_rss: ") + e.what());
        }
    }

    if (result.empty()) {
        warnings.push_back("news: no headlines collected");
    }
    return result;
}
MarketContext ExchangeClient::getMarketContext() {
    MarketContext ctx;
    ctx.server_time = currentTimeMs();

    try {
        std::vector<std::string> symbols = {"BTCUSDT", "ETHUSDT", "SOLUSDT"};
        ctx.tickers = getTickers(symbols);
        ctx.balances = getSpotBalance();
        ctx.open_orders = getPendingOrders();
        ctx.price_history = getPriceHistory();

        for (const auto& [ccy, bal] : ctx.balances) {
            if (ccy != "USDT" && bal.total() > 0.0001) {
                ctx.positions[ccy + "USDT"] = bal.total();
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[EXCHANGE] ������ ��������� ���������: " << e.what() << "\n";
    }

    ctx.external.sentiment = fetchFearGreed(ctx.external.warnings);
    ctx.external.global_market = fetchGlobalMarket(ctx.external.warnings);
    ctx.external.news = fetchNewsHeadlines(ctx.external.warnings, 5);
    for (const auto& warning : ctx.external.warnings) {
        std::cerr << "[EXTERNAL] warning: " << warning << "\n";
    }

    return ctx;
}

// ============================================================================
// Проверка соединения
// ============================================================================
bool ExchangeClient::isConnected() {
    if (m_dry_run || m_access_id.empty()) return true;
    
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
    result.market = cmd.symbol;
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











