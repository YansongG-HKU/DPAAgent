#include <QtTest/QtTest>
#include "AgentConfig.h"
#include "OpenAIWire.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dpapi.h>

using namespace Model;

namespace {

class EnvironmentRestore {
public:
    explicit EnvironmentRestore(const char* name = "DPA_LLM_API_KEY")
        : _name(name),
          _wasSet(qEnvironmentVariableIsSet(_name.constData())),
          _value(qgetenv(_name.constData())) {}
    ~EnvironmentRestore() {
        if (_wasSet) qputenv(_name.constData(), _value);
        else qunsetenv(_name.constData());
        _value.fill('\0');
    }
private:
    QByteArray _name;
    bool _wasSet;
    QByteArray _value;
};

QByteArray protectForCurrentUser(const QByteArray& plaintext) {
    DATA_BLOB input;
    input.cbData = static_cast<DWORD>(plaintext.size());
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.constData()));
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"DPA AgentConfig test", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) return {};
    QByteArray encrypted(reinterpret_cast<const char*>(output.pbData), static_cast<int>(output.cbData));
    LocalFree(output.pbData);
    return encrypted.toBase64();
}

bool writeConfig(const QString& path, const QJsonObject& object) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) >= 0;
}

} // namespace

class TstAgentConfig : public QObject {
    Q_OBJECT
private slots:
    void spark_profile_reuses_authenticated_loopback_without_fallback() {
        EnvironmentRestore restoreEnvironment;
        qunsetenv("DPA_LLM_API_KEY");
        const QString path = QFINDTESTDATA("../../docs/examples/dpa-agent-spark.json");
        QVERIFY(QFile::exists(path));
        const auto profile = loadLlmConfigDetailed(path.toStdString());
        QVERIFY(profile.configErrors.empty() && profile.apiKeyError.empty());
        QCOMPARE(int(profile.routes.size()), 1);
        QVERIFY(profile.routes.front().enabled);
        QCOMPARE(profile.config.provider, std::string("openai_compat"));
        QCOMPARE(profile.config.model, std::string("qwen3.8-27b-bf16"));
        QCOMPARE(profile.config.baseUrl, std::string("http://127.0.0.1:18308/v1/chat/completions"));
        QVERIFY(profile.config.apiKey.empty() && !profile.legacyPlaintextApiKey);
        QCOMPARE(profile.config.timeoutMs, 600000);
        QVERIFY(profile.config.enableThinking.has_value() && !*profile.config.enableThinking);
        QCOMPARE(profile.budget.maxModelCalls, 8);
        QCOMPARE(profile.budget.maxInputTokens, 262144LL);
        QCOMPARE(profile.budget.maxOutputTokens, 32768LL);

        qputenv("DPA_LLM_API_KEY", "spark-profile-test-key");
        const auto authenticated = loadLlmConfigDetailed(path.toStdString());
        QVERIFY(authenticated.configErrors.empty() && authenticated.apiKeyError.empty());
        QCOMPARE(int(authenticated.apiKeySource), int(ApiKeySource::Environment));
        QCOMPARE(authenticated.config.apiKey, std::string("spark-profile-test-key"));
        QVERIFY(validateCredentialedEndpoint(authenticated.config).empty());
        QCOMPARE(resolveOpenAICompatEndpoint(authenticated.config.baseUrl), profile.config.baseUrl);
        const auto body = QByteArray::fromStdString(buildOpenAIRequestBody(
            {{"user", "Synthetic offline profile check"}}, "[]", authenticated.config));
        QCOMPARE(QJsonDocument::fromJson(body).object().value("max_tokens").toInt(), 4096);
        QVERIFY(!body.contains("spark-profile-test-key"));
        QCOMPARE(QJsonDocument::fromJson(body).object().value("chat_template_kwargs")
                     .toObject().value("enable_thinking"), QJsonValue(false));

        auto insecure = authenticated.config;
        insecure.baseUrl = "http://192.0.2.25:8308/v1/chat/completions";
        QCOMPARE(validateCredentialedEndpoint(insecure), std::string("insecure_credential_transport"));
    }

