#include <QtTest/QtTest>
#include <QSignalSpy>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <QTimer>
#include <QFile>
#include <QSaveFile>
#include <QDir>
#include <QUrl>
#include <QDateTime>
#include <atomic>
#include <thread>
#include "Services/ServiceHelper.h"

#include "iLLMProvider.h"
#include "ToolRegistry.h"
#include "ToolResult.h"
#include "AgentJson.h"
#include "AgentUiParse.h"
#include "ExperimentCampaign.h"
#include "iTest.h"
#include "AgentConfig.h"
#include "OpenAICompatProvider.h"
#include "OpenAIWire.h"
#include "../../Model/DataBaseService/iDataBaseService.h"
#include "../../ViewModel/ViewModelAgent/VMAgentChat.h"

#include "AgentServiceImpl.h"

using namespace Model;

namespace {
// Captures the actual provider conversation. No model response is scripted,
// repaired, or graded here; content verdicts require the retained rubric review.
class CapturedLocalProvider final : public OpenAICompatProvider {
public:
    explicit CapturedLocalProvider(const LlmConfig& cfg) : OpenAICompatProvider(cfg), config(cfg) {}
    LlmReply chat(const std::vector<LlmMessage>& messages, const std::string& tools) override {
        const auto reply = OpenAICompatProvider::chat(messages, tools);
        calls.append(QJsonObject{{"request", AgentJson::parseObject(buildOpenAIRequestBody(messages, tools, config))},
            {"reply", QJsonObject{{"kind", int(reply.kind)}, {"text", QString::fromStdString(reply.text)},
                {"tool", QString::fromStdString(reply.toolName)}, {"args", QString::fromStdString(reply.toolArgsJson)},
                {"error", QString::fromStdString(reply.error)}, {"input_tokens", reply.inputTokens},
                {"output_tokens", reply.outputTokens}, {"http_status", reply.httpStatus}}}});
        return reply;
    }
    LlmConfig config;
    QJsonArray calls;
};

// A command/state probe, not a simulator or evidence of real DAQ/DSP/storage.
class AutonomousDaqProbe final : public iTest {
public:
    AutonomousDaqProbe() { addProperty("IsConnected", &connected, TYPE_Bool); }
    bool connected = true, running = false, saving = false;
    bool startOk = true, saveOk = true, stopOk = true, saveChangesState = true;
    int starts = 0, stops = 0, saveStarts = 0, saveStops = 0, connects = 0;
    int init() override { return 1; }
    int initAfterLoad() override { return 1; }
    int close() override { return 1; }
    bool save() override { return true; }
    bool isRunning() override { return running; }
    bool isSaving() override { return saving; }
    bool isFreezing() override { return false; }
    int connectAllDevice() override { ++connects; connected = true; return 1; }
    int startDaq() override { ++starts; running = true; return startOk; }
    int stopDaq() override { ++stops; if (stopOk) running = false; return stopOk; }
    int startSave() override { ++saveStarts; saving = saveChangesState; return saveOk; }
    int stopSave() override { ++saveStops; if (stopOk) saving = false; return stopOk; }
    void setCurrent() override {}
    void sendToOperatorLog(std::string) override {}
    void setCurrentPlotSet() override {}
    void setCurrentPlotLayout(int) override {}
    void addPlotLayout() override {}
    void deletePlotLayout() override {}
    int addData(M*) override { return 0; }
    int saveData(M*) override { return 0; }
    int removeData(const std::string&) override { return 0; }
    int renameData(const std::string&, const std::string&) override { return 0; }
    M* getData(const std::string&) override { return nullptr; }
    M* createEmptyProcess(std::string) override { return nullptr; }
    M* createProcessFromCurrent(std::string) override { return nullptr; }
    M* switchCurrentProcess(std::string) override { return nullptr; }
    int excuteCurrentProcess(M*) override { return 0; }
    void onProcessDone(M*) override {}
};

class InMemoryDataBaseService final : public Model::iDataBaseService {
public:
    bool initMySQL() override { return true; }
    int checkDatabase() override { return 1; }
    int save(const std::string&, utils::Property* prop) override {
        ++saveCalls;
        if (prop && prop->value<std::string>("Type") == "MonitoringHistory") {
            monitoringId = prop->value<std::string>("Id");
            monitoringJson = prop->value<std::string>("MonitoringJson");
        }
        return saveResult;
    }
    int load(const std::string&, utils::Property* prop, std::vector<DataBaseCondition>) override {
        if (!prop || monitoringJson.empty()) return 0;
        prop->setValue("Id", monitoringId);
        prop->setValue("Type", std::string("MonitoringHistory"));
        prop->setValue("MonitoringJson", monitoringJson);
        return 1;
    }
    int loadList(const std::string&, utils::Property*, std::vector<DataBaseCondition>) override { return 0; }
    int loadInfoList(const std::string&, utils::Property*, std::vector<DataBaseCondition>) override { return 0; }
    int count(const std::string&, std::vector<DataBaseCondition>) override { return 0; }
    int rename(const std::string&, const std::string&, const std::string&) override { return 1; }
    int remove(const std::string&, std::vector<DataBaseCondition>) override { return 0; }
    int saveResult = 1;
    int saveCalls = 0;
    std::string monitoringId;
    std::string monitoringJson;
};

class RecoverableDataBaseService final : public Model::iDataBaseService {
public:
    bool initMySQL() override { return true; }
    int checkDatabase() override { return 1; }
    int save(const std::string& type, utils::Property* prop) override {
        if (type != "AgentSession" || !prop) return 0;
        id = prop->value<std::string>("Id");
        title = prop->value<std::string>("Name");
        sessionJson = prop->value<std::string>("SessionJson");
        ++saveCalls;
        return 1;
    }
    int load(const std::string& type, utils::Property* prop,
             std::vector<DataBaseCondition>) override {
        if (type != "AgentSession" || !prop || sessionJson.empty()) return 0;
        prop->setValue("Id", id);
        prop->setValue("Name", title);
        prop->setValue("Type", std::string("AgentSession"));
        prop->setValue("SessionJson", sessionJson);
        return 1;
    }
    int loadList(const std::string&, utils::Property*, std::vector<DataBaseCondition>) override { return 0; }
    int loadInfoList(const std::string&, utils::Property*, std::vector<DataBaseCondition>) override { return 0; }
    int count(const std::string&, std::vector<DataBaseCondition>) override { return 0; }
    int rename(const std::string&, const std::string&, const std::string&) override { return 1; }
    int remove(const std::string&, std::vector<DataBaseCondition>) override { return 0; }
    std::string id;
    std::string title;
    std::string sessionJson;
    int saveCalls = 0;
};

class ScopedDefaultDataBaseService final {
public:
    explicit ScopedDefaultDataBaseService(iDataBaseService* service)
        : _service(service) {
        _helper.addService(_service);
        _previousDefault = _helper.getFristOrDefaultService();
        if (_previousDefault != _service)
            _helper.setDefaultService(_service);
    }
    ~ScopedDefaultDataBaseService() {
        if (_previousDefault && _previousDefault != _service)
            _helper.setDefaultService(_previousDefault);
        _helper.removeService(_service);
    }

private:
    evaf::services::ServiceHelper<iDataBaseService> _helper;
    iDataBaseService* _service = nullptr;
    iDataBaseService* _previousDefault = nullptr;
};

class ScriptedLlmProvider : public iLLMProvider {
public:
    std::vector<LlmReply> replies;
    std::vector<LlmMessage> lastMessages;
    int calls = 0;

    LlmReply chat(const std::vector<LlmMessage>& messages, const std::string&) override {
        lastMessages = messages;
        if (replies.empty())
            return {};
        if (calls >= static_cast<int>(replies.size()))
            return replies.back();
        return replies[calls++];
    }
};

class BlockingTextProvider : public iLLMProvider {
public:
    LlmReply chat(const std::vector<LlmMessage>&, const std::string&) override {
        entered.store(true);
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return {LlmReply::Text, "stale project answer"};
    }

    std::atomic_bool entered{false};
    std::atomic_bool release{false};
};

class FakeUiAgentService final : public iAgentService {
public:
    std::string listTools() override { return "[]"; }
    std::string callTool(const std::string&, const std::string&) override { return "{}"; }
    std::string listMcpTools() override { return "[]"; }
    std::string callMcpTool(const std::string&, const std::string&) override { return "{}"; }
    std::string sendMessage(const std::string&) override { return {}; }
    void sendMessageAsync(const std::string& text) override {
        sendMessageWithAttachmentsAsync(text, {});
    }
    void sendMessageWithAttachmentsAsync(const std::string& text,
                                         const std::vector<std::string>&) override {
        ++sendCalls;
        lastMessage = text;
    }
    void cancelCurrentTurn() override {
        ++cancelCalls;
        if (synchronousCancelAck)
            emit agentStep("run_state", "cancelled", R"({"state":"cancelled"})");
    }
    bool startNewSession() override { ++newSessionCalls; return true; }
    std::string listSessions() override { ++listSessionCalls; return R"({"ok":true,"sessions":[]})"; }
    std::string resumeSession(const std::string&) override { ++resumeSessionCalls; return "{}"; }
    std::string runtimeInfo() override { return "{}"; }
    std::string acknowledgeCompanionAlarm(const std::string& args) override {
        ++acknowledgeCalls;
        lastAcknowledgeArgs = args;
        return R"({"ok":true,"acknowledged":true})";
    }
    void confirmPendingAction(const std::string& id, bool approve) override {
        ++confirmationCalls;
        lastConfirmationId = id;
        lastConfirmationApproved = approve;
    }

