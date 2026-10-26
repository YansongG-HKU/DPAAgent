#include "AgentConfig.h"
#include <QFile>
#include <QString>
#include <QByteArray>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHostAddress>
#include <QSet>
#include <QUrl>
#include <algorithm>
#include <cmath>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dpapi.h>

#pragma comment(lib, "Crypt32.lib")

namespace Model {
namespace {

constexpr int kMaxFallbackRoutes = 3;
constexpr long long kMaxPriceMicrosPerMillionTokens = 1000000000000LL;

LlmConfig defaultConfig() {
    LlmConfig cfg;
    cfg.provider = "openai_compat";
    cfg.baseUrl  = "http://127.0.0.1:11434/v1/chat/completions";
    cfg.model    = "qwen3:30b-a3b";
    cfg.apiKey   = "";
    cfg.maxTokens = 2048;
    return cfg;
}

QString configPath(const std::string& overridePath) {
    if (!overridePath.empty())
        return QFileInfo(QString::fromStdString(overridePath)).absoluteFilePath();

    const QString exeDir = QCoreApplication::applicationDirPath();
    const QString besideExe = exeDir + "/dpa_agent_llm.json";
    if (!exeDir.isEmpty() && QFile::exists(besideExe)) return QFileInfo(besideExe).absoluteFilePath();

    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return appData.isEmpty() ? QString() : QFileInfo(appData + "/dpa_agent_llm.json").absoluteFilePath();
}

bool decryptCurrentUserDpapi(const QString& base64, std::string& plaintext, std::string& error) {
    const auto decoded = QByteArray::fromBase64Encoding(
        base64.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.isEmpty()) {
        error = "invalid_dpapi_base64";
        return false;
    }

    QByteArray encrypted = decoded.decoded;
    DATA_BLOB input;
    input.cbData = static_cast<DWORD>(encrypted.size());
    input.pbData = reinterpret_cast<BYTE*>(encrypted.data());
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        error = "dpapi_decrypt_failed";
        return false;
    }

    QByteArray cleartext(reinterpret_cast<const char*>(output.pbData), static_cast<int>(output.cbData));
    if (output.pbData) {
        SecureZeroMemory(output.pbData, output.cbData);
        LocalFree(output.pbData);
    }
    plaintext.assign(cleartext.constData(), cleartext.size());
    cleartext.fill('\0');
    return true;
}

void securelyClear(std::string& value) {
    if (!value.empty()) SecureZeroMemory(&value[0], value.size());
    value.clear();
}

bool validRouteId(const QString& id) {
    if (id.isEmpty() || id.size() > 64) return false;
    for (const QChar c : id) {
        const ushort value = c.unicode();
        const bool asciiAlphaNumeric = (value >= 'a' && value <= 'z')
            || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9');
        if (!asciiAlphaNumeric && value != '-' && value != '_') return false;
    }
    return true;
}

std::string routeEnvironmentName(const std::string& id, bool primary) {
    if (primary) return "DPA_LLM_API_KEY";
    std::string name = "DPA_LLM_API_KEY_";
    for (unsigned char c : id) {
        if (c >= 'a' && c <= 'z') name.push_back(static_cast<char>(c - 'a' + 'A'));
        else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) name.push_back(static_cast<char>(c));
        else name.push_back('_');
    }
    return name;
}

bool readInteger(const QJsonObject& object, const char* key,
                 long long minimum, long long maximum, long long& value) {
    if (!object.contains(key)) return true;
    const QJsonValue raw = object.value(key);
    if (!raw.isDouble()) return false;
    const double number = raw.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
        || number < static_cast<double>(minimum) || number > static_cast<double>(maximum))
        return false;
    value = static_cast<long long>(number);
    return true;
}

void addConfigError(LlmConfigLoadResult& result, const std::string& routeId,
                    const std::string& code) {
    result.configErrors.push_back("route:" + routeId + ":" + code);
}

void disableRoute(LlmRouteConfig& route, LlmConfigLoadResult& result,
                  const std::string& code) {
    route.enabled = false;
    if (route.disabledReason.empty()) route.disabledReason = code;
    addConfigError(result, route.id, code);
}