    void validates_thinking_option_without_fallback_inheritance() {
        EnvironmentRestore restoreEnvironment;
        qunsetenv("DPA_LLM_API_KEY");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("dpa_agent_llm.json");
        const QString endpoint = "http://127.0.0.1:18308/v1/chat/completions";
        QJsonObject config{{"baseUrl", endpoint}};
        QVERIFY(writeConfig(path, config));
        QVERIFY(!loadLlmConfigDetailed(path.toStdString()).config.enableThinking.has_value());

        for (bool enabled : {false, true}) {
            config["enableThinking"] = enabled;
            QVERIFY(writeConfig(path, config));
            const auto loaded = loadLlmConfigDetailed(path.toStdString());
            QVERIFY(loaded.configErrors.empty() && loaded.routes.front().enabled);
            QVERIFY(loaded.config.enableThinking.has_value());
            QCOMPARE(*loaded.config.enableThinking, enabled);
        }
        for (const auto& value : QJsonArray{QJsonValue(QJsonValue::Null), 0, "false", QJsonObject{}, QJsonArray{}}) {
            config["enableThinking"] = value;
            QVERIFY(writeConfig(path, config));
            const auto loaded = loadLlmConfigDetailed(path.toStdString());
            QVERIFY(!loaded.routes.front().enabled && !loaded.config.enableThinking.has_value());
            QCOMPARE(loaded.routes.front().disabledReason, std::string("invalid_enableThinking"));
        }
        config["enableThinking"] = false;
        for (const auto& url : {"https://example.com/v1", "https://example.com/v1/responses"}) {
            config["baseUrl"] = url;
            QVERIFY(writeConfig(path, config));
            const auto loaded = loadLlmConfigDetailed(path.toStdString());
            QVERIFY(!loaded.routes.front().enabled);
            QCOMPARE(loaded.routes.front().disabledReason, std::string("enableThinking_requires_chat_completions"));
        }
        config["baseUrl"] = endpoint;
        config["provider"] = "claude";
        QVERIFY(writeConfig(path, config));
        QVERIFY(!loadLlmConfigDetailed(path.toStdString()).routes.front().enabled);

        config["provider"] = "openai_compat";
        config["fallbacks"] = QJsonArray{QJsonObject{{"id", "other-api"},
            {"baseUrl", "https://example.com/v1/responses"}, {"allowDiagnosticData", true}}};
        QVERIFY(writeConfig(path, config));
        const auto loaded = loadLlmConfigDetailed(path.toStdString());
        QVERIFY(loaded.configErrors.empty());
        QCOMPARE(int(loaded.routes.size()), 2);
        QVERIFY(loaded.routes[1].enabled && !loaded.routes[1].config.enableThinking.has_value());
    }