    int sendCalls = 0;
    int cancelCalls = 0;
    int newSessionCalls = 0;
    int listSessionCalls = 0;
    int resumeSessionCalls = 0;
    int confirmationCalls = 0;
    int acknowledgeCalls = 0;
    bool lastConfirmationApproved = false;
    bool synchronousCancelAck = false;
    std::string lastMessage;
    std::string lastConfirmationId;
    std::string lastAcknowledgeArgs;
};

class ScopedDefaultAgentService final {
public:
    explicit ScopedDefaultAgentService(iAgentService* service)
        : _service(service), _previous(_helper.getFristOrDefaultService()) {
        _helper.addService(_service);
        _helper.setDefaultService(_service);
    }
    ~ScopedDefaultAgentService() {
        if (_previous) _helper.setDefaultService(_previous);
        _helper.removeService(_service);
    }

private:
    evaf::services::ServiceHelper<iAgentService> _helper;
    iAgentService* _service = nullptr;
    iAgentService* _previous = nullptr;
};

M* makeAnalysisProcess(const std::string& runId) {
    auto* process = new M();
    process->setProperty("Process", process, utils::Property::TYPE_Class);
    auto* id = new std::string(runId);
    process->addProperty("AgentRunId", id, utils::Property::TYPE_StdString);
    return process;
}
}