void applyRouteFields(const QJsonObject& values, LlmRouteConfig& route,
                      LlmConfigLoadResult& result) {
    auto boundedString = [&](const char* key, std::string& target, int maxBytes) {
        if (!values.contains(key)) return;
        if (!values.value(key).isString()) {
            disableRoute(route, result, std::string("invalid_") + key);
            return;
        }
        const QByteArray utf8 = values.value(key).toString().toUtf8();
        if (utf8.isEmpty() || utf8.size() > maxBytes) {
            disableRoute(route, result, std::string("invalid_") + key);
            return;
        }
        target.assign(utf8.constData(), utf8.size());
    };
    boundedString("provider", route.config.provider, 32);
    boundedString("model", route.config.model, 256);
    boundedString("baseUrl", route.config.baseUrl, 2048);

    long long maxTokens = route.config.maxTokens;
    if (!readInteger(values, "maxTokens", 1, 131072, maxTokens))
        disableRoute(route, result, "invalid_maxTokens");
    else
        route.config.maxTokens = static_cast<int>(maxTokens);

    long long timeoutMs = route.config.timeoutMs;
    if (values.contains("timeoutMs")) {
        if (!readInteger(values, "timeoutMs", 0, 600000, timeoutMs)
            || (timeoutMs > 0 && timeoutMs < 1000))
            disableRoute(route, result, "invalid_timeoutMs");
        else
            route.config.timeoutMs = static_cast<int>(timeoutMs);
    }

    if (route.config.provider != "openai_compat" && route.config.provider != "claude")
        disableRoute(route, result, "unsupported_provider");

    const QUrl endpoint(QString::fromStdString(route.config.baseUrl));
    const QString scheme = endpoint.scheme().toLower();
    if (!endpoint.isValid() || endpoint.host().isEmpty()
        || (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")))
        disableRoute(route, result, "invalid_endpoint");

    // Provider-specific options must be explicit on each route, never inherited
    // by a fallback that may use a different API or model template.
    route.config.enableThinking.reset();
    if (values.contains("enableThinking")) {
        if (!values.value("enableThinking").isBool())
            disableRoute(route, result, "invalid_enableThinking");
        else if (route.config.provider != "openai_compat"
                 || !(endpoint.path().endsWith("/chat/completions", Qt::CaseInsensitive)
                      || endpoint.path().endsWith("/chat/completions/", Qt::CaseInsensitive)))
            disableRoute(route, result, "enableThinking_requires_chat_completions");
        else
            route.config.enableThinking = values.value("enableThinking").toBool();
    }
}

void loadRouteSecret(const QJsonObject& values, LlmRouteConfig& route) {
    const QByteArray environmentName = QByteArray::fromStdString(route.apiKeyEnvironment);
    if (qEnvironmentVariableIsSet(environmentName.constData())) {
        QByteArray environmentKey = qgetenv(environmentName.constData());
        route.config.apiKey.assign(environmentKey.constData(), environmentKey.size());
        environmentKey.fill('\0');
        route.apiKeySource = ApiKeySource::Environment;
        return;
    }

    if (values.contains("apiKeyDpapi")) {
        if (!values.value("apiKeyDpapi").isString()) {
            route.apiKeyError = "invalid_dpapi_field";
            return;
        }
        if (decryptCurrentUserDpapi(values.value("apiKeyDpapi").toString(),
                                    route.config.apiKey, route.apiKeyError))
            route.apiKeySource = ApiKeySource::DpapiCurrentUser;
        return; // fail closed: encrypted data never falls back to plaintext
    }

    if (values.contains("apiKey") && values.value("apiKey").isString()) {
        route.config.apiKey = values.value("apiKey").toString().toStdString();
        route.apiKeySource = ApiKeySource::LegacyPlaintext;
        route.legacyPlaintextApiKey = true;
    }
}

void applyPricing(const QJsonObject& values, LlmRouteConfig& route,
                  LlmConfigLoadResult& result) {
    const bool hasInput = values.contains("inputCostMicrosPerMillionTokens");
    const bool hasOutput = values.contains("outputCostMicrosPerMillionTokens");
    if (!hasInput && !hasOutput) return;
    if (!hasInput || !hasOutput) {
        route.pricingError = "incomplete_pricing";
        addConfigError(result, route.id, route.pricingError);
        return;
    }

    long long input = 0;
    long long output = 0;
    if (!readInteger(values, "inputCostMicrosPerMillionTokens", 0,
                     kMaxPriceMicrosPerMillionTokens, input)
        || !readInteger(values, "outputCostMicrosPerMillionTokens", 0,
                        kMaxPriceMicrosPerMillionTokens, output)) {
        route.pricingError = "invalid_pricing";
        addConfigError(result, route.id, route.pricingError);
        return;
    }
    route.pricingConfigured = true;
    route.inputCostMicrosPerMillionTokens = input;
    route.outputCostMicrosPerMillionTokens = output;
}

void finalizeRoute(LlmRouteConfig& route, LlmConfigLoadResult& result) {
    if (!route.apiKeyError.empty()) {
        securelyClear(route.config.apiKey);
        disableRoute(route, result, route.apiKeyError);
        return;
    }
    const std::string transportError = validateCredentialedEndpoint(route.config);
    if (!transportError.empty()) {
        securelyClear(route.config.apiKey);
        route.apiKeyError = transportError;
        disableRoute(route, result, transportError);
    }
}

void parseRoutingAndBudget(const QJsonObject& values, LlmConfigLoadResult& result) {
    result.budget.maxModelCalls = 8;

    if (values.contains("routing")) {
        if (!values.value("routing").isObject()) {
            result.configErrors.push_back("routing:invalid_object");
        } else {
            const QJsonObject routing = values.value("routing").toObject();
            long long threshold = result.routing.failureThreshold;
            long long cooldown = result.routing.cooldownMs;
            if (!readInteger(routing, "failureThreshold", 1, 10, threshold))
                result.configErrors.push_back("routing:invalid_failureThreshold");
            else
                result.routing.failureThreshold = static_cast<int>(threshold);
            if (!readInteger(routing, "cooldownMs", 1000, 3600000, cooldown))
                result.configErrors.push_back("routing:invalid_cooldownMs");
            else
                result.routing.cooldownMs = cooldown;
        }
    }

    if (!values.contains("budget")) return;
    if (!values.value("budget").isObject()) {
        result.configErrors.push_back("budget:invalid_object");
        return;
    }
    const QJsonObject budget = values.value("budget").toObject();
    long long modelCalls = result.budget.maxModelCalls;
    long long inputTokens = result.budget.maxInputTokens;
    long long outputTokens = result.budget.maxOutputTokens;
    long long costMicros = result.budget.maxEstimatedCostMicros;
    if (!readInteger(budget, "maxModelCallsPerTurn", 1, 8, modelCalls))
        result.configErrors.push_back("budget:invalid_maxModelCallsPerTurn");
    else
        result.budget.maxModelCalls = static_cast<int>(modelCalls);
    if (!readInteger(budget, "maxInputTokensPerTurn", 0, 100000000, inputTokens))
        result.configErrors.push_back("budget:invalid_maxInputTokensPerTurn");
    else
        result.budget.maxInputTokens = inputTokens;
    if (!readInteger(budget, "maxOutputTokensPerTurn", 0, 100000000, outputTokens))
        result.configErrors.push_back("budget:invalid_maxOutputTokensPerTurn");
    else
        result.budget.maxOutputTokens = outputTokens;
    if (!readInteger(budget, "maxEstimatedCostMicrosPerTurn", 0,
                     1000000000000000LL, costMicros))
        result.configErrors.push_back("budget:invalid_maxEstimatedCostMicrosPerTurn");
    else
        result.budget.maxEstimatedCostMicros = costMicros;
}

} // namespace

std::string validateCredentialedEndpoint(const LlmConfig& config) {
    if (config.apiKey.empty()) return {};

    const QUrl endpoint(QString::fromStdString(config.baseUrl));
    if (!endpoint.isValid() || endpoint.host().isEmpty())
        return "insecure_credential_transport";

    const QString host = endpoint.host().toLower();
    bool loopback = host == QStringLiteral("localhost");
    QHostAddress address;
    if (!loopback && address.setAddress(host)) loopback = address.isLoopback();

    if (!loopback && endpoint.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0)
        return "insecure_credential_transport";
    return {};
}

LlmConfigLoadResult loadLlmConfigDetailed(const std::string& pathOverride) {
    LlmConfigLoadResult result;
    const QString path = configPath(pathOverride);
    result.configPath = path.toStdString();

    QJsonObject values;
    if (!path.isEmpty()) {
        QFile file(path);
        if (file.exists()) {
            if (!file.open(QIODevice::ReadOnly)) {
                result.configErrors.push_back("config:read_failed");
            } else {
                QByteArray raw = file.readAll();
                file.close();
                QJsonParseError parseError;
                const QJsonDocument document = QJsonDocument::fromJson(raw, &parseError);
                raw.fill('\0');
                if (parseError.error != QJsonParseError::NoError || !document.isObject())
                    result.configErrors.push_back("config:invalid_json");
                else
                    values = document.object();
            }
        }
    }

    parseRoutingAndBudget(values, result);

    LlmRouteConfig primary;
    primary.id = "primary";
    primary.config = defaultConfig();
    primary.apiKeyEnvironment = routeEnvironmentName(primary.id, true);
    applyRouteFields(values, primary, result);
    loadRouteSecret(values, primary);
    applyPricing(values, primary, result);
    finalizeRoute(primary, result);
    result.routes.push_back(primary);

    result.config = primary.config;
    result.apiKeySource = primary.apiKeySource;
    result.legacyPlaintextApiKey = primary.legacyPlaintextApiKey;
    result.apiKeyError = primary.apiKeyError;

    if (values.contains("fallbacks")) {
        if (!values.value("fallbacks").isArray()) {
            result.configErrors.push_back("fallbacks:invalid_array");
        } else {
            const QJsonArray fallbacks = values.value("fallbacks").toArray();
            QSet<QString> routeIds{QStringLiteral("primary")};
            QSet<QString> routeEnvironmentNames;
            if (fallbacks.size() > kMaxFallbackRoutes)
                result.configErrors.push_back("fallbacks:route_limit_exceeded");
            const int count = std::min(fallbacks.size(), kMaxFallbackRoutes);
            for (int index = 0; index < count; ++index) {
                LlmRouteConfig route;
                route.config = primary.config;
                securelyClear(route.config.apiKey);

                if (!fallbacks.at(index).isObject()) {
                    route.id = "invalid-" + std::to_string(index + 1);
                    route.enabled = false;
                    route.disabledReason = "invalid_route_object";
                    addConfigError(result, route.id, route.disabledReason);
                    result.routes.push_back(std::move(route));
                    continue;
                }
                const QJsonObject object = fallbacks.at(index).toObject();
                const QString id = object.value("id").toString();
                if (!validRouteId(id) || routeIds.contains(id)) {
                    route.id = "invalid-" + std::to_string(index + 1);
                    route.enabled = false;
                    route.disabledReason = validRouteId(id) ? "duplicate_route_id" : "invalid_route_id";
                    addConfigError(result, route.id, route.disabledReason);
                    result.routes.push_back(std::move(route));
                    continue;
                }
                route.id = id.toStdString();
                routeIds.insert(id);
                route.apiKeyEnvironment = routeEnvironmentName(route.id, false);
                const QString environmentName =
                    QString::fromStdString(route.apiKeyEnvironment);
                if (routeEnvironmentNames.contains(environmentName)) {
                    route.enabled = false;
                    route.disabledReason = "duplicate_route_environment";
                    addConfigError(result, route.id, route.disabledReason);
                    result.routes.push_back(std::move(route));
                    continue;
                }
                routeEnvironmentNames.insert(environmentName);

                if (object.value("enabled").isBool() && !object.value("enabled").toBool()) {
                    route.enabled = false;
                    route.disabledReason = "disabled_by_config";
                }
                if (!object.value("allowDiagnosticData").isBool()
                    || !object.value("allowDiagnosticData").toBool())
                    disableRoute(route, result, "diagnostic_data_not_authorized");
                if (!object.contains("baseUrl"))
                    disableRoute(route, result, "missing_baseUrl");

                applyRouteFields(object, route, result);
                loadRouteSecret(object, route);
                applyPricing(object, route, result);
                finalizeRoute(route, result);
                result.routes.push_back(std::move(route));
            }
        }
    }

    return result;
}

LlmConfig loadLlmConfig() {
    return loadLlmConfigDetailed().config;
}

}