    void enforces_secret_priority_and_legacy_marker() {
        EnvironmentRestore restoreEnvironment;
        qunsetenv("DPA_LLM_API_KEY");

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("dpa_agent_llm.json");
        const QByteArray dpapi = protectForCurrentUser("dpapi-test-key");
        QVERIFY(!dpapi.isEmpty());
        QVERIFY(writeConfig(path, QJsonObject{
            {"provider", "claude"},
            {"model", "test-model"},
            {"apiKeyDpapi", QString::fromLatin1(dpapi)},
            {"apiKey", "legacy-must-not-win"}
        }));

        LlmConfigLoadResult encrypted = loadLlmConfigDetailed(path.toStdString());
        QCOMPARE(encrypted.config.apiKey, std::string("dpapi-test-key"));
        QCOMPARE(encrypted.config.model, std::string("test-model"));
        QCOMPARE((int)encrypted.apiKeySource, (int)ApiKeySource::DpapiCurrentUser);
        QVERIFY(!encrypted.legacyPlaintextApiKey);
        QVERIFY(encrypted.apiKeyError.empty());
        QCOMPARE(QString::fromStdString(encrypted.configPath), QFileInfo(path).absoluteFilePath());

        qputenv("DPA_LLM_API_KEY", "environment-test-key");
        LlmConfigLoadResult environment = loadLlmConfigDetailed(path.toStdString());
        QCOMPARE(environment.config.apiKey, std::string("environment-test-key"));
        QCOMPARE((int)environment.apiKeySource, (int)ApiKeySource::Environment);
        QVERIFY(environment.apiKeyError.empty());
        qunsetenv("DPA_LLM_API_KEY");

        QVERIFY(writeConfig(path, QJsonObject{
            {"apiKeyDpapi", "not base64!"},
            {"apiKey", "legacy-must-not-fallback"}
        }));
        LlmConfigLoadResult invalid = loadLlmConfigDetailed(path.toStdString());
        QVERIFY(invalid.config.apiKey.empty());
        QCOMPARE((int)invalid.apiKeySource, (int)ApiKeySource::None);
        QCOMPARE(invalid.apiKeyError, std::string("invalid_dpapi_base64"));
        QVERIFY(!invalid.legacyPlaintextApiKey);

        QVERIFY(writeConfig(path, QJsonObject{{"apiKey", "legacy-test-key"}}));
        LlmConfigLoadResult legacy = loadLlmConfigDetailed(path.toStdString());
        QCOMPARE(legacy.config.apiKey, std::string("legacy-test-key"));
        QCOMPARE((int)legacy.apiKeySource, (int)ApiKeySource::LegacyPlaintext);
        QVERIFY(legacy.legacyPlaintextApiKey);

        QFile unchanged(path);
        QVERIFY(unchanged.open(QIODevice::ReadOnly));
        const QJsonObject stillLegacy = QJsonDocument::fromJson(unchanged.readAll()).object();
        QVERIFY(stillLegacy.contains("apiKey"));
        QVERIFY(!stillLegacy.contains("apiKeyDpapi"));
    }

    void rejects_credentials_over_remote_plaintext() {
        EnvironmentRestore restoreEnvironment;
        qunsetenv("DPA_LLM_API_KEY");

        LlmConfig direct;
        direct.apiKey = "direct-secret";
        direct.baseUrl = "http://example.com/v1/chat/completions";
        QCOMPARE(validateCredentialedEndpoint(direct),
                 std::string("insecure_credential_transport"));

        direct.baseUrl = "https://example.com/v1/chat/completions";
        QVERIFY(validateCredentialedEndpoint(direct).empty());
        direct.baseUrl = "http://localhost:11434/v1/chat/completions";
        QVERIFY(validateCredentialedEndpoint(direct).empty());
        direct.baseUrl = "http://127.0.0.1:11434/v1/chat/completions";
        QVERIFY(validateCredentialedEndpoint(direct).empty());
        direct.baseUrl = "http://[::1]:11434/v1/chat/completions";
        QVERIFY(validateCredentialedEndpoint(direct).empty());

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("dpa_agent_llm.json");
        QVERIFY(writeConfig(path, QJsonObject{
            {"baseUrl", "http://192.0.2.25/v1/chat/completions"},
            {"apiKey", "must-be-cleared"}
        }));
        const LlmConfigLoadResult loaded = loadLlmConfigDetailed(path.toStdString());
        QVERIFY(loaded.config.apiKey.empty());
        QCOMPARE(loaded.apiKeyError, std::string("insecure_credential_transport"));
        QCOMPARE((int)loaded.apiKeySource, (int)ApiKeySource::LegacyPlaintext);
    }