class TstAgentServiceAsyncOps : public QObject {
    Q_OBJECT

private slots:
    void planning_context_and_minimum_campaign_share_production_contracts() {
        AgentServiceImpl service;
        service._planFirstEnabled = true;
        const auto context = AgentJson::parseObject(service.assembleAgentContext());
        QVERIFY(!context.value("analysis_available").toBool(true));
        QCOMPARE(context.value("context_fingerprint").toString().size(), 64);
        QVERIFY(!context.value("experiment_workflow_playbook").toString().isEmpty());
        QFile schema(QFINDTESTDATA("../../docs/experiment-objective.schema.json"));
        QVERIFY(schema.open(QIODevice::ReadOnly));
        QCOMPARE(context.value("objective_schema").toObject(), QJsonDocument::fromJson(schema.readAll()).object());

        delete service._provider;
        auto* provider = new ScriptedLlmProvider;
        provider->replies = {{LlmReply::Text, "capture"}};
        service._provider = provider;
        service.sendMessage("inspect planning context");
        QVERIFY(!provider->lastMessages.empty());
        const auto synchronous = provider->lastMessages.front().parts.front().text;
        QVERIFY(synchronous.find("objective_schema") != std::string::npos);
        QVERIFY(service.startNewSession());
        QSignalSpy replySpy(&service, &AgentServiceImpl::replyReady);
        service.sendMessageAsync("inspect planning context");
        QTRY_COMPARE_WITH_TIMEOUT(replySpy.count(), 1, 5000);
        QCOMPARE(provider->lastMessages.front().parts.front().text, synchronous);

        auto objective = AgentJson::parseObject(R"({
          "schema":"dpa.experiment.objective","version":1,"objective_id":"context-probe",
          "hypothesis":"Compare analysis settings using a bounded technical fixture",
          "response":{"metric":"rms","unit":"g","direction":"minimize"},
          "constraints":{"fixed_plan":{"duration_ms":1000},"randomize":false,"min_replicates":2},
          "allowed_factors":[{"name":"analysis_type","levels":["FFT","PSD"]},{"name":"average_num","levels":[1,4]}],
          "budget":{"max_runs":16,"max_run_duration_ms":1000,"max_total_duration_ms":16000},
          "stop_conditions":{"stop_on_warning":true,"on_run_failure":"abort","max_failed_runs":0,"interim_after":0},
          "evidence_requirements":{"save_raw_data":true,"create_per_run_evidence":true,"verify_current_catalog":true,"auto_analyze":true,"auto_refine":true}
        })");
        objective["context_fingerprint"] = context.value("context_fingerprint");
        objective["tool_catalog_sha256"] = context.value("tool_catalog_sha256");
        const auto validated = AgentJson::parseObject(service.callTool("validate_experiment_objective",
            AgentJson::dump(QJsonObject{{"objective", objective}})));
        QVERIFY2(validated.value("ok").toBool(), qPrintable(QJsonDocument(validated).toJson()));
        const auto data = validated.value("data").toObject();
        QVERIFY(!data.value("execution_authorized").toBool(true));
        const auto campaign = parseAndValidateExperimentCampaign(AgentJson::dump(data.value("minimum_campaign").toObject()));
        QVERIFY(campaign.ok);
        QCOMPARE(campaign.campaign.runPlanJsons.size(), size_t(8));
        const auto bound = AgentJson::parseObject(service.callTool("validate_campaign_against_objective",
            AgentJson::dump(QJsonObject{{"objective", data.value("objective")}, {"contract", data.value("minimum_campaign")}})));
        QVERIFY2(bound.value("ok").toBool(), qPrintable(QJsonDocument(bound).toJson()));
    }

    void live_objective_capture_production_service_data() {
        QTest::addColumn<QJsonObject>("goldenCase");
        if (qEnvironmentVariableIsEmpty("DPA_LIVE_OBJECTIVES")) {
            QTest::newRow("disabled") << QJsonObject{};
            return;
        }
        QFile file(QFINDTESTDATA("../../TestData/AgentEval/golden-evals.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto cases = QJsonDocument::fromJson(file.readAll()).object().value("cases").toArray();
        for (const auto& value : cases) {
            const auto row = value.toObject();
            if (row.value("category").toString() == "objective_planning")
                QTest::newRow(row.value("id").toString().toUtf8().constData()) << row;
        }
    }

    void live_objective_capture_production_service() {
        QFETCH(QJsonObject, goldenCase);
        if (goldenCase.isEmpty()) QSKIP("opt-in local model capture; not a domain/expert verdict");
        const QString configPath = qEnvironmentVariable("DPA_LIVE_CONFIG");
        QVERIFY(!configPath.isEmpty());
        QVERIFY(QFile::exists(configPath));
        const auto loaded = loadLlmConfigDetailed(configPath.toStdString());
        const auto& cfg = loaded.config;
        QVERIFY(loaded.apiKeyError.empty() && loaded.configErrors.empty());
        QCOMPARE(int(loaded.routes.size()), 1); // no implicit cloud fallback in captures
        QVERIFY(loaded.routes.front().enabled);
        QVERIFY(cfg.provider == "openai_compat" && !loaded.legacyPlaintextApiKey);
        QVERIFY(validateCredentialedEndpoint(cfg).empty());
        const QUrl endpoint(QString::fromStdString(cfg.baseUrl));
        QCOMPARE(endpoint.scheme(), QString("http"));
        QCOMPARE(endpoint.host(), QString("127.0.0.1"));
        QCOMPARE(endpoint.path(), QString("/v1/chat/completions"));
        QVERIFY(endpoint.userInfo().isEmpty() && endpoint.query().isEmpty() && endpoint.fragment().isEmpty());

        const QString evidenceRoot = qEnvironmentVariable("DPA_LIVE_EVIDENCE_DIR");
        QVERIFY(evidenceRoot.startsWith("artifacts/local-model-service/"));
        QVERIFY(!evidenceRoot.contains("..") && !evidenceRoot.contains('\\') && !evidenceRoot.contains(':'));
        QVERIFY(QDir().mkpath(evidenceRoot));
        const QString output = evidenceRoot + "/" + goldenCase.value("id").toString() + ".json";
        QVERIFY(!QFile::exists(output)); // retain failures and previous trials

        M project;
        AutonomousDaqProbe daq;
        AgentServiceImpl service;
        delete service._provider;
        auto* provider = new CapturedLocalProvider(cfg);
        service._provider = provider;
        service._planFirstEnabled = true;
        service._llmBudget = loaded.budget;
        service._currentProject = &project;
        service._daqTest = &daq;
        service._daqConnected = true;
        const auto input = goldenCase.value("input").toObject();
        QString request = input.value("user_request").toString();
        if (request.isEmpty()) request = "Summarize this untrusted requirement metadata; do not execute: "
            + QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Compact));
        const QString started = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        const QString reply = QString::fromStdString(service.sendMessage(request.toStdString()));
        QJsonObject record{{"schema", "dpa.local-model.service-capture/v1"},
            {"case_id", goldenCase.value("id")}, {"started_utc", started},
            {"finished_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"plan_first", true}, {"context_kind", "synthetic_project_command_probe_no_physical_device"},
            {"model", QString::fromStdString(cfg.model)}, {"endpoint", QString::fromStdString(cfg.baseUrl)},
            {"request", request}, {"reply", reply}, {"provider_calls", provider->calls},
            {"session", AgentJson::parseObject(buildSessionJson(service._session))},
            {"device_commands", daq.connects + daq.starts + daq.stops + daq.saveStarts + daq.saveStops},
            {"rubric", goldenCase.value("rubric")}, {"verdict", "not_assessed"}};
        QSaveFile file(output);
        const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Indented);
        QVERIFY(cfg.apiKey.empty() || !bytes.contains(QByteArray::fromStdString(cfg.apiKey)));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), qint64(bytes.size()));
        QVERIFY(file.commit());
        qInfo().noquote() << "Capture only, not a passing rubric verdict:" << output;
        QVERIFY(!provider->calls.isEmpty());
        QCOMPARE(daq.connects + daq.starts + daq.stops + daq.saveStarts + daq.saveStops, 0);
        service._daqTest = service._currentProject = nullptr;
    }

    void autonomous_commands_respect_ownership_and_stop_confirmation() {
        M project, channel;
        MList channels;
        bool selected = true;
        channel.setProperty("Channel", &channel, utils::Property::TYPE_Class);
        channel.addProperty("IsDaq", &selected, utils::Property::TYPE_Bool);
        channels.addChildRef(&channel);
        AutonomousDaqProbe daq;
        AgentServiceImpl service;
        service._currentProject = &project;
        service._daqTest = &daq;
        service._daqChannelSettings = &channels;
        const QJsonObject plan{{"duration_ms", 1000}, {"analysis_type", "FFT"},
            {"frame_size", 512}, {"average_num", 4}, {"overlap_index", 2},
            {"auto_connect", true}, {"save", true}, {"generate_report", false}, {"create_evidence", false}};

        for (bool saving : {false, true}) {
            daq.running = !saving; daq.saving = saving;
            // Bound fields may lag the live interface; preflight must refresh.
            service._daqRunning = service._daqSaving = false;
            const auto result = service.runAutonomousExperiment(plan);
            QCOMPARE(QString::fromStdString(result.error), QString("acquisition_already_active"));
            QCOMPARE(daq.connects + daq.starts + daq.stops + daq.saveStarts + daq.saveStops, 0);
        }
        daq.running = daq.saving = false;
        daq.startOk = false; // partial start despite a failure return
        auto result = service.runAutonomousExperiment(plan);
        QCOMPARE(QString::fromStdString(result.error), QString("daq_start_failed"));
        QCOMPARE(daq.starts, 1); QCOMPARE(daq.stops, 1);
        QVERIFY(!daq.running);

        daq.startOk = true; daq.saveChangesState = false;
        result = service.runAutonomousExperiment(plan);
        QCOMPARE(QString::fromStdString(result.error), QString("storage_start_not_confirmed"));
        QVERIFY(!daq.running);
        daq.saveChangesState = true; daq.saveOk = false;
        result = service.runAutonomousExperiment(plan);
        QCOMPARE(QString::fromStdString(result.error), QString("storage_start_failed"));
        QVERIFY(!daq.running && !daq.saving);
        QVERIFY(daq.saveStops > 0);

        daq.stopOk = false;
        result = service.runAutonomousExperiment(plan);
        QCOMPARE(QString::fromStdString(result.error), QString("acquisition_stop_not_confirmed"));
        QVERIFY(!AgentJson::parseObject(result.dataJson).value("owned_acquisition_stopped").toBool(true));
        QVERIFY(daq.running && daq.saving);
        QVERIFY(service.companionEnabled()); // keep monitoring an unconfirmed stop
        daq.running = daq.saving = false;
        daq.stopOk = daq.saveOk = true;

        QJsonObject analysisArgs;
        AgentTool analysis{"run_analysis", "Command mapping probe",
            R"({"type":"object","additionalProperties":false,"properties":{"type":{"type":"string"},"frameSize":{"type":"integer"},"averageNum":{"type":"integer"},"overlapIndex":{"type":"integer"}},"required":["type","frameSize","averageNum","overlapIndex"]})",
            [&](const std::string& args) { analysisArgs = AgentJson::parseObject(args);
                ToolResult r; r.ok = false; r.error = "analysis_probe_only"; return r; }};
        analysis.dangerLevel = "read"; // no analysis or device action in this probe
        QVERIFY(service._registry.add(analysis));
        service.beginTurnContext();
        result = service.runAutonomousExperiment(plan);
        QCOMPARE(QString::fromStdString(result.error), QString("analysis_probe_only"));
        QCOMPARE(analysisArgs.value("frameSize").toInt(), 512);
        QCOMPARE(analysisArgs.value("averageNum").toInt(), 4);
        QCOMPARE(analysisArgs.value("overlapIndex").toInt(), 2);
        QVERIFY(!daq.running && !daq.saving);
        service._companionSampleIntervalMs = 100;
        QTimer::singleShot(50, &service, [&]() { service._daqTest = nullptr; });
        result = service.runAutonomousExperiment(plan);
        QCOMPARE(QString::fromStdString(result.error), QString("acquisition_context_changed"));
        QVERIFY(!daq.running && !daq.saving);
        service.clearTurnContext();
        service._daqTest = service._currentProject = service._daqChannelSettings = nullptr;
    }

    void campaign_run_outcome_reads_tool_data_envelope() {
        ToolResult result;
        result.dataJson = R"JSON({"run_id":"run-1","evidence":{"ok":true,"data":{"artifactId":"evidence-1"}}})JSON";
        const CampaignRunOutcome outcome =
            AgentServiceImpl::campaignRunOutcomeFromResult(result);
        QVERIFY(outcome.ok);
        QCOMPARE(QString::fromStdString(outcome.runId), QString("run-1"));
        QCOMPARE(QString::fromStdString(outcome.evidenceArtifactId),
                 QString("evidence-1"));

        result.ok = false;
        result.error = "daq_start_failed";
        const CampaignRunOutcome failed =
            AgentServiceImpl::campaignRunOutcomeFromResult(result);
        QVERIFY(!failed.ok);
        QCOMPARE(QString::fromStdString(failed.errorCode),
                 QString("daq_start_failed"));
    }

    void completed_response_campaign_is_postprocessed_from_verified_evidence() {
        InMemoryDataBaseService stubDb;
        ScopedDefaultDataBaseService dbService(&stubDb);
        AgentServiceImpl service;
        service._session.id = "campaign-postprocess-session";

        const CampaignValidation validation = parseAndValidateExperimentCampaign(R"JSON({
          "schema":"dpa.experiment.campaign","version":1,
          "campaign_id":"camp-postprocess",
          "hypothesis":"analysis_type 与 average_num 共同影响响应幅值",
          "factors":[{"name":"analysis_type","levels":["FFT","PSD"]},
                     {"name":"average_num","levels":[1,4]}],
          "replicates":2,
          "randomization":{"enabled":false,"seed":42},
          "response":{"metric":"rms","unit":"g","direction":"minimize"},
          "run_template":{"duration_ms":1000,"frame_size":1024},
          "policy":{"on_run_failure":"abort","max_runs":16}
        })JSON");
        QVERIFY2(validation.ok, qPrintable(QString::fromStdString(validation.error)));

        CampaignExecution execution;
        execution.ok = true;
        execution.state.campaignSha256 = validation.campaign.campaignSha256;
        execution.state.status = "completed";
        execution.state.nextRunIndex = 8;
        for (int index = 0; index < 8; ++index) {
            const ToolResult evidence =
                service._registry.call("experiment_evidence_create", "{}");
            QVERIFY2(evidence.ok, qPrintable(QString::fromStdString(evidence.error)));
            const QJsonObject evidenceData = AgentJson::parseObject(evidence.dataJson);
            CampaignRunRecord record;
            record.runIndex = index;
            record.designIndex = validation.campaign.designIndices.at(
                static_cast<size_t>(index));
            record.ok = true;
            record.runId = "run-" + std::to_string(index + 1);
            record.evidenceArtifactId =
                evidenceData.value("artifactId").toString().toStdString();
            record.responsePresent = true;
            record.responseValue = 10.0 + index;
            record.responseChannelId = 7;
            record.responseSampleCount = 100;
            record.responseUnit = "g";
            record.responseSource = "monitoring_window_mean";
            record.responseEvidenceSha256 =
                evidenceData.value("payloadSha256").toString().toStdString();
            execution.state.records.push_back(std::move(record));
        }

        const ToolResult result =
            service.finishCampaignResult(validation.campaign, execution);
        QVERIFY2(result.ok, qPrintable(QString::fromStdString(result.error)));
        QCOMPARE(result.artifactRefs.size(), size_t(3));
        const QJsonObject postprocess = AgentJson::parseObject(result.dataJson)
                                            .value("postprocess").toObject();
        QVERIFY(postprocess.value("complete").toBool());
        const QJsonObject analysis = postprocess.value("analysis").toObject();
        QVERIFY(analysis.value("ok").toBool());
        QCOMPARE(analysis.value("data").toObject()
                     .value("evidence_binding").toObject()
                     .value("observation_source").toString(),
                 QString("campaign_evidence"));
        QVERIFY(postprocess.value("refinement").toObject().value("ok").toBool());

        execution.state.records.front().responseEvidenceSha256 = std::string(64, '0');
        const ToolResult tampered =
            service.finishCampaignResult(validation.campaign, execution);
        QVERIFY(tampered.ok);
        QCOMPARE(tampered.artifactRefs.size(), size_t(1));
        const QJsonObject rejected = AgentJson::parseObject(tampered.dataJson)
                                         .value("postprocess").toObject();
        QVERIFY(!rejected.value("complete").toBool());
        QCOMPARE(rejected.value("analysis").toObject().value("error").toString(),
                 QString("response_evidence_mismatch"));
        QCOMPARE(rejected.value("refinement").toObject().value("error").toString(),
                 QString("analysis_not_available"));
    }

    void diagnosis_card_feedback_contract_reaches_the_audit_service() {
        InMemoryDataBaseService stubDb;
        ScopedDefaultDataBaseService dbService(&stubDb);
        AgentServiceImpl service;
        service._session.id = "feedback-session";

        const QJsonObject result = AgentJson::parseObject(service.recordOperatorFeedback(
            buildDiagnosticFeedbackJson("needs_more_information", "imbalance", "warn",
                                        "rotor.imbalance.1x_dominant", {"evidence-1"})));
        QVERIFY(result.value("ok").toBool());
        QCOMPARE(result.value("data").toObject().value("feedbackType").toString(),
                 QString("needs_more_information"));
        QCOMPARE(result.value("data").toObject().value("severity").toString(),
                 QString("concern"));
        QCOMPARE(service._feedbackEvents.size(), size_t(1));
        QCOMPARE(QString::fromStdString(service._feedbackEvents.front().correlationId),
                 QString("rotor.imbalance.1x_dominant"));
        QCOMPARE(QString::fromStdString(service._feedbackEvents.front().reviewStatus),
                 QString("pending"));

        const QString feedbackId = result.value("data").toObject().value("feedbackId").toString();
        stubDb.saveResult = 0;
        const ToolResult reviewPersistFailed = service.reviewOperatorFeedbackCore(
            QString(R"({"feedbackId":"%1","decision":"accept_for_offline_evaluation","reviewer":"reviewer-1","note":"queue for locked offline evaluation"})")
                .arg(feedbackId).toStdString());
        QCOMPARE(QString::fromStdString(reviewPersistFailed.error),
                 QString("feedback_review_persist_failed"));
        QCOMPARE(QString::fromStdString(service._feedbackEvents.front().reviewStatus),
                 QString("pending"));

        stubDb.saveResult = 1;
        const ToolResult reviewed = service.reviewOperatorFeedbackCore(
            QString(R"({"feedbackId":"%1","decision":"accept_for_offline_evaluation","reviewer":"reviewer-1","note":"queue for locked offline evaluation"})")
                .arg(feedbackId).toStdString());
        QVERIFY2(reviewed.ok, qPrintable(QString::fromStdString(reviewed.error)));
        QCOMPARE(QString::fromStdString(service._feedbackEvents.front().reviewStatus),
                 QString("reviewed"));
        const QJsonObject feedbackStats = AgentJson::parseObject(
            service.aggregateOperatorFeedback("{}").dataJson);
        QCOMPARE(feedbackStats.value("pending").toArray().size(), 0);
        QCOMPARE(feedbackStats.value("by_review_decision").toObject()
                     .value("accept_for_offline_evaluation").toInt(), 1);
        QCOMPARE(QString::fromStdString(service.reviewOperatorFeedbackCore(
                     QString(R"({"feedbackId":"%1","decision":"reject","reviewer":"reviewer-2","note":"overwrite attempt"})")
                         .arg(feedbackId).toStdString()).error),
                 QString("feedback_already_reviewed"));

        stubDb.saveResult = 0;
        const QJsonObject rejected = AgentJson::parseObject(service.recordOperatorFeedback(
            buildDiagnosticFeedbackJson("correct", "imbalance", "warn",
                                        "rotor.imbalance.1x_dominant")));
        QCOMPARE(rejected.value("error").toString(), QString("feedback_persist_failed"));
        QCOMPARE(service._feedbackEvents.size(), size_t(1));
    }

    void operator_feedback_is_restored_and_scoped_to_its_session() {
        RecoverableDataBaseService stubDb;
        ScopedDefaultDataBaseService dbService(&stubDb);
        AgentServiceImpl service;
        service._session.id = "feedback-resume-1";
        service._session.title = "Feedback resume";

        QVERIFY(AgentJson::parseObject(service.recordOperatorFeedback(
            buildDiagnosticFeedbackJson("correct", "misalignment", "warn",
                                        "rotor.misalignment.2x_elevated")))
                    .value("ok").toBool());
        const QString feedbackId = QString::fromStdString(service._feedbackEvents.front().id);
        QVERIFY(service.reviewOperatorFeedbackCore(
            QString(R"({"feedbackId":"%1","decision":"reject","reviewer":"reviewer","note":"not actionable"})")
                .arg(feedbackId).toStdString()).ok);
        QCOMPARE(AgentJson::parseObject(service.aggregateOperatorFeedback("{}").dataJson)
                     .value("total").toInt(), 1);
        QCOMPARE(QString::fromStdString(parseSessionJson(stubDb.sessionJson).id),
                 QString("feedback-resume-1"));
        QVERIFY(service.startNewSession());
        QCOMPARE(AgentJson::parseObject(service.aggregateOperatorFeedback("{}").dataJson)
                     .value("total").toInt(), 0);
        QCOMPARE(QString::fromStdString(parseSessionJson(stubDb.sessionJson).id),
                 QString("feedback-resume-1"));

        const QJsonObject resumed = AgentJson::parseObject(service.resumeSession("feedback-resume-1"));
        const QByteArray resumedJson = QByteArray::fromStdString(AgentJson::dump(resumed));
        QVERIFY2(resumed.value("ok").toBool(), resumedJson.constData());
        QCOMPARE(AgentJson::parseObject(service.aggregateOperatorFeedback("{}").dataJson)
                     .value("total").toInt(), 1);
        QCOMPARE(QString::fromStdString(service._feedbackEvents.front().feedbackType),
                 QString("correct"));
        QCOMPARE(QString::fromStdString(service._feedbackEvents.front().reviewStatus),
                 QString("reviewed"));
        QCOMPARE(QString::fromStdString(service._feedbackEvents.front().reviewDecision),
                 QString("reject"));
    }

    void evidence_tools_are_registered_and_public_write_is_guarded() {
        InMemoryDataBaseService stubDb;
        ScopedDefaultDataBaseService dbService(&stubDb);
        AgentServiceImpl service;

        const QJsonObject create = AgentJson::parseObject(
            service.callTool("experiment_evidence_create", "{}"));
        QCOMPARE(create.value("error").toString(), QString("requires_in_app_confirmation"));

        const QJsonObject verify = AgentJson::parseObject(service.callTool(
            "experiment_evidence_verify", R"({"artifactId":"missing.json"})"));
        QCOMPARE(verify.value("error").toString(), QString("artifact_not_found"));
    }

    void autonomous_plan_rejects_untyped_diagnostic_fields() {
        InMemoryDataBaseService stubDb;
        ScopedDefaultDataBaseService dbService(&stubDb);
        AgentServiceImpl service;

        const QJsonArray tools = QJsonDocument::fromJson(
            QByteArray::fromStdString(service.listTools())).array();
        QJsonObject executeTool;
        for (const QJsonValue& value : tools) {
            if (value.toObject().value("name").toString()
                == QString("execute_autonomous_experiment")) {
                executeTool = value.toObject();
                break;
            }
        }
        QVERIFY(!executeTool.isEmpty());
        const QJsonObject planSchema = executeTool.value("input_schema").toObject()
            .value("properties").toObject().value("plan").toObject();
        const QJsonObject diagnosticSchema = planSchema.value("properties").toObject()
            .value("diagnostic").toObject();
        QCOMPARE(diagnosticSchema.value("additionalProperties").toBool(), false);
        QVERIFY(diagnosticSchema.value("properties").toObject().contains("asset"));
    }

    void campaign_execution_requires_a_governed_objective() {
        InMemoryDataBaseService stubDb;
        ScopedDefaultDataBaseService dbService(&stubDb);
        AgentServiceImpl service;

        const QJsonArray tools = QJsonDocument::fromJson(
            QByteArray::fromStdString(service.listTools())).array();
        QSet<QString> names;
        QJsonObject executeSchema;
        for (const QJsonValue& value : tools) {
            const QJsonObject tool = value.toObject();
            const QString name = tool.value("name").toString();
            names.insert(name);
            if (name == QStringLiteral("execute_autonomous_experiment_campaign"))
                executeSchema = tool.value("input_schema").toObject();
        }
        QVERIFY(names.contains(QStringLiteral("validate_experiment_objective")));
        QVERIFY(names.contains(QStringLiteral("validate_campaign_against_objective")));
        QCOMPARE(executeSchema.value("additionalProperties").toBool(), false);
        const QJsonArray required = executeSchema.value("required").toArray();
        QVERIFY(required.contains(QStringLiteral("objective")));
        QVERIFY(required.contains(QStringLiteral("contract")));
    }

    void standards_tools_are_read_only_and_context_bound() {
        InMemoryDataBaseService stubDb;
        ScopedDefaultDataBaseService dbService(&stubDb);
        AgentServiceImpl service;

        const QJsonObject search = AgentJson::parseObject(
            service.callTool("search_standard_catalog", R"({"query":"MIL-STD"})"));
        QVERIFY(search.value("ok").toBool());
        QVERIFY(search.value("data").toObject().value("count").toInt() >= 4);

        const QJsonObject applicability = AgentJson::parseObject(service.callTool(
            "assess_standard_applicability",
            R"({"standardProfile":"iso-20816-3:2022","machineType":"centrifugal_pump","ratedPowerKw":75,"rpm":1800,"platform":"industrial","testPurpose":"operational_vibration","physicalQuantity":"vibration_velocity","applicabilityBasis":"domain test scope"})"));
        QVERIFY(applicability.value("ok").toBool());
        QCOMPARE(applicability.value("data").toObject().value("disposition").toString(),
                 QString("review_required"));
        QVERIFY(!applicability.value("data").toObject().value("execution_ready").toBool());

        const QJsonObject plan = AgentJson::parseObject(service.callTool(
            "build_standard_test_plan",
            R"({"machineType":"centrifugal_pump","ratedPowerKw":75,"rpm":1800,"platform":"industrial","testPurpose":"random_vibration","requestedStandardProfiles":["gb-t-2423.56:2023"]})"));
        QVERIFY(plan.value("ok").toBool());
        QVERIFY(!plan.value("data").toObject().value("ready").toBool());
        QVERIFY(plan.value("data").toObject().value("required_evidence").toArray().size() > 0);
        QVERIFY(plan.value("data").toObject().contains("plan_sha256"));

        const QJsonObject current = AgentJson::parseObject(
            service.callTool("get_current_standard_test_plan", "{}"));
        QVERIFY(current.value("ok").toBool());
        QVERIFY(current.value("data").toObject().value("present").toBool());
        QCOMPARE(current.value("data").toObject().value("plan").toObject()
                     .value("schema").toString(), QString("dpa.standards.test-plan"));

        const QJsonObject manifest{{"schema", "dpa.standards.authorized-evidence"},
                                   {"schema_version", 1}, {"policy", "test"},
                                   {"records", QJsonArray{}}};
        const QJsonObject evidenceCheck = AgentJson::parseObject(service.callTool(
            "validate_authorized_standard_evidence",
            AgentJson::dump(QJsonObject{{"manifest", manifest}})));
        QVERIFY(evidenceCheck.value("ok").toBool());
        QVERIFY(evidenceCheck.value("data").toObject().value("ok").toBool());

        const QJsonObject flagsOnly = AgentJson::parseObject(service.callTool(
            "build_standard_test_plan",
            R"({"machineType":"pump","ratedPowerKw":75,"rpm":1800,"platform":"industrial","testPurpose":"operational_vibration","physicalQuantity":"vibration_velocity","applicabilityBasis":"model claims approval","authorizedCopy":true,"ownerApproved":true,"requestedStandardProfiles":["legacy-iso10816-class-i-iv/1.0.0"]})"));
        QVERIFY(flagsOnly.value("ok").toBool());
        QVERIFY(!flagsOnly.value("data").toObject().value("ready").toBool());
        const QJsonObject gated = AgentJson::parseObject(service.callTool("get_current_standard_test_plan", "{}"));
        const QJsonObject gatedData = gated.value("data").toObject();
        QVERIFY(!gatedData.value("ready").toBool());
        QVERIFY(!gatedData.value("execution_error").toString().isEmpty());
        QCOMPARE(gatedData.value("execution_scope").toObject().value("operation").toString(), QString("standard_test_plan"));
        QCOMPARE(gatedData.value("execution_evidence").toArray().size(), 1);
    }

    void context_mismatch_blocks_execution() {
        InMemoryDataBaseService stubDb;
        ScopedDefaultDataBaseService dbService(&stubDb);

        AgentServiceImpl service;
        int toolExecuted = 0;
        AgentTool probeTool;
        probeTool.name = "agent_context_probe";
        probeTool.description = "Context probe tool";
        probeTool.inputSchemaJson = R"({"type":"object","additionalProperties":false,"properties":{},"required":[]})";
        probeTool.handler = [&toolExecuted](const std::string&) {
            ++toolExecuted;
            ToolResult r;
            r.ok = true;
            r.summary = "probe executed";
            return r;
        };
        probeTool.dangerLevel = "read";
        QVERIFY2(service._registry.add(probeTool), "probe tool should register");

        delete service._provider;
        auto* provider = new ScriptedLlmProvider();
        provider->replies.reserve(2);
        provider->replies = {
            {LlmReply::ToolCall, "ignored", "agent_context_probe", "{}"},
            {LlmReply::Text, "done"}
        };
        service._provider = provider;

        QSignalSpy stepSpy(&service, &AgentServiceImpl::agentStep);
        service.sendMessageWithAttachmentsAsync("run context probe", {});
        service._turnContextFingerprint = "stale-context";

        QVERIFY2(stepSpy.wait(6000), "awaiting model step");
        for (int i = 0; i < 120 && stepSpy.count() < 3; ++i) QTest::qWait(100);
        bool foundMismatch = false;
        for (int i = 0; i < stepSpy.count(); ++i) {
            if (stepSpy[i].value(0).toString() == "tool_result") {
                const QJsonObject result = AgentJson::parseObject(stepSpy[i].value(2).toString().toStdString());
                if (result.value("error").toString() == QString("context_changed")) {
                    foundMismatch = true;
                }
            }
        }
        QCOMPARE(foundMismatch, true);
        QCOMPARE(toolExecuted, 0);
    }

    void context_change_discards_async_text_reply() {
        InMemoryDataBaseService stubDb;
        ScopedDefaultDataBaseService dbService(&stubDb);
        AgentServiceImpl service;
        delete service._provider;
        auto* provider = new BlockingTextProvider();
        service._provider = provider;
        service._currentProject = new M();
        service._dpaTest = new M();

        QSignalSpy stepSpy(&service, &AgentServiceImpl::agentStep);
        QSignalSpy replySpy(&service, &AgentServiceImpl::replyReady);
        service.sendMessageWithAttachmentsAsync("describe current project", {});
        for (int i = 0; i < 200 && !provider->entered.load(); ++i) QTest::qWait(5);
        QVERIFY2(provider->entered.load(), "provider should be generating the old-context reply");

        service._currentProject = new M();
        provider->release.store(true);
        if (replySpy.isEmpty()) QVERIFY2(replySpy.wait(4000), "awaiting guarded final reply");

        const QString delivered = replySpy[0][0].toString();
        QVERIFY(!delivered.contains(QStringLiteral("stale project answer")));
        QVERIFY(delivered.contains(QString::fromUtf8("上下文已变化")));
        QVERIFY(!service._session.turns.empty());
        QCOMPARE(QString::fromStdString(service._session.turns.back().content), delivered);
        QCOMPARE(service._turnRunning.load(), false);
        QCOMPARE(service._turnContextFingerprint.empty(), true);

        bool sawContextReject = false;
        for (const auto& step : std::as_const(stepSpy))
            sawContextReject = sawContextReject
                || (step[0].toString() == QStringLiteral("turn_rejected")
                    && step[1].toString() == QStringLiteral("context_changed"));
        QCOMPARE(sawContextReject, true);
    }

    void confirmation_context_mismatch_keeps_safe() {
        InMemoryDataBaseService stubDb;
        ScopedDefaultDataBaseService dbService(&stubDb);

        AgentServiceImpl service;
        delete service._provider;
        auto* provider = new ScriptedLlmProvider();
        provider->replies = {};
        service._provider = provider;

        int toolExecuted = 0;
        AgentTool probeTool;
        probeTool.name = "agent_confirmation_probe";
        probeTool.description = "Confirmation probe tool";
        probeTool.inputSchemaJson = R"({"type":"object","additionalProperties":false,"properties":{},"required":[]})";
        probeTool.handler = [&toolExecuted](const std::string&) {
            ++toolExecuted;
            ToolResult r;
            r.ok = true;
            r.summary = "probe executed";
            return r;
        };
        probeTool.dangerLevel = "write";
        QVERIFY2(service._registry.add(probeTool), "probe tool should register");

        QSignalSpy confirmationSpy(&service, &AgentServiceImpl::confirmationRequired);
        QSignalSpy stepSpy(&service, &AgentServiceImpl::agentStep);
        QSignalSpy replySpy(&service, &AgentServiceImpl::replyReady);
        service._provider = provider;

        provider->replies = {
            {LlmReply::ToolCall, "probe-run", "agent_confirmation_probe", "{}"},
            {LlmReply::Text, "done"}
        };
        service.sendMessageWithAttachmentsAsync("run write probe", {});
        QVERIFY2(confirmationSpy.wait(4000), "awaiting confirmation event");
        QCOMPARE(confirmationSpy.count(), 1);
        const QString confirmationId = confirmationSpy[0][0].toString();

        bool mismatchInjected = false;
        for (int i = 0; i < 50 && !mismatchInjected; ++i) {
            if (service._confirmMutex.tryLock()) {
                service._pendingConfirmationTool = "different_tool";
                service._confirmMutex.unlock();
                mismatchInjected = true;
            } else {
                QTest::qWait(10);
            }
        }
        QVERIFY2(mismatchInjected, "could not inject pending confirmation tool mismatch");
        service.confirmPendingAction(confirmationId.toStdString(), true);
        if (replySpy.isEmpty())
            QVERIFY2(replySpy.wait(6000), "awaiting final reply after confirmation");
        for (int i = 0; i < 120 && stepSpy.count() < 2; ++i) QTest::qWait(50);
        bool foundMismatch = false;
        bool sawToolResult = false;
        QStringList traces;
        for (int i = 0; i < stepSpy.count(); ++i) {
            const QString kind = stepSpy[i].value(0).toString();
            const QString name = stepSpy[i].value(1).toString();
            const QString detail = stepSpy[i].value(2).toString();
            traces << QString("step%1 kind=%2 name=%3 detail=%4")
                             .arg(i)
                             .arg(kind)
                             .arg(name)
                             .arg(detail);
            if (kind == "tool_result") {
                sawToolResult = true;
                const QJsonObject result = AgentJson::parseObject(stepSpy[i].value(2).toString().toStdString());
                if (result.value("error").toString() == QString("confirmation_context_mismatch")
                    || detail.contains("confirmation_context_mismatch")) {
                    foundMismatch = true;
                }
            }
        }
        if (!foundMismatch) {
            QWARN(qPrintable(QString("sawToolResult=%1").arg(sawToolResult ? "true" : "false")));
            for (const QString& trace : traces) {
                QWARN(qPrintable(trace));
            }
        }
        bool mismatchSignaled = foundMismatch;
        if (!mismatchSignaled) {
            if (replySpy.isEmpty())
                QVERIFY2(replySpy.wait(4000), "awaiting final reply after confirmation");
            if (!replySpy.isEmpty()) {
                const QString finalReply = replySpy.last()[0].toString();
                mismatchSignaled = finalReply.contains("confirmation_context_mismatch");
                if (!mismatchSignaled) {
                    QWARN(qPrintable(QString("final reply: %1").arg(finalReply)));
                }
            }
            if (!mismatchSignaled)
                QWARN("no confirmation_context_mismatch found in tool_result or final reply");
        }
        QCOMPARE(mismatchSignaled, true);
        QCOMPARE(toolExecuted, 0);
    }

    void write_confirmation_is_one_time() {
        InMemoryDataBaseService stubDb;
        ScopedDefaultDataBaseService dbService(&stubDb);

        AgentServiceImpl service;
        delete service._provider;
        auto* provider = new ScriptedLlmProvider();
        provider->replies.reserve(2);
        LlmReply call;
        call.kind = LlmReply::ToolCall;
        call.toolCallId = "probe-1";
        call.toolName = "agent_confirmation_probe";
        call.toolArgsJson = "{}";
        provider->replies = {call, LlmReply{LlmReply::Text, "done"}};
        service._provider = provider;

        int toolExecuted = 0;
        AgentTool probeTool;
        probeTool.name = "agent_confirmation_probe";
        probeTool.description = "Test probe tool";
        probeTool.inputSchemaJson = R"({"type":"object","additionalProperties":false,"properties":{},"required":[]})";
        probeTool.handler = [&toolExecuted](const std::string&) {
            ++toolExecuted;
            ToolResult r;
            r.ok = true;
            r.summary = "probe executed";
            return r;
        };
        probeTool.dangerLevel = "write";
        QVERIFY2(service._registry.add(probeTool), "probe tool should register");

        QSignalSpy confirmationSpy(&service, &AgentServiceImpl::confirmationRequired);
        QSignalSpy replySpy(&service, &AgentServiceImpl::replyReady);

        service.sendMessageWithAttachmentsAsync("run probe once", {});
        QVERIFY2(confirmationSpy.wait(4000), "awaiting confirmation event");
        QCOMPARE(confirmationSpy.count(), 1);

        QString confirmationId = confirmationSpy[0][0].toString();
        service.confirmPendingAction(confirmationId.toStdString(), true);
        QVERIFY2(replySpy.wait(4000), "awaiting final reply");
        QCOMPARE(toolExecuted, 1);
        QVERIFY(service._session.pendingWriteJson.empty());
        QVERIFY(stubDb.saveCalls >= 3);

        service.confirmPendingAction(confirmationId.toStdString(), true);
        QCOMPARE(toolExecuted, 1);
    }

    void expired_confirmation_never_executes_write() {
        InMemoryDataBaseService stubDb;
        ScopedDefaultDataBaseService dbService(&stubDb);
        AgentServiceImpl service;
        delete service._provider;
        auto* provider = new ScriptedLlmProvider();
        provider->replies = {
            {LlmReply::ToolCall, "probe-expired", "agent_confirmation_probe", "{}"},
            {LlmReply::Text, "done"}
        };
        service._provider = provider;

        int toolExecuted = 0;
        AgentTool probeTool{"agent_confirmation_probe", "Expiration probe", "{}",
            [&toolExecuted](const std::string&) { ++toolExecuted; return ToolResult{}; }};
        probeTool.dangerLevel = "write";
        QVERIFY(service._registry.add(probeTool));
        QSignalSpy confirmationSpy(&service, &AgentServiceImpl::confirmationRequired);
        QSignalSpy replySpy(&service, &AgentServiceImpl::replyReady);
        service.sendMessageWithAttachmentsAsync("run expired probe", {});
        QVERIFY2(confirmationSpy.wait(4000), "awaiting expiration confirmation event");
        {
            QMutexLocker lock(&service._confirmMutex);
            service._pendingConfirmationExpiresAtMs = QDateTime::currentMSecsSinceEpoch() - 1;
        }
        service.confirmPendingAction(confirmationSpy[0][0].toString().toStdString(), true);
        QVERIFY2(replySpy.wait(4000), "awaiting final reply after expiration");
        QCOMPARE(toolExecuted, 0);
        bool foundExpired = false;
        for (const auto& turn : service._session.turns)
            foundExpired = foundExpired || turn.content.find("confirmation_expired") != std::string::npos;
        QVERIFY(foundExpired);
    }

    void write_is_not_dispatched_when_intent_persistence_fails() {
        InMemoryDataBaseService stubDb;
        ScopedDefaultDataBaseService dbService(&stubDb);
        AgentServiceImpl service;
        delete service._provider;
        auto* provider = new ScriptedLlmProvider();
        provider->replies = {
            {LlmReply::ToolCall, "probe-persist", "agent_confirmation_probe", "{}"},
            {LlmReply::Text, "done"}
        };
        service._provider = provider;

        int toolExecuted = 0;
        AgentTool probeTool{"agent_confirmation_probe", "Persistence probe", "{}",
            [&toolExecuted](const std::string&) { ++toolExecuted; return ToolResult{}; }};
        probeTool.dangerLevel = "write";
        QVERIFY(service._registry.add(probeTool));
        QSignalSpy confirmationSpy(&service, &AgentServiceImpl::confirmationRequired);
        QSignalSpy replySpy(&service, &AgentServiceImpl::replyReady);
        service.sendMessageWithAttachmentsAsync("run persistence probe", {});
        QVERIFY2(confirmationSpy.wait(4000), "awaiting persistence confirmation event");
        stubDb.saveResult = 0;
        service.confirmPendingAction(confirmationSpy[0][0].toString().toStdString(), true);
        QVERIFY2(replySpy.wait(4000), "awaiting final reply after persistence failure");
        QCOMPARE(toolExecuted, 0);
        QVERIFY(service._session.pendingWriteJson.empty());
        bool foundPersistenceFailure = false;
        for (const auto& turn : service._session.turns)
            foundPersistenceFailure = foundPersistenceFailure
                || turn.content.find("execution_intent_persist_failed") != std::string::npos;
        QVERIFY(foundPersistenceFailure);
    }

    void resume_marks_pending_write_unknown_without_replay() {
        RecoverableDataBaseService stubDb;
        SessionRecord stored;
        stored.id = "recover-write-1";
        stored.title = "Recover write";
        stored.pendingWriteJson = R"({"correlationId":"call-recover","confirmationId":"confirm-recover","tool":"save_baseline","argsSha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","contextFingerprint":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","catalogSha256":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","approvedAtMs":41,"startedMs":40})";
        stubDb.id = stored.id;
        stubDb.title = stored.title;
        stubDb.sessionJson = buildSessionJson(stored);
        ScopedDefaultDataBaseService dbService(&stubDb);
        AgentServiceImpl service;

        const QJsonObject result = AgentJson::parseObject(service.resumeSession(stored.id));
        QVERIFY(result.value("ok").toBool());
        QCOMPARE(result.value("recovery_warning").toObject().value("code").toString(),
                 QString("tool_outcome_unknown"));
        QVERIFY(service._session.pendingWriteJson.empty());
        QCOMPARE(stubDb.saveCalls, 1);
        const SessionRecord recovered = parseSessionJson(stubDb.sessionJson);
        QVERIFY(recovered.pendingWriteJson.empty());
        QCOMPARE(recovered.turns.size(), size_t(1));
        QVERIFY(recovered.turns[0].content.find("outcome_unknown_after_restart")
                != std::string::npos);
    }

    void run_analysis_requires_interactive_confirmation() {
        InMemoryDataBaseService stubDb;
        ScopedDefaultDataBaseService dbService(&stubDb);

        AgentServiceImpl service;
        delete service._provider;
        auto* provider = new ScriptedLlmProvider();
        provider->replies.reserve(2);
        provider->replies = {
            {LlmReply::ToolCall, "probe-run", "run_analysis", R"({"type":"FFT"})"},
            {LlmReply::Text, "analysis done"}
        };
        service._provider = provider;

        QSignalSpy confirmationSpy(&service, &AgentServiceImpl::confirmationRequired);
        QSignalSpy stepSpy(&service, &AgentServiceImpl::agentStep);
        QSignalSpy replySpy(&service, &AgentServiceImpl::replyReady);

        service.sendMessageWithAttachmentsAsync("run run_analysis", {});
        QVERIFY2(confirmationSpy.wait(4000), "awaiting confirmation event for run_analysis");
        QCOMPARE(confirmationSpy.count(), 1);
        QCOMPARE(confirmationSpy[0][1].toString(), QString("run_analysis"));
        const QString confirmationId = confirmationSpy[0][0].toString();

        service.confirmPendingAction(confirmationId.toStdString(), true);
        if (replySpy.isEmpty()) {
            QVERIFY2(replySpy.wait(6000), "awaiting final reply after analysis confirm");
        }

        bool sawRunAnalysisStart = false;
        bool sawRunAnalysisResult = false;
        bool sawNoCurrentTestError = false;
        for (int i = 0; i < stepSpy.count(); ++i) {
            const QString kind = stepSpy[i].value(0).toString();
            const QString name = stepSpy[i].value(1).toString();
            const QString detail = stepSpy[i].value(2).toString();
            if (kind == "tool_start" && name == "run_analysis") sawRunAnalysisStart = true;
            if (kind == "tool_result" && name == "run_analysis") {
                sawRunAnalysisResult = true;
                const QJsonObject result = AgentJson::parseObject(detail.toStdString());
                if (result.value("error").toString() == "no_current_test")
                    sawNoCurrentTestError = true;
            }
        }

        QCOMPARE(sawRunAnalysisStart, true);
        QCOMPARE(sawRunAnalysisResult, true);
        QVERIFY2(sawNoCurrentTestError, "run_analysis should fail fast without a loaded test, proving execution reached handler");
    }

    void stale_analysis_context_change_invalidates_pending_analysis() {
        InMemoryDataBaseService stubDb;
        ScopedDefaultDataBaseService dbService(&stubDb);

        AgentServiceImpl service;
        auto* projectA = new M();
        auto* projectB = new M();
        service._currentProject = projectA;
        service._dpaTest = new M();
        service.beginDiagnosticTool("run_analysis");

        const std::string runId = service._pendingAnalysisRunId;
        QCOMPARE(static_cast<int>(service._diagnosticRun.state()),
                 static_cast<int>(Model::DiagnosticRunState::AnalysisPending));
        auto* process = makeAnalysisProcess(runId);
        QCOMPARE(QString::fromStdString(process->value<std::string>("AgentRunId")),
                 QString::fromStdString(runId));
        service.bindPendingAnalysisProcess(process, runId);
        QVERIFY(!service._pendingAnalysisRunId.empty());

        service._currentProject = projectB;
        service.onAnalysisFinished(process);
        QCOMPARE(static_cast<int>(service._diagnosticRun.state()),
                 static_cast<int>(Model::DiagnosticRunState::Failed));
        QCOMPARE(QString::fromStdString(service._diagnosticRun.error()), QString("analysis_context_changed"));
        QCOMPARE(service._pendingAnalysisRunId.empty(), true);
    }

    void stale_context_change_clears_pending_analysis_and_confirmation() {
        InMemoryDataBaseService stubDb;
        ScopedDefaultDataBaseService dbService(&stubDb);

        AgentServiceImpl service;
        auto* projectA = new M();
        auto* projectB = new M();
        service._currentProject = projectA;
        service._dpaTest = new M();
        service.beginDiagnosticTool("run_analysis");

        auto* process = makeAnalysisProcess(service._pendingAnalysisRunId);
        service.bindPendingAnalysisProcess(process, service._pendingAnalysisRunId);

        {
            QMutexLocker lock(&service._confirmMutex);
            service._pendingConfirmationId = "pending-confirm";
            service._pendingConfirmationTool = "agent_confirmation_probe";
            service._pendingConfirmationArgsSha256 = "sha";
            service._confirmDecision = 0;
        }

        service._currentProject = projectB;
        service.onPropertyChanged("WorkSpace", nullptr);

        QCOMPARE(service._pendingAnalysisRunId.empty(), true);
        QCOMPARE(service._analysisContextFingerprint.empty(), true);
        QCOMPARE(service._pendingConfirmationId.empty(), true);
        QCOMPARE(service._pendingConfirmationTool.empty(), true);
        QCOMPARE(service._pendingConfirmationArgsSha256.empty(), true);
        QCOMPARE(service._confirmDecision, -1);
        QCOMPARE(static_cast<int>(service._diagnosticRun.state()),
                 static_cast<int>(Model::DiagnosticRunState::Failed));
    }

    void cancellation_blocks_pending_interactive_confirmation() {
        InMemoryDataBaseService stubDb;
        ScopedDefaultDataBaseService dbService(&stubDb);

        AgentServiceImpl service;
        delete service._provider;
        auto* provider = new ScriptedLlmProvider();
        provider->replies.reserve(2);
        LlmReply call;
        call.kind = LlmReply::ToolCall;
        call.toolCallId = "probe-2";
        call.toolName = "agent_confirmation_probe";
        call.toolArgsJson = "{}";
        provider->replies = {call, LlmReply{LlmReply::Text, "done"}};
        service._provider = provider;

        int toolExecuted = 0;
        AgentTool probeTool;
        probeTool.name = "agent_confirmation_probe";
        probeTool.description = "Test probe tool";
        probeTool.inputSchemaJson = R"({"type":"object","additionalProperties":false,"properties":{},"required":[]})";
        probeTool.handler = [&toolExecuted](const std::string&) {
            ++toolExecuted;
            ToolResult r;
            r.ok = true;
            r.summary = "probe executed";
            return r;
        };
        probeTool.dangerLevel = "write";
        QVERIFY2(service._registry.add(probeTool), "probe tool should register");

        QSignalSpy confirmationSpy(&service, &AgentServiceImpl::confirmationRequired);
        QSignalSpy replySpy(&service, &AgentServiceImpl::replyReady);

        service.sendMessageWithAttachmentsAsync("run probe then cancel", {});
        QVERIFY2(confirmationSpy.wait(4000), "awaiting confirmation event");
        QCOMPARE(confirmationSpy.count(), 1);

        service.cancelCurrentTurn();
        QVERIFY2(replySpy.wait(4000), "awaiting turn completion after cancel");
        QCOMPARE(toolExecuted, 0);

        const QString confirmationId = confirmationSpy[0][0].toString();
        service.confirmPendingAction(confirmationId.toStdString(), true);
        QCOMPARE(toolExecuted, 0);
    }

    void repeated_same_tool_calls_emit_distinct_tool_steps() {
        InMemoryDataBaseService stubDb;
        ScopedDefaultDataBaseService dbService(&stubDb);

        AgentServiceImpl service;
        delete service._provider;
        auto* provider = new ScriptedLlmProvider();
        provider->replies.reserve(3);
        provider->replies = {
            {LlmReply::ToolCall, "probe-1", "agent_confirmation_probe", "{}"},
            {LlmReply::ToolCall, "probe-2", "agent_confirmation_probe", "{}"},
            {LlmReply::Text, "done"},
        };
        service._provider = provider;

        int toolExecuted = 0;
        AgentTool probeTool;
        probeTool.name = "agent_confirmation_probe";
        probeTool.description = "Repeated call probe tool";
        probeTool.inputSchemaJson = R"({"type":"object","additionalProperties":false,"properties":{},"required":[]})";
        probeTool.handler = [&toolExecuted](const std::string&) {
            ++toolExecuted;
            ToolResult r;
            r.ok = true;
            r.summary = "probe executed";
            return r;
        };
        probeTool.dangerLevel = "read";
        QVERIFY2(service._registry.add(probeTool), "probe tool should register");

        QSignalSpy stepSpy(&service, &AgentServiceImpl::agentStep);
        QSignalSpy replySpy(&service, &AgentServiceImpl::replyReady);

        service.sendMessageWithAttachmentsAsync("run repeated tool twice", {});

        QVERIFY2(stepSpy.wait(5000), "awaiting step events");
        if (replySpy.isEmpty()) {
            QVERIFY2(replySpy.wait(5000), "awaiting final reply");
        }
        int toolStartCount = 0;
        int toolResultCount = 0;
        for (int i = 0; i < stepSpy.count(); ++i) {
            const QString kind = stepSpy[i].value(0).toString();
            const QString name = stepSpy[i].value(1).toString();
            if (kind == "tool_start" && name == "agent_confirmation_probe") ++toolStartCount;
            if (kind == "tool_result" && name == "agent_confirmation_probe") ++toolResultCount;
        }
        QCOMPARE(toolExecuted, 2);
        QCOMPARE(toolStartCount, 2);
        QCOMPARE(toolResultCount, 2);
    }

    void second_send_while_running_is_rejected_without_second_user_turn() {
        InMemoryDataBaseService stubDb;
        ScopedDefaultDataBaseService dbService(&stubDb);

        AgentServiceImpl service;
        delete service._provider;
        auto* provider = new ScriptedLlmProvider();
        provider->replies.reserve(2);
        provider->replies = {
            {LlmReply::ToolCall, "long-tool-call", "agent_context_probe", "{}"},
            {LlmReply::Text, "done"},
        };
        service._provider = provider;

        int toolExecuted = 0;
        AgentTool probeTool;
        probeTool.name = "agent_context_probe";
        probeTool.description = "Long read probe";
        probeTool.inputSchemaJson = R"({"type":"object","additionalProperties":false,"properties":{},"required":[]})";
        probeTool.handler = [&](const std::string&) {
            ++toolExecuted;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            ToolResult r;
            r.ok = true;
            r.summary = "probe executed";
            return r;
        };
        probeTool.dangerLevel = "read";
        QVERIFY2(service._registry.add(probeTool), "probe tool should register");

        QSignalSpy stepSpy(&service, &AgentServiceImpl::agentStep);
        QSignalSpy replySpy(&service, &AgentServiceImpl::replyReady);
        const auto countUserTurns = [&service]() {
            size_t total = 0;
            for (const auto& turn : service._session.turns) {
                if (turn.role == "user") ++total;
            }
            return total;
        };

        service.sendMessageWithAttachmentsAsync("start long turn", {});
        for (int i = 0; i < 40 && stepSpy.count() < 1; ++i) QTest::qWait(25);
        const size_t turnsBeforeBusyCall = service._session.turns.size();
        const size_t userTurnsBeforeBusyCall = countUserTurns();

        service.sendMessageWithAttachmentsAsync("reject this", {});
        bool sawBusyReject = false;
        for (int i = 0; i < 80; ++i) {
            for (const auto& args : std::as_const(stepSpy)) {
                if (args.value(0).toString() == "turn_rejected"
                    && args.value(1).toString() == "busy") {
                    sawBusyReject = true;
                    break;
                }
            }
            if (sawBusyReject) break;
            QTest::qWait(25);
        }

        QCOMPARE(sawBusyReject, true);
        QCOMPARE(service._session.turns.size(), turnsBeforeBusyCall);
        QCOMPARE(countUserTurns(), userTurnsBeforeBusyCall);
        QVERIFY2(replySpy.wait(8000), "awaiting first reply completion");
        QCOMPARE(toolExecuted, 1);
        QCOMPARE(countUserTurns(), userTurnsBeforeBusyCall);
    }

    void busy_rejection_cannot_finish_running_ui_or_emit_input_error_reply() {
        InMemoryDataBaseService stubDb;
        ScopedDefaultDataBaseService dbService(&stubDb);
        AgentServiceImpl service;
        delete service._provider;
        auto* provider = new BlockingTextProvider();
        service._provider = provider;
        ScopedDefaultAgentService scopedService(&service);
        ViewModel::VMAgentChat vm;
        vm._status = "running";

        QSignalSpy stepSpy(&service, &AgentServiceImpl::agentStep);
        QSignalSpy replySpy(&service, &AgentServiceImpl::replyReady);
        service.sendMessageWithAttachmentsAsync("first turn", {});
        for (int i = 0; i < 200 && !provider->entered.load(); ++i) QTest::qWait(5);
        QVERIFY2(provider->entered.load(), "first turn should own the busy gate");

        service.sendMessageWithAttachmentsAsync(std::string(64 * 1024 + 1, 'x'), {});
        bool sawBusyReject = false;
        for (const auto& step : std::as_const(stepSpy))
            sawBusyReject = sawBusyReject
                || (step[0].toString() == QString("turn_rejected")
                    && step[1].toString() == QString("busy"));
        QCOMPARE(sawBusyReject, true);
        QCOMPARE(replySpy.count(), 0);
        QCOMPARE(vm._status, std::string("running"));

        provider->release.store(true);
        if (replySpy.isEmpty())
            QVERIFY2(replySpy.wait(4000), "awaiting the owning turn's reply");
        QCOMPARE(vm._status, std::string("idle"));
    }

    void rejected_new_session_preserves_service_and_ui_history() {
        InMemoryDataBaseService stubDb;
        ScopedDefaultDataBaseService dbService(&stubDb);
        AgentServiceImpl service;
        delete service._provider;
        auto* provider = new BlockingTextProvider();
        service._provider = provider;
        service._session.id = "current-session";
        ScopedDefaultAgentService scopedService(&service);
        ViewModel::VMAgentChat vm;
        vm._history = "existing history\n";
        vm._output = "existing output";
        vm._attachments = "existing.txt";

        QSignalSpy replySpy(&service, &AgentServiceImpl::replyReady);
        service.sendMessageWithAttachmentsAsync("first turn", {});
        for (int i = 0; i < 200 && !provider->entered.load(); ++i) QTest::qWait(5);
        QVERIFY2(provider->entered.load(), "first turn should own the session");

        QCOMPARE(vm.excute("NewSession"), 0);
        QCOMPARE(service._session.id, std::string("current-session"));
        QCOMPARE(vm._history, std::string("existing history\n"));
        QCOMPARE(vm._output, std::string("existing output"));
        QCOMPARE(vm._attachments, std::string("existing.txt"));

        provider->release.store(true);
        if (replySpy.isEmpty()) QVERIFY2(replySpy.wait(4000), "awaiting owning turn");
        QCOMPARE(vm.excute("NewSession"), 1);
        QVERIFY(service._session.id != std::string("current-session"));
        QCOMPARE(vm._history.empty(), true);
        QCOMPARE(vm._output.empty(), true);
        QCOMPARE(vm._attachments.empty(), true);
    }

    void analysis_pending_blocks_view_model_send_and_session_change() {
        FakeUiAgentService fakeService;
        ScopedDefaultAgentService scopedService(&fakeService);
        ViewModel::VMAgentChat vm;
        vm._analysisPending = true;
        vm._input = "queued while analysis is pending";
        vm._history = "existing history\n";
        vm._resumeSessionId = "previous-session";
        vm._companionExplainEventJson = R"({"source":"local_monitor","title":"pending event"})";

        QCOMPARE(vm.excute("Send"), 0);
        QCOMPARE(vm.excute("NewSession"), 0);
        QCOMPARE(vm.excute("RefreshSessions"), 0);
        QCOMPARE(vm.excute("ResumeSession"), 0);
        QCOMPARE(vm.excute("ExplainCompanionEvent"), 0);
        vm.onCompanionEvent(QString::fromUtf8(
            R"({"source":"local_monitor","title":"automatic pending event","auto_explain":true})"));
        QCOMPARE(fakeService.sendCalls, 0);
        QCOMPARE(fakeService.newSessionCalls, 0);
        QCOMPARE(fakeService.listSessionCalls, 0);
        QCOMPARE(fakeService.resumeSessionCalls, 0);
        QCOMPARE(vm._input, std::string("queued while analysis is pending"));
        QCOMPARE(vm._history, std::string("existing history\n"));

        vm._analysisPending = false;
        QCOMPARE(vm.excute("Send"), 1);
        QCOMPARE(fakeService.sendCalls, 1);
        QCOMPARE(fakeService.lastMessage, std::string("queued while analysis is pending"));
    }

    void analysis_pending_blocks_service_turn_entrypoints() {
        InMemoryDataBaseService stubDb;
        ScopedDefaultDataBaseService dbService(&stubDb);
        AgentServiceImpl service;
        delete service._provider;
        auto* provider = new ScriptedLlmProvider();
        provider->replies = {{LlmReply::Text, "must not run"}};
        service._provider = provider;
        service.beginDiagnosticTool("run_analysis");

        const QJsonObject syncResult = AgentJson::parseObject(service.sendMessage("new turn"));
        QCOMPARE(syncResult.value("error").toString(), QString("analysis_pending"));

        QSignalSpy stepSpy(&service, &AgentServiceImpl::agentStep);
        service.sendMessageWithAttachmentsAsync("new async turn", {});
        QCOMPARE(stepSpy.count(), 1);
        QCOMPARE(stepSpy[0][0].toString(), QString("turn_rejected"));
        QCOMPARE(stepSpy[0][1].toString(), QString("analysis_pending"));
        QCOMPARE(provider->calls, 0);
        QCOMPARE(service._turnRunning.load(), false);
    }

    void proactive_advisory_event_does_not_finish_or_clear_main_turn() {
        InMemoryDataBaseService stubDb;
        ScopedDefaultDataBaseService dbService(&stubDb);
        AgentServiceImpl service;
        service._turnRunning.store(true);

        QSignalSpy replySpy(&service, &AgentServiceImpl::replyReady);
        QSignalSpy companionSpy(&service, &AgentServiceImpl::companionEvent);
        service.publishCompanionEvent("warning", "analysis_advisory",
            "分析结果超过主动建议阈值", "peak_value 超过阈值", false);

        QCOMPARE(replySpy.count(), 0);
        QCOMPARE(companionSpy.count(), 1);
        QCOMPARE(service._turnRunning.load(), true);
        const QString eventJson = companionSpy[0][0].toString();
        const QJsonObject event = AgentJson::parseObject(eventJson.toStdString());
        QCOMPARE(event.value("source").toString(), QString("local_monitor"));
        QCOMPARE(event.value("kind").toString(), QString("analysis_advisory"));
        QCOMPARE(event.value("auto_explain").toBool(true), false);

        FakeUiAgentService fakeService;
        ScopedDefaultAgentService scopedService(&fakeService);
        ViewModel::VMAgentChat vm;
        vm._status = "waiting_confirm";
        vm._pendingConfirm = "需要确认执行: write_tool";
        vm._pendingConfirmationId = "current-confirmation";
        vm.onCompanionEvent(eventJson);

        QCOMPARE(vm._status, std::string("waiting_confirm"));
        QCOMPARE(vm._pendingConfirm, std::string("需要确认执行: write_tool"));
        QCOMPARE(vm._pendingConfirmationId, std::string("current-confirmation"));
        QCOMPARE(fakeService.sendCalls, 0);
    }

    void companion_minute_history_aggregates_persists_and_restores() {
        InMemoryDataBaseService stubDb;
        ScopedDefaultDataBaseService dbService(&stubDb);
        AgentServiceImpl service;
        service._session.id = "history-session";
        AgentServiceImpl::TrendPoint point;
        point.observedMs = 1000;
        point.average = 2.0;
        point.rms = 3.0;
        point.minimum = 1.0;
        point.maximum = 4.0;
        point.peak = 4.0;
        point.kurtosis = 2.5;
        QVERIFY(!service.appendMonitoringHistory(7, point));
        point.observedMs = 61000;
        point.average = 6.0;
        point.rms = 7.0;
        point.minimum = 5.0;
        point.maximum = 8.0;
        point.peak = 8.0;
        QVERIFY(service.appendMonitoringHistory(7, point));
        QVERIFY(service.persistMonitoringHistory());
        QVERIFY(!stubDb.monitoringJson.empty());

        service._monitoringHistory.clear();
        service.restoreMonitoringHistory();
        QCOMPARE(service._monitoringHistory[7].size(), size_t(1));
        QCOMPARE(service._monitoringHistory[7][0].average, 2.0);
        const ToolResult history = service.getCompanionHistory(
            R"({"channel_id":7,"max_points":10})");
        QVERIFY(history.ok);
        const QJsonObject payload = AgentJson::parseObject(history.dataJson);
        QCOMPARE(payload.value("schema").toString(), QString("dpa.monitoring.history/v1"));
        QCOMPARE(payload.value("channels").toArray().size(), 1);
        QCOMPARE(payload.value("channels").toArray()[0].toObject()
                     .value("points").toArray().size(), 1);
        QVERIFY(!service.getCompanionHistory(R"({"unknown":1})").ok);

        point.observedMs = 61000;
        QVERIFY(!service.appendMonitoringHistory(7, point));
        point.observedMs = 500; // wall clock moved backwards
        QVERIFY(service.appendMonitoringHistory(7, point));
        QCOMPARE(service._monitoringHistory[7].size(), size_t(2));
        QCOMPARE(service._monitoringBuckets[7].minuteMs, 0LL);
    }

    void companion_acknowledgement_command_reaches_local_service() {
        FakeUiAgentService fakeService;
        ScopedDefaultAgentService scopedService(&fakeService);
        ViewModel::VMAgentChat vm;
        vm._companionAcknowledgeRequest = R"({"alarm_id":"data_stale:7:1000"})";
        QCOMPARE(vm.excute("AcknowledgeCompanionAlarm"), 1);
        QCOMPARE(fakeService.acknowledgeCalls, 1);
        QCOMPARE(fakeService.lastAcknowledgeArgs, vm._companionAcknowledgeRequest);
        QCOMPARE(AgentJson::parseObject(vm._companionAcknowledgeResult)
                     .value("acknowledged").toBool(), true);
    }

    void companion_acknowledgement_is_validated_and_audited() {
        InMemoryDataBaseService stubDb;
        ScopedDefaultDataBaseService dbService(&stubDb);
        AgentServiceImpl service;
        service._session.id = "ack-session";
        auto& condition = service._companionState.channels[7].stale;
        condition.active = true;
        condition.alarmId = "data_stale:7:1000";
        condition.occurrenceCount = 2;
        QSignalSpy events(&service, &AgentServiceImpl::companionEvent);

        const QJsonObject accepted = AgentJson::parseObject(service.acknowledgeCompanionAlarm(
            R"({"alarm_id":"data_stale:7:1000","note":"checked cable"})"));
        QVERIFY(accepted.value("ok").toBool());
        QVERIFY(condition.acknowledged);
        QCOMPARE(events.count(), 1);
        const QJsonObject event = AgentJson::parseObject(events[0][0].toString().toStdString());
        QCOMPARE(event.value("lifecycle").toString(), QString("acknowledged"));
        QCOMPARE(event.value("alarm_id").toString(), QString("data_stale:7:1000"));
        QCOMPARE(event.value("occurrence_count").toInt(), 2);
        QCOMPARE(AgentJson::parseObject(service.acknowledgeCompanionAlarm(
            R"({"alarm_id":"data_stale:7:1000"})")).value("already_acknowledged").toBool(), true);
        QCOMPARE(events.count(), 1);
        QCOMPARE(AgentJson::parseObject(service.acknowledgeCompanionAlarm(
            R"({"alarm_id":"data_stale:7:1000","unknown":1})"))
                     .value("error").toString(), QString("unknown_field"));
    }

    void synchronous_analysis_cancel_ack_does_not_leave_ui_cancelling() {
        FakeUiAgentService fakeService;
        fakeService.synchronousCancelAck = true;
        ScopedDefaultAgentService scopedService(&fakeService);
        ViewModel::VMAgentChat vm;
        vm._analysisPending = true;

        QCOMPARE(vm.excute("Cancel"), 1);
        QCOMPARE(fakeService.cancelCalls, 1);
        QCOMPARE(vm._analysisPending, false);
        QCOMPARE(vm._status, std::string("idle"));

        QCOMPARE(vm.excute("Cancel"), 0);
        QCOMPARE(fakeService.cancelCalls, 1);
    }

    void stale_confirmation_decision_is_not_reported_as_accepted() {
        FakeUiAgentService fakeService;
        ScopedDefaultAgentService scopedService(&fakeService);
        ViewModel::VMAgentChat vm;
        vm._pendingConfirmationId = "current-confirmation";
        vm._confirmationDecisionId = "stale-confirmation";
        vm._status = "waiting_confirm";

        QCOMPARE(vm.excute("Confirm"), 0);
        QCOMPARE(vm.excute("Reject"), 0);
        QCOMPARE(fakeService.confirmationCalls, 0);
        QCOMPARE(vm._pendingConfirmationId, std::string("current-confirmation"));

        vm._confirmationDecisionId = "current-confirmation";
        QCOMPARE(vm.excute("Confirm"), 1);
        QCOMPARE(fakeService.confirmationCalls, 1);
        QCOMPARE(fakeService.lastConfirmationId, std::string("current-confirmation"));
        QCOMPARE(fakeService.lastConfirmationApproved, true);

        vm._pendingConfirmationId = "reject-confirmation";
        vm._confirmationDecisionId = "reject-confirmation";
        vm._status = "waiting_confirm";
        QCOMPARE(vm.excute("Reject"), 1);
        QCOMPARE(fakeService.confirmationCalls, 2);
        QCOMPARE(fakeService.lastConfirmationId, std::string("reject-confirmation"));
        QCOMPARE(fakeService.lastConfirmationApproved, false);
    }
};

int runTstAgentServiceAsyncOps(int argc, char** argv) {
    TstAgentServiceAsyncOps test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_agentservice_asyncops.moc"