    void loads_authorized_fallback_routing_and_budget() {
        EnvironmentRestore primaryEnvironment;
        EnvironmentRestore fallbackEnvironment("DPA_LLM_API_KEY_AGNES_DEV");
        qunsetenv("DPA_LLM_API_KEY");
        qunsetenv("DPA_LLM_API_KEY_AGNES_DEV");

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("dpa_agent_llm.json");
        const QByteArray primaryDpapi = protectForCurrentUser("primary-test-key");
        const QByteArray fallbackDpapi = protectForCurrentUser("fallback-test-key");
        QVERIFY(!primaryDpapi.isEmpty());
        QVERIFY(!fallbackDpapi.isEmpty());

        const QJsonObject authorizedFallback{
            {"id", "agnes-dev"},
            {"provider", "openai_compat"},
            {"model", "agnes-test"},
            {"baseUrl", "https://fallback.example/v1/chat/completions"},
            {"allowDiagnosticData", true},
            {"apiKeyDpapi", QString::fromLatin1(fallbackDpapi)},
            {"inputCostMicrosPerMillionTokens", 0},
            {"outputCostMicrosPerMillionTokens", 0}
        };
        const QJsonObject blockedFallback{
            {"id", "blocked"},
            {"provider", "openai_compat"},
            {"model", "blocked-test"},
            {"baseUrl", "https://blocked.example/v1/chat/completions"}
        };
        QVERIFY(writeConfig(path, QJsonObject{
            {"provider", "openai_compat"},
            {"model", "primary-test"},
            {"baseUrl", "https://primary.example/v1/chat/completions"},
            {"apiKeyDpapi", QString::fromLatin1(primaryDpapi)},
            {"inputCostMicrosPerMillionTokens", 1000000},
            {"outputCostMicrosPerMillionTokens", 2000000},
            {"routing", QJsonObject{{"failureThreshold", 1}, {"cooldownMs", 1234}}},
            {"budget", QJsonObject{
                {"maxModelCallsPerTurn", 3},
                {"maxInputTokensPerTurn", 100},
                {"maxOutputTokensPerTurn", 50},
                {"maxEstimatedCostMicrosPerTurn", 10000}
            }},
            {"fallbacks", QJsonArray{authorizedFallback, blockedFallback}}
        }));

        const LlmConfigLoadResult loaded = loadLlmConfigDetailed(path.toStdString());
        QCOMPARE((int)loaded.routes.size(), 3);
        QVERIFY(loaded.routes[0].enabled);
        QCOMPARE(loaded.routes[0].config.apiKey, std::string("primary-test-key"));
        QCOMPARE((int)loaded.routes[0].apiKeySource, (int)ApiKeySource::DpapiCurrentUser);
        QVERIFY(loaded.routes[0].pricingConfigured);

        const LlmRouteConfig& fallback = loaded.routes[1];
        QVERIFY(fallback.enabled);
        QCOMPARE(fallback.id, std::string("agnes-dev"));
        QCOMPARE(fallback.apiKeyEnvironment, std::string("DPA_LLM_API_KEY_AGNES_DEV"));
        QCOMPARE(fallback.config.apiKey, std::string("fallback-test-key"));
        QCOMPARE((int)fallback.apiKeySource, (int)ApiKeySource::DpapiCurrentUser);
        QVERIFY(fallback.pricingConfigured);
        QCOMPARE(fallback.inputCostMicrosPerMillionTokens, 0LL);
        QCOMPARE(fallback.outputCostMicrosPerMillionTokens, 0LL);

        QVERIFY(!loaded.routes[2].enabled);
        QCOMPARE(loaded.routes[2].disabledReason,
                 std::string("diagnostic_data_not_authorized"));
        QCOMPARE((int)loaded.configErrors.size(), 1);
        QCOMPARE(loaded.configErrors[0],
                 std::string("route:blocked:diagnostic_data_not_authorized"));
        QCOMPARE(loaded.routing.failureThreshold, 1);
        QCOMPARE(loaded.routing.cooldownMs, 1234LL);
        QCOMPARE(loaded.budget.maxModelCalls, 3);
        QCOMPARE(loaded.budget.maxInputTokens, 100LL);
        QCOMPARE(loaded.budget.maxOutputTokens, 50LL);
        QCOMPARE(loaded.budget.maxEstimatedCostMicros, 10000LL);
    }
};

int runTstAgentConfig(int argc, char** argv) {
    TstAgentConfig test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_agentconfig.moc"
