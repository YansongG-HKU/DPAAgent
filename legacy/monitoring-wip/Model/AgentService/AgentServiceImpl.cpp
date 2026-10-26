#include "AgentServiceImpl.h"
#include "McpBridgeService.h"
#include "OpenAICompatProvider.h"
#include "AgentConfig.h"
#include "LlmRouting.h"
#include "tools/GetContextTool.h"
#include "tools/ListDataTool.h"
#include "tools/ReadChannelTool.h"
#include "tools/RunAnalysisTool.h"
#include "tools/ResultSummaryTool.h"
#include "tools/GenerateReportTool.h"
#include "tools/SituationAssembler.h"
#include "StandardsCatalog.h"
#include "StandardsEvidence.h"
#include "iPlotCapture.h"
#include "Diagnostics.h"
#include "EnvelopeAnalysis.h"
#include "ExperimentEvidence.h"
#include "MeasurementContract.h"
#include "MeasurementAdapterContract.h"
#include "AnalysisPlan.h"
#include "ExperimentCampaign.h"
#include "CampaignAnalysis.h"
#include "CampaignPower.h"
#include "ExperimentRefinement.h"
#include "ExperimentKnowledge.h"
#include "StatsSidecar.h"
#include "AgentArtifactPath.h"
#include "AgentJson.h"
#include "iProjectService.h"
#include "iDataBaseService.h"
#include "iDataProcess.h"
#include "iData.h"
#include <QtConcurrent/QtConcurrent>
#include <QMetaObject>
#include <QDateTime>
#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFutureWatcher>
#include <QMimeDatabase>
#include <QMimeType>
#include <QSaveFile>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTextCodec>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
using namespace Model;

static constexpr qint64 kConfirmationTimeoutMs = 5 * 60 * 1000;
static constexpr const char* kContextChangedReply =
    "[上下文已变化] 项目、试验、当前分析或来源数据在回复生成期间发生变化；旧回复已丢弃，请在当前上下文重新提问。";

namespace {
class AgentSessionProperty final : public utils::Property {
public:
    AgentSessionProperty() {
        setProperty("AgentSessionRecord", this, utils::Property::TYPE_Class);
        addProperty("Id", &id, utils::Property::TYPE_StdString, true);
        addProperty("Name", &name, utils::Property::TYPE_StdString, true);
        addProperty("LastFixTime", &lastFixTime, utils::Property::TYPE_StdString, true);
        addProperty("Type", &type, utils::Property::TYPE_StdString, true);
        addProperty("SessionJson", &sessionJson, utils::Property::TYPE_StdString, true);
        addProperty("BaselineJson", &baselineJson, utils::Property::TYPE_StdString, true);
        addProperty("MonitoringJson", &monitoringJson, utils::Property::TYPE_StdString, true);
    }

    std::string id;
    std::string name;
    std::string lastFixTime;
    std::string type;
    std::string sessionJson;
    std::string baselineJson;
    std::string monitoringJson;
};
}

static ToolResult jsonResult(const std::string& summary, const QJsonObject& data) {
    ToolResult r;
    r.summary = summary;
    r.dataJson = AgentJson::dump(data);
    return r;
}

static ToolResult errorResult(const std::string& code, const std::string& summary);

static ToolResult importMdfMetadata(const std::string& argsJson) {
    const QJsonObject args = AgentJson::parseObject(argsJson);
    const QString inputPath = args.value("path").toString();
    const QFileInfo inputInfo(inputPath);
    if (inputPath.isEmpty() || !inputInfo.exists() || !inputInfo.isFile()
        || inputInfo.isSymLink())
        return errorResult("mdf_input_invalid", "MDF/MF4 input must be an existing non-link file.");
    if (inputInfo.size() <= 0 || inputInfo.size() > 8LL * 1024 * 1024 * 1024)
        return errorResult("mdf_input_size_invalid", "MDF/MF4 input size is outside the safe read limit.");

    QString script = qEnvironmentVariable("DPA_MDF_ADAPTER_SCRIPT");
    if (script.isEmpty()) {
        QDir repoRoot(QCoreApplication::applicationDirPath());
        repoRoot.cdUp();
        repoRoot.cdUp();
        script = repoRoot.filePath("scripts/measurement-adapters/import_mdf.py");
    }
    if (!QFileInfo::exists(script))
        return errorResult("mdf_adapter_missing", "The read-only MDF adapter script is not installed.");
    const QString python = qEnvironmentVariable("DPA_PYTHON").isEmpty()
        ? QStandardPaths::findExecutable("python")
        : qEnvironmentVariable("DPA_PYTHON");
    if (python.isEmpty())
        return errorResult("mdf_runtime_missing", "Python is required for the MDF adapter.");

    QStringList arguments{script, inputInfo.absoluteFilePath()};
    const QJsonArray channels = args.value("channels").toArray();
    for (const QJsonValue& value : channels) {
        if (!value.isString() || value.toString().isEmpty() || value.toString().size() > 256)
            return errorResult("mdf_channel_invalid", "MDF channel names must be non-empty strings.");
        arguments << "--channel" << value.toString();
    }
    QProcess process;
    process.setProgram(python);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(5000) || !process.waitForFinished(120000)) {
        process.kill();
        return errorResult("mdf_adapter_timeout", "MDF adapter did not finish within the read-only timeout.");
    }
    const QByteArray output = process.readAllStandardOutput();
    if (output.size() > 1024 * 1024)
        return errorResult("mdf_adapter_output_too_large", "MDF adapter output exceeded the metadata limit.");
    const QJsonObject result = AgentJson::parseObject(output.toStdString());
    if (process.exitCode() != 0 || result.value("ok").toBool() != true)
        return errorResult(result.value("error").toString().toStdString(),
                           "MDF metadata import failed closed.");
    return jsonResult("MDF/MF4 metadata imported through the read-only external adapter.", result);
}

static QJsonObject experimentPreflightJson(const ExperimentPreflight& preflight) {
    QJsonArray blockers;
    for (const auto& code : preflight.blockers)
        blockers.append(QString::fromStdString(code));
    QJsonArray warnings;
    for (const auto& code : preflight.warnings)
        warnings.append(QString::fromStdString(code));
    QJsonArray nodes;
    for (const auto& node : preflight.nodes) {
        nodes.append(QJsonObject{{"id", QString::fromStdString(node.id)},
                                 {"state", QString::fromStdString(node.state)}});
    }
    const auto& snapshot = preflight.snapshot;
    return QJsonObject{
        {"overall", QString::fromStdString(preflight.overall)},
        {"ready_to_acquire", preflight.readyToAcquire},
        {"ready_to_monitor", preflight.readyToMonitor},
        {"has_project", snapshot.hasProject},
        {"has_daq_test", snapshot.hasDaqTest},
        {"connected", snapshot.connected},
        {"acquiring", snapshot.acquiring},
        {"saving", snapshot.saving},
        {"companion_enabled", snapshot.companionEnabled},
        {"selected_channels", snapshot.selectedChannels},
        {"configured_monitored_channels", snapshot.monitoredChannels},
        {"valid_limit_channels", snapshot.validLimitChannels},
        {"active_monitored_channels", snapshot.activeMonitoredChannels},
        {"data_sources", snapshot.dataSources},
        {"blockers", blockers},
        {"warnings", warnings},
        {"nodes", nodes}
    };
}

static ToolResult errorResult(const std::string& code, const std::string& summary) {
    ToolResult r;
    r.ok = false;
    r.error = code;
    r.summary = summary;
    return r;
}

static StandardApplicabilityRequest parseStandardApplicabilityRequest(const QJsonObject& input) {
    StandardApplicabilityRequest request;
    request.standardProfile = input.value("standardProfile").toString().toStdString();
    request.machineType = input.value("machineType").toString().toStdString();
    request.ratedPowerKw = input.value("ratedPowerKw").toDouble();
    request.rpm = input.value("rpm").toDouble();
    request.platform = input.value("platform").toString().toStdString();
    request.testPurpose = input.value("testPurpose").toString().toStdString();
    request.physicalQuantity = input.value("physicalQuantity").toString().toStdString();
    request.applicabilityBasis = input.value("applicabilityBasis").toString().toStdString();
    request.authorizedCopy = input.value("authorizedCopy").toBool();
    request.ownerApproved = input.value("ownerApproved").toBool();
    request.authorizedEvidenceValidated = input.value("authorizedEvidenceValidated").toBool();
    request.authorizedEvidenceManifestSha256 = input.value("authorizedEvidenceManifestSha256")
        .toString().toStdString();
    return request;
}

static QJsonObject standardCatalogEntryJson(const StandardCatalogEntry& entry) {
    return QJsonObject{
        {"id", QString::fromStdString(entry.id)},
        {"authority", QString::fromStdString(entry.authority)},
        {"title", QString::fromStdString(entry.title)},
        {"status", QString::fromStdString(entry.status)},
        {"applicability", QString::fromStdString(entry.applicability)},
        {"execution_status", QString::fromStdString(entry.executionStatus)},
        {"access", QString::fromStdString(entry.access)},
        {"source_url", QString::fromStdString(entry.sourceUrl)},
        {"requires_owner_approval", entry.requiresOwnerApproval}
    };
}

static ToolResult measurementBindingError(const std::string& code) {
    if (code == "no_current_test") return errorResult(code, "当前没有打开的分析试验。");
    if (code == "measurement_data_mismatch")
        return errorResult(code, "测量档案的数据 ID 与当前分析来源不一致。");
    if (code == "measurement_channel_mismatch")
        return errorResult(code, "测量档案的通道与当前分析来源不一致。");
    if (code == "measurement_unit_mismatch")
        return errorResult(code, "测量档案的单位与当前数据通道不一致。");
    if (code == "measurement_sampling_rate_mismatch")
        return errorResult(code, "测量档案的采样率与当前数据通道不一致。");
    return errorResult("current_result_provenance_unavailable",
                       "当前分析结果无法追溯到明确的数据 ID、通道、单位和采样率，诊断已停止。");
}

static ToolResult currentAnalysisError(const std::string& code, const char* operation) {
    const std::string action = operation ? operation : "使用当前分析结果";
    if (code == "no_current_test") return errorResult(code, "当前没有打开的分析试验，无法" + action + "。");
    if (code == "no_current_result") return errorResult(code, "当前没有已发布的分析结果，无法" + action + "。");
    if (code == "unsupported_result_domain")
        return errorResult(code, "当前结果不是受支持的频域分析结果，已拒绝" + action + "。");
    if (code == "invalid_spectrum_axis")
        return errorResult(code, "当前频谱坐标无效或并非严格递增，已拒绝" + action + "。");
    if (code == "band_exceeds_nyquist")
        return errorResult(code, "当前频谱超出来源采样率的 Nyquist 上限，已拒绝" + action + "。");
    if (code == "no_peaks")
        return errorResult(code, "当前频谱没有可用峰值；未作健康或故障判断。");
    return measurementBindingError(code);
}

static MeasurementProfile baselineMeasurement(const BaselineArgs& args) {
    MeasurementProfile measurement;
    measurement.dataId = args.dataId;
    measurement.channel = args.channel;
    measurement.measurementPoint = args.measurementPoint;
    measurement.direction = args.direction;
    measurement.physicalQuantity = args.physicalQuantity;
    measurement.unit = args.unit;
    measurement.samplingRateHz = args.samplingRateHz;
    measurement.standardProfile = args.standardProfile;
    return measurement;
}

static ToolResult validateBaselineMeasurement(const BaselineArgs& args,
                                              const MachineBaseline& current) {
    const MeasurementProfile requested = baselineMeasurement(args);
    const ProfileValidation profile = validateMeasurementProfile(requested);
    if (!profile.valid()) {
        ToolResult result = errorResult("invalid_measurement_profile",
            "基线测量档案缺失、物理量与单位不兼容，或标准配置不受支持。");
        QJsonArray errors;
        for (const std::string& error : profile.errors)
            errors.append(QString::fromStdString(error));
        result.dataJson = AgentJson::dump(QJsonObject{{"errors", errors}});
        return result;
    }
    MeasurementBinding actual;
    actual.dataId = current.provenance.sourceDataId;
    actual.channel = current.provenance.sourceChannel;
    actual.unit = current.provenance.sourceUnit;
    actual.samplingRateHz = current.provenance.samplingRateHz;
    const std::string bindingError = validateMeasurementBinding(requested, actual);
    if (!bindingError.empty()) return measurementBindingError(bindingError);
    if (args.standardProfile == kNoStandardProfile
        && (args.standardApplicabilityConfirmed || !args.standardApplicabilityBasis.empty()
            || args.severityVelocityRmsMmS > 0 || !args.severitySource.empty()
            || !args.severityDataRef.empty() || args.legacyIso10816MachineClass > 0)) {
        return errorResult("standard_profile_mismatch",
            "standardProfile 为 none 时不得附带 legacy 严重度输入。");
    }
    if (args.standardProfile == kLegacyIso10816ClassProfile
        && baselineSeverityZone(args).empty()) {
        return errorResult("baseline_severity_unassessed",
            "legacy 严重度必须绑定领域确认的适用性依据、外部速度 RMS 来源/数据引用和 Class 1..4。");
    }
    return {};
}

static BaselineObservation baselineObservation(const MachineBaseline& baseline,
                                               const BaselineArgs* args = nullptr) {
    BaselineObservation observation;
    observation.capturedMs = baseline.capturedMs;
    observation.isoZone = baseline.isoZone;
    observation.peakFreqsHz = baseline.peakFreqsHz;
    observation.provenance = baseline.provenance;
    observation.standardProfile = baseline.standardProfile;
    observation.legacyMachineClass = baseline.legacyMachineClass;
    if (args) {
        observation.standardApplicabilityBasis = args->standardApplicabilityBasis;
        observation.severitySource = args->severitySource;
        observation.severityDataRef = args->severityDataRef;
    }
    return observation;
}

static std::string diagnosticInputSchema(bool requireProfiles) {
    std::string schema = R"JSON({
      "type":"object","additionalProperties":false,
      "properties":{
        "asset":{"type":"object","additionalProperties":false,
          "properties":{
            "assetId":{"type":"string","minLength":1},
            "machineType":{"type":"string","minLength":1},
            "ratedPowerKw":{"type":"number","minimum":0,"description":"Provide ratedPowerKw > 0, or provide applicabilityInfo."},
            "applicabilityInfo":{"type":"string","description":"Domain-owned applicability basis when rated power is unavailable."},
            "rpm":{"type":"number","minimum":0.000001},
            "rpmSource":{"type":"string","minLength":1},
            "operatingCondition":{"type":"string","minLength":1},
            "bearing":{"type":"object","additionalProperties":false,
              "properties":{
                "rollingElements":{"type":"integer","minimum":3},
                "ballDiaMm":{"type":"number","minimum":0.000001},
                "pitchDiaMm":{"type":"number","minimum":0.000001},
                "contactAngleDeg":{"type":"number","minimum":0,"maximum":90}
              },
            "required":["rollingElements","ballDiaMm","pitchDiaMm","contactAngleDeg"]}
          },
          "required":["assetId","machineType","rpm","rpmSource","operatingCondition"]},
        "measurement":{"type":"object","additionalProperties":false,
          "properties":{
            "dataId":{"type":"string","minLength":1},
            "channel":{"type":"integer","minimum":0},
            "measurementPoint":{"type":"string","minLength":1},
            "direction":{"type":"string","minLength":1},
            "physicalQuantity":{"type":"string","enum":["vibration_acceleration","vibration_velocity","vibration_displacement"]},
            "unit":{"type":"string","minLength":1},
            "samplingRateHz":{"type":"number","minimum":0.000001},
            "standardProfile":{"type":"string","enum":["none","legacy-iso10816-class-i-iv/1.0.0"]}
          },
          "required":["dataId","channel","measurementPoint","direction","physicalQuantity","unit","samplingRateHz","standardProfile"]},
        "standard":{"type":"object","additionalProperties":false,
          "properties":{
            "applicabilityConfirmed":{"type":"boolean"},
            "applicabilityBasis":{"type":"string","minLength":1},
            "velocityRmsMmS":{"type":"number","minimum":0},
            "velocityRmsSource":{"type":"string","minLength":1},
            "velocityRmsDataRef":{"type":"string","minLength":1},
            "legacyMachineClass":{"type":"integer","minimum":1,"maximum":4}
          }},
        "freqTolRatio":{"type":"number","minimum":0.001,"maximum":0.25}
      },
      "required":__PROFILE_REQUIRED__
    })JSON";
    const std::string marker = "__PROFILE_REQUIRED__";
    schema.replace(schema.find(marker), marker.size(), requireProfiles ? "[\"asset\",\"measurement\"]" : "[]");
    return schema;
}

static std::string baselineInputSchema() {
    return R"JSON({"type":"object","additionalProperties":false,
      "properties":{
        "assetId":{"type":"string","minLength":1},
        "measurementPoint":{"type":"string","minLength":1},
        "direction":{"type":"string","minLength":1},
        "operatingCondition":{"type":"string","minLength":1},
        "dataId":{"type":"string","minLength":1},
        "channel":{"type":"integer","minimum":0},
        "physicalQuantity":{"type":"string","enum":["vibration_acceleration","vibration_velocity","vibration_displacement"]},
        "unit":{"type":"string","minLength":1},
        "samplingRateHz":{"type":"number","minimum":0.000001},
        "standardProfile":{"type":"string","enum":["none","legacy-iso10816-class-i-iv/1.0.0"]},
        "standard":{"type":"object","additionalProperties":false,"properties":{
          "applicabilityConfirmed":{"type":"boolean"},
          "applicabilityBasis":{"type":"string","minLength":1},
          "velocityRmsMmS":{"type":"number","minimum":0},
          "velocityRmsSource":{"type":"string","minLength":1},
          "velocityRmsDataRef":{"type":"string","minLength":1},
          "legacyMachineClass":{"type":"integer","minimum":1,"maximum":4}}},
        "topN":{"type":"integer","minimum":1,"maximum":20}
      },
      "required":["assetId","measurementPoint","direction","operatingCondition","dataId","channel","physicalQuantity","unit","samplingRateHz","standardProfile"]})JSON";
}

static std::string autonomousExperimentInputSchema() {
    // Keep the autonomous command strict. Reuse the bounded diagnostic profile
    // contract instead of accepting an untyped nested object.
    return std::string(R"JSON({"type":"object","additionalProperties":false,"properties":{"plan":{"type":"object","additionalProperties":false,"properties":{"duration_ms":{"type":"integer","minimum":1000,"maximum":600000},"auto_connect":{"type":"boolean"},"save":{"type":"boolean"},"analysis_type":{"type":"string","enum":["FFT","PSD","DFT","RMSPRO","AVGPRO","PEAKPRO"]},"frame_size":{"type":"integer","enum":[128,256,512,1024,2048,4096,8192,16384,32768]},"average_num":{"type":"integer","minimum":1,"maximum":99},"overlap_index":{"type":"integer","minimum":0,"maximum":5},"stop_on_warning":{"type":"boolean"},"standard_plan_required":{"type":"boolean"},"diagnostic":)JSON")
        + diagnosticInputSchema(false)
        + R"JSON(,"generate_report":{"type":"boolean"},"create_evidence":{"type":"boolean"}},"required":["duration_ms","analysis_type"]}},"required":["plan"]})JSON";
}

static std::string autonomousExperimentCampaignInputSchema() {
    // The outer surface stays minimal; the campaign contract itself is held to
    // an exact-key, domain-bounded grammar by ExperimentCampaign validation.
    return R"JSON({"type":"object","additionalProperties":false,"properties":{"contract":{"type":"object"}},"required":["contract"]})JSON";
}

static int estimateTokens(const std::string& text) {
    return static_cast<int>(text.size() / 4 + 1);
}

static std::vector<LlmMessage> recentHistory(const SessionRecord& session) {
    const size_t start = session.turns.size() > 12 ? session.turns.size() - 12 : 0;
    std::vector<LlmMessage> history;
    for (size_t i = start; i < session.turns.size(); ++i) {
        const auto& turn = session.turns[i];
        if ((turn.role == "user" || turn.role == "assistant") && !turn.content.empty())
            history.push_back({turn.role, turn.content});
    }
    return history;
}

static std::vector<LlmMessage> compactedHistory(const SessionRecord& session) {
    constexpr int kHistoryTokenBudget = 8000;
    constexpr size_t kRecentTurnsFull = 4;
    constexpr size_t kMaxSummaryLen = 100;
    constexpr size_t kKeepRecentTurns = 4;
    const size_t totalTurns = session.turns.size();
    if (totalTurns == 0) return {};

    std::vector<LlmMessage> result;

    // Separate text turns from tool turns.  Keep only user/assistant turns
    // since tool turns are intermediate and the LLM re-derives them.
    struct TextTurn { size_t index; std::string role; std::string content; int estTokens; };
    std::vector<TextTurn> textTurns;
    textTurns.reserve(totalTurns);
    for (size_t i = 0; i < totalTurns; ++i) {
        const auto& turn = session.turns[i];
        if ((turn.role == "user" || turn.role == "assistant") && !turn.content.empty()) {
            textTurns.push_back({i, turn.role, turn.content, estimateTokens(turn.content)});
        }
    }

    if (textTurns.size() <= kKeepRecentTurns) {
        // Few turns — return everything.
        for (const auto& t : textTurns)
            result.push_back({t.role, t.content});
        return result;
    }

    // Phase 1: always include the last kKeepRecentTurns turns in full.
    const size_t recentStart = textTurns.size() - kKeepRecentTurns;
    int recentTokenCost = 0;
    for (size_t i = recentStart; i < textTurns.size(); ++i)
        recentTokenCost += textTurns[i].estTokens;

    // Remaining budget for older turns.
    int olderBudget = kHistoryTokenBudget - recentTokenCost;
    if (olderBudget < 0) olderBudget = 0;

    // Phase 2: for older turns, keep full text while budget allows,
    // then fall back to 100-char summaries, then drop if needed.
    // Build in chronological order so the LLM sees the correct sequence.
    for (size_t i = 0; i < recentStart; ++i) {
        const auto& t = textTurns[i];
        if (t.estTokens <= olderBudget) {
            // Full turn fits within remaining budget.
            result.push_back({t.role, t.content});
            olderBudget -= t.estTokens;
        } else if (kMaxSummaryLen < t.content.size()) {
            // Budget exhausted — emit a summary prefix instead.
            std::string summary = t.content.substr(0, kMaxSummaryLen);
            summary += "…";
            const int summaryTokens = estimateTokens(summary);
            if (summaryTokens <= olderBudget) {
                result.push_back({t.role, std::move(summary)});
                olderBudget -= summaryTokens;
            }
            // else: even the summary doesn't fit — drop this turn entirely.
        } else {
            // Content is already short enough; try the full text.
            if (t.estTokens <= olderBudget) {
                result.push_back({t.role, t.content});
                olderBudget -= t.estTokens;
            }
        }
    }

    // Phase 3: append the recent turns (guaranteed full fidelity).
    for (size_t i = recentStart; i < textTurns.size(); ++i)
        result.push_back({textTurns[i].role, textTurns[i].content});

    return result;
}

static QString mcpDir() {
    QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (!base.isEmpty()) return base + "/DPA-2026/mcp";
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/mcp";
}

static QString artifactRootDir() {
    return mcpDir() + "/artifacts";
}

static QString knowledgeFilePath() {
    // Cross-session ledger: experiment memory outlives any single session.
    return mcpDir() + QStringLiteral("/experiment-knowledge/v1/knowledge.jsonl");
}

static bool compactKnowledgeLedger(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    std::vector<QByteArray> lines;
    while (!file.atEnd()) {
        QByteArray line = file.readLine();
        if (!line.trimmed().isEmpty())
            lines.push_back(std::move(line));
    }
    file.close();
    if (lines.empty()) return false;
    const qint64 totalBytes = [&]() {
        qint64 sum = 0;
        for (const auto& l : lines) sum += l.size();
        return sum;
    }();
    const qint64 halfBytes = totalBytes / 2;
    qint64 accumulated = 0;
    size_t keepFrom = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        accumulated += lines[i].size();
        if (accumulated >= halfBytes) { keepFrom = i + 1; break; }
    }
    if (keepFrom >= lines.size()) keepFrom = lines.size() / 2;
    if (keepFrom == 0) keepFrom = lines.size();
    std::vector<QByteArray> kept(lines.begin() + static_cast<ptrdiff_t>(keepFrom), lines.end());
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return false;
    for (const auto& l : kept)
        out.write(l);
    out.close();
    return true;
}

static bool appendKnowledgeLine(const std::string& entryJson) {
    constexpr qint64 kMaxLedgerBytes = 2 * 1024 * 1024;
    const QString path = knowledgeFilePath();
    QFileInfo info(path);
    if (info.exists() && info.size() > kMaxLedgerBytes)
        compactKnowledgeLedger(path);
    if (!QDir().mkpath(info.absolutePath())) return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return false;
    const QByteArray line = QByteArray::fromStdString(entryJson) + "\n";
    return file.write(line) == line.size();
}

// Memory injection follows the layered-memory consensus: the model sees a
// bounded digest (newest findings, one-line summaries with key numbers), while
// raw entries stay in the ledger artifact for on-demand retrieval.
// Workflow playbook: cross-step orchestration knowledge belongs in the system
// prompt (StateFlow, NeurIPS 2024), not in per-tool descriptions.
constexpr char kCampaignWorkflowPlaybook[] =
    "试验战役工作流（用户要求系统性研究因素影响时按此链路执行）："
    "1) estimate_autonomous_experiment_campaign_power 校验设计功效"
    "（可用 sweep_experiment_power_curve 一次取得重复数 2..5 的功效曲线）；"
    "2) 构造契约并 execute_autonomous_experiment_campaign 执行（写操作需用户确认）；"
    "3) analyze_autonomous_experiment_campaign 得出效应与检验结论；"
    "4) 需要下一轮时 refine_..._campaign 生成精化提案并请用户授权；"
    "suggest_experiment_refinement_next 可作为只读统计咨询提供 GP 建议水平，"
    "compute_sequential_boundary 可为含期中的战役给出名义 p 上限边界；"
    "5) 结果含 awaiting_interim 时向用户转述检查点并等待 continue|abort 决定，"
    "用 resume_autonomous_experiment_campaign 恢复；"
    "约束：契约字段只允许白名单旋钮、成功运行必须携带证据绑定、"
    "统计结论只能来自分析工具不得手算；sidecar 降级时必须转述 warnings。";

static void appendKnowledgeContext(QJsonObject& context) {    QFile file(knowledgeFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QJsonArray all;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty()) continue;
        const KnowledgeEntryParse parsed = parseKnowledgeEntry(
            std::string(line.constData(), static_cast<size_t>(line.size())));
        if (!parsed.ok) continue;
        const QJsonObject entry = AgentJson::parseObject(parsed.entry.canonicalJson);
        QString dominant = QStringLiteral("无显著因子");
        double range = 0.0;
        double pValue = -1.0;
        for (const auto& factorValue : entry.value("factors").toArray()) {
            const QJsonObject factor = factorValue.toObject();
            if (!factor.value("testable").toBool(false)) continue;
            const double candidate = factor.value("effect_range").toDouble(0);
            if (candidate > range) {
                range = candidate;
                pValue = factor.value("perm_p").toDouble(-1.0);
                dominant = factor.value("name").toString();
            }
        }
        QJsonObject summary;
        summary["recorded_at_ms"] = entry.value("recorded_at_ms");
        summary["campaign_id"] = entry.value("campaign_id");
        summary["finding"] = QStringLiteral("最强因子 %1，效应范围 %2，p=%3")
            .arg(dominant).arg(range, 0, 'g', 4).arg(pValue, 0, 'g', 3);
        summary["residual_std"] = entry.value("residual_std");
        summary["source_artifact_id"] = entry.value("source_artifact_id");
        all.append(summary);
    }
    const int count = all.size();
    const int take = std::min(3, count);
    if (take <= 0) return;
    QJsonArray recent;
    for (int i = count - take; i < count; ++i) recent.append(all.at(i));
    context["experiment_knowledge"] = recent;
}

static void appendWorkflowPlaybook(QJsonObject& context) {
    context["experiment_workflow_playbook"] = kCampaignWorkflowPlaybook;
}

// Single source of truth for what the model is told, so the UI and MCP
// consumers can audit the exact briefing (no hidden asymmetry).
static QJsonObject buildExperimentBriefing() {
    QJsonObject briefing;
    appendKnowledgeContext(briefing);
    appendWorkflowPlaybook(briefing);
    return briefing;
}

static bool validOpaqueId(const QString& id) {
    if (id.isEmpty() || id.size() > 160) return false;
    for (const QChar c : id)
        if (!c.isLetterOrNumber() && c != '-' && c != '_' && c != '.') return false;
    return id != "." && id != ".." && !id.contains("..");
}

static QString artifactScopeDir(const std::string& scopeId) {
    const QString scope = QString::fromStdString(scopeId);
    if (!validOpaqueId(scope)) return {};
    return artifactRootDir() + "/" + scope;
}

static QString existingArtifactPath(const std::string& scopeId, const QString& id) {
    if (!validOpaqueId(id)) return {};
    const QString scope = artifactScopeDir(scopeId);
    if (scope.isEmpty()) return {};
    return AgentArtifactPath::existingRegularFileWithinScope(scope, scope + "/" + id);
}

static QString writeTextArtifact(const std::string& scopeId, const QString& prefix,
                                 const QString& ext, const QString& text) {
    constexpr qint64 maxArtifactBytes = 1024 * 1024;
    const QByteArray bytes = text.toUtf8();
    if (bytes.size() > maxArtifactBytes) return {};
    const QString scope = artifactScopeDir(scopeId);
    if (scope.isEmpty() || !QDir().mkpath(scope) || QFileInfo(scope).isSymLink()) return {};
    QString id = prefix + "_" + QString::number(QDateTime::currentMSecsSinceEpoch()) +
        "_" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ext;
    if (!validOpaqueId(id)) return {};
    QSaveFile f(scope + "/" + id);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)
        || f.write(bytes) != bytes.size() || !f.commit()) return {};
    return id;
}

static ToolResult attachReportArtifact(ToolResult r, const std::string& scopeId) {
    if (!r.ok) return r;
    QJsonObject data = AgentJson::parseObject(r.dataJson);
    QString report = data.value("report").toString();
    if (report.isEmpty()) return r;
    QString id = writeTextArtifact(scopeId, "analysis_report", ".md", report);
    if (id.isEmpty()) return errorResult("artifact_write_failed",
        "报告超过 1 MiB 或无法安全写入当前会话制品目录。");
    data["artifactId"] = id;
    r.dataJson = AgentJson::dump(data);
    r.artifactRefs.push_back(id.toStdString());
    return r;
}

static QJsonArray listArtifacts(const std::string& scopeId) {
    QJsonArray arr;
    const QString scope = artifactScopeDir(scopeId);
    if (scope.isEmpty() || QFileInfo(scope).isSymLink()) return arr;
    QDir dir(scope);
    const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Time);
    const int count = std::min(entries.size(), 100);
    for (int i = 0; i < count; ++i) {
        const QFileInfo& info = entries.at(i);
        QJsonObject o;
        o["artifactId"] = info.fileName();
        o["filename"] = info.fileName();
        o["size"] = double(info.size());
        o["modified"] = info.lastModified().toString(Qt::ISODateWithMs);
        arr.append(o);
    }
    return arr;
}

static ToolResult readArtifact(const std::string& scopeId, const std::string& argsJson) {
    constexpr qint64 maxArtifactBytes = 1024 * 1024;
    QString id = AgentJson::parseObject(argsJson).value("artifactId").toString();
    QString path = existingArtifactPath(scopeId, id);
    if (path.isEmpty()) return errorResult("artifact_not_found", "Artifact was not found in the current session.");
    const QFileInfo info(path);
    if (info.size() > maxArtifactBytes)
        return errorResult("artifact_too_large", "Artifact exceeds the 1 MiB read limit.");
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return errorResult("artifact_read_failed", "Failed to read artifact.");
    QByteArray bytes = f.read(maxArtifactBytes + 1);
    if (bytes.size() > maxArtifactBytes)
        return errorResult("artifact_too_large", "Artifact exceeds the 1 MiB read limit.");
    QJsonObject data;
    data["artifactId"] = id;
    data["content"] = QString::fromUtf8(bytes);
    data["size"] = double(bytes.size());
    return jsonResult("Artifact read.", data);
}

static ToolResult exportArtifact(const std::string& scopeId, const std::string& argsJson) {
    QJsonObject args = AgentJson::parseObject(argsJson);
    QString id = args.value("artifactId").toString();
    QString src = existingArtifactPath(scopeId, id);
    if (src.isEmpty()) return errorResult("artifact_not_found", "Artifact was not found in the current session.");
    QString dest = args.value("destinationPath").toString();
    if (dest.isEmpty()) return errorResult("destination_required", "An explicit destinationPath is required.");
    QFileInfo destInfo(dest);
    if (destInfo.isDir()) dest = destInfo.absoluteFilePath() + "/" + id;
    if (QFile::exists(dest)) return errorResult("target_exists", "Destination already exists.");
    const QFileInfo parentInfo(QFileInfo(dest).absolutePath());
    if (!parentInfo.exists() || !parentInfo.isDir() || parentInfo.isSymLink())
        return errorResult("destination_invalid", "Destination parent must be an existing non-link directory.");
    if (!QFile::copy(src, dest)) return errorResult("artifact_export_failed", "Failed to copy artifact.");
    return jsonResult("Artifact exported to the approved destination.",
                      QJsonObject{{"artifactId", id}, {"filename", QFileInfo(dest).fileName()}});
}

static QJsonObject currentSimulatorEvidence(M* daqTest) {
    const QJsonObject absent{{"present", false}};
    if (!daqTest || !QCoreApplication::arguments().contains(QStringLiteral("--simulate-daq")))
        return absent;
    auto* settings = (M*)daqTest->childSelf("DeviceSettings");
    if (!settings) return absent;
    M* simulator = nullptr;
    for (auto* entry : settings->getChildren()) {
        auto* device = (M*)entry;
        auto* model = device ? device->child("Description", "Model") : nullptr;
        const bool verified = device && device->creatorKey() == "SimulatedDevice"
            && model && model->get<std::string>() == "SimulatedDevice";
        if (!verified || simulator) return absent;
        simulator = device;
    }
    if (!simulator || !simulator->child("SimulationRecipeJson")
        || !simulator->child("SimulationRecipeSha256")
        || !simulator->child("SimulationSampleIndex")) return absent;
    const QByteArray recipeBytes = QByteArray::fromStdString(
        simulator->value<std::string>("SimulationRecipeJson"));
    QJsonParseError parseError;
    const QJsonDocument recipe = QJsonDocument::fromJson(recipeBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !recipe.isObject()) return absent;
    const QByteArray canonical = QJsonDocument(recipe.object()).toJson(QJsonDocument::Compact);
    const QString digest = QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex();
    if (digest.toStdString() != simulator->value<std::string>("SimulationRecipeSha256"))
        return absent;
    const std::int64_t sampleIndex = simulator->value<std::int64_t>("SimulationSampleIndex");
    const int elapsedMs = simulator->value<int>("SimulationElapsedMs");
    if (sampleIndex < 0 || elapsedMs < 0) return absent;
    return QJsonObject{{"present", true}, {"recipe", recipe.object()},
                       {"recipe_sha256", digest}, {"elapsed_ms", elapsedMs},
                       {"sample_index", static_cast<double>(sampleIndex)},
                       {"sampling", simulator->value<bool>("IsSampling")},
                       {"saving", simulator->value<bool>("IsSaving")},
                       {"paused", simulator->value<bool>("SimulationPaused")}};
}

static std::string experimentContextSha256(M* project, M* daqTest) {
    QJsonObject context;
    if (project) {
        context["project_id"] = QString::fromStdString(project->value<std::string>("Id"));
        context["project_name"] = QString::fromStdString(project->value<std::string>("Name"));
    }
    if (daqTest) {
        context["test_id"] = QString::fromStdString(daqTest->value<std::string>("Id"));
        context["test_name"] = QString::fromStdString(daqTest->value<std::string>("Name"));
    }
    return QCryptographicHash::hash(QJsonDocument(context).toJson(QJsonDocument::Compact),
                                    QCryptographicHash::Sha256).toHex().toStdString();
}

static ToolResult verifyEvidenceArtifact(const std::string& scopeId,
                                         const std::string& argsJson,
                                         const std::string& currentCatalogSha256) {
    constexpr qint64 maxArtifactBytes = 1024 * 1024;
    const QString id = AgentJson::parseObject(argsJson).value("artifactId").toString();
    const QString path = existingArtifactPath(scopeId, id);
    if (path.isEmpty())
        return errorResult("artifact_not_found", "Evidence artifact was not found in the current session.");
    QFile file(path);
    if (QFileInfo(path).size() > maxArtifactBytes || !file.open(QIODevice::ReadOnly))
        return errorResult("evidence_read_failed", "Evidence artifact could not be read safely.");
    const QByteArray bytes = file.read(maxArtifactBytes + 1);
    if (bytes.size() > maxArtifactBytes)
        return errorResult("evidence_too_large", "Evidence artifact exceeds the 1 MiB verification limit.");
    const ExperimentEvidenceVerification verification = verifyExperimentEvidenceJson(
        bytes.toStdString(), currentCatalogSha256);
    if (!verification.ok)
        return errorResult(verification.error, "Evidence verification failed closed; nothing was replayed.");
    QJsonParseError replayError;
    const QJsonDocument replay = QJsonDocument::fromJson(
        QByteArray::fromStdString(verification.replayJson), &replayError);
    if (replayError.error != QJsonParseError::NoError || !replay.isObject())
        return errorResult("evidence_replay_invalid", "Verified evidence could not be reconstructed.");
    return jsonResult(verification.catalogMatches
        ? "Evidence integrity verified and timeline reconstructed without side effects."
        : "Evidence integrity verified; timeline reconstructed with tool-catalog drift reported.",
        QJsonObject{{"artifactId", id}, {"verified", true},
                    {"payloadSha256", QString::fromStdString(verification.payloadSha256)},
                    {"catalogMatchesCurrent", verification.catalogMatches},
                    {"timelineEntries", verification.timelineEntries},
                    {"toolEvents", verification.toolEvents},
                    {"companionEvents", verification.companionEvents},
                    {"omittedEvents", verification.omittedEvents},
                    {"standardsPlanPresent", verification.standardsPlanPresent},
                    {"standardsPlanSha256", QString::fromStdString(verification.standardsPlanSha256)},
                    {"simulatorRecipePresent", verification.simulatorRecipePresent},
                    {"replay", replay.object()}});
}

AgentServiceImpl::AgentServiceImpl() {
    // Production defaults to plan-first. Test/diagnostic harnesses may opt out
    // explicitly while migrating legacy scripted tool-loop fixtures.
    _planFirstEnabled = qEnvironmentVariable("DPA_PLAN_FIRST", "1") != "0";
    setProperty("AgentService", this, utils::Property::TYPE_Class);
    addProperty("ProjectService", &_projectService, utils::Property::TYPE_ClassPtr);
    addProperty("CurrentProject", &_currentProject, utils::Property::TYPE_ClassPtr);
    addProperty("分析试验",        &_dpaTest,        utils::Property::TYPE_ClassPtr);
    addProperty("采集试验",        &_daqTest,        utils::Property::TYPE_ClassPtr);
    addProperty("WorkSpace",       &_daqWorkSpace,   utils::Property::TYPE_ClassPtr);
    addProperty("ChannelSettings", &_daqChannelSettings, utils::Property::TYPE_ClassPtr);
    addProperty("IsDaqStart",      &_daqRunning,     utils::Property::TYPE_Bool);
    addProperty("IsSaveStart",     &_daqSaving,      utils::Property::TYPE_Bool);
    addProperty("IsConnected",     &_daqConnected,   utils::Property::TYPE_Bool);
    loadCompanionConfig();
    _companionTimer = new QTimer(this);
    _companionTimer->setTimerType(Qt::CoarseTimer);
    _companionTimer->setInterval(_companionSampleIntervalMs);
    connect(_companionTimer, &QTimer::timeout, this, &AgentServiceImpl::pollCompanion);
    _registry.add({
        "get_context",
        "获取当前分析试验与通道列表",
        R"({"type":"object","additionalProperties":false,"properties":{},"required":[]})",
        [this](const std::string&){ return buildContextResult(snapshotFromTest(_dpaTest, _currentProject)); },
        "dpa.project.current", "获取当前项目", R"({"type":"object"})", "project", "read"
    });
    _registry.add({
        "experiment_preflight",
        "Run a deterministic preflight across project, device, channels, acquisition, storage and companion state",
        R"({"type":"object","additionalProperties":false,"properties":{},"required":[]})",
        [this](const std::string&){
            return jsonResult("Experiment preflight completed.",
                              experimentPreflightJson(experimentPreflight()));
        },
        "dpa.experiment.preflight", "Experiment preflight", R"({"type":"object"})",
        "experiment", "read"
    });
    _registry.add({
        "autonomous_experiment_status",
        "读取当前自主试验执行状态、运行边界、停止原因和产物摘要",
        R"({"type":"object","additionalProperties":false,"properties":{},"required":[]})",
        [this](const std::string&) { return autonomousExperimentStatus(); },
        "dpa.experiment.status", "Autonomous experiment status", R"({"type":"object"})",
        "experiment", "read"
    });
    _registry.add({
        "execute_autonomous_experiment",
        "在已验证上下文和应用内授权后，按受限试验计划自动完成采集、持续监控、停止、分析、报告和证据固化",
        autonomousExperimentInputSchema(),
        [this](const std::string& args) {
            const QJsonObject input = AgentJson::parseObject(args);
            return runAutonomousExperiment(input.value("plan").toObject());
        },
        "dpa.experiment.execute", "Execute bounded autonomous experiment", R"({"type":"object"})",
        "experiment", "write"
    });
    _registry.add({
        "execute_autonomous_experiment_campaign",
        "在应用内授权后，按确定性因子设计展开试验战役：逐运行执行受控自主试验并强制逐运行证据绑定",
        autonomousExperimentCampaignInputSchema(),
        [this](const std::string& args) {
            const QJsonObject input = AgentJson::parseObject(args);
            return runAutonomousExperimentCampaign(input.value("contract").toObject());
        },
        "dpa.experiment.campaign.execute", "Execute bounded experiment campaign",
        R"({"type":"object"})", "experiment", "write"
    });
    _registry.add({
        "resume_autonomous_experiment_campaign",
        "在中期检查点恢复战役：continue 继续剩余运行或 abort 受控终止；需原契约、检查点工件 ID 与决定（运营/安全监督门，非统计期中检验）",
        R"({"type":"object","additionalProperties":false,"properties":{"contract":{"type":"object"},"checkpoint_artifact_id":{"type":"string","minLength":1,"maxLength":160},"decision":{"type":"string","enum":["continue","abort"]}},"required":["contract","checkpoint_artifact_id","decision"]})",
        [this](const std::string& args) {
            return resumeAutonomousExperimentCampaign(args);
        },
        "dpa.experiment.campaign.resume", "Resume experiment campaign",
        R"({"type":"object"})", "experiment", "write"
    });
    _registry.add({
        "analyze_autonomous_experiment_campaign",
        "对已完成战役的逐运行响应值做确定性统计：水平均值、主效应、Fisher 随机化检验与纯误差噪声；结果固化为会话证据工件",
        R"({"type":"object","additionalProperties":false,"properties":{"contract":{"type":"object"},"campaign_result_artifact_id":{"type":"string","minLength":1,"maxLength":160},"observations":{"type":"array","items":{"type":"object"}},"seed":{"type":"integer","minimum":0},"permutations":{"type":"integer","minimum":199,"maximum":4999}},"required":["contract","campaign_result_artifact_id","observations","seed"]})",
        [this](const std::string& args) {
            return analyzeAutonomousExperimentCampaign(args);
        },
        "dpa.experiment.campaign.analyze", "Analyze experiment campaign",
        R"({"type":"object"})", "experiment", "write"
    });
    _registry.add({
        "estimate_autonomous_experiment_campaign_power",
        "对计划中的战役做基于模拟的功效估计：与随机化检验精确配套，含零假设校准自检；结果固化为会话证据工件",
        R"({"type":"object","additionalProperties":false,"properties":{"contract":{"type":"object"},"factor":{"type":"string","minLength":1},"effect_range":{"type":"number","minimum":0},"noise_std":{"type":"number","minimum":0},"seed":{"type":"integer","minimum":0},"alpha":{"type":"number","minimum":0.0001,"maximum":0.5},"simulations":{"type":"integer","minimum":50,"maximum":2000},"permutations":{"type":"integer","minimum":99,"maximum":1999}},"required":["contract","factor","effect_range","noise_std","seed"]})",
        [this](const std::string& args) {
            return estimateAutonomousExperimentCampaignPower(args);
        },
        "dpa.experiment.campaign.power", "Estimate experiment campaign power",
        R"({"type":"object"})", "experiment", "write"
    });
    _registry.add({
        "refine_autonomous_experiment_campaign",
        "把已完成战役的确定性分析结论变成下一轮设计：重要因子在最优水平附近精化，其余固定于最优水平；提议契约自校验并附功效估计，执行仍需新授权",
        R"({"type":"object","additionalProperties":false,"properties":{"contract":{"type":"object"},"campaign_result_artifact_id":{"type":"string","minLength":1,"maxLength":160},"observations":{"type":"array","items":{"type":"object"}},"seed":{"type":"integer","minimum":0},"threshold":{"type":"number","minimum":0.0001,"maximum":100}},"required":["contract","campaign_result_artifact_id","observations","seed"]})",
        [this](const std::string& args) {
            return refineAutonomousExperimentCampaign(args);
        },
        "dpa.experiment.campaign.refine", "Refine experiment campaign design",
        R"({"type":"object"})", "experiment", "write"
    });
    _registry.add({
        "suggest_experiment_refinement_next",
        "只读统计咨询：外部统计 sidecar 用高斯过程期望改进给出下一轮精化的旋钮水平建议；sidecar 不可用或越域时降级为内置确定性网格建议并在 warnings 如实告警",
        R"({"type":"object","additionalProperties":false,"properties":{"contract":{"type":"object"},"observations":{"type":"array","items":{"type":"object"}},"seed":{"type":"integer","minimum":0},"candidate_budget":{"type":"integer","minimum":16,"maximum":1024}},"required":["contract","observations","seed"]})",
        [this](const std::string& args) {
            return suggestExperimentRefinementNext(args);
        },
        "dpa.stats.sidecar.suggest", "Suggest next refinement levels",
        R"({"type":"object"})", "experiment", "read"
    });
    _registry.add({
        "compute_sequential_boundary",
        "只读统计咨询：按 Lan-DeMets 消耗函数（obrien_fleming|pocock）计算成组序贯边界的各次期中名义 p 上限；无内置替代，失败即如实报错",
        R"({"type":"object","additionalProperties":false,"properties":{"design":{"type":"string","enum":["obrien_fleming","pocock"]},"max_interims":{"type":"integer","minimum":1,"maximum":10},"alpha":{"type":"number","minimum":0.0001,"maximum":0.5},"interim_count":{"type":"integer","minimum":1}},"required":["design","max_interims"]})",
        [this](const std::string& args) {
            return computeSequentialBoundary(args);
        },
        "dpa.stats.sidecar.seq_boundary", "Compute sequential boundary",
        R"({"type":"object"})", "experiment", "read"
    });
    _registry.add({
        "sweep_experiment_power_curve",
        "只读功效规划：对同一契约扫描重复数 2..5 的估计功效（与单点功效估计共用确定性引擎），供功效面板一次绘制",
        R"({"type":"object","additionalProperties":false,"properties":{"contract":{"type":"object"},"factor":{"type":"string","minLength":1},"effect_range":{"type":"number","minimum":0},"noise_std":{"type":"number","minimum":0},"seed":{"type":"integer","minimum":0},"alpha":{"type":"number","minimum":0.0001,"maximum":0.5},"simulations":{"type":"integer","minimum":50,"maximum":2000},"permutations":{"type":"integer","minimum":99,"maximum":1999},"replicates_sweep":{"type":"array","maxItems":6}},"required":["contract","factor","effect_range","noise_std","seed"]})",
        [this](const std::string& args) {
            return sweepCampaignPowerCurve(args);
        },
        "dpa.experiment.campaign.power.curve", "Sweep campaign power curve",
        R"({"type":"object"})", "experiment", "read"
    });
    _registry.add({
        "list_experiment_knowledge",
        "读取试验知识账本：历次战役的确定性分析结论（仅来自已验证分析结果，最新在前）",
        R"({"type":"object","additionalProperties":false,"properties":{"max_items":{"type":"integer","minimum":1,"maximum":200}},"required":[]})",
        [this](const std::string& args) {
            return listExperimentKnowledge(args);
        },
        "dpa.experiment.knowledge.list", "List experiment knowledge",
        R"({"type":"object"})", "experiment", "read"
    });
    _registry.add({
        "get_experiment_briefing",
        "读取智能体当前看到的实验简报：工作流剧本与知识摘要（可审计模型上下文）",
        R"({"type":"object","additionalProperties":false,"properties":{},"required":[]})",
        [this](const std::string&) {
            return jsonResult("实验简报已生成。", buildExperimentBriefing());
        },
        "dpa.experiment.briefing", "Get experiment briefing",
        R"({"type":"object"})", "experiment", "read"
    });
    _registry.add({
        "get_monitoring_status",
        "读取持续监控快照、健康状态、心跳和 OpenTelemetry 风格基础指标；只返回元数据",
        R"({"type":"object","additionalProperties":false,"properties":{},"required":[]})",
        [this](const std::string&) {
            return jsonResult("持续监控状态快照已生成。",
                              AgentJson::parseObject(companionStatus()));
        },
        "dpa.monitoring.status", "Read continuous monitoring status", R"({"type":"object"})",
        "monitoring", "read"
    });
    _registry.add({
        "get_monitoring_events",
        "读取当前会话最近的持续监控事件；事件已限长，仅包含元数据和受限摘要",
        R"({"type":"object","additionalProperties":false,"properties":{"max_items":{"type":"integer","minimum":1,"maximum":200}},"required":[]})",
        [this](const std::string& args) {
            const QJsonObject input = AgentJson::parseObject(args);
            int maxItems = input.value("max_items").toInt(50);
            maxItems = std::max(1, std::min(200, maxItems));
            QJsonArray events;
            const int begin = std::max(0, int(_session.companionEvents.size()) - maxItems);
            for (int i = begin; i < int(_session.companionEvents.size()); ++i) {
                const QJsonDocument event = QJsonDocument::fromJson(
                    QByteArray::fromStdString(_session.companionEvents[size_t(i)]));
                if (event.isObject()) events.append(event.object());
            }
            return jsonResult("持续监控事件读取完成。", QJsonObject{
                {"schema", "dpa.monitoring.events/v1"},
                {"count", events.size()},
                {"events", events},
                {"privacy", "metadata_and_bounded_message_only"}
            });
        },
        "dpa.monitoring.events", "Read bounded monitoring event history", R"({"type":"object"})",
        "monitoring", "read"
    });
    _registry.add({
        "list_data", "列出当前分析试验工作区的数据集",
        R"({"type":"object","additionalProperties":false,"properties":{},"required":[]})",
        [this](const std::string&){ return buildListDataResult(snapshotDataListFromTest(_dpaTest)); },
        "dpa.data.list", "列出数据", R"({"type":"object"})", "data", "read"
    });
    _registry.add({
        "list_standard_profiles",
        "列出试验标准知识库（仅官方元数据、适用性和许可状态；未经批准的标准不可直接用于阈值判定）",
        R"({"type":"object","additionalProperties":false,"properties":{},"required":[]})",
        [this](const std::string&) {
            return jsonResult("标准目录已加载；执行批准仍由领域负责人和授权证据控制。",
                              AgentJson::parseObject(standardsCatalogJson()));
        },
        "dpa.standards.catalog", "List governed test standards catalog", R"({"type":"object"})",
        "standards", "read"
    });
    _registry.add({
        "search_standard_catalog",
        "按编号、机构、标题或适用范围检索试验标准目录；只返回治理元数据，不返回受版权保护的全文",
        R"({"type":"object","additionalProperties":false,"properties":{"query":{"type":"string","maxLength":160},"authority":{"type":"string","maxLength":40},"executionStatus":{"type":"string","enum":["catalog_only","execution-approved","controlled_source_required","purchase_required"]}},"required":[]})",
        [this](const std::string& args) {
            const QJsonObject input = AgentJson::parseObject(args);
            const auto entries = searchStandardCatalog(
                input.value("query").toString().toStdString(),
                input.value("authority").toString().toStdString(),
                input.value("executionStatus").toString().toStdString());
            QJsonArray records;
            for (const auto& entry : entries) records.append(standardCatalogEntryJson(entry));
            return jsonResult("标准目录检索完成。", QJsonObject{
                {"count", records.size()},
                {"records", records},
                {"execution_policy", "catalog_only_until_authorized_and_domain_approved"}
            });
        },
        "dpa.standards.catalog.search", "Search governed standards metadata", R"({"type":"object"})",
        "standards", "read"
    });
    _registry.add({
        "assess_standard_applicability",
        "基于领域负责人提供的设备、平台、目的和测量上下文评估标准适用性；不会自动生成试验等级或合格阈值",
        R"({"type":"object","additionalProperties":false,"properties":{"standardProfile":{"type":"string","minLength":1},"machineType":{"type":"string"},"ratedPowerKw":{"type":"number","minimum":0},"rpm":{"type":"number","minimum":0},"platform":{"type":"string"},"testPurpose":{"type":"string"},"physicalQuantity":{"type":"string"},"applicabilityBasis":{"type":"string"},"authorizedCopy":{"type":"boolean"},"ownerApproved":{"type":"boolean"}},"required":["standardProfile"]})",
        [this](const std::string& args) {
            const StandardApplicabilityResult assessment =
                assessStandardApplicability(parseStandardApplicabilityRequest(AgentJson::parseObject(args)));
            return jsonResult("标准适用性评估完成；执行资格仍受授权副本和领域批准控制。",
                              AgentJson::parseObject(standardApplicabilityJson(assessment)));
        },
        "dpa.standards.applicability", "Assess standard applicability", R"({"type":"object"})",
        "standards", "read"
    });
    _registry.add({
        "build_standard_test_plan",
        "为指定设备和试验目的生成标准组合预检，列出适用性、阻塞项、授权和证据缺口；不执行设备动作",
        R"({"type":"object","additionalProperties":false,"properties":{"machineType":{"type":"string","minLength":1},"ratedPowerKw":{"type":"number","minimum":0},"rpm":{"type":"number","minimum":0},"platform":{"type":"string","minLength":1},"testPurpose":{"type":"string","minLength":1},"physicalQuantity":{"type":"string"},"applicabilityBasis":{"type":"string"},"authorizedCopy":{"type":"boolean"},"ownerApproved":{"type":"boolean"},"authorizedEvidenceManifest":{"type":"object"},"requestedStandardProfiles":{"type":"array","minItems":1,"items":{"type":"string","minLength":1}}},"required":["machineType","platform","testPurpose","requestedStandardProfiles"]})",
        [this](const std::string& args) {
            const QJsonObject input = AgentJson::parseObject(args);
            StandardTestPlanRequest request;
            request.machineType = input.value("machineType").toString().toStdString();
            request.ratedPowerKw = input.value("ratedPowerKw").toDouble();
            request.rpm = input.value("rpm").toDouble();
            request.platform = input.value("platform").toString().toStdString();
            request.testPurpose = input.value("testPurpose").toString().toStdString();
            request.physicalQuantity = input.value("physicalQuantity").toString().toStdString();
            request.applicabilityBasis = input.value("applicabilityBasis").toString().toStdString();
            request.authorizedCopy = input.value("authorizedCopy").toBool();
            request.ownerApproved = input.value("ownerApproved").toBool();
            request.experimentContextSha256 = experimentContextSha256(_currentProject, _daqTest);
            for (const auto& value : input.value("requestedStandardProfiles").toArray())
                request.requestedStandardProfiles.push_back(value.toString().toStdString());
            const QJsonObject manifest = input.value("authorizedEvidenceManifest").toObject();
            if (!manifest.isEmpty()) {
                const StandardEvidenceValidation evidenceValidation =
                    validateAuthorizedEvidenceManifestJson(AgentJson::dump(manifest));
                if (!evidenceValidation.ok)
                    return errorResult(evidenceValidation.error,
                                       "授权标准证据清单未通过校验，标准计划保持 review-required。");
                std::set<std::string> covered;
                for (const auto& record : manifest.value("records").toArray())
                    covered.insert(record.toObject().value("standard_id").toString().toStdString());
                for (const auto& profile : request.requestedStandardProfiles) {
                    if (!covered.count(profile))
                        return errorResult("authorized_evidence_coverage_missing",
                                           "授权证据清单未覆盖所请求的标准 profile。");
                }
                request.authorizedEvidenceValidated = true;
                request.authorizedEvidenceManifestSha256 = QCryptographicHash::hash(
                    QJsonDocument(manifest).toJson(QJsonDocument::Compact),
                    QCryptographicHash::Sha256).toHex().toStdString();
            }
            const std::string planJson = buildStandardTestPlanJson(request);
            if (planJson.empty())
                return errorResult("standard_plan_invalid", "标准试验计划上下文未通过有限值校验。");
            const StandardTestPlanValidation validation = validateStandardTestPlanJson(
                planJson, request.experimentContextSha256);
            if (!validation.ok)
                return errorResult(validation.error, "标准试验计划未通过确定性重算校验。");
            _currentStandardPlanJson = planJson;
            const QJsonObject plan = AgentJson::parseObject(planJson);
            return jsonResult("标准试验计划预检完成；请先处理返回的阻塞项和证据缺口。",
                              plan);
        },
        "dpa.standards.test_plan", "Build governed standards test-plan preflight", R"({"type":"object"})",
        "standards", "read"
    });
    _registry.add({
        "get_current_standard_test_plan",
        "读取与当前项目/试验上下文绑定的最近标准试验计划；上下文变化后自动失效",
        R"({"type":"object","additionalProperties":false,"properties":{},"required":[]})",
        [this](const std::string&) {
            if (_currentStandardPlanJson.empty())
                return jsonResult("当前试验尚未绑定标准试验计划。",
                                  QJsonObject{{"present", false},
                                              {"reason", "standard_plan_not_bound"}});
            const StandardTestPlanValidation validation = validateStandardTestPlanJson(
                _currentStandardPlanJson, experimentContextSha256(_currentProject, _daqTest));
            if (!validation.ok) {
                _currentStandardPlanJson.clear();
                return jsonResult("当前标准试验计划已因上下文变化失效。",
                                  QJsonObject{{"present", false},
                                              {"reason", QString::fromStdString(validation.error)}});
            }
            return jsonResult("当前标准试验计划有效。",
                              QJsonObject{{"present", true},
                                          {"plan", AgentJson::parseObject(_currentStandardPlanJson)},
                                          {"plan_sha256", QString::fromStdString(validation.planSha256)},
                                          {"ready", validation.ready}});
        },
        "dpa.standards.test_plan.current", "Read current bound standards plan", R"({"type":"object"})",
        "standards", "read"
    });
    _registry.add({
        "validate_authorized_standard_evidence",
        "校验授权标准证据清单的哈希、受控本地引用和条款级引用；不会读取或上传标准全文",
        R"({"type":"object","additionalProperties":false,"properties":{"manifest":{"type":"object"}},"required":["manifest"]})",
        [](const std::string& args) {
            const QJsonObject input = AgentJson::parseObject(args);
            const QJsonObject manifest = input.value("manifest").toObject();
            if (manifest.isEmpty()) return errorResult("authorized_evidence_manifest_missing", "授权标准证据清单不能为空。");
            const StandardEvidenceValidation validation =
                validateAuthorizedEvidenceManifestJson(AgentJson::dump(manifest));
            return jsonResult(validation.ok
                                  ? "授权标准证据清单校验通过。"
                                  : "授权标准证据清单校验失败；执行资格保持关闭。",
                              AgentJson::parseObject(standardEvidenceValidationJson(validation)));
        },
        "dpa.standards.evidence.validate", "Validate authorized standards evidence manifest",
        R"({"type":"object"})", "standards", "read"
    });
    _registry.add({
        "read_channel", "读取某通道的降采样波形与统计（隐私：仅降采样+统计，不返回原始全量）",
        R"({"type":"object","additionalProperties":false,"properties":{"dataId":{"type":"string","minLength":1},"channel":{"type":"integer","minimum":0},"start":{"type":"number"},"len":{"type":"number","minimum":0},"maxPoints":{"type":"integer","minimum":1,"maximum":5000}},"required":["dataId","channel"]})",
        [this](const std::string& a){ auto args=parseReadChannelArgs(a); return buildReadChannelResult(readChannelFromTest(_dpaTest, args), args); },
        "dpa.data.read_channel", "读取通道", R"({"type":"object"})", "data", "read"
    });
    _registry.add({
        "validate_measurement_contract",
        "校验规范化测量、数据质量和证据引用是否完整绑定；只校验元数据和哈希，不读取原始波形",
        R"({"type":"object","additionalProperties":false,"properties":{"measurement":{"type":"object"},"quality":{"type":"object"},"evidence":{"type":"object"}},"required":["measurement","quality","evidence"]})",
        [](const std::string& args) {
            const QJsonObject input = AgentJson::parseObject(args);
            const std::string measurementJson = AgentJson::dump(input.value("measurement").toObject());
            const std::string qualityJson = AgentJson::dump(input.value("quality").toObject());
            const std::string evidenceJson = AgentJson::dump(input.value("evidence").toObject());
            const ContractValidation validation = validateMeasurementBundleJson(
                measurementJson, qualityJson, evidenceJson);
            if (!validation.ok) {
                ToolResult result = errorResult(validation.error,
                    "规范化测量契约校验失败；该数据不得进入诊断判定。\n");
                result.dataJson = AgentJson::dump(QJsonObject{{"valid", false},
                    {"error", QString::fromStdString(validation.error)}});
                return result;
            }
            const QJsonObject measurement = input.value("measurement").toObject();
            return jsonResult("规范化测量、质量报告和证据引用均已绑定。", QJsonObject{
                {"valid", true},
                {"measurement_id", measurement.value("measurement_id")},
                {"source_data_id", measurement.value("source_data_id")},
                {"sample_count", measurement.value("sample_count")},
                {"privacy", "metadata_and_hashes_only"}
            });
        },
        "dpa.measurement.contract.validate", "Validate canonical measurement bundle",
        R"({"type":"object"})", "data", "read"
    });
    _registry.add({
        "list_measurement_adapters",
        "列出测量适配器边界和真实实现状态；planned/blocked 适配器只作为边界信息，不代表当前可用",
        R"({"type":"object","additionalProperties":false,"properties":{},"required":[]})",
        [](const std::string&) {
            const QJsonObject catalog = AgentJson::parseObject(measurementAdapterCatalogJson());
            return jsonResult("测量适配器目录已加载；只有 implementation_status=implemented 的适配器可用。",
                              catalog);
        },
        "dpa.measurement.adapters.list", "List measurement adapter capabilities",
        R"({"type":"object"})", "data", "read"
    });
    _registry.add({
        "import_mdf_metadata",
        "通过只读外部 asammdf 进程导入 MDF/MF4 元数据、质量和证据；不返回原始波形",
        R"({"type":"object","additionalProperties":false,"properties":{"path":{"type":"string","minLength":1,"maxLength":4096},"channels":{"type":"array","maxItems":128,"items":{"type":"string","minLength":1,"maxLength":256}}},"required":["path"]})",
        [](const std::string& a) { return importMdfMetadata(a); },
        "dpa.measurement.adapters.mdf.import", "Import MDF metadata",
        R"({"type":"object"})", "data", "read"
    });
    _registry.add({
        "execute_analysis_plan",
        "Validate and execute a deterministic read-only analysis plan bound to the current context and tool catalog",
        R"({"type":"object","additionalProperties":false,"properties":{"plan":{"type":"object"}},"required":["plan"]})",
        [this](const std::string& a) {
            const QJsonObject input = AgentJson::parseObject(a);
            const QJsonObject planObject = input.value("plan").toObject();
            if (planObject.isEmpty()) return errorResult("plan_missing", "分析计划不能为空。");
            const std::string expectedContext = experimentContextSha256(_currentProject, _daqTest);
            const AnalysisPlanValidation validation = parseAndValidateAnalysisPlan(
                QJsonDocument(planObject).toJson(QJsonDocument::Compact).toStdString(),
                _registry, expectedContext, false);
            if (!validation.ok) {
                QJsonObject detail{{"step_index", validation.stepIndex}};
                ToolResult result = errorResult(validation.error, "分析计划未通过上下文、工具目录或权限校验。");
                result.dataJson = AgentJson::dump(detail);
                return result;
            }
            const AnalysisPlanExecution execution = executeAnalysisPlan(
                validation.plan, [this](const std::string& tool, const std::string& args) {
                    return executeTool(tool, args, ToolOrigin::AgentSync);
                });
            QJsonArray stepResults;
            for (const ToolResult& stepResult : execution.results)
                stepResults.append(AgentJson::parseObject(stepResult.toJson()));
            QJsonObject data{
                {"plan_id", QString::fromStdString(validation.plan.planId)},
                {"plan_sha256", QString::fromStdString(validation.plan.planSha256)},
                {"completed_steps", execution.completedSteps},
                {"results", stepResults}
            };
            if (!execution.ok) {
                ToolResult result = errorResult(execution.error, "分析计划在确定性执行器中止。");
                result.dataJson = AgentJson::dump(data);
                return result;
            }
            return jsonResult("分析计划已按确定性顺序完成。", data);
        },
        "dpa.analysis.plan.execute", "Execute read-only analysis plan", R"({"type":"object"})",
        "analysis", "read"
    });
    _registry.add({
        "run_analysis", "对当前分析试验执行一种单通道分析(频谱FFT/DFT/PSD/PS/ESD/FSD/RMSSPECTRUM/SPL/SRS/RF、滤波FIR/IIR、微积分DIFF/INTEGR、重采样RESAMP、自相关AUTOC/AUTO、统计AVGPRO/RMSPRO/PEAKPRO/ABSPEAKPRO/PEAKCOEPRO、正弦解调SINEPROCESS 等)",
        R"({"type":"object","additionalProperties":false,"properties":{"type":{"type":"string","enum":["FFT","DFT","ESD","FSD","PSD","PS","RMSSPECTRUM","SPL","SRS","RF","FIR","IIR","DIFF","INTEGR","RESAMP","DELAP","AUTOC","AUTO","AVGPRO","PEAKPRO","ABSPEAKPRO","PEAKCOEPRO","RMSPRO","SINEPROCESS"]},"frameSize":{"type":"integer","enum":[128,256,512,1024,2048,4096,8192,16384,32768]},"averageNum":{"type":"integer","minimum":1,"maximum":99},"overlapIndex":{"type":"integer","minimum":0,"maximum":5}},"required":["type"]})",
        [this](const std::string& a){
            auto args = parseRunAnalysisArgs(a);
            auto validation = validateAnalysisRequest(args);
            AnalysisOutcome outcome;
            if (validation.ok) {
                outcome = runAnalysisOnTest(_dpaTest, args, _pendingAnalysisRunId,
                    [this](M* process, const std::string& runId) {
                        bindPendingAnalysisProcess(process, runId);
                    });
            }
            return buildRunAnalysisResult(validation, outcome, args);
        },
        "dpa.analysis.run", "Run DPA analysis", R"({"type":"object"})", "analysis", "write"
    });
    _registry.add({
        "get_result_summary", "获取当前分析结果的主峰与统计摘要",
        R"({"type":"object","additionalProperties":false,"properties":{},"required":[]})",
        [this](const std::string&){ return buildResultSummaryResult(resultSummaryFromTest(_dpaTest)); },
        "dpa.analysis.summary", "获取分析摘要", R"({"type":"object"})", "analysis", "read"
    });
    _registry.add({
        "generate_report", "基于当前分析试验的真实数据生成 Markdown 报告；提供转速/轴承参数时附加接地诊断结论",
        diagnosticInputSchema(false),
        [this](const std::string& a){
            ReportData data = gatherReportData(_dpaTest);
            if (!AgentJson::parseObject(a).isEmpty()) {
                const DiagnoseConfig config = parseDiagnoseArgs(a);
                const ProfileValidation profile = validateDiagnosticProfile(config.asset, config.measurement);
                if (!profile.valid())
                    return buildDiagnoseResult(diagnoseVibration({}, config));
                const CurrentDiagnosticSpectrum spectrum = currentDiagnosticSpectrum(_dpaTest);
                if (!spectrum.valid) return currentAnalysisError(spectrum.error, "生成诊断报告");
                const std::string bindingError = validateCurrentMeasurementBinding(_dpaTest, config.measurement);
                if (!bindingError.empty()) return measurementBindingError(bindingError);
                const ToolResult diagnosis = diagnoseSpectrum(spectrum.peaks, config);
                if (!diagnosis.ok) return diagnosis;
                data.provenance.standardProfile = config.measurement.standardProfile;
                data.sections.push_back(diagnosticFindingsSection(
                    diagnoseVibration(spectrum.peaks, config)));
            }
            if (!_currentStandardPlanJson.empty()) {
                const StandardTestPlanValidation planValidation = validateStandardTestPlanJson(
                    _currentStandardPlanJson, experimentContextSha256(_currentProject, _daqTest));
                if (!planValidation.ok) {
                    data.provenance.standardPlanDisposition = "standard_plan_invalid_or_context_stale";
                } else {
                    data.provenance.standardPlanSha256 = planValidation.planSha256;
                    data.provenance.standardPlanDisposition = planValidation.ready
                        ? "bound_plan_execution_ready"
                        : "bound_plan_review_required";
                }
            }
            // Baseline deviation section: compare current features against
            // any saved baseline for the same asset/measurement/analysis config.
            {
                const BaselineArgs baselineArgs;
                const MachineBaseline current = currentBaselineFeatures(_dpaTest, baselineArgs.topN);
                if (current.valid) {
                    const std::string key = baselineKey(baselineArgs, current.configFingerprint);
                    if (!key.empty()) {
                        MachineBaseline base;
                        auto it = _baselines.find(key);
                        if (it != _baselines.end()) base = it->second;
                        else { base = restoreBaseline(key); if (base.valid) _baselines[key] = base; }
                        if (base.valid) {
                            const std::string curIso = baselineSeverityZone(baselineArgs);
                            DeviationReport dev = compareToBaseline(base, current.peakFreqsHz, curIso, 0.05);
                            ReportSection sec;
                            sec.title = "基线偏离";
                            sec.body = "基线来源：" + (base.assetId.empty() ? std::string("已保存基线") : base.assetId) + "\n";
                            sec.body += "新增峰数量：" + std::to_string(dev.newPeaksHz.size()) + "\n";
                            if (!dev.newPeaksHz.empty()) {
                                for (double freq : dev.newPeaksHz) {
                                    sec.body += "- " + std::to_string(freq) + " Hz\n";
                                }
                            }
                            if (dev.isoWorsened)
                                sec.body += "ISO 严重度已恶化\n";
                            if (!dev.severity.empty())
                                sec.body += "严重度：" + dev.severity + "\n";
                            if (!dev.summary.empty())
                                sec.body += dev.summary + "\n";
                            if (dev.newPeaksHz.empty() && !dev.isoWorsened)
                                sec.body += "当前频谱与基线一致，未检测到显著偏离。\n";
                            data.sections.push_back(std::move(sec));
                        }
                    }
                }
            }
            return attachReportArtifact(buildReportResult(data), _session.id);
        },
        "dpa.generate_report", "Generate analysis report", R"({"type":"object"})", "artifact", "write"
    });
    _registry.add({
        "artifact_list", "List MCP artifacts produced by the current bridge",
        R"({"type":"object","additionalProperties":false,"properties":{},"required":[]})",
        [this](const std::string&){ return jsonResult("Artifacts listed.", QJsonObject{{"artifacts", listArtifacts(_session.id)}}); },
        "dpa.artifact.list", "List artifacts", R"({"type":"object"})", "artifact", "read"
    });
    _registry.add({"artifact_read", "Read a current-session artifact by opaque id", R"({"type":"object","additionalProperties":false,"properties":{"artifactId":{"type":"string","minLength":1,"maxLength":160}},"required":["artifactId"]})", [this](const std::string& a){ return readArtifact(_session.id, a); }, "dpa.artifact.read", "Read artifact", R"({"type":"object"})", "artifact", "read"});
    _registry.add({"artifact_export", "Export a current-session artifact to an explicitly approved existing directory", R"({"type":"object","additionalProperties":false,"properties":{"artifactId":{"type":"string","minLength":1,"maxLength":160},"destinationPath":{"type":"string","minLength":1}},"required":["artifactId","destinationPath"]})", [this](const std::string& a){ return exportArtifact(_session.id, a); }, "dpa.artifact.export", "Export artifact", R"({"type":"object"})", "artifact", "write"});
    _registry.add({
        "experiment_evidence_create",
        "Create a privacy-minimized, integrity-protected evidence artifact for the current experiment session",
        R"({"type":"object","additionalProperties":false,"properties":{},"required":[]})",
        [this](const std::string&) {
            ExperimentEvidenceSnapshot snapshot;
            snapshot.generatedAtMs = QDateTime::currentMSecsSinceEpoch();
            snapshot.toolCatalogSha256 = _registry.approvalCatalogSha256();
            snapshot.experimentContextSha256 = experimentContextSha256(_currentProject, _daqTest);
            snapshot.preflightJson = AgentJson::dump(experimentPreflightJson(experimentPreflight()));
            snapshot.companionStatusJson = companionStatus();
            snapshot.diagnosticRunJson = _session.diagnosticRunJson.empty()
                ? _diagnosticRun.toJson() : _session.diagnosticRunJson;
            snapshot.standardsPlanJson = _currentStandardPlanJson.empty()
                ? R"({"present":false})" : AgentJson::dump(QJsonObject{
                    {"present", true},
                    {"plan", AgentJson::parseObject(_currentStandardPlanJson)}});
            snapshot.simulatorJson = AgentJson::dump(currentSimulatorEvidence(_daqTest));
            const std::string evidence = buildExperimentEvidenceJson(_session, snapshot);
            if (evidence.empty())
                return errorResult("evidence_build_failed", "Evidence snapshot failed strict validation or size limits.");
            const ExperimentEvidenceVerification verification = verifyExperimentEvidenceJson(
                evidence, snapshot.toolCatalogSha256);
            if (!verification.ok)
                return errorResult(verification.error, "Evidence failed self-verification and was not written.");
            const QString id = writeTextArtifact(
                _session.id, "experiment_evidence", ".json",
                QString::fromUtf8(evidence.data(), static_cast<int>(evidence.size())));
            if (id.isEmpty())
                return errorResult("artifact_write_failed", "Evidence could not be written atomically to the current session.");
            ToolResult result = jsonResult("Experiment evidence was created and self-verified.",
                QJsonObject{{"artifactId", id},
                            {"payloadSha256", QString::fromStdString(verification.payloadSha256)},
                            {"timelineEntries", verification.timelineEntries},
                            {"toolEvents", verification.toolEvents},
                            {"companionEvents", verification.companionEvents},
                            {"omittedEvents", verification.omittedEvents},
                            {"standardsPlanPresent", verification.standardsPlanPresent},
                            {"standardsPlanSha256", QString::fromStdString(verification.standardsPlanSha256)},
                            {"simulatorRecipePresent", verification.simulatorRecipePresent},
                            {"captureMode", "metadata_only"}});
            result.artifactRefs.push_back(id.toStdString());
            return result;
        },
        "dpa.experiment.evidence.create", "Create experiment evidence",
        R"({"type":"object","additionalProperties":false,"properties":{},"required":[]})", "artifact", "write"
    });
    _registry.add({
        "experiment_evidence_verify",
        "Verify and reconstruct a current-session evidence artifact without invoking models, tools, or devices",
        R"({"type":"object","additionalProperties":false,"properties":{"artifactId":{"type":"string","minLength":1,"maxLength":160}},"required":["artifactId"]})",
        [this](const std::string& a) {
            return verifyEvidenceArtifact(_session.id, a, _registry.approvalCatalogSha256());
        },
        "dpa.experiment.evidence.verify", "Verify experiment evidence",
        R"({"type":"object"})", "artifact", "read"
    });
    _registry.add({
        "capture_result_plot", "截取当前分析结果绘图(频谱/波形)为图像供模型查看",
        R"({"type":"object","additionalProperties":false,"properties":{},"required":[]})",
        [](const std::string&) {
            ToolResult r;
            auto cap = GetDefaultService2(iPlotCapture);
            if (!cap) return errorResult("plot_capture_unavailable", "绘图截取服务未就绪。");
            std::string b64 = cap->captureCurrentPngBase64();
            if (b64.empty()) return errorResult("no_current_plot", "当前没有可截取的绘图 cell。");
            ImageArtifact im; im.base64 = b64; im.mediaType = "image/png"; im.caption = "current plot";
            r.images.push_back(im);
            r.summary = "已截取当前绘图为图像。";
            return r;
        },
        "dpa.capture_result_plot", "Capture current plot", R"({"type":"object"})", "analysis", "read"
    });
    _registry.add({
        "diagnose", "对当前分析结果做旋转机械故障诊断(需 rpm，可选轴承几何/ISO)，结论接地到真实峰值",
        diagnosticInputSchema(true),
        [this](const std::string& a){
            const DiagnoseConfig config = parseDiagnoseArgs(a);
            const ProfileValidation profile = validateDiagnosticProfile(config.asset, config.measurement);
            if (!profile.valid()) return buildDiagnoseResult(diagnoseVibration({}, config));
            const CurrentDiagnosticSpectrum spectrum = currentDiagnosticSpectrum(_dpaTest);
            if (!spectrum.valid) return currentAnalysisError(spectrum.error, "执行诊断");
            const std::string bindingError = validateCurrentMeasurementBinding(_dpaTest, config.measurement);
            if (!bindingError.empty()) return measurementBindingError(bindingError);
            return diagnoseSpectrum(spectrum.peaks, config);
        },
        "dpa.diagnose", "Diagnose faults", R"({"type":"object"})", "analysis", "read"
    });
    _registry.add({
        "envelope_spectrum", "对数据集某通道的原始波形做包络解调谱(带通->Hilbert包络->FFT)，返回包络谱主峰(轴承故障BPFO/BPFI诊断关键)",
        R"({"type":"object","additionalProperties":false,"properties":{"dataId":{"type":"string","minLength":1},"channel":{"type":"integer","minimum":0},"bandLoHz":{"type":"number","minimum":0},"bandHiHz":{"type":"number","minimum":0}},"required":["dataId","channel"]})",
        [this](const std::string& a){
            auto o = AgentJson::parseObject(a);
            auto peaks = envelopeSpectrumPeaks(_dpaTest, o.value("dataId").toString().toStdString(),
                o.value("channel").toInt(0), o.value("bandLoHz").toDouble(0.0),
                o.value("bandHiHz").toDouble(0.0), 10);
            return buildPeaksResult(peaks, "包络谱");
        },
        "dpa.envelope_spectrum", "Envelope spectrum", R"({"type":"object"})", "analysis", "read"
    });
    _registry.add({
        "save_baseline", "按资产、测点、方向、工况、物理量和分析配置指纹保存当前已确认正常频谱峰值，供跨会话偏离监测",
        baselineInputSchema(),
        [this](const std::string& a){
            BaselineArgs args = parseBaselineArgs(a);
            MachineBaseline b = currentBaselineFeatures(_dpaTest, args.topN);
            if (!b.valid) return currentAnalysisError(b.error, "记录基线");
            const ToolResult validation = validateBaselineMeasurement(args, b);
            if (!validation.ok) return validation;
            const std::string key = baselineKey(args, b.configFingerprint);
            if (key.empty()) return errorResult("missing_baseline_identity", "基线身份或分析配置指纹不完整。");
            b.assetId = args.assetId;
            b.measurementPoint = args.measurementPoint;
            b.direction = args.direction;
            b.operatingCondition = args.operatingCondition;
            b.physicalQuantity = args.physicalQuantity;
            b.unit = args.unit;
            b.standardProfile = args.standardProfile;
            b.legacyMachineClass = args.legacyIso10816MachineClass;
            b.isoZone = baselineSeverityZone(args);
            b.capturedMs = QDateTime::currentMSecsSinceEpoch();
            MachineBaseline previous;
            auto previousIt = _baselines.find(key);
            if (previousIt != _baselines.end()) previous = previousIt->second;
            else previous = restoreBaseline(key);
            if (previous.valid) {
                b.history = previous.history;
                if (b.history.empty())
                    b.history.push_back(baselineObservation(previous));
            }
            b.history.push_back(baselineObservation(b, &args));
            constexpr size_t maxBaselineHistory = 50;
            if (b.history.size() > maxBaselineHistory)
                b.history.erase(b.history.begin(), b.history.end() - maxBaselineHistory);
            if (!persistBaseline(key, b)) return errorResult("baseline_persist_failed", "基线未能写入本地数据库。");
            _baselines[key] = b;
            return jsonResult("已保存资产基线: " + args.assetId,
                QJsonObject{{"assetId", QString::fromStdString(args.assetId)},
                            {"measurementPoint", QString::fromStdString(args.measurementPoint)},
                            {"direction", QString::fromStdString(args.direction)},
                            {"operatingCondition", QString::fromStdString(args.operatingCondition)},
                            {"standardProfile", QString::fromStdString(args.standardProfile)},
                            {"capturedMs", double(b.capturedMs)},
                            {"observationCount", int(b.history.size())}});
        },
        "dpa.save_baseline", "Save machine baseline", R"({"type":"object"})", "analysis", "write"
    });
    _registry.add({
        "compare_baseline", "按完整测量身份和分析配置读取持久基线，输出新增峰与外部 legacy 严重度偏离；不伪造频谱 RMS",
        baselineInputSchema(),
        [this](const std::string& a){
            BaselineArgs args = parseBaselineArgs(a);
            MachineBaseline cur = currentBaselineFeatures(_dpaTest, args.topN);
            if (!cur.valid) return currentAnalysisError(cur.error, "对比基线");
            const ToolResult validation = validateBaselineMeasurement(args, cur);
            if (!validation.ok) return validation;
            const std::string key = baselineKey(args, cur.configFingerprint);
            if (key.empty()) return errorResult("missing_baseline_identity", "基线身份或分析配置指纹不完整。");
            const std::string curIso = baselineSeverityZone(args);
            MachineBaseline base;
            auto it = _baselines.find(key);
            if (it != _baselines.end()) base = it->second;
            else {
                base = restoreBaseline(key);
                if (base.valid) _baselines[key] = base;
            }
            DeviationReport d = compareToBaseline(base, cur.peakFreqsHz, curIso, 0.05);
            return buildDeviationResult(d);
        },
        "dpa.compare_baseline", "Compare to baseline", R"({"type":"object"})", "analysis", "read"
    });
    _registry.add({
        "baseline_trend", "按完整测量身份和当前分析配置指纹读取最近 50 次已确认基线趋势",
        baselineInputSchema(),
        [this](const std::string& a){
            const BaselineArgs args = parseBaselineArgs(a);
            const MachineBaseline current = currentBaselineFeatures(_dpaTest, args.topN);
            if (!current.valid) return currentAnalysisError(current.error, "读取基线趋势");
            const ToolResult validation = validateBaselineMeasurement(args, current);
            if (!validation.ok) return validation;
            const std::string key = baselineKey(args, current.configFingerprint);
            if (key.empty()) return errorResult("missing_baseline_identity", "基线身份或分析配置指纹不完整。");
            MachineBaseline baseline;
            auto it = _baselines.find(key);
            if (it != _baselines.end()) baseline = it->second;
            else {
                baseline = restoreBaseline(key);
                if (baseline.valid) _baselines[key] = baseline;
            }
            return buildBaselineTrendResult(baseline);
        },
        "dpa.baseline_trend", "Read baseline trend", R"({"type":"object"})", "analysis", "read"
    });
    _registry.add({
        "record_operator_feedback", "记录操作员对诊断结果的评估：误报/漏报/正确/需补充信息/处置/纠正，关联到证据或工具调用",
        R"({"type":"object","additionalProperties":false,"properties":{"correlationId":{"type":"string","maxLength":160},"evidenceRefs":{"type":"array","maxItems":20,"items":{"type":"string","maxLength":160}},"feedbackType":{"type":"string","enum":["false_positive","false_negative","correct","needs_more_information","disposition","correction","other"]},"severity":{"type":"string","enum":["info","concern","critical"],"default":"info"},"summary":{"type":"string","minLength":1,"maxLength":4000},"operatorNote":{"type":"string","maxLength":4000}},"required":["feedbackType","summary"]})",
        [this](const std::string& a){ return recordFeedbackCore(a); },
        "dpa.record_operator_feedback", "Record operator feedback on diagnostics", R"({"type":"object"})", "analysis", "write"
    });
    _registry.add({
        "get_companion_trend",
        "读取陪试通道最近 120 个有效 1 秒统计窗口、时域特征、EWMA 与显示用线性斜率；只读，不改变任何告警决策",
        R"({"type":"object"})",
        [this](const std::string& a){ return getCompanionTrend(a); },
        "dpa.companion.trend", "Read companion channel trends", R"({"type":"object"})",
        "analysis", "read"
    });
    _registry.add({
        "get_companion_history",
        "读取当前会话 SQLite 中的本地陪试分钟汇总历史；可按通道筛选，最多返回每通道 1440 点",
        R"({"type":"object","additionalProperties":false,"properties":{"channel_id":{"type":"integer","minimum":0},"max_points":{"type":"integer","minimum":1,"maximum":1440}},"required":[]})",
        [this](const std::string& a){ return getCompanionHistory(a); },
        "dpa.companion.history", "Read local companion minute history", R"({"type":"object"})",
        "analysis", "read"
    });
    _registry.add({
        "aggregate_operator_feedback",
        "聚合当前会话的操作员反馈记录：按类型与严重度计数，附最近 10 条限长摘要；只读",
        R"({"type":"object"})",
        [this](const std::string& a){ return aggregateOperatorFeedback(a); },
        "dpa.operator_feedback.stats", "Aggregate operator feedback stats", R"({"type":"object"})",
        "analysis", "read"
    });
    _registry.add({
        "list_audit_log",
        "List recent MCP bridge audit log entries (tool call history with timestamp, tool, status, elapsed_ms, client_ip)",
        R"({"type":"object","additionalProperties":false,"properties":{"count":{"type":"integer","minimum":1,"maximum":100}},"required":[]})",
        [](const std::string& args) {
            const QJsonObject input = AgentJson::parseObject(args);
            int count = input.value("count").toInt(20);
            count = std::max(1, std::min(100, count));
            const QJsonArray entries = McpBridgeService::readAuditLog(count);
            QJsonObject data;
            data["entries"] = entries;
            data["count"] = double(entries.size());
            return jsonResult("Audit log entries retrieved.", data);
        },
        "dpa.mcp.audit", "MCP audit log", R"({"type":"object"})",
        "mcp", "read"
    });
    const LlmConfigLoadResult loadedConfig = loadLlmConfigDetailed();
    const LlmConfig& cfg = loadedConfig.config;
    _llmBudget = loadedConfig.budget;

    auto keySourceName = [](ApiKeySource source) {
        switch (source) {
        case ApiKeySource::Environment: return "environment";
        case ApiKeySource::DpapiCurrentUser: return "dpapi_current_user";
        case ApiKeySource::LegacyPlaintext: return "legacy_plaintext";
        case ApiKeySource::None: return "none";
        }
        return "none";
    };
    auto safeEndpoint = [](const std::string& raw) {
        QUrl endpoint(QString::fromStdString(raw));
        if (!endpoint.isValid()) return QStringLiteral("<invalid>");
        endpoint.setUserInfo(QString());
        endpoint.setQuery(QString());
        endpoint.setFragment(QString());
        return endpoint.toString(QUrl::FullyEncoded);
    };

    QJsonArray runtimeRoutes;
    std::vector<RoutedLlmRoute> providerRoutes;
    for (const auto& route : loadedConfig.routes) {
        QJsonObject routeInfo{
            {"id", QString::fromStdString(route.id)},
            {"provider", QString::fromStdString(route.config.provider)},
            {"model", QString::fromStdString(route.config.model)},
            {"base_url", safeEndpoint(route.config.baseUrl)},
            {"enabled", route.enabled},
            {"disabled_reason", QString::fromStdString(route.disabledReason)},
            {"api_key_environment", QString::fromStdString(route.apiKeyEnvironment)},
            {"api_key_source", keySourceName(route.apiKeySource)},
            {"api_key_configured", !route.config.apiKey.empty()},
            {"legacy_plaintext_api_key", route.legacyPlaintextApiKey},
            {"api_key_error", QString::fromStdString(route.apiKeyError)},
            {"pricing_configured", route.pricingConfigured},
            {"pricing_error", QString::fromStdString(route.pricingError)}
        };
        if (route.pricingConfigured) {
            routeInfo["input_cost_micros_per_million_tokens"] =
                double(route.inputCostMicrosPerMillionTokens);
            routeInfo["output_cost_micros_per_million_tokens"] =
                double(route.outputCostMicrosPerMillionTokens);
        }
        runtimeRoutes.append(routeInfo);
        if (!route.enabled) continue;

        RoutedLlmRoute routed;
        routed.id = route.id;
        routed.providerName = route.config.provider;
        routed.model = route.config.model;
        routed.pricingConfigured = route.pricingConfigured;
        routed.inputCostMicrosPerMillionTokens = route.inputCostMicrosPerMillionTokens;
        routed.outputCostMicrosPerMillionTokens = route.outputCostMicrosPerMillionTokens;
        if (route.config.provider == "claude")
            routed.provider.reset(new ClaudeProvider(route.config));
        else
            routed.provider.reset(new OpenAICompatProvider(route.config));
        providerRoutes.push_back(std::move(routed));
    }
    const int enabledProviderRoutes = static_cast<int>(providerRoutes.size());
    _provider = new RoutedLlmProvider(std::move(providerRoutes),
                                      loadedConfig.routing.failureThreshold,
                                      loadedConfig.routing.cooldownMs);
    // UUIDs avoid overwriting a prior session when two sessions are created in the
    // same millisecond or the clock moves backwards.
    _session.id    = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    _session.title = "Agent Chat";
    _mcpBridge = new McpBridgeService(this);
    const bool mcpStarted = _mcpBridge->start();
    QJsonArray configErrors;
    for (const auto& error : loadedConfig.configErrors)
        configErrors.append(QString::fromStdString(error));
    const QJsonArray registeredTools = QJsonDocument::fromJson(
        QByteArray::fromStdString(_registry.listJson())).array();
    _runtimeInfoJson = AgentJson::dump(QJsonObject{
        {"provider", QString::fromStdString(cfg.provider)},
        {"model", QString::fromStdString(cfg.model)},
        {"base_url", safeEndpoint(cfg.baseUrl)},
        {"config_path", QString::fromStdString(loadedConfig.configPath)},
        {"api_key_source", keySourceName(loadedConfig.apiKeySource)},
        {"api_key_configured", !cfg.apiKey.empty()},
        {"legacy_plaintext_api_key", loadedConfig.legacyPlaintextApiKey},
        {"api_key_error", QString::fromStdString(loadedConfig.apiKeyError)},
        {"routing_mode", "priority_fallback"},
        {"provider_routes", runtimeRoutes},
        {"enabled_provider_route_count", enabledProviderRoutes},
        {"routing_policy", QJsonObject{
            {"failure_threshold", loadedConfig.routing.failureThreshold},
            {"cooldown_ms", double(loadedConfig.routing.cooldownMs)}
        }},
        {"turn_budget", QJsonObject{
            {"max_model_calls", loadedConfig.budget.maxModelCalls},
            {"max_input_tokens", double(loadedConfig.budget.maxInputTokens)},
            {"max_output_tokens", double(loadedConfig.budget.maxOutputTokens)},
            {"max_estimated_cost_micros", double(loadedConfig.budget.maxEstimatedCostMicros)}
        }},
        {"config_errors", configErrors},
        {"agent_policy_version", kAgentPolicyVersion},
        {"plan_first", _planFirstEnabled},
        {"tool_count", registeredTools.size()},
        {"tool_catalog_sha256", QString::fromStdString(_registry.approvalCatalogSha256())},
        {"mcp_mode", "read_only"},
        {"mcp_started", mcpStarted},
        {"mcp_manifest_path", mcpStarted ? _mcpBridge->manifestPath() : QString()}
    });
}
AgentServiceImpl::~AgentServiceImpl() {
    if (flushMonitoringHistory(QDateTime::currentMSecsSinceEpoch(), true))
        persistMonitoringHistory();
    stopCompanionStatistics();
    cancelCurrentTurn();
    if (_turnFuture.isRunning()) {
        QFutureWatcher<void> watcher;
        QEventLoop loop;
        QObject::connect(&watcher, &QFutureWatcher<void>::finished, &loop, &QEventLoop::quit);
        watcher.setFuture(_turnFuture);
        if (_turnFuture.isRunning()) loop.exec();
    }
    delete _mcpBridge; _mcpBridge = nullptr;
    delete _provider; _provider = nullptr;
}

bool AgentServiceImpl::persistSession() {
    auto dB = GetDefaultService2(iDataBaseService);
    if (!dB) return false;
    AgentSessionProperty prop;
    prop.id = _session.id;
    prop.name = _session.title;
    prop.lastFixTime = std::to_string(QDateTime::currentMSecsSinceEpoch());
    prop.type = "AgentSession";
    prop.sessionJson = buildSessionJson(_session);
    if (prop.sessionJson.empty()) return false;
    return dB->save("AgentSession", &prop) != 0;
}

void AgentServiceImpl::appendSessionTurn(const std::string& role, const std::string& content) {
    constexpr int maxTurnBytes = 512 * 1024;
    constexpr size_t maxTurns = 200;
    constexpr size_t maxSessionBytes = 4 * 1024 * 1024;
    QByteArray bounded = QByteArray::fromStdString(content);
    bool truncated = false;
    if (bounded.size() > maxTurnBytes) {
        bounded = bounded.left(maxTurnBytes);
        truncated = true;
    }
    std::string safeContent = QString::fromUtf8(bounded).toStdString();
    if (truncated) safeContent += "\n[session turn truncated at 512 KiB]";
    _session.turns.push_back({role, std::move(safeContent)});

    size_t totalBytes = 0;
    for (const SessionTurn& turn : _session.turns)
        totalBytes += turn.role.size() + turn.content.size();
    while (_session.turns.size() > 1
           && (_session.turns.size() > maxTurns || totalBytes > maxSessionBytes)) {
        totalBytes -= _session.turns.front().role.size() + _session.turns.front().content.size();
        _session.turns.erase(_session.turns.begin());
    }
}

void AgentServiceImpl::loadCompanionConfig() {
    QSettings settings(QSettings::NativeFormat, QSettings::UserScope,
                       QStringLiteral("DPA"), QStringLiteral("DPA-2026"));
    settings.beginGroup("AICompanion");
    _companionConfig.enabled = settings.value("enabled", false).toBool();
    _companionSampleIntervalMs = settings.value("sampleIntervalMs", 1000).toInt();
    _companionConfig.sustainedSamples = settings.value("sustainedSamples", 3).toInt();
    _companionConfig.cooldownMs = settings.value("cooldownMs", 60000).toLongLong();
    _companionConfig.escalationMs = settings.value("escalationMs", 1800000).toLongLong();
    _companionSummaryIntervalMs = settings.value("summaryIntervalMs", 300000).toLongLong();
    _companionStaleAfterMs = settings.value("staleAfterMs", 5000).toLongLong();
    _companionConfig.hysteresisRatio = settings.value("hysteresisRatio", 0.02).toDouble();
    _companionAutoExplain = settings.value("autoExplain", true).toBool();
    _companionAutoSummary = settings.value("autoSummary", true).toBool();
    settings.endGroup();

    // Non-persistent packaged smoke-test override; normal launches keep the saved opt-in state.
    if (QCoreApplication::arguments().contains(QStringLiteral("--enable-companion"))) {
        _companionConfig.enabled = true;
    }

    // Clamp legacy/corrupt local settings to the same trust-boundary limits used
    // by configureCompanion().
    _companionSampleIntervalMs = std::max(250, std::min(10000, _companionSampleIntervalMs));
    _companionConfig.sustainedSamples = std::max(1, std::min(10, _companionConfig.sustainedSamples));
    _companionConfig.cooldownMs = std::max(5000LL, std::min(3600000LL, _companionConfig.cooldownMs));
    _companionConfig.escalationMs = std::max(60000LL, std::min(86400000LL, _companionConfig.escalationMs));
    _companionSummaryIntervalMs = std::max(60000LL, std::min(3600000LL, _companionSummaryIntervalMs));
    _companionStaleAfterMs = std::max(1000LL, std::min(60000LL, _companionStaleAfterMs));
    _companionConfig.hysteresisRatio = std::max(0.0, std::min(0.25, _companionConfig.hysteresisRatio));
}

void AgentServiceImpl::saveCompanionConfig() const {
    QSettings settings(QSettings::NativeFormat, QSettings::UserScope,
                       QStringLiteral("DPA"), QStringLiteral("DPA-2026"));
    settings.beginGroup("AICompanion");
    settings.setValue("enabled", _companionConfig.enabled);
    settings.setValue("sampleIntervalMs", _companionSampleIntervalMs);
    settings.setValue("sustainedSamples", _companionConfig.sustainedSamples);
    settings.setValue("cooldownMs", qlonglong(_companionConfig.cooldownMs));
    settings.setValue("escalationMs", qlonglong(_companionConfig.escalationMs));
    settings.setValue("summaryIntervalMs", qlonglong(_companionSummaryIntervalMs));
    settings.setValue("staleAfterMs", qlonglong(_companionStaleAfterMs));
    settings.setValue("hysteresisRatio", _companionConfig.hysteresisRatio);
    settings.setValue("autoExplain", _companionAutoExplain);
    settings.setValue("autoSummary", _companionAutoSummary);
    settings.endGroup();
}

std::string AgentServiceImpl::companionConfig() {
    return AgentJson::dump(QJsonObject{
        {"enabled", _companionConfig.enabled},
        {"sample_interval_ms", _companionSampleIntervalMs},
        {"sustained_samples", _companionConfig.sustainedSamples},
        {"cooldown_ms", double(_companionConfig.cooldownMs)},
        {"escalation_ms", double(_companionConfig.escalationMs)},
        {"summary_interval_ms", double(_companionSummaryIntervalMs)},
        {"stale_after_ms", double(_companionStaleAfterMs)},
        {"hysteresis_ratio", _companionConfig.hysteresisRatio},
        {"auto_explain", _companionAutoExplain},
        {"auto_summary", _companionAutoSummary}
    });
}

std::string AgentServiceImpl::configureCompanion(const std::string& configJson) {
    if (configJson.size() > 16 * 1024)
        return AgentJson::dump(QJsonObject{{"ok", false}, {"error", "config_too_large"}});
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromStdString(configJson), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return AgentJson::dump(QJsonObject{{"ok", false}, {"error", "invalid_json"}});
    const QJsonObject object = document.object();
    const QStringList allowed = {"enabled", "sample_interval_ms", "sustained_samples",
        "cooldown_ms", "escalation_ms", "summary_interval_ms", "stale_after_ms", "hysteresis_ratio",
        "auto_explain", "auto_summary"};
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!allowed.contains(it.key()))
            return AgentJson::dump(QJsonObject{{"ok", false}, {"error", "unknown_field"},
                                               {"field", it.key()}});
    }

    CompanionConfig next = _companionConfig;
    int sampleInterval = _companionSampleIntervalMs;
    long long summaryInterval = _companionSummaryIntervalMs;
    long long staleAfter = _companionStaleAfterMs;
    bool autoExplain = _companionAutoExplain;
    bool autoSummary = _companionAutoSummary;
    QString error;
    auto readInteger = [&](const char* key, long long minimum, long long maximum,
                           long long& target) {
        if (!object.contains(key)) return true;
        const QJsonValue value = object.value(key);
        const double number = value.toDouble(std::numeric_limits<double>::quiet_NaN());
        if (!value.isDouble() || !std::isfinite(number) || std::floor(number) != number
            || number < minimum || number > maximum) {
            error = QString::fromLatin1(key);
            return false;
        }
        target = static_cast<long long>(number);
        return true;
    };
    auto readBool = [&](const char* key, bool& target) {
        if (!object.contains(key)) return true;
        if (!object.value(key).isBool()) { error = QString::fromLatin1(key); return false; }
        target = object.value(key).toBool();
        return true;
    };
    long long sample = sampleInterval;
    long long sustained = next.sustainedSamples;
    long long cooldown = next.cooldownMs;
    long long escalation = next.escalationMs;
    if (!readInteger("sample_interval_ms", 250, 10000, sample)
        || !readInteger("sustained_samples", 1, 10, sustained)
        || !readInteger("cooldown_ms", 5000, 3600000, cooldown)
        || !readInteger("escalation_ms", 60000, 86400000, escalation)
        || !readInteger("summary_interval_ms", 60000, 3600000, summaryInterval)
        || !readInteger("stale_after_ms", 1000, 60000, staleAfter)
        || !readBool("auto_explain", autoExplain)
        || !readBool("auto_summary", autoSummary)
        || !readBool("enabled", next.enabled)) {
        return AgentJson::dump(QJsonObject{{"ok", false}, {"error", "invalid_field"},
                                           {"field", error}});
    }
    if (object.contains("hysteresis_ratio")) {
        const QJsonValue value = object.value("hysteresis_ratio");
        const double ratio = value.toDouble(std::numeric_limits<double>::quiet_NaN());
        if (!value.isDouble() || !std::isfinite(ratio) || ratio < 0 || ratio > 0.25)
            return AgentJson::dump(QJsonObject{{"ok", false}, {"error", "invalid_field"},
                                               {"field", "hysteresis_ratio"}});
        next.hysteresisRatio = ratio;
    }
    sampleInterval = int(sample);
    next.sustainedSamples = int(sustained);
    next.cooldownMs = cooldown;
    next.escalationMs = escalation;
    _companionConfig = next;
    _companionSampleIntervalMs = sampleInterval;
    _companionSummaryIntervalMs = summaryInterval;
    _companionStaleAfterMs = staleAfter;
    _companionAutoExplain = autoExplain;
    _companionAutoSummary = autoSummary;
    saveCompanionConfig();
    if (_companionTimer) _companionTimer->setInterval(_companionSampleIntervalMs);
    refreshCompanionRuntime(true);
    const QJsonObject saved = QJsonDocument::fromJson(
        QByteArray::fromStdString(companionConfig())).object();
    return AgentJson::dump(QJsonObject{{"ok", true}, {"config", saved}});
}

std::string AgentServiceImpl::acknowledgeCompanionAlarm(const std::string& argsJson) {
    if (argsJson.size() > 8 * 1024)
        return AgentJson::dump(QJsonObject{{"ok", false}, {"error", "args_too_large"}});
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromStdString(argsJson), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return AgentJson::dump(QJsonObject{{"ok", false}, {"error", "invalid_json"}});
    const QJsonObject object = document.object();
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (it.key() != QStringLiteral("alarm_id") && it.key() != QStringLiteral("note"))
            return AgentJson::dump(QJsonObject{{"ok", false}, {"error", "unknown_field"},
                                               {"field", it.key()}});
    }
    const QString alarmId = object.value("alarm_id").toString();
    if (!object.value("alarm_id").isString() || alarmId.isEmpty() || alarmId.size() > 128)
        return AgentJson::dump(QJsonObject{{"ok", false}, {"error", "invalid_alarm_id"}});
    for (const QChar character : alarmId) {
        if (character.isLetterOrNumber() || character == '-' || character == '_'
            || character == ':') continue;
        return AgentJson::dump(QJsonObject{{"ok", false}, {"error", "invalid_alarm_id"}});
    }
    const QString note = object.value("note").toString();
    if (object.contains("note") && (!object.value("note").isString()
        || note.toUtf8().size() > 1024))
        return AgentJson::dump(QJsonObject{{"ok", false}, {"error", "invalid_note"}});

    int channelId = -1;
    int occurrenceCount = 0;
    bool alreadyAcknowledged = false;
    for (const auto& channel : _companionState.channels) {
        const CompanionConditionState* conditions[] = {
            &channel.second.high, &channel.second.low,
            &channel.second.stale, &channel.second.invalid
        };
        for (const CompanionConditionState* condition : conditions) {
            if (!condition->active || condition->alarmId != alarmId.toStdString()) continue;
            channelId = channel.first;
            occurrenceCount = condition->occurrenceCount;
            alreadyAcknowledged = condition->acknowledged;
            break;
        }
        if (channelId >= 0) break;
    }
    if (channelId < 0)
        return AgentJson::dump(QJsonObject{{"ok", false}, {"error", "alarm_not_active"}});
    if (!alreadyAcknowledged) {
        const long long now = QDateTime::currentMSecsSinceEpoch();
        if (!Model::acknowledgeCompanionAlarm(_companionState, alarmId.toStdString(), now))
            return AgentJson::dump(QJsonObject{{"ok", false}, {"error", "alarm_not_active"}});
        const std::string message = note.isEmpty() ? "操作员已确认收到该活动告警。"
            : ("操作员已确认收到该活动告警。备注：" + note.toStdString());
        publishCompanionEvent("info", "alarm_acknowledged", "活动告警已确认",
                              message, false, channelId, 0, 0, "",
                              alarmId.toStdString(), "acknowledged",
                              occurrenceCount, true);
    }
    return AgentJson::dump(QJsonObject{{"ok", true}, {"alarm_id", alarmId},
                                       {"acknowledged", true},
                                       {"already_acknowledged", alreadyAcknowledged}});
}

void AgentServiceImpl::setCompanionEnabled(bool enabled) {
    if (_companionConfig.enabled == enabled) {
        publishCompanionStatus();
        return;
    }
    _companionConfig.enabled = enabled;
    saveCompanionConfig();
    refreshCompanionRuntime(true);
}

void AgentServiceImpl::appendCompanionEvent(const std::string& eventJson) {
    if (eventJson.size() > 64 * 1024) return;
    QJsonParseError error;
    const QJsonDocument event = QJsonDocument::fromJson(
        QByteArray::fromStdString(eventJson), &error);
    if (error.error != QJsonParseError::NoError || !event.isObject()) return;
    _session.companionEvents.push_back(eventJson);
    constexpr size_t maxEvents = 200;
    while (_session.companionEvents.size() > maxEvents)
        _session.companionEvents.erase(_session.companionEvents.begin());
    persistSession();
}

void AgentServiceImpl::stopCompanionStatistics() {
    for (auto& task : _companionStatTasks) {
        auto* data = dynamic_cast<iData*>(task.data.data());
        if (!data || task.channelIds.empty()) continue;
        Property ids(&task.channelIds);
        data->excute("StopStatisticCalc", &ids);
    }
    _companionStatTasks.clear();
}

void AgentServiceImpl::restartCompanionStatistics() {
    stopCompanionStatistics();
    _companionState = CompanionState{};
    if (!_companionConfig.enabled || !_daqRunning || !_daqWorkSpace || !_daqChannelSettings)
        return;

    std::set<int> monitored;
    for (auto* entry : _daqChannelSettings->getChildren()) {
        auto* channel = entry ? (M*)entry->self() : nullptr;
        if (!channel || !channel->value<bool>("IsDaq") || !channel->value<bool>("Ismonitored"))
            continue;
        monitored.insert(channel->value<int>("ChannelId"));
    }
    const long long now = QDateTime::currentMSecsSinceEpoch();
    for (auto* entry : _daqWorkSpace->getChildren()) {
        auto* data = entry ? dynamic_cast<iData*>((M*)entry->self()) : nullptr;
        if (!data) continue;
        CompanionStatTask task;
        task.data = data;
        const int channelCount = data->childrenCount("Channels");
        for (int i = 0; i < channelCount; ++i) {
            const int channelId = data->value<int>("Channels", i, "ChannelId");
            if (monitored.count(channelId)) task.channelIds.push_back(channelId);
        }
        if (task.channelIds.empty()) continue;
        Property ids(&task.channelIds);
        data->excute("StartStatisticCalc", &ids);
        for (int channelId : task.channelIds) {
            const int index = data->channelIndex(channelId);
            if (index < 0) continue;
            task.lastSequence[channelId] = data->value<__int64>(
                "Channels", index, "StatisticsSequence");
            task.lastAdvancedMs[channelId] = now;
        }
        _companionStatTasks.push_back(std::move(task));
    }
}

void AgentServiceImpl::refreshCompanionRuntime(bool rebuildStatistics) {
    const bool shouldMonitor = _companionConfig.enabled && _daqTest && _daqRunning;
    if (!shouldMonitor) {
        if (_companionTimer) _companionTimer->stop();
        stopCompanionStatistics();
        if (_companionRunStartedMs > 0) {
            const long long stoppedAt = QDateTime::currentMSecsSinceEpoch();
            const long long elapsed = std::max(0LL, stoppedAt - _companionRunStartedMs);
            if (flushMonitoringHistory(stoppedAt, true)) persistMonitoringHistory();
            publishCompanionEvent("info", "run_finished", "陪试监控结束",
                "本次监控持续 " + std::to_string(elapsed / 1000)
                    + " 秒，记录 " + std::to_string(_companionEventCount) + " 个事件。",
                _companionConfig.enabled && _companionAutoSummary);
            _companionRunStartedMs = 0;
            _companionLastSummaryMs = 0;
            _companionState = CompanionState{};
            _companionDeviceEventLastMs.clear();
        }
        publishCompanionStatus();
        return;
    }

    if (_companionRunStartedMs == 0) {
        _companionRunStartedMs = QDateTime::currentMSecsSinceEpoch();
        _companionFirstStartMs = _companionRunStartedMs;
        _companionLastSummaryMs = _companionRunStartedMs;
        _companionLastPollMs = 0;
        _companionLastDataMs = 0;
        _companionLastEventMs = 0;
        _companionLastSampleMs = 0;
        _companionPollCount = 0;
        _companionSampleCount = 0;
        _companionInvalidWindowCount = 0;
        _companionMissedWindowCount = 0;
        _companionEventCount = 0;
        _companionState = CompanionState{};
        _companionDeviceEventLastMs.clear();
        _channelTrends.clear();
        _monitoringBuckets.clear();
        rebuildStatistics = true;
        publishCompanionEvent("info", "run_started", "智能陪试已开始",
                              "正在本地监测启用通道、设备事件和数据连续性。", false);
    }
    if (rebuildStatistics) restartCompanionStatistics();
    if (_companionTimer) {
        _companionTimer->setInterval(_companionSampleIntervalMs);
        if (!_companionTimer->isActive()) _companionTimer->start();
    }
    publishCompanionStatus();
}

void AgentServiceImpl::pollCompanion() {
    if (!_companionConfig.enabled || !_daqRunning) {
        refreshCompanionRuntime(false);
        return;
    }
    struct ChannelMeta {
        std::string name;
        std::string unit;
        bool limitEnabled = false;
        double low = 0;
        double high = 0;
    };
    std::map<int, ChannelMeta> channels;
    if (_daqChannelSettings) {
        for (auto* entry : _daqChannelSettings->getChildren()) {
            auto* channel = entry ? (M*)entry->self() : nullptr;
            if (!channel || !channel->value<bool>("IsDaq")
                || !channel->value<bool>("Ismonitored")) continue;
            ChannelMeta meta;
            meta.name = channel->value<std::string>("Name");
            meta.unit = channel->value<std::string>("Unit");
            meta.limitEnabled = channel->value<bool>("IsLimit");
            meta.low = channel->value<float>("LowLimit");
            meta.high = channel->value<float>("UpLimit");
            channels[channel->value<int>("ChannelId")] = meta;
        }
    }

    const long long now = QDateTime::currentMSecsSinceEpoch();
    _companionLastPollMs = now;
    ++_companionPollCount;
    bool historyDirty = false;
    for (auto& task : _companionStatTasks) {
        auto* data = dynamic_cast<iData*>(task.data.data());
        if (!data) continue;
        const int count = data->childrenCount("Channels");
        for (int i = 0; i < count; ++i) {
            const int channelId = data->value<int>("Channels", i, "ChannelId");
            const auto meta = channels.find(channelId);
            if (meta == channels.end()) continue;
            const long long sequence = data->value<__int64>(
                "Channels", i, "StatisticsSequence");
            auto sequenceIt = task.lastSequence.find(channelId);
            if (sequenceIt == task.lastSequence.end()) {
                task.lastSequence[channelId] = sequence;
                task.lastAdvancedMs[channelId] = now;
                continue;
            }
            const long long previousSequence = sequenceIt->second;
            StatisticWindowReadRequest request;
            request.channelId = channelId;
            request.afterSequence = previousSequence;
            const bool bufferedWindows = data->excute(
                "ReadStatisticWindows", &request) != 0;
            std::vector<StatisticWindowSnapshot> windows;
            if (bufferedWindows) {
                windows = std::move(request.windows);
            } else if (sequence != previousSequence) {
                StatisticWindowSnapshot latest;
                latest.sequence = sequence;
                latest.completedAtMs = now;
                latest.count = data->value<__int64>("Channels", i, "StatisticsCount");
                latest.valid = data->value<bool>("Channels", i, "StatisticsValid");
                latest.hasX = data->value<bool>("Channels", i, "StatisticsHasX");
                latest.startX = data->value<double>("Channels", i, "StatisticsStartX");
                latest.endX = data->value<double>("Channels", i, "StatisticsEndX");
                latest.value = data->value<double>("Channels", i, "Value");
                latest.average = data->value<double>("Channels", i, "Avg");
                latest.rms = data->value<double>("Channels", i, "Rms");
                latest.stddev = data->value<double>("Channels", i, "StdDev");
                latest.meanAbs = data->value<double>("Channels", i, "MeanAbs");
                latest.minimum = data->value<double>("Channels", i, "Min");
                latest.maximum = data->value<double>("Channels", i, "Max");
                latest.peak = data->value<double>("Channels", i, "Peak");
                latest.peakPeak = data->value<double>("Channels", i, "PeakPeak");
                latest.crestFactor = data->value<double>("Channels", i, "CrestFactor");
                latest.shapeFactor = data->value<double>("Channels", i, "ShapeFactor");
                latest.impulseFactor = data->value<double>("Channels", i, "ImpulseFactor");
                latest.kurtosis = data->value<double>("Channels", i, "Kurtosis");
                windows.push_back(latest);
            }

            if (!windows.empty()) {
                if (windows.front().sequence > previousSequence + 1)
                    _companionMissedWindowCount += windows.front().sequence - previousSequence - 1;
                sequenceIt->second = windows.back().sequence;
                const long long latestCompletedMs = windows.back().completedAtMs > 0
                    && windows.back().completedAtMs <= now ? windows.back().completedAtMs : now;
                task.lastAdvancedMs[channelId] = latestCompletedMs;
                _companionLastDataMs = std::max(_companionLastDataMs, latestCompletedMs);
            } else if (bufferedWindows && request.latestSequence > previousSequence) {
                _companionMissedWindowCount += request.latestSequence - previousSequence;
                sequenceIt->second = request.latestSequence;
                task.lastAdvancedMs[channelId] = now;
                _companionLastDataMs = now;
            }
            const long long lastAdvanced = task.lastAdvancedMs[channelId];
            const bool stale = now - lastAdvanced >= _companionStaleAfterMs;
            const std::string channelName = meta->second.name.empty()
                ? data->value<std::string>("Channels", i, "Name") : meta->second.name;
            const long long timestampStart = data->value<__int64>(
                "Channels", i, "TimestampStart");
            for (const auto& window : windows) {
                CompanionChannelSample sample;
                sample.channelId = channelId;
                sample.name = channelName;
                sample.unit = meta->second.unit;
                sample.value = window.value;
                sample.minimum = window.minimum;
                sample.maximum = window.maximum;
                sample.hasValue = window.valid && std::isfinite(sample.value)
                    && std::isfinite(sample.minimum) && std::isfinite(sample.maximum);
                sample.qualityInvalid = !sample.hasValue;
                sample.limitEnabled = meta->second.limitEnabled;
                sample.lowLimit = meta->second.low;
                sample.highLimit = meta->second.high;

                if (sample.hasValue) ++_companionSampleCount;
                else ++_companionInvalidWindowCount;
                const long long observedMs = window.completedAtMs > 0
                    && window.completedAtMs <= now ? window.completedAtMs : now;
                if (sample.hasValue) {
                    _companionLastSampleMs = std::max(_companionLastSampleMs, observedMs);
                    ChannelTrend& trend = _channelTrends[channelId];
                    TrendPoint point;
                    point.observedMs = observedMs;
                    point.sequence = window.sequence;
                    point.sampleCount = window.count;
                    point.sourceAxisValid = window.hasX;
                    point.sourceStartX = window.startX;
                    point.sourceEndX = window.endX;
                    if (point.sourceAxisValid)
                        point.sourceTimestampUs = sampleTimestampUs(timestampStart, point.sourceEndX);
                    point.value = sample.value;
                    point.average = window.average;
                    point.rms = window.rms;
                    point.stddev = window.stddev;
                    point.minimum = sample.minimum;
                    point.maximum = sample.maximum;
                    point.peak = window.peak;
                    point.peakPeak = window.peakPeak;
                    point.crestFactor = window.crestFactor;
                    point.shapeFactor = window.shapeFactor;
                    point.impulseFactor = window.impulseFactor;
                    point.kurtosis = window.kurtosis;
                    trend.points.push_back(point);
                    if (trend.points.size() > kTrendCap)
                        trend.points.erase(trend.points.begin());
                    trend.lastMs = observedMs;
                    _channelStreamStats[channelId].push(sample.value);
                    if (_channelTimeWindows.find(channelId) == _channelTimeWindows.end())
                        _channelTimeWindows.emplace(channelId, utils::makeDefaultAggregator());
                    _channelTimeWindows[channelId].push(sample.value, observedMs);
                    _channelSampleBuffers[channelId].push(sample.value);
                    historyDirty = appendMonitoringHistory(channelId, point) || historyDirty;
                }
                const auto decisions = evaluateCompanionChannels(
                    {sample}, _companionConfig, observedMs, _companionState);
                for (const auto& decision : decisions) publishCompanionDecision(decision);
                if (sample.hasValue && sample.limitEnabled) {
                    double slopePerMin = std::numeric_limits<double>::quiet_NaN();
                    const auto& trend = _channelTrends[channelId];
                    if (trend.points.size() >= 2) {
                        const size_t n = (std::min)(trend.points.size(), size_t(120));
                        const size_t start = trend.points.size() - n;
                        double sumX = 0, sumY = 0, sumXY = 0, sumXX = 0;
                        for (size_t k = 0; k < n; ++k) {
                            const double x = static_cast<double>(k);
                            const double y = trend.points[start + k].value;
                            sumX += x; sumY += y;
                            sumXY += x * y; sumXX += x * x;
                        }
                        const double denom = static_cast<double>(n) * sumXX - sumX * sumX;
                        if (std::abs(denom) > 1e-12) {
                            slopePerMin = (static_cast<double>(n) * sumXY - sumX * sumY) / denom;
                            slopePerMin *= 60.0 / (_companionSampleIntervalMs > 0 ? _companionSampleIntervalMs / 1000.0 : 1.0);
                        }
                    }
                    const auto alerts = _alertEngine.evaluate(
                        std::to_string(channelId),
                        sample.value, sample.minimum, sample.maximum,
                        sample.highLimit, sample.lowLimit,
                        window.kurtosis, slopePerMin,
                        observedMs, stale, sample.qualityInvalid);
                    for (const auto& alert : alerts) {
                        publishCompanionEvent(
                            alert.severity == AlertEngine::Severity::Critical ? "alarm"
                                : alert.severity == AlertEngine::Severity::Warning ? "warning" : "info",
                            alert.alarmId,
                            alert.description,
                            std::string(), false);
                    }
                }
            }

            if (stale) {
                CompanionChannelSample sample;
                sample.channelId = channelId;
                sample.name = channelName;
                sample.unit = meta->second.unit;
                sample.value = data->value<double>("Channels", i, "Value");
                sample.minimum = data->value<double>("Channels", i, "Min");
                sample.maximum = data->value<double>("Channels", i, "Max");
                sample.limitEnabled = meta->second.limitEnabled;
                sample.lowLimit = meta->second.low;
                sample.highLimit = meta->second.high;
                sample.stale = true;
                const auto decisions = evaluateCompanionChannels(
                    {sample}, _companionConfig, now, _companionState);
                for (const auto& decision : decisions) publishCompanionDecision(decision);
            }
        }
    }
    if (historyDirty) persistMonitoringHistory();

    if (_companionAutoSummary && _companionSummaryIntervalMs > 0
        && now - _companionLastSummaryMs >= _companionSummaryIntervalMs) {
        _companionLastSummaryMs = now;
        int active = 0;
        for (const auto& item : _companionState.channels) {
            active += item.second.high.active ? 1 : 0;
            active += item.second.low.active ? 1 : 0;
            active += item.second.stale.active ? 1 : 0;
            active += item.second.invalid.active ? 1 : 0;
        }
        publishCompanionEvent("info", "periodic_summary", "陪试阶段摘要",
            "已配置监测 " + std::to_string(channels.size()) + " 个通道，当前活动告警 "
                + std::to_string(active) + " 个。", true);
    }
    publishCompanionStatus();
}

ToolResult AgentServiceImpl::getCompanionTrend(const std::string& args) {
    (void)args;   // read-only snapshot; input intentionally ignored
    QJsonArray channels;
    for (const auto& entry : _channelTrends) {
        const int channelId = entry.first;
        const ChannelTrend& trend = entry.second;
        if (trend.points.empty()) continue;
        QString channelName;
        QString channelUnit;
        bool limitEnabled = false;
        double limitLow = 0;
        double limitHigh = 0;
        if (_daqChannelSettings) {
            for (auto* settingsEntry : _daqChannelSettings->getChildren()) {
                auto* channel = settingsEntry ? (M*)settingsEntry->self() : nullptr;
                if (!channel || channel->value<int>("ChannelId") != channelId) continue;
                channelName = QString::fromStdString(channel->value<std::string>("Name"));
                channelUnit = QString::fromStdString(channel->value<std::string>("Unit"));
                limitEnabled = channel->value<bool>("IsLimit");
                limitLow = channel->value<float>("LowLimit");
                limitHigh = channel->value<float>("UpLimit");
                break;
            }
        }
        std::string status = "normal";
        auto stateIt = _companionState.channels.find(channelId);
        if (stateIt != _companionState.channels.end()) {
            if (stateIt->second.high.active || stateIt->second.low.active)
                status = "alarm";
            else if (stateIt->second.stale.active || stateIt->second.invalid.active)
                status = "warn";
        }
        const size_t take = std::min<size_t>(120, trend.points.size());
        QJsonArray series;
        QJsonArray points;
        double ewma = trend.points[trend.points.size() - take].value;
        double meanMinutes = 0;
        double meanValue = 0;
        const long long firstMs = trend.points[trend.points.size() - take].observedMs;
        for (size_t i = trend.points.size() - take; i < trend.points.size(); ++i) {
            const TrendPoint& point = trend.points[i];
            ewma = i == trend.points.size() - take ? point.value
                : 0.2 * point.value + 0.8 * ewma;
            const double minutes = double(point.observedMs - firstMs) / 60000.0;
            meanMinutes += minutes;
            meanValue += point.value;
            series.append(point.value);
            points.append(QJsonObject{
                {"observed_ms", double(point.observedMs)},
                {"source_timestamp_us", point.sourceTimestampUs > 0
                    ? QJsonValue(double(point.sourceTimestampUs)) : QJsonValue(QJsonValue::Null)},
                {"source_start_s", point.sourceAxisValid
                    ? QJsonValue(point.sourceStartX) : QJsonValue(QJsonValue::Null)},
                {"source_end_s", point.sourceAxisValid
                    ? QJsonValue(point.sourceEndX) : QJsonValue(QJsonValue::Null)},
                {"sequence", double(point.sequence)},
                {"sample_count", double(point.sampleCount)},
                {"value", point.value},
                {"avg", point.average},
                {"rms", point.rms},
                {"stddev", point.stddev},
                {"min", point.minimum},
                {"max", point.maximum},
                {"peak", point.peak},
                {"peak_peak", point.peakPeak},
                {"crest_factor", point.crestFactor},
                {"shape_factor", point.shapeFactor},
                {"impulse_factor", point.impulseFactor},
                {"kurtosis", point.kurtosis}
            });
        }
        meanMinutes /= take;
        meanValue /= take;
        double numerator = 0;
        double denominator = 0;
        for (size_t i = trend.points.size() - take; i < trend.points.size(); ++i) {
            const TrendPoint& point = trend.points[i];
            const double minutes = double(point.observedMs - firstMs) / 60000.0;
            numerator += (minutes - meanMinutes) * (point.value - meanValue);
            denominator += (minutes - meanMinutes) * (minutes - meanMinutes);
        }
        const double slopePerMinute = denominator > 0 ? numerator / denominator : 0;
        const TrendPoint& latest = trend.points.back();
        const QString updatedAt =
            QDateTime::fromMSecsSinceEpoch(trend.lastMs).toString("HH:mm:ss");
        channels.append(QJsonObject{
            {"channel_id", channelId},
            {"name", channelName},
            {"current", latest.value},
            {"unit", channelUnit},
            {"status", QString::fromStdString(status)},
            {"updated_at", updatedAt},
            {"limit_enabled", limitEnabled},
            {"limit_lo", limitLow},
            {"limit_hi", limitHigh},
            {"ewma", ewma},
            {"slope_per_minute", slopePerMinute},
            {"trend_assessment", "display_only_no_qualified_baseline"},
            {"source_time_quality", latest.sourceTimestampUs > 0
                ? "channel_start_plus_sample_axis"
                : (latest.sourceAxisValid ? "relative_sample_axis" : "observation_clock_only")},
            {"trend", series},
            {"points", points}
        });
    }
    QJsonObject payload{
        {"ok", true},
        {"schema", QStringLiteral("dpa.companion.trend/v2")},
        {"window_seconds", 1},
        {"ewma_lambda", 0.2},
        {"channels", channels}
    };
    if (channels.isEmpty())
        payload["note"] = QStringLiteral("no_trend_data_yet");
    return jsonResult("陪试通道趋势已读取。", payload);
}

bool AgentServiceImpl::appendMonitoringHistory(int channelId, const TrendPoint& point) {
    const bool flushed = flushMonitoringHistory(point.observedMs, false);
    const long long minuteMs = point.observedMs / 60000 * 60000;
    MonitoringBucket& bucket = _monitoringBuckets[channelId];
    if (bucket.count == 0) {
        bucket.minuteMs = minuteMs;
        bucket.minimum = point.minimum;
        bucket.maximum = point.maximum;
    }
    bucket.count++;
    bucket.sumAverage += point.average;
    bucket.sumRms += point.rms;
    bucket.minimum = std::min(bucket.minimum, point.minimum);
    bucket.maximum = std::max(bucket.maximum, point.maximum);
    bucket.peak = std::max(bucket.peak, point.peak);
    bucket.sumKurtosis += point.kurtosis;
    return flushed;
}

bool AgentServiceImpl::flushMonitoringHistory(long long throughMs, bool includePartial) {
    const long long currentMinute = throughMs / 60000 * 60000;
    bool changed = false;
    for (auto it = _monitoringBuckets.begin(); it != _monitoringBuckets.end();) {
        const MonitoringBucket& bucket = it->second;
        // A wall-clock correction must start a new bucket instead of mixing two minutes.
        if (bucket.count == 0 || (!includePartial && bucket.minuteMs == currentMinute)) {
            ++it;
            continue;
        }
        MonitoringHistoryPoint point;
        point.minuteMs = bucket.minuteMs;
        point.count = bucket.count;
        point.average = bucket.sumAverage / bucket.count;
        point.rms = bucket.sumRms / bucket.count;
        point.minimum = bucket.minimum;
        point.maximum = bucket.maximum;
        point.peak = bucket.peak;
        point.kurtosis = bucket.sumKurtosis / bucket.count;
        auto& history = _monitoringHistory[it->first];
        history.push_back(point);
        if (history.size() > kHistoryCapPerChannel) history.erase(history.begin());
        it = _monitoringBuckets.erase(it);
        changed = true;
    }

    size_t total = 0;
    for (const auto& channel : _monitoringHistory) total += channel.second.size();
    // ponytail: compact row storage is capped at 20k minute points; use a normalized
    // historian table only when validated retention/channel-count requirements exceed it.
    while (total > kHistoryCapTotal) {
        auto oldest = _monitoringHistory.end();
        for (auto it = _monitoringHistory.begin(); it != _monitoringHistory.end(); ++it) {
            if (it->second.empty()) continue;
            if (oldest == _monitoringHistory.end()
                || it->second.front().minuteMs < oldest->second.front().minuteMs)
                oldest = it;
        }
        if (oldest == _monitoringHistory.end()) break;
        oldest->second.erase(oldest->second.begin());
        --total;
    }
    return changed;
}

static std::string monitoringRecordId(const std::string& sessionId) {
    return "monitoring:" + sessionId;
}

bool AgentServiceImpl::persistMonitoringHistory() const {
    auto dB = GetDefaultService2(iDataBaseService);
    if (!dB || _session.id.empty()) return false;
    QJsonArray channels;
    for (const auto& channel : _monitoringHistory) {
        QJsonArray points;
        for (const MonitoringHistoryPoint& point : channel.second) {
            points.append(QJsonArray{
                double(point.minuteMs), double(point.count), point.average,
                point.rms, point.minimum, point.maximum, point.peak, point.kurtosis
            });
        }
        channels.append(QJsonObject{{"channel_id", channel.first}, {"points", points}});
    }
    const QByteArray serialized = QJsonDocument(QJsonObject{
        {"schema", "dpa.monitoring.history/v1"},
        {"session_id", QString::fromStdString(_session.id)},
        {"resolution_ms", 60000},
        {"channels", channels}
    }).toJson(QJsonDocument::Compact);
    if (serialized.size() > 8 * 1024 * 1024) return false;

    AgentSessionProperty prop;
    prop.id = monitoringRecordId(_session.id);
    prop.name = "Local monitoring minute history";
    prop.lastFixTime = std::to_string(QDateTime::currentMSecsSinceEpoch());
    prop.type = "MonitoringHistory";
    prop.monitoringJson = serialized.toStdString();
    return dB->save("AgentSession", &prop) != 0;
}

void AgentServiceImpl::restoreMonitoringHistory() {
    _monitoringBuckets.clear();
    _monitoringHistory.clear();
    auto dB = GetDefaultService2(iDataBaseService);
    if (!dB || _session.id.empty()) return;
    AgentSessionProperty prop;
    DataBaseCondition id;
    id._filed = "Id";
    id._value1 = monitoringRecordId(_session.id);
    if (!dB->load("AgentSession", &prop, {id}) || prop.type != "MonitoringHistory"
        || prop.monitoringJson.empty() || prop.monitoringJson.size() > 8 * 1024 * 1024)
        return;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromStdString(prop.monitoringJson), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return;
    const QJsonObject root = document.object();
    if (root.value("schema").toString() != QStringLiteral("dpa.monitoring.history/v1")
        || root.value("session_id").toString().toStdString() != _session.id
        || !root.value("channels").isArray()) return;

    std::map<int, std::vector<MonitoringHistoryPoint>> restored;
    size_t total = 0;
    const QJsonArray channels = root.value("channels").toArray();
    if (channels.size() > 512) return;
    for (const QJsonValue& channelValue : channels) {
        if (!channelValue.isObject()) return;
        const QJsonObject channel = channelValue.toObject();
        const QJsonValue idValue = channel.value("channel_id");
        if (!idValue.isDouble() || std::floor(idValue.toDouble()) != idValue.toDouble()
            || idValue.toDouble() < 0 || idValue.toDouble() > std::numeric_limits<int>::max()
            || !channel.value("points").isArray()) return;
        const int channelId = int(idValue.toDouble());
        if (restored.count(channelId)) return;
        const QJsonArray points = channel.value("points").toArray();
        if (points.size() > int(kHistoryCapPerChannel)) return;
        auto& output = restored[channelId];
        for (const QJsonValue& pointValue : points) {
            if (!pointValue.isArray()) return;
            const QJsonArray values = pointValue.toArray();
            if (values.size() != 8) return;
            for (const QJsonValue& value : values)
                if (!value.isDouble() || !std::isfinite(value.toDouble())) return;
            if (std::floor(values[0].toDouble()) != values[0].toDouble()
                || std::floor(values[1].toDouble()) != values[1].toDouble()
                || values[0].toDouble() < 0 || values[0].toDouble() > 9007199254740991.0
                || values[1].toDouble() < 1 || values[1].toDouble() > 1000000000.0) return;
            output.push_back({
                static_cast<long long>(values[0].toDouble()),
                static_cast<long long>(values[1].toDouble()),
                values[2].toDouble(), values[3].toDouble(), values[4].toDouble(),
                values[5].toDouble(), values[6].toDouble(), values[7].toDouble()
            });
            if (++total > kHistoryCapTotal) return;
        }
    }
    _monitoringHistory = std::move(restored);
}

ToolResult AgentServiceImpl::getCompanionHistory(const std::string& args) {
    if (args.size() > 4096) return errorResult("args_too_large", "历史查询参数过大。");
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromStdString(args.empty() ? "{}" : args), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return errorResult("invalid_json", "历史查询参数不是 JSON 对象。");
    const QJsonObject input = document.object();
    for (auto it = input.begin(); it != input.end(); ++it) {
        if (it.key() != QStringLiteral("channel_id") && it.key() != QStringLiteral("max_points"))
            return errorResult("unknown_field", "历史查询包含未知字段。");
    }
    int selectedChannel = -1;
    int maxPoints = 1440;
    auto readInteger = [&](const char* key, int minimum, int maximum, int& target) {
        if (!input.contains(key)) return true;
        const QJsonValue value = input.value(key);
        const double number = value.toDouble(std::numeric_limits<double>::quiet_NaN());
        if (!value.isDouble() || !std::isfinite(number) || std::floor(number) != number
            || number < minimum || number > maximum) return false;
        target = int(number);
        return true;
    };
    if (!readInteger("channel_id", 0, std::numeric_limits<int>::max(), selectedChannel)
        || !readInteger("max_points", 1, 1440, maxPoints))
        return errorResult("invalid_field", "channel_id 或 max_points 超出允许范围。");

    QJsonArray channels;
    size_t total = 0;
    for (const auto& channel : _monitoringHistory) {
        total += channel.second.size();
        if (selectedChannel >= 0 && channel.first != selectedChannel) continue;
        QJsonArray points;
        const size_t take = std::min<size_t>(maxPoints, channel.second.size());
        for (size_t i = channel.second.size() - take; i < channel.second.size(); ++i) {
            const MonitoringHistoryPoint& point = channel.second[i];
            points.append(QJsonObject{
                {"minute_ms", double(point.minuteMs)}, {"window_count", double(point.count)},
                {"mean", point.average}, {"mean_window_rms", point.rms},
                {"min", point.minimum}, {"max", point.maximum},
                {"peak", point.peak}, {"mean_kurtosis", point.kurtosis}
            });
        }
        channels.append(QJsonObject{{"channel_id", channel.first}, {"points", points}});
    }
    QJsonObject payload{
        {"ok", true}, {"schema", "dpa.monitoring.history/v1"},
        {"resolution_ms", 60000}, {"channels", channels},
        {"stored_points", double(total)},
        {"retention_ceiling", QJsonObject{{"per_channel_points", 1440},
                                           {"total_points", 20000}}}
    };
    if (channels.isEmpty()) payload["note"] = "no_finalized_history_yet";
    return jsonResult("本地陪试分钟历史已读取。", payload);
}

void AgentServiceImpl::publishCompanionDecision(const CompanionDecision& decision) {
    const QString channel = QString::fromStdString(decision.channelName).left(256);
    const QString value = QString::number(decision.value, 'g', 8);
    const QString limit = QString::number(decision.limit, 'g', 8);
    const QString unit = QString::fromStdString(decision.unit).left(64);
    QString title;
    QString message;
    if (decision.kind == "limit_high") {
        title = channel + QString::fromUtf8(" 上限超限");
        message = QString::fromUtf8("1 秒窗口最大值 %1 %2 超过上限 %3 %2，已连续 %4 次确认。")
            .arg(value, unit, limit).arg(_companionConfig.sustainedSamples);
    } else if (decision.kind == "limit_low") {
        title = channel + QString::fromUtf8(" 下限超限");
        message = QString::fromUtf8("1 秒窗口最小值 %1 %2 低于下限 %3 %2，已连续 %4 次确认。")
            .arg(value, unit, limit).arg(_companionConfig.sustainedSamples);
    } else if (decision.kind == "data_stale") {
        title = channel + QString::fromUtf8(" 数据中断");
        message = QString::fromUtf8("超过 %1 毫秒未检测到新数据，请检查设备连接和采集状态。")
            .arg(_companionStaleAfterMs);
    } else if (decision.kind == "data_invalid") {
        title = channel + QString::fromUtf8(" 数据质量异常");
        message = QString::fromUtf8("1 秒统计窗口包含非有限值或不可用统计量；该窗口未参与限值判断。");
    } else {
        title = channel + QString::fromUtf8(" 已恢复");
        message = decision.kind == "data_stale_recovered"
            ? QString::fromUtf8("已重新检测到连续数据。")
            : decision.kind == "data_invalid_recovered"
                ? QString::fromUtf8("统计窗口已恢复为有效有限值。")
            : QString::fromUtf8("读数已越过滞回带并恢复到配置限值内。");
    }
    if (decision.lifecycle == "escalated")
        message += QString::fromUtf8(" 告警持续未确认，已在软件端升级；系统未自动停机。");
    publishCompanionEvent(decision.severity, decision.kind,
        title.toStdString(), message.toStdString(), !decision.recovery && _companionAutoExplain,
        decision.channelId, decision.value, decision.limit, decision.unit,
        decision.alarmId, decision.lifecycle, decision.occurrenceCount,
        decision.acknowledged);
}

void AgentServiceImpl::publishCompanionEvent(const std::string& severity,
    const std::string& kind, const std::string& title, const std::string& message,
    bool autoExplain, int channelId, double value, double limit, const std::string& unit,
    const std::string& alarmId, const std::string& lifecycle,
    int occurrenceCount, bool acknowledged) {
    const long long eventNow = QDateTime::currentMSecsSinceEpoch();
    QJsonObject event{
        {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"timestamp_ms", double(eventNow)},
        {"source", "local_monitor"},
        {"severity", QString::fromStdString(severity).left(16)},
        {"kind", QString::fromStdString(kind).left(64)},
        {"title", QString::fromStdString(title).left(512)},
        {"message", QString::fromStdString(message).left(4096)},
        {"auto_explain", autoExplain}
    };
    if (channelId >= 0) {
        event["channel_id"] = channelId;
        event["value"] = std::isfinite(value) ? value : 0;
        event["limit"] = std::isfinite(limit) ? limit : 0;
        event["unit"] = QString::fromStdString(unit).left(64);
    }
    if (!alarmId.empty()) {
        event["alarm_id"] = QString::fromStdString(alarmId).left(128);
        event["lifecycle"] = QString::fromStdString(lifecycle).left(32);
        event["occurrence_count"] = occurrenceCount;
        event["acknowledged"] = acknowledged;
    }
    const std::string json = AgentJson::dump(event);
    _companionLastEventMs = eventNow;
    ++_companionEventCount;
    appendCompanionEvent(json);
    emit companionEvent(QString::fromStdString(json));
    publishCompanionStatus();
}

void AgentServiceImpl::handleCompanionDeviceEvent(const std::string& index,
                                                   utils::Property* prop) {
    if (!_companionConfig.enabled || _companionRunStartedMs == 0) return;
    std::string device = prop ? prop->value<std::string>("Name") : std::string();
    if (device.empty()) device = "采集设备";
    device = QString::fromStdString(device).left(256).toStdString();
    std::string detail;
    if (prop && (index == "Current_Error" || index == "Current_Info"))
        detail = prop->value<std::string>(index);
    detail = QString::fromStdString(detail).left(4096).toStdString();
    const auto decision = describeCompanionDeviceEvent(index, device, detail, _daqConnected);
    if (!decision.valid) return;
    const long long now = QDateTime::currentMSecsSinceEpoch();
    const std::string key = index + "|" + device + "|" + decision.message;
    auto previous = _companionDeviceEventLastMs.find(key);
    if (previous != _companionDeviceEventLastMs.end()
        && now - previous->second < _companionConfig.cooldownMs) return;
    _companionDeviceEventLastMs[key] = now;
    publishCompanionEvent(decision.severity, decision.kind, decision.title, decision.message,
                          decision.explain && _companionAutoExplain);
}

ExperimentPreflight AgentServiceImpl::experimentPreflight() const {
    ExperimentPreflightInput input;
    input.hasProject = _currentProject != nullptr;
    input.hasDaqTest = _daqTest != nullptr;
    input.connected = _daqConnected;
    input.acquiring = _daqRunning;
    input.saving = _daqSaving;
    input.companionEnabled = _companionConfig.enabled;

    if (_daqChannelSettings) {
        for (auto* entry : _daqChannelSettings->getChildren()) {
            auto* channel = entry ? (M*)entry->self() : nullptr;
            if (!channel || !channel->value<bool>("IsDaq")) continue;
            ++input.selectedChannels;
            if (!channel->value<bool>("Ismonitored")) continue;
            ++input.monitoredChannels;
            const double low = channel->value<float>("LowLimit");
            const double high = channel->value<float>("UpLimit");
            if (channel->value<bool>("IsLimit") && std::isfinite(low)
                && std::isfinite(high) && low < high)
                ++input.validLimitChannels;
        }
    }
    if (_daqWorkSpace) {
        for (auto* entry : _daqWorkSpace->getChildren()) {
            auto* model = entry ? (M*)entry->self() : nullptr;
            if (model && dynamic_cast<iData*>(model)) ++input.dataSources;
        }
    }
    for (const auto& task : _companionStatTasks) {
        if (task.data)
            input.activeMonitoredChannels += int(task.channelIds.size());
    }
    return evaluateExperimentPreflight(input);
}

std::string AgentServiceImpl::companionStatus() {
    const long long now = QDateTime::currentMSecsSinceEpoch();
    int monitored = 0;
    for (const auto& task : _companionStatTasks) monitored += int(task.channelIds.size());
    int active = 0;
    int criticalAlerts = 0;
    int unacknowledgedAlerts = 0;
    int staleChannels = 0;
    int invalidChannels = 0;
    QJsonArray activeAlarms;
    for (const auto& item : _companionState.channels) {
        auto appendAlarm = [&](const CompanionConditionState& condition,
                               const char* kind, const char* baseSeverity) {
            if (!condition.active) return;
            ++active;
            if (!condition.acknowledged) ++unacknowledgedAlerts;
            if (std::string(baseSeverity) == "alarm" || condition.escalated)
                ++criticalAlerts;
            activeAlarms.append(QJsonObject{
                {"alarm_id", QString::fromStdString(condition.alarmId)},
                {"channel_id", item.first},
                {"kind", kind},
                {"severity", condition.escalated ? "critical" : baseSeverity},
                {"activated_ms", double(condition.activatedMs)},
                {"last_fired_ms", double(condition.lastFiredMs)},
                {"occurrence_count", condition.occurrenceCount},
                {"acknowledged", condition.acknowledged},
                {"acknowledged_ms", double(condition.acknowledgedMs)}
            });
        };
        appendAlarm(item.second.high, "limit_high", "alarm");
        appendAlarm(item.second.low, "limit_low", "alarm");
        appendAlarm(item.second.stale, "data_stale", "warning");
        appendAlarm(item.second.invalid, "data_invalid", "warning");
        staleChannels += item.second.stale.active ? 1 : 0;
        invalidChannels += item.second.invalid.active ? 1 : 0;
    }
    QString state = "off";
    if (_companionConfig.enabled && !_daqTest) state = "unavailable";
    else if (_companionConfig.enabled && !_daqRunning) state = "waiting";
    else if (_companionConfig.enabled && _daqRunning && monitored == 0) state = "unavailable";
    else if (_companionConfig.enabled && _daqRunning) state = "monitoring";
    const long long pollAge = _companionLastPollMs > 0
        ? std::max(0LL, now - _companionLastPollMs) : 0;
    const long long dataAge = _companionLastDataMs > 0
        ? std::max(0LL, now - _companionLastDataMs) : 0;
    const long long eventAge = _companionLastEventMs > 0
        ? std::max(0LL, now - _companionLastEventMs) : 0;
    const ExperimentPreflight preflight = experimentPreflight();
    MonitoringHealthInput healthInput;
    healthInput.enabled = _companionConfig.enabled;
    healthInput.hasDaqTest = _daqTest != nullptr;
    healthInput.connected = _daqConnected;
    healthInput.acquiring = _daqRunning;
    healthInput.saving = _daqSaving;
    healthInput.monitoredChannels = preflight.snapshot.monitoredChannels;
    healthInput.activeMonitoredChannels = monitored;
    healthInput.activeAlerts = criticalAlerts;
    healthInput.invalidChannels = invalidChannels;
    healthInput.dataFresh = monitored > 0 && staleChannels == 0 && _companionLastDataMs > 0;
    healthInput.dataAgeMs = dataAge;
    healthInput.staleAfterMs = _companionStaleAfterMs;
    const MonitoringHealth health = evaluateMonitoringHealth(healthInput);
    QJsonArray reasons;
    for (const auto& reason : health.reasons)
        reasons.append(QString::fromStdString(reason));
    // Companion health: compare last sample time against current time.
    QString companionHealth = "disconnected";
    if (_companionConfig.enabled && _companionRunStartedMs > 0) {
        const long long sampleAge = _companionLastSampleMs > 0
            ? std::max(0LL, now - _companionLastSampleMs) : 0;
        const long long staleThreshold = std::max(_companionSampleIntervalMs * 2LL, _companionStaleAfterMs);
        if (_companionLastSampleMs > 0 && sampleAge <= staleThreshold)
            companionHealth = "healthy";
        else
            companionHealth = "stale";
    }
    const long long uptimeMs = _companionFirstStartMs > 0
        ? std::max(0LL, now - _companionFirstStartMs) : 0;
    const long long elapsedMs = _companionRunStartedMs > 0
        ? std::max(0LL, now - _companionRunStartedMs) : 0;
    const double windowRateHz = elapsedMs > 0
        ? double(_companionSampleCount) * 1000.0 / elapsedMs : 0;
    const double pollRateHz = elapsedMs > 0
        ? double(_companionPollCount) * 1000.0 / elapsedMs : 0;
    const QString lastSampleAt = _companionLastSampleMs > 0
        ? QDateTime::fromMSecsSinceEpoch(_companionLastSampleMs).toString("HH:mm:ss") : QString();
    const QJsonObject healthJson{
        {"state", QString::fromStdString(health.state)},
        {"severity", QString::fromStdString(health.severity)},
        {"live", health.live},
        {"ready", health.ready},
        {"reasons", reasons},
        {"companion", companionHealth}
    };
    const QJsonObject metrics{
        {"dpa.monitoring.enabled", _companionConfig.enabled ? 1 : 0},
        {"dpa.monitoring.connected", _daqConnected ? 1 : 0},
        {"dpa.monitoring.acquiring", _daqRunning ? 1 : 0},
        {"dpa.monitoring.saving", _daqSaving ? 1 : 0},
        {"dpa.monitoring.monitored_channels", monitored},
        {"dpa.monitoring.active_alerts", active},
        {"dpa.monitoring.unacknowledged_alerts", unacknowledgedAlerts},
        {"dpa.monitoring.stale_channels", staleChannels},
        {"dpa.monitoring.invalid_channels", invalidChannels},
        {"dpa.monitoring.event_count", _companionEventCount},
        {"dpa.monitoring.valid_window_count", double(_companionSampleCount)},
        {"dpa.monitoring.invalid_window_count", double(_companionInvalidWindowCount)},
        {"dpa.monitoring.missed_window_count", double(_companionMissedWindowCount)},
        {"dpa.monitoring.heartbeat_age_ms", double(pollAge)},
        {"dpa.monitoring.data_age_ms", double(dataAge)}
    };
    return AgentJson::dump(QJsonObject{
        {"schema", "dpa.monitoring.status/v2"},
        {"captured_at_ms", double(now)},
        {"enabled", _companionConfig.enabled}, {"state", state},
        {"connected", _daqConnected}, {"saving", _daqSaving},
        {"acquiring", _daqRunning},
        {"monitored_channels", monitored}, {"active_monitored_channels", monitored},
        {"active_alerts", active},
        {"unacknowledged_alerts", unacknowledgedAlerts},
        {"active_alarms", activeAlarms},
        {"stale_channels", staleChannels}, {"invalid_channels", invalidChannels},
        {"run_started_ms", double(_companionRunStartedMs)},
        {"elapsed_ms", double(elapsedMs)},
        {"event_count", _companionEventCount},
        {"last_poll_ms", double(_companionLastPollMs)},
        {"poll_age_ms", double(pollAge)},
        {"last_data_ms", double(_companionLastDataMs)},
        {"data_age_ms", double(dataAge)},
        {"last_event_ms", double(_companionLastEventMs)},
        {"event_age_ms", double(eventAge)},
        {"uptime_ms", double(uptimeMs)},
        {"sample_count", _companionSampleCount},
        {"invalid_window_count", double(_companionInvalidWindowCount)},
        {"missed_window_count", double(_companionMissedWindowCount)},
        {"poll_count", double(_companionPollCount)},
        {"window_rate_hz", windowRateHz},
        {"monitor_refresh_rate_hz", pollRateHz},
        {"last_sample_ms", double(_companionLastSampleMs)},
        {"last_sample_at", lastSampleAt},
        {"health", healthJson},
        {"metrics", metrics},
        {"telemetry", QJsonObject{
            {"model", "structured_json"},
            {"semantic_conventions", "opentelemetry-style"},
            {"exporter", "local_read_only_api"},
            {"privacy", "metadata_only"}
        }},
        {"preflight", experimentPreflightJson(preflight)}
    });
}

void AgentServiceImpl::publishCompanionStatus() {
    const std::string status = companionStatus();
    if (status == _lastCompanionStatusJson) return;
    _lastCompanionStatusJson = status;
    emit companionStatusChanged(QString::fromStdString(status));
}

static std::string baselineRecordId(const std::string& key) {
    const QByteArray digest = QCryptographicHash::hash(
        QByteArray::fromStdString(key), QCryptographicHash::Sha256).toHex();
    return "baseline:" + digest.toStdString();
}

bool AgentServiceImpl::persistBaseline(const std::string& key, const MachineBaseline& baseline) {
    auto dB = GetDefaultService2(iDataBaseService);
    if (!dB || key.empty() || !baseline.valid) return false;
    AgentSessionProperty prop;
    prop.id = baselineRecordId(key);
    prop.name = baseline.assetId + "/" + baseline.measurementPoint + "/" + baseline.direction;
    prop.lastFixTime = std::to_string(baseline.capturedMs);
    prop.type = "MachineBaseline";
    prop.baselineJson = serializeBaseline(baseline);
    return dB->save("AgentSession", &prop) != 0;
}

MachineBaseline AgentServiceImpl::restoreBaseline(const std::string& key) const {
    auto dB = GetDefaultService2(iDataBaseService);
    if (!dB || key.empty()) return {};
    AgentSessionProperty prop;
    DataBaseCondition id;
    id._filed = "Id";
    id._value1 = baselineRecordId(key);
    if (!dB->load("AgentSession", &prop, {id})) return {};
    return parseBaseline(prop.baselineJson);
}

static bool isAnalysisRunTool(const std::string& name) {
    return name == "run_analysis" || name == "dpa.analysis.run";
}

static bool isDiagnoseTool(const std::string& name) {
    return name == "diagnose" || name == "dpa.diagnose";
}

static bool isReportTool(const std::string& name) {
    return name == "generate_report" || name == "dpa.generate_report";
}

static bool consumesPublishedAnalysisResult(const std::string& name) {
    return isAnalysisRunTool(name)
        || name == "get_result_summary" || name == "dpa.analysis.summary"
        || name == "capture_result_plot" || name == "dpa.capture_result_plot"
        || isDiagnoseTool(name) || isReportTool(name)
        || name == "save_baseline" || name == "dpa.save_baseline"
        || name == "compare_baseline" || name == "dpa.compare_baseline"
        || name == "baseline_trend" || name == "dpa.baseline_trend"
        || name == "record_operator_feedback" || name == "dpa.record_operator_feedback";
}

static QString pointerToken(const void* pointer) {
    return QString::number(reinterpret_cast<quintptr>(pointer), 16);
}

static std::string currentTurnContextFingerprint(M* project, M* dpaTest) {
    QJsonObject context;
    context["project_ptr"] = pointerToken(project);
    context["test_ptr"] = pointerToken(dpaTest);
    if (project) {
        context["project_id"] = QString::fromStdString(project->value<std::string>("Id"));
        context["project_name"] = QString::fromStdString(project->value<std::string>("Name"));
    }
    if (dpaTest) {
        context["test_id"] = QString::fromStdString(dpaTest->value<std::string>("Id"));
        context["test_name"] = QString::fromStdString(dpaTest->value<std::string>("Name"));
        M* process = dpaTest->value<M*>("PlotSet", "CurrentLayout", "CurrentCell", "Process");
        context["process_ptr"] = pointerToken(process);
        if (process) {
            context["process_type"] = QString::fromStdString(process->value<std::string>("ProcessType"));
            context["agent_run_id"] = QString::fromStdString(process->value<std::string>("AgentRunId"));
            M* source = process->value<M*>("Units", 0, "SourceSelect", "Data");
            context["source_ptr"] = pointerToken(source);
            context["source_data_id"] = source
                ? QString::fromStdString(source->value<std::string>("Id")) : QString();
            context["source_channel"] = process->value<int>("Units", 0, "SourceSelect", "ChannelId");
            context["published_result_ptr"] = pointerToken(
                process->value<M*>("PublishedResultData"));
        }
    }
    return QCryptographicHash::hash(QJsonDocument(context).toJson(QJsonDocument::Compact),
                                    QCryptographicHash::Sha256).toHex().toStdString();
}

void AgentServiceImpl::beginTurnContext() {
    QMutexLocker lock(&_turnContextMutex);
    _turnContextFingerprint = currentTurnContextFingerprint(_currentProject, _dpaTest);
}

void AgentServiceImpl::refreshTurnContext() {
    QMutexLocker lock(&_turnContextMutex);
    if (!_turnContextFingerprint.empty())
        _turnContextFingerprint = currentTurnContextFingerprint(_currentProject, _dpaTest);
}

void AgentServiceImpl::clearTurnContext() {
    QMutexLocker lock(&_turnContextMutex);
    _turnContextFingerprint.clear();
}

bool AgentServiceImpl::turnContextMatches() const {
    QMutexLocker lock(&_turnContextMutex);
    return !_turnContextFingerprint.empty()
        && _turnContextFingerprint == currentTurnContextFingerprint(_currentProject, _dpaTest);
}

void AgentServiceImpl::publishDiagnosticRun() {
    _session.diagnosticRunJson = _diagnosticRun.toJson();
    persistSession();
    emit agentStep(QStringLiteral("run_state"),
                   QString::fromLatin1(diagnosticRunStateName(_diagnosticRun.state())),
                   QString::fromStdString(_session.diagnosticRunJson));
}

std::string AgentServiceImpl::currentAnalysisContextFingerprint() const {
    QMutexLocker lock(&_turnContextMutex);
    return currentTurnContextFingerprint(_currentProject, _dpaTest);
}

std::string AgentServiceImpl::currentStandardPlanJson() const {
    return _currentStandardPlanJson;
}

ToolResult AgentServiceImpl::autonomousExperimentStatus() {
    return jsonResult("自主试验状态快照已生成。", QJsonObject{
        {"schema", "dpa.experiment.status/v1"},
        {"run_id", QString::fromStdString(_autonomousRunId)},
        {"state", QString::fromStdString(_autonomousState)},
        {"active", _autonomousExecutionActive},
        {"started_ms", double(_autonomousStartedMs)},
        {"finished_ms", double(_autonomousFinishedMs)},
        {"duration_ms", double(_autonomousDurationMs)},
        {"stop_reason", QString::fromStdString(_autonomousStopReason)},
        {"plan_sha256", QString::fromStdString(_autonomousPlanSha256)},
        {"monitoring", AgentJson::parseObject(companionStatus())}
    });
}

bool AgentServiceImpl::waitForAutonomousAnalysis(long long timeoutMs) {
    if (_diagnosticRun.state() != DiagnosticRunState::AnalysisPending)
        return _diagnosticRun.state() == DiagnosticRunState::AnalysisComplete;
    QEventLoop loop;
    QTimer tick;
    QTimer timeout;
    tick.setInterval(50);
    timeout.setSingleShot(true);
    const auto check = [&]() {
        if (_diagnosticRun.state() != DiagnosticRunState::AnalysisPending)
            loop.quit();
    };
    QObject::connect(&tick, &QTimer::timeout, &loop, check);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    tick.start();
    timeout.start(static_cast<int>(std::max<long long>(1, std::min<long long>(timeoutMs, 300000))));
    check();
    if (_diagnosticRun.state() == DiagnosticRunState::AnalysisPending)
        loop.exec();
    tick.stop();
    timeout.stop();
    return _diagnosticRun.state() == DiagnosticRunState::AnalysisComplete;
}

ToolResult AgentServiceImpl::runAutonomousExperiment(const QJsonObject& plan) {
    if (_autonomousExecutionActive)
        return errorResult("autonomous_experiment_busy", "已有自主试验正在执行。请先停止或等待当前运行结束。");
    if (plan.isEmpty())
        return errorResult("autonomous_plan_missing", "自主试验计划不能为空。");

    const qint64 durationMs = static_cast<qint64>(plan.value("duration_ms").toDouble());
    if (durationMs < 1000 || durationMs > 600000)
        return errorResult("autonomous_duration_invalid", "自主试验时长必须在 1 秒到 10 分钟之间。");
    const std::string analysisType = plan.value("analysis_type").toString().toStdString();
    const std::set<std::string> analysisTypes{"FFT", "PSD", "DFT", "RMSPRO", "AVGPRO", "PEAKPRO"};
    if (!analysisTypes.count(analysisType))
        return errorResult("autonomous_analysis_type_invalid", "自主试验分析类型不在受控白名单内。");
    if (!_daqTest)
        return errorResult("no_daq_test", "当前没有可执行采集试验。");

    iTest* test = reinterpret_cast<iTest*>(_daqTest);
    const bool previousCompanion = _companionConfig.enabled;
    const bool autoConnect = plan.value("auto_connect").toBool(false);
    const bool save = plan.value("save").toBool(true);
    const bool stopOnWarning = plan.value("stop_on_warning").toBool(false);
    const bool standardRequired = plan.value("standard_plan_required").toBool(false);
    bool daqStarted = false;
    bool saveStarted = false;

    auto syncDaqState = [&]() {
        _daqRunning = test->isRunning();
        _daqSaving = test->isSaving();
        _daqConnected = _daqTest->value<bool>("IsConnected");
    };
    auto stopAcquisition = [&]() {
        syncDaqState();
        if (_daqSaving) test->stopSave();
        if (_daqRunning) test->stopDaq();
        syncDaqState();
        daqStarted = false;
        saveStarted = false;
    };
    auto finishError = [&](const std::string& code, const std::string& message) {
        stopAcquisition();
        _autonomousExecutionActive = false;
        _autonomousState = "failed";
        _autonomousStopReason = code;
        _autonomousFinishedMs = QDateTime::currentMSecsSinceEpoch();
        if (!previousCompanion) setCompanionEnabled(false);
        publishCompanionStatus();
        return errorResult(code, message);
    };

    if (autoConnect) {
        syncDaqState();
        if (!_daqConnected && test->connectAllDevice() <= 0)
            return finishError("device_connect_failed", "自主试验无法连接采集设备。");
        syncDaqState();
    }
    ExperimentPreflight preflight = experimentPreflight();
    if (!preflight.readyToAcquire) {
        ToolResult result = finishError("autonomous_preflight_blocked", "自主试验预检未通过，未执行设备动作。");
        result.dataJson = AgentJson::dump(experimentPreflightJson(preflight));
        return result;
    }
    if (standardRequired) {
        if (_currentStandardPlanJson.empty())
            return finishError("standard_plan_required", "该自主试验要求已绑定且有效的标准试验计划。");
        const StandardTestPlanValidation validation = validateStandardTestPlanJson(
            _currentStandardPlanJson, experimentContextSha256(_currentProject, _daqTest));
        if (!validation.ok || !validation.ready)
            return finishError("standard_plan_not_ready", "绑定的标准试验计划未通过执行就绪校验。");
    }

    _autonomousExecutionActive = true;
    _autonomousRunId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    _autonomousState = "arming";
    _autonomousStopReason.clear();
    _autonomousStartedMs = QDateTime::currentMSecsSinceEpoch();
    _autonomousFinishedMs = 0;
    _autonomousDurationMs = durationMs;
    _autonomousPlanSha256 = QCryptographicHash::hash(
        QJsonDocument(plan).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex().toStdString();
    setCompanionEnabled(true);

    syncDaqState();
    if (!daqStarted && test->startDaq() <= 0)
        return finishError("daq_start_failed", "采集设备拒绝启动，自主试验已安全退出。");
    daqStarted = true;
    syncDaqState();
    if (!_daqRunning)
        return finishError("daq_start_not_confirmed", "采集启动命令返回后未观察到运行状态。");
    if (save) {
        if (test->startSave() <= 0)
            return finishError("storage_start_failed", "数据存储未能启动，自主试验已安全停止采集。");
        saveStarted = true;
    }
    syncDaqState();
    refreshCompanionRuntime(true);
    _autonomousState = "monitoring";
    publishCompanionEvent("info", "autonomous_run_started", "自主试验已开始",
                          "试验执行器已按授权计划启动采集并进入持续监控。", false);

    QEventLoop loop;
    QTimer tick;
    QTimer durationTimer;
    tick.setInterval(std::max(100, _companionSampleIntervalMs));
    durationTimer.setSingleShot(true);
    QObject::connect(&tick, &QTimer::timeout, &loop, [&]() {
        syncDaqState();
        const QJsonObject status = AgentJson::parseObject(companionStatus());
        const QJsonObject health = status.value("health").toObject();
        const QString severity = health.value("severity").toString();
        const QString healthState = health.value("state").toString();
        if (_cancelRequested.load()) {
            _autonomousStopReason = "cancelled";
            loop.quit();
        } else if (!_daqRunning) {
            _autonomousStopReason = "acquisition_stopped";
            loop.quit();
        } else if (severity == QStringLiteral("critical") || healthState == QStringLiteral("critical")) {
            _autonomousStopReason = "monitoring_critical";
            loop.quit();
        } else if (stopOnWarning && severity == QStringLiteral("warning")) {
            _autonomousStopReason = "monitoring_warning";
            loop.quit();
        }
    });
    QObject::connect(&durationTimer, &QTimer::timeout, &loop, [&]() {
        _autonomousStopReason = "duration_reached";
        loop.quit();
    });
    tick.start();
    durationTimer.start(static_cast<int>(durationMs));
    loop.exec();
    tick.stop();
    durationTimer.stop();
    stopAcquisition();
    refreshCompanionRuntime(true);
    if (_autonomousStopReason == "cancelled")
        return finishError("cancelled", "自主试验已由用户取消并安全停止。");
    if (_autonomousStopReason == "monitoring_critical" || _autonomousStopReason == "monitoring_warning"
        || _autonomousStopReason == "acquisition_stopped")
        return finishError(_autonomousStopReason, "持续监控触发停止条件，自主试验未进入分析阶段。");

    _autonomousState = "analyzing";
    QJsonObject analysisArgs{{"type", QString::fromStdString(analysisType)}};
    for (const char* key : {"frame_size", "average_num", "overlap_index"}) {
        if (plan.contains(key)) analysisArgs[key] = plan.value(key);
    }
    ToolResult analysis = executeTool("run_analysis",
        AgentJson::dump(analysisArgs), ToolOrigin::Autonomous);
    if (!analysis.ok)
        return finishError(analysis.error.empty() ? "analysis_failed" : analysis.error,
                           "自主试验采集完成，但确定性分析启动失败。");
    if (!waitForAutonomousAnalysis(180000))
        return finishError("analysis_not_completed", "自主试验分析未在限定时间内完成。");

    QJsonObject result{{"run_id", QString::fromStdString(_autonomousRunId)},
                       {"stop_reason", QString::fromStdString(_autonomousStopReason)},
                       {"analysis", AgentJson::parseObject(analysis.toJson())}};
    const QJsonObject diagnostic = plan.value("diagnostic").toObject();
    if (!diagnostic.isEmpty()) {
        ToolResult diagnosis = executeTool("diagnose", AgentJson::dump(diagnostic), ToolOrigin::Autonomous);
        result["diagnosis"] = AgentJson::parseObject(diagnosis.toJson());
        if (!diagnosis.ok)
            return finishError(diagnosis.error.empty() ? "diagnosis_failed" : diagnosis.error,
                               "自主试验分析完成，但诊断步骤失败。");
    }
    if (plan.value("generate_report").toBool(true)) {
        const ToolResult report = executeTool("generate_report",
            diagnostic.isEmpty() ? R"({})" : AgentJson::dump(diagnostic), ToolOrigin::Autonomous);
        result["report"] = AgentJson::parseObject(report.toJson());
        if (!report.ok)
            return finishError(report.error.empty() ? "report_failed" : report.error,
                               "自主试验分析完成，但报告生成失败。");
    }
    if (plan.value("create_evidence").toBool(true)) {
        const ToolResult evidence = executeTool("experiment_evidence_create", "{}", ToolOrigin::Autonomous);
        result["evidence"] = AgentJson::parseObject(evidence.toJson());
        if (!evidence.ok)
            return finishError(evidence.error.empty() ? "evidence_failed" : evidence.error,
                               "自主试验完成，但证据固化失败。");
    }
    _autonomousState = "completed";
    _autonomousExecutionActive = false;
    _autonomousFinishedMs = QDateTime::currentMSecsSinceEpoch();
    publishCompanionEvent("info", "autonomous_run_completed", "自主试验已完成",
                          "采集、持续监控、分析、报告和证据固化已按计划完成。", false);
    if (!previousCompanion) setCompanionEnabled(false);
    publishCompanionStatus();
    result["state"] = QString::fromStdString(_autonomousState);
    result["duration_ms"] = double(_autonomousFinishedMs - _autonomousStartedMs);
    return jsonResult("自主试验闭环已完成。", result);
}

ToolResult AgentServiceImpl::runAutonomousExperimentCampaign(const QJsonObject& contract) {
    if (_autonomousExecutionActive)
        return errorResult("autonomous_experiment_busy", "已有自主试验正在执行。请先停止或等待当前运行结束。");
    if (!_daqTest)
        return errorResult("no_daq_test", "当前没有可执行采集试验。");

    const QByteArray contractBytes = QJsonDocument(contract).toJson(QJsonDocument::Compact);
    const CampaignValidation validation = parseAndValidateExperimentCampaign(
        std::string(contractBytes.constData(), static_cast<size_t>(contractBytes.size())));
    if (!validation.ok)
        return errorResult("campaign_contract_invalid",
                           "试验战役契约未通过确定性校验：" + validation.error);

    auto executor = [&](int runIndex, int designIndex, const std::string& planJson)
            -> CampaignRunOutcome {
        Q_UNUSED(runIndex);
        Q_UNUSED(designIndex);
        CampaignRunOutcome outcome;
        const ToolResult result = runAutonomousExperiment(AgentJson::parseObject(planJson));
        if (!result.ok) {
            outcome.errorCode = result.error.empty() ? "run_failed" : result.error;
            return outcome;
        }
        const QJsonObject data = AgentJson::parseObject(result.toJson());
        outcome.ok = true;
        outcome.runId = data.value("run_id").toString().toStdString();
        outcome.evidenceArtifactId = data.value("evidence").toObject()
            .value("artifactId").toString().toStdString();
        return outcome;
    };
    auto cancelProbe = [this]() { return _cancelRequested.load(); };

    // One authorization covers the whole bounded campaign; each individual run
    // still passes the full preflight/monitoring/evidence closed loop.
    const CampaignExecution execution = executeExperimentCampaign(
        validation.campaign, std::string(), executor, cancelProbe);
    if (!execution.ok && !execution.error.empty())
        return errorResult(execution.error, "试验战役状态校验失败，未执行任何设备动作。");
    if (execution.state.stopReason == "evidence_missing")
        return errorResult("evidence_missing", "某次运行缺少证据产物，战役已按结构违规中止并固化状态。");
    QString checkpointArtifactId;
    if (execution.awaitingInterim) {
        checkpointArtifactId = writeTextArtifact(_session.id, "campaign_checkpoint",
            ".json", QString::fromStdString(serializeCampaignState(execution.state)));
        if (checkpointArtifactId.isEmpty())
            return errorResult("artifact_write_failed",
                               "中期检查点无法安全写入会话制品目录。");
    }
    ToolResult result = finishCampaignResult(validation.campaign, execution);
    if (execution.awaitingInterim && !checkpointArtifactId.isEmpty()) {
        QJsonObject data = AgentJson::parseObject(result.dataJson);
        data["awaiting_interim"] = true;
        data["checkpoint_artifact_id"] = checkpointArtifactId;
        data["resume_hint"] = QStringLiteral(
            "使用 resume_autonomous_experiment_campaign 提供 contract、"
            "checkpoint_artifact_id 与 decision=continue|abort；"
            "该检查点为运营/安全监督门，不构成经校正的期中统计检验。");
        result.dataJson = AgentJson::dump(data);
    }
    return result;
}

ToolResult AgentServiceImpl::finishCampaignResult(const ExperimentCampaign& campaign,
                                                  const CampaignExecution& execution) {
    int okRuns = 0;
    QJsonArray runs;
    for (const CampaignRunRecord& record : execution.state.records) {
        if (record.ok) ++okRuns;
        runs.append(QJsonObject{
            {"run_index", record.runIndex},
            {"design_index", record.designIndex},
            {"levels", AgentJson::parseObject(
                campaign.designLevelsJson[static_cast<size_t>(record.designIndex)])},
            {"ok", record.ok},
            {"error_code", QString::fromStdString(record.errorCode)},
            {"run_id", QString::fromStdString(record.runId)},
            {"evidence_artifact_id", QString::fromStdString(record.evidenceArtifactId)}});
    }
    QJsonObject summary{
        {"schema", "dpa.experiment.campaign.result/v1"},
        {"campaign_id", QString::fromStdString(campaign.campaignId)},
        {"hypothesis", QString::fromStdString(campaign.hypothesis)},
        {"campaign_sha256", QString::fromStdString(campaign.campaignSha256)},
        {"status", QString::fromStdString(execution.state.status)},
        {"stop_reason", QString::fromStdString(execution.state.stopReason)},
        {"planned_runs", double(campaign.runPlanJsons.size())},
        {"runs_ok", okRuns},
        {"runs_failed", double(execution.state.records.size()) - double(okRuns)},
        {"runs", runs}};
    const QByteArray summaryBytes = QJsonDocument(summary).toJson(QJsonDocument::Compact);
    const QString artifactId = writeTextArtifact(_session.id, "experiment_campaign", ".json",
        QString::fromUtf8(summaryBytes));
    if (artifactId.isEmpty())
        return errorResult("artifact_write_failed", "战役汇总证据无法安全写入会话制品目录。");
    summary["summary_artifact_id"] = artifactId;
    QString message;
    if (execution.state.status == "paused_interim")
        message = QStringLiteral(
            "试验战役已在中期检查点暂停：已完成运行的证据绑定已固化，"
            "请审查后决定继续或终止。");
    else if (execution.state.status != "completed")
        message = QStringLiteral("试验战役已受控中止，已完成运行的证据绑定已固化。");
    else if (summary["runs_failed"].toDouble() > 0)
        message = QStringLiteral("试验战役已按计划完成，失败运行已作为缺失数据记录。");
    else
        message = QStringLiteral("试验战役闭环已完成，全部运行已绑定逐运行证据。");
    ToolResult result = jsonResult(message.toStdString(), summary);
    result.artifactRefs.push_back(artifactId.toStdString());
    return result;
}

ToolResult AgentServiceImpl::analyzeAutonomousExperimentCampaign(const std::string& args) {
    // Governance gate: observations must bind to the verified run records of
    // this exact campaign's result artifact before any statistics run.
    QJsonObject input = AgentJson::parseObject(args);
    const QString campaignResultArtifactId =
        input.value("campaign_result_artifact_id").toString();
    const QByteArray contractBytes = QJsonDocument(
        input.value("contract").toObject()).toJson(QJsonDocument::Compact);
    const CampaignValidation gate = parseAndValidateExperimentCampaign(
        std::string(contractBytes.constData(), static_cast<size_t>(contractBytes.size())));
    if (!gate.ok)
        return errorResult("campaign_contract_invalid", "契约校验失败：" + gate.error);
    if (campaignResultArtifactId.isEmpty())
        return errorResult("evidence_artifact_required",
                           "分析必须绑定 campaign_result_artifact_id，不接受脱离证据的观测。");
    const QString evidencePath = existingArtifactPath(_session.id, campaignResultArtifactId);
    if (evidencePath.isEmpty())
        return errorResult("artifact_missing", "战役结果工件不存在或不在当前会话内。");
    QFile evidenceFile(evidencePath);
    if (!evidenceFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return errorResult("artifact_unreadable", "战役结果工件无法读取。");
    const QByteArray evidenceContent = evidenceFile.readAll();
    const QByteArray observationBytes = QJsonDocument(
        input.value("observations").toArray()).toJson(QJsonDocument::Compact);
    const EvidenceBoundObservations bound = bindObservationsToEvidence(
        std::string(evidenceContent.constData(), static_cast<size_t>(evidenceContent.size())),
        gate.campaign.campaignSha256,
        std::string(observationBytes.constData(), static_cast<size_t>(observationBytes.size())),
        0.75);
    if (!bound.ok)
        return errorResult(bound.error.empty() ? "evidence_binding_failed" : bound.error,
                           "观测未通过证据绑定校验：仅限已完成且绑定证据的运行，"
                           "覆盖率不得低于 0.75。");
    const QJsonArray boundObservations = QJsonDocument::fromJson(
        QByteArray::fromStdString(bound.observationsJson)).array();
    QJsonObject coreInput{
        {"contract", input.value("contract")},
        {"observations", boundObservations},
        {"seed", input.value("seed")}};
    if (input.contains(QStringLiteral("permutations")))
        coreInput["permutations"] = input.value("permutations");
    const CampaignAnalysisOutput analysis = analyzeExperimentCampaign(
        AgentJson::dump(coreInput));
    if (!analysis.ok)
        return errorResult(
            analysis.error.empty() ? "campaign_analysis_failed" : analysis.error,
            "战役分析请求未通过确定性校验，未产生任何结论。");
    const QString analysisArtifactId = writeTextArtifact(_session.id, "campaign_analysis", ".json",
        QString::fromStdString(analysis.resultJson));
    if (analysisArtifactId.isEmpty())
        return errorResult("artifact_write_failed", "战役分析结果无法安全写入会话制品目录。");
    QJsonObject payload = AgentJson::parseObject(analysis.resultJson);
    payload["analysis_artifact_id"] = analysisArtifactId;
    payload["evidence_binding"] = QJsonObject{
        {"campaign_result_artifact_id", campaignResultArtifactId},
        {"verified_runs", bound.verifiedRuns}};
    // The ledger is derived observability: a failed append never invalidates
    // the analysis itself, which is already persisted as a session artifact.
    const KnowledgeBuildOutput entry = buildKnowledgeEntry(
        analysis.resultJson, analysisArtifactId.toStdString(),
        QDateTime::currentMSecsSinceEpoch());
    payload["knowledge_recorded"] =
        entry.ok && appendKnowledgeLine(entry.entryJson);
    ToolResult result = jsonResult(
        "试验战役确定性分析完成：效应估计与随机化检验已绑定战役契约并固化证据。",
        payload);
    result.artifactRefs.push_back(analysisArtifactId.toStdString());
    return result;
}

ToolResult AgentServiceImpl::estimateAutonomousExperimentCampaignPower(
    const std::string& args) {
    const CampaignPowerOutput power = estimateCampaignPower(args);
    if (!power.ok)
        return errorResult(
            power.error.empty() ? "campaign_power_failed" : power.error,
            "功效估计请求未通过确定性校验，未产生任何结论。");
    const QString artifactId = writeTextArtifact(_session.id, "campaign_power", ".json",
        QString::fromStdString(power.resultJson));
    if (artifactId.isEmpty())
        return errorResult("artifact_write_failed", "功效估计结果无法安全写入会话制品目录。");
    QJsonObject payload = AgentJson::parseObject(power.resultJson);
    payload["power_artifact_id"] = artifactId;
    ToolResult result = jsonResult(
        "试验战役功效估计完成：估计值与零假设校准已绑定战役契约并固化证据。",
        payload);
    result.artifactRefs.push_back(artifactId.toStdString());
    return result;
}

ToolResult AgentServiceImpl::refineAutonomousExperimentCampaign(const std::string& args) {
    // Same governance gate as analysis: proposals must consume evidence-bound
    // findings, never a free-floating observation set.
    QJsonObject input = AgentJson::parseObject(args);
    const QString sourceArtifactId = input.value("campaign_result_artifact_id").toString();
    const QByteArray contractBytes = QJsonDocument(
        input.value("contract").toObject()).toJson(QJsonDocument::Compact);
    const CampaignValidation gate = parseAndValidateExperimentCampaign(
        std::string(contractBytes.constData(), static_cast<size_t>(contractBytes.size())));
    if (!gate.ok)
        return errorResult("campaign_contract_invalid", "契约校验失败：" + gate.error);
    if (sourceArtifactId.isEmpty())
        return errorResult("evidence_artifact_required",
                           "精化必须绑定 campaign_result_artifact_id，不接受脱离证据的观测。");
    const QString evidencePath = existingArtifactPath(_session.id, sourceArtifactId);
    if (evidencePath.isEmpty())
        return errorResult("artifact_missing", "战役结果工件不存在或不在当前会话内。");
    QFile evidenceFile(evidencePath);
    if (!evidenceFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return errorResult("artifact_unreadable", "战役结果工件无法读取。");
    const QByteArray evidenceContent = evidenceFile.readAll();
    const QByteArray observationBytes = QJsonDocument(
        input.value("observations").toArray()).toJson(QJsonDocument::Compact);
    const EvidenceBoundObservations bound = bindObservationsToEvidence(
        std::string(evidenceContent.constData(), static_cast<size_t>(evidenceContent.size())),
        gate.campaign.campaignSha256,
        std::string(observationBytes.constData(), static_cast<size_t>(observationBytes.size())),
        0.75);
    if (!bound.ok)
        return errorResult(bound.error.empty() ? "evidence_binding_failed" : bound.error,
                           "观测未通过证据绑定校验：仅限已完成且绑定证据的运行，"
                           "覆盖率不得低于 0.75。");
    const QJsonArray boundObservations = QJsonDocument::fromJson(
        QByteArray::fromStdString(bound.observationsJson)).array();
    QJsonObject coreInput{
        {"contract", input.value("contract")},
        {"observations", boundObservations},
        {"seed", input.value("seed")}};
    if (input.contains(QStringLiteral("threshold")))
        coreInput["threshold"] = input.value("threshold");
    const CampaignRefinementOutput refinement = refineExperimentCampaign(
        AgentJson::dump(coreInput));
    if (!refinement.ok)
        return errorResult(
            refinement.error.empty() ? "campaign_refinement_failed" : refinement.error,
            "精化提议未通过确定性校验，未产生任何设计变更。");
    const QString artifactId = writeTextArtifact(_session.id, "campaign_refinement",
        ".json", QString::fromStdString(refinement.resultJson));
    if (artifactId.isEmpty())
        return errorResult("artifact_write_failed", "精化提议无法安全写入会话制品目录。");
    QJsonObject payload = AgentJson::parseObject(refinement.resultJson);
    payload["refinement_artifact_id"] = artifactId;
    ToolResult result = jsonResult(
        "下一轮战役设计已生成：提议契约已自校验并绑定源战役结论；执行需新的应用内授权。",
        payload);
    result.artifactRefs.push_back(artifactId.toStdString());
    return result;
}

ToolResult AgentServiceImpl::suggestExperimentRefinementNext(const std::string& args) {
    const StatsSuggestionOutput suggestion =
        buildExperimentRefinementSuggestion(args);
    if (!suggestion.ok)
        return errorResult(
            suggestion.error.empty() ? "stats_suggestion_failed"
                                     : suggestion.error,
            "精化建议请求未通过确定性校验，未产生任何建议。");
    const QJsonObject payload = AgentJson::parseObject(suggestion.resultJson);
    return jsonResult(
        payload.value("source").toString() == QStringLiteral("stats_sidecar")
            ? "统计 sidecar 已给出下一轮精化建议；数值仅供设计参考，执行需另行授权。"
            : "sidecar 不可用或未采用，已降级为内置确定性网格建议（结果含告警字段）。",
        payload);
}

ToolResult AgentServiceImpl::computeSequentialBoundary(const std::string& args) {
    QJsonObject request = AgentJson::parseObject(args);
    if (request.isEmpty())
        return errorResult("seq_boundary_request_invalid",
                           "请求必须是 JSON 对象。");
    request.insert(QStringLiteral("method"), QStringLiteral("seq_boundary"));
    const StatsSidecarOutput sidecar = runStatsSidecarRequest(
        AgentJson::dump(request));
    if (!sidecar.ok)
        return errorResult(
            sidecar.error.empty() ? "stats_sidecar_failed" : sidecar.error,
            "序贯边界计算不可用：无内置替代，未产生任何边界结论。");
    const QJsonObject response = AgentJson::parseObject(sidecar.resultJson);
    return jsonResult("成组序贯边界已由外部统计进程按 Lan–DeMets 消耗函数计算。",
                      response);
}

ToolResult AgentServiceImpl::sweepCampaignPowerCurve(const std::string& args) {
    const QJsonObject input = AgentJson::parseObject(args);
    static const QStringList knownKeys{"contract", "factor", "effect_range",
                                       "noise_std", "seed", "alpha",
                                       "simulations", "permutations",
                                       "replicates_sweep"};
    for (auto it = input.begin(); it != input.end(); ++it)
        if (!knownKeys.contains(it.key()))
            return errorResult("power_curve_schema_invalid",
                               ("请求包含未知字段：" + it.key()).toStdString());
    QJsonArray sweep;
    if (input.contains(QStringLiteral("replicates_sweep"))) {
        if (!input.value("replicates_sweep").isArray())
            return errorResult("power_curve_schema_invalid",
                               "replicates_sweep 必须是数组。");
        sweep = input.value("replicates_sweep").toArray();
    }
    if (sweep.isEmpty())
        sweep = QJsonArray{2.0, 3.0, 4.0, 5.0};
    if (sweep.size() > 6)
        return errorResult("power_curve_schema_invalid",
                           "replicates_sweep 最多 6 个点。");

    QJsonArray curve;
    for (const QJsonValue& entry : sweep) {
        if (!entry.isDouble() || entry.toDouble() < 2.0 || entry.toDouble() > 5.0)
            return errorResult("power_curve_schema_invalid",
                               "重复数必须在 2..5 范围内。");
        QJsonObject contract = input.value("contract").toObject();
        contract["replicates"] = entry.toDouble();
        QJsonObject pointInput = input;
        pointInput.remove(QStringLiteral("replicates_sweep"));
        pointInput["contract"] = contract;
        const QByteArray pointBytes =
            QJsonDocument(pointInput).toJson(QJsonDocument::Compact);
        const CampaignPowerOutput power = estimateCampaignPower(
            std::string(pointBytes.constData(), static_cast<size_t>(pointBytes.size())));
        if (!power.ok)
            return errorResult(power.error.empty() ? "campaign_power_failed"
                                                   : power.error,
                               ("功效曲线在重复数 "
                                   + QString::number(entry.toDouble())
                                + " 处无法估计；未产出部分曲线。").toStdString());
        const QJsonObject report =
            AgentJson::parseObject(power.resultJson);
        curve.append(QJsonObject{
            {"replicates", entry.toDouble()},
            {"estimated_power", report.value("estimated_power")},
            {"calibrated", report.value("calibrated")}});
    }
    QJsonObject payload{
        {"schema", QStringLiteral("dpa.experiment.campaign.power_curve/v1")},
        {"curve", curve},
        {"source", QStringLiteral("estimateCampaignPower")}};
    return jsonResult("功效曲线扫描完成：与单点功效估计使用同一确定性引擎。",
                      payload);
}

ToolResult AgentServiceImpl::recordFeedbackCore(const std::string& args) {
    const QJsonObject input = AgentJson::parseObject(args);
    const QString feedbackType = input.value("feedbackType").toString();
    static const QStringList validTypes{"false_positive", "false_negative", "correct",
                                        "needs_more_information", "disposition", "correction", "other"};
    if (!validTypes.contains(feedbackType))
        return errorResult("feedback_type_invalid",
                           "feedbackType 必须是 false_positive/false_negative/correct/needs_more_information/disposition/correction/other 之一。");
    const QString severity = input.value("severity").toString().isEmpty()
        ? QStringLiteral("info") : input.value("severity").toString();
    static const QStringList validSeverities{"info", "concern", "critical"};
    if (!validSeverities.contains(severity))
        return errorResult("feedback_severity_invalid", "severity 必须是 info/concern/critical 之一。");
    const QString summary = input.value("summary").toString().trimmed();
    if (summary.isEmpty())
        return errorResult("feedback_summary_empty", "summary 不能为空。");
    if (summary.size() > 4000)
        return errorResult("feedback_summary_too_long", "summary 不得超过 4000 字符。");

    FeedbackEvent event;
    event.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    event.timestampMs = QDateTime::currentMSecsSinceEpoch();
    event.sessionId = _session.id;
    event.correlationId = input.value("correlationId").toString().toStdString();
    const QJsonArray refsArray = input.value("evidenceRefs").toArray();
    for (const QJsonValue& v : refsArray)
        event.evidenceRefs.push_back(v.toString().toStdString());
    event.feedbackType = feedbackType.toStdString();
    event.severity = severity.toStdString();
    event.summary = summary.toStdString();
    event.operatorNote = input.value("operatorNote").toString().trimmed().toStdString();
    event.contextFingerprint = currentAnalysisContextFingerprint();
    event.toolCatalogSha256 = _registry.approvalCatalogSha256();

    QJsonObject audit;
    audit["event"] = "operator_feedback";
    audit["feedbackId"] = QString::fromStdString(event.id);
    audit["feedbackType"] = feedbackType;
    audit["severity"] = severity;
    audit["timestampMs"] = double(event.timestampMs);
    if (!event.correlationId.empty())
        audit["correlationId"] = QString::fromStdString(event.correlationId);
    if (!event.evidenceRefs.empty()) {
        QJsonArray refs;
        for (const auto& r : event.evidenceRefs)
            refs.append(QString::fromStdString(r));
        audit["dpa.evidence_refs"] = refs;
    }
    audit["summary"] = summary;
    if (!event.operatorNote.empty())
        audit["operatorNote"] = QString::fromStdString(event.operatorNote);
    audit["contextFingerprint"] = QString::fromStdString(event.contextFingerprint);
    audit["toolCatalogSha256"] = QString::fromStdString(event.toolCatalogSha256);
    audit["dpa.telemetry.schema"] = "dpa.telemetry.event";
    audit["dpa.telemetry.version"] = 1;
    audit["dpa.plan_first"] = _planFirstEnabled;

    const std::string content = AgentJson::dump(audit);
    const std::vector<SessionTurn> turnsBeforeFeedback = _session.turns;
    appendSessionTurn("operator_feedback", content);
    if (!persistSession()) {
        _session.turns = turnsBeforeFeedback;
        return errorResult("feedback_persist_failed",
                           "操作员反馈未能写入本地会话数据库，未报告为已记录。");
    }
    auto appendExternal = [content]() {
        constexpr qint64 maxAuditBytes = 1024 * 1024;
        const QString directory = mcpDir();
        if (!QDir().mkpath(directory) || QFileInfo(directory).isSymLink()) return;
        const QString path = directory + "/operator-feedback.ndjson";
        const QString rotated = directory + "/operator-feedback.1.ndjson";
        if (QFileInfo(path).size() >= maxAuditBytes) {
            QFile::remove(rotated);
            QFile::rename(path, rotated);
        }
        QFile f(path);
        if (f.open(QIODevice::Append | QIODevice::Text))
            f.write(QByteArray::fromStdString(content) + "\n");
    };
    appendExternal();

    _feedbackEvents.push_back(event);
    constexpr size_t maxFeedbackEvents = 500;
    if (_feedbackEvents.size() > maxFeedbackEvents)
        _feedbackEvents.erase(_feedbackEvents.begin(),
                              _feedbackEvents.end() - maxFeedbackEvents);

    QJsonObject resultPayload{
        {"schema", QStringLiteral("dpa.operator_feedback/v1")},
        {"feedbackId", QString::fromStdString(event.id)},
        {"feedbackType", feedbackType},
        {"severity", severity},
        {"timestampMs", double(event.timestampMs)},
        {"sessionFeedbackCount", int(_feedbackEvents.size())}};
    return jsonResult("操作员反馈已记录：" + feedbackType.toStdString(),
                      resultPayload);
}

std::string AgentServiceImpl::recordOperatorFeedback(const std::string& argsJson) {
    // UI-originated audit write: shared core, no confirmation card (the operator
    // already decided by clicking the button). Mirrors configureCompanion().
    return recordFeedbackCore(argsJson).toJson();
}

ToolResult AgentServiceImpl::aggregateOperatorFeedback(const std::string& args) {
    (void)args;   // read-only; aggregation covers the whole current session
    std::map<std::string, int> typeCounts;
    std::map<std::string, int> severityCounts;
    for (const FeedbackEvent& event : _feedbackEvents) {
        ++typeCounts[event.feedbackType.empty() ? "other" : event.feedbackType];
        ++severityCounts[event.severity.empty() ? "info" : event.severity];
    }
    QJsonObject byType;
    for (const auto& kv : typeCounts)
        byType[QString::fromStdString(kv.first)] = kv.second;
    QJsonObject bySeverity;
    for (const auto& kv : severityCounts)
        bySeverity[QString::fromStdString(kv.first)] = kv.second;
    QJsonArray recent;
    const size_t n = _feedbackEvents.size();
    const size_t begin = n > 10 ? n - 10 : 0;
    for (size_t i = begin; i < n; ++i) {
        const FeedbackEvent& event = _feedbackEvents[i];
        recent.append(QJsonObject{
            {"id", QString::fromStdString(event.id)},
            {"feedbackType", QString::fromStdString(event.feedbackType)},
            {"severity", QString::fromStdString(event.severity)},
            {"summary", QString::fromStdString(event.summary).left(120)},
            {"timestampMs", double(event.timestampMs)}
        });
    }
    QJsonObject payload{
        {"ok", true},
        {"schema", QStringLiteral("dpa.operator_feedback.stats/v1")},
        {"total", int(_feedbackEvents.size())},
        {"by_type", byType},
        {"by_severity", bySeverity},
        {"recent", recent}
    };
    return jsonResult("操作员反馈统计已聚合。", payload);
}

ToolResult AgentServiceImpl::resumeAutonomousExperimentCampaign(const std::string& args) {
    if (_autonomousExecutionActive)
        return errorResult("autonomous_experiment_busy", "已有自主试验正在执行。请先停止或等待当前运行结束。");
    if (!_daqTest)
        return errorResult("no_daq_test", "当前没有可执行采集试验。");
    const QJsonObject input = AgentJson::parseObject(args);
    const QString decision = input.value("decision").toString();
    const QString checkpointId = input.value("checkpoint_artifact_id").toString();
    if (decision != QStringLiteral("continue") && decision != QStringLiteral("abort"))
        return errorResult("decision_invalid", "决定必须是 continue 或 abort。");
    const QByteArray contractBytes = QJsonDocument(
        input.value("contract").toObject()).toJson(QJsonDocument::Compact);
    const CampaignValidation validation = parseAndValidateExperimentCampaign(
        std::string(contractBytes.constData(), static_cast<size_t>(contractBytes.size())));
    if (!validation.ok)
        return errorResult("campaign_contract_invalid",
                           "战役契约未通过确定性校验：" + validation.error);
    const QString path = existingArtifactPath(_session.id, checkpointId);
    if (path.isEmpty())
        return errorResult("checkpoint_artifact_missing",
                           "检查点工件不存在或不在当前会话制品目录内。");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return errorResult("checkpoint_artifact_unreadable", "检查点工件无法读取。");
    const QByteArray content = file.readAll();
    const CampaignStateValidation stateCheck = parseCampaignState(
        std::string(content.constData(), static_cast<size_t>(content.size())));
    if (!stateCheck.ok)
        return errorResult(stateCheck.error, "检查点状态未通过确定性校验。");
    if (stateCheck.state.campaignSha256 != validation.campaign.campaignSha256)
        return errorResult("checkpoint_campaign_mismatch", "检查点与所提供契约不匹配，拒绝恢复。");
    if (stateCheck.state.status == "completed")
        return errorResult("campaign_already_completed", "该战役已经完成，无需恢复。");

    CampaignState state = stateCheck.state;
    if (decision == QStringLiteral("abort")) {
        state.status = "aborted";
        state.stopReason = "aborted_at_interim";
        // Rewrite the checkpoint in place so later resumes see a terminal state.
        file.close();
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            file.write(QByteArray::fromStdString(serializeCampaignState(state)));
        return finishCampaignResult(validation.campaign,
                                    CampaignExecution{false, false, std::string(),
                                                      std::move(state)});
    }

    auto executor = [&](int runIndex, int designIndex, const std::string& planJson)
            -> CampaignRunOutcome {
        Q_UNUSED(runIndex);
        Q_UNUSED(designIndex);
        CampaignRunOutcome outcome;
        const ToolResult result = runAutonomousExperiment(AgentJson::parseObject(planJson));
        if (!result.ok) {
            outcome.errorCode = result.error.empty() ? "run_failed" : result.error;
            return outcome;
        }
        const QJsonObject data = AgentJson::parseObject(result.toJson());
        outcome.ok = true;
        outcome.runId = data.value("run_id").toString().toStdString();
        outcome.evidenceArtifactId = data.value("evidence").toObject()
            .value("artifactId").toString().toStdString();
        return outcome;
    };
    auto cancelProbe = [this]() { return _cancelRequested.load(); };
    const CampaignExecution execution = executeExperimentCampaign(
        validation.campaign, serializeCampaignState(state), executor, cancelProbe);
    if (!execution.ok && !execution.error.empty())
        return errorResult(execution.error, "恢复执行的状态校验失败，未执行任何设备动作。");
    if (execution.state.stopReason == "evidence_missing")
        return errorResult("evidence_missing", "某次运行缺少证据产物，战役已按结构违规中止并固化状态。");
    // Keep the checkpoint artifact current so further pauses stay resumable.
    file.close();
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        file.write(QByteArray::fromStdString(serializeCampaignState(execution.state)));
    ToolResult result = finishCampaignResult(validation.campaign, execution);
    if (execution.awaitingInterim) {
        QJsonObject data = AgentJson::parseObject(result.dataJson);
        data["awaiting_interim"] = true;
        data["checkpoint_artifact_id"] = checkpointId;
        result.dataJson = AgentJson::dump(data);
    }
    return result;
}

ToolResult AgentServiceImpl::listExperimentKnowledge(const std::string& args) {    const QJsonObject input = AgentJson::parseObject(args);
    int maxItems = input.contains(QStringLiteral("max_items"))
        ? input.value("max_items").toInt(50) : 50;
    maxItems = std::max(1, std::min(200, maxItems));
    QJsonArray bounded;
    long long validEntries = 0;
    long long corruptLines = 0;
    QFile file(knowledgeFilePath());
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!file.atEnd()) {
            const QByteArray line = file.readLine().trimmed();
            if (line.isEmpty()) continue;
            const KnowledgeEntryParse parsed = parseKnowledgeEntry(
                std::string(line.constData(), static_cast<size_t>(line.size())));
            if (!parsed.ok) { ++corruptLines; continue; }
            ++validEntries;
            bounded.append(AgentJson::parseObject(parsed.entry.canonicalJson));
            if (bounded.size() > maxItems) bounded.removeAt(0);
        }
    }
    return jsonResult("试验知识账本读取完成。",
        QJsonObject{{"schema", "dpa.experiment.knowledge.list/v1"},
                    {"total_entries", static_cast<double>(validEntries)},
                    {"corrupt_lines", static_cast<double>(corruptLines)},
                    {"returned", static_cast<double>(bounded.size())},
                    {"entries", bounded}});
}

void AgentServiceImpl::beginDiagnosticTool(const std::string& name) {
    if (!isAnalysisRunTool(name) && !isDiagnoseTool(name) && !isReportTool(name)) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool changed = false;

    // Starting analysis after diagnosis/report progress is an explicit new run. A failed
    // analysis, however, retries the same persisted step.
    if (isAnalysisRunTool(name)) {
        const bool retryAnalysis = _diagnosticRun.state() == DiagnosticRunState::Failed
            && _diagnosticRun.currentStep() == DiagnosticRunState::AnalysisPending;
        if (retryAnalysis) {
            changed = _diagnosticRun.apply(DiagnosticRunEvent::Retry, now);
        } else if (_diagnosticRun.state() != DiagnosticRunState::ContextReady
                   && _diagnosticRun.state() != DiagnosticRunState::AnalysisPending) {
            _diagnosticRun = DiagnosticRun(now);
            changed = true;
        }
    } else if (_diagnosticRun.state() == DiagnosticRunState::Failed) {
        const bool alignedRetry = (isDiagnoseTool(name)
                                   && _diagnosticRun.currentStep() == DiagnosticRunState::AnalysisComplete)
            || (isReportTool(name)
                && _diagnosticRun.currentStep() == DiagnosticRunState::ReportPending);
        if (alignedRetry) changed = _diagnosticRun.apply(DiagnosticRunEvent::Retry, now);
    }

    if (isAnalysisRunTool(name) && _diagnosticRun.state() == DiagnosticRunState::ContextReady)
        changed = _diagnosticRun.apply(DiagnosticRunEvent::StartAnalysis, now) || changed;
    else if (isReportTool(name) && _diagnosticRun.state() == DiagnosticRunState::DiagnosisComplete)
        changed = _diagnosticRun.apply(DiagnosticRunEvent::RequestReport, now) || changed;

    if (isAnalysisRunTool(name) && _diagnosticRun.state() == DiagnosticRunState::AnalysisPending) {
        clearPendingAnalysisProcess();
        _pendingAnalysisRunId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        _analysisCancelRequested = false;
        _analysisContextFingerprint = currentAnalysisContextFingerprint();
    }
    if (changed) publishDiagnosticRun();
}

void AgentServiceImpl::finishDiagnosticTool(const std::string& name, const ToolResult& result) {
    if (!isAnalysisRunTool(name) && !isDiagnoseTool(name) && !isReportTool(name)) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool changed = false;
    if (!result.ok) {
        const bool active = (isAnalysisRunTool(name) && _diagnosticRun.state() == DiagnosticRunState::AnalysisPending)
            || (isDiagnoseTool(name) && _diagnosticRun.state() == DiagnosticRunState::AnalysisComplete)
            || (isReportTool(name) && _diagnosticRun.state() == DiagnosticRunState::ReportPending);
        if (active) {
            const std::string error = !result.error.empty() ? result.error
                : (!result.summary.empty() ? result.summary : std::string("tool_failed"));
            changed = _diagnosticRun.apply(DiagnosticRunEvent::Fail, now, error);
        }
    } else if (isDiagnoseTool(name) && _diagnosticRun.state() == DiagnosticRunState::AnalysisComplete) {
        changed = _diagnosticRun.apply(DiagnosticRunEvent::CompleteDiagnosis, now);
    } else if (isReportTool(name) && _diagnosticRun.state() == DiagnosticRunState::ReportPending) {
        changed = _diagnosticRun.apply(DiagnosticRunEvent::CompleteReport, now);
    }
    if (!result.ok && isAnalysisRunTool(name)) clearPendingAnalysisProcess();
    if (changed) publishDiagnosticRun();
}

void AgentServiceImpl::bindPendingAnalysisProcess(M* process, const std::string& runId) {
    if (!process || runId.empty() || runId != _pendingAnalysisRunId
        || _diagnosticRun.state() != DiagnosticRunState::AnalysisPending) return;
    if (_pendingAnalysisConnection) QObject::disconnect(_pendingAnalysisConnection);
    _pendingAnalysisProcess = process;
    const QPointer<M> guardedProcess(process);
    _pendingAnalysisConnection = connect(
        process,
        static_cast<void (M::*)(const std::string&, utils::Property*)>(&M::sigPropertyChange),
        this,
        [this, guardedProcess, runId](const std::string& index, utils::Property*) {
            if (index == "ProcessAbort" || index == "OnDelete")
                onAnalysisProcessAborted(guardedProcess.data(), runId);
        });
}

void AgentServiceImpl::clearPendingAnalysisProcess() {
    if (_pendingAnalysisConnection) QObject::disconnect(_pendingAnalysisConnection);
    _pendingAnalysisConnection = QMetaObject::Connection{};
    _pendingAnalysisProcess.clear();
    _pendingAnalysisRunId.clear();
    _analysisCancelRequested = false;
    _analysisContextFingerprint.clear();
}

void AgentServiceImpl::invalidatePendingAnalysis(const std::string& error) {
    const std::string reason = error.empty() ? "analysis_context_changed" : error;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool changed = _diagnosticRun.state() == DiagnosticRunState::AnalysisPending
        && _diagnosticRun.apply(DiagnosticRunEvent::Fail, now, reason);
    clearPendingAnalysisProcess();
    if (changed) publishDiagnosticRun();
}

void AgentServiceImpl::onAnalysisProcessAborted(M* process, const std::string& runId) {
    if (!process || process != _pendingAnalysisProcess.data()
        || runId.empty() || runId != _pendingAnalysisRunId
        || process->value<std::string>("AgentRunId") != runId
        || _diagnosticRun.state() != DiagnosticRunState::AnalysisPending) return;
    if (_analysisContextFingerprint != currentAnalysisContextFingerprint())
        return clearPendingAnalysisProcess();
    const bool cancelled = _analysisCancelRequested;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool changed = cancelled
        ? _diagnosticRun.apply(DiagnosticRunEvent::Cancel, now)
        : _diagnosticRun.apply(DiagnosticRunEvent::Fail, now, "analysis_aborted");
    clearPendingAnalysisProcess();
    if (changed) publishDiagnosticRun();
}
void AgentServiceImpl::invalidatePendingConfirmation() {
    QMutexLocker lk(&_confirmMutex);
    if (_pendingConfirmationId.empty() && _pendingConfirmationTool.empty()
        && _pendingConfirmationArgsSha256.empty()) return;
    _confirmDecision = -1;
    _pendingConfirmationId.clear();
    _pendingConfirmationTool.clear();
    _pendingConfirmationArgsSha256.clear();
    _pendingConfirmationExpiresAtMs = 0;
    _confirmCond.wakeAll();
}
void AgentServiceImpl::initBindings() {
    // Subscribe current project -> current analysis test (key matches the canonical
    // iProjectService registration: simplified "分析试验"). Called from Activator::
    // afterStart(), when iProjectService is already registered. SubscribeAll() in
    // onPropertyChanged() then auto-populates _currentProject / _dpaTest as signals fire.
    Subscribe(AgentServiceImpl, "ProjectService", iProjectService);
    subscribe("CurrentProject");
    subscribe("CurrentProject", "分析试验");
    subscribe("CurrentProject", "采集试验");
    subscribe("CurrentProject", "采集试验", "WorkSpace");
    subscribe("CurrentProject", "采集试验", "ChannelSettings");
    subscribe("CurrentProject", "采集试验", "IsDaqStart");
    subscribe("CurrentProject", "采集试验", "IsSaveStart");
    subscribe("CurrentProject", "采集试验", "IsConnected");
}
bool AgentServiceImpl::isMcpToolWrite(const std::string& mcpName) {
    return _registry.isWriteMcp(mcpName);
}
void AgentServiceImpl::onPropertyChanged(const std::string& Index, utils::Property* prop) {
    if (Index == "AnalysisDone") onAnalysisFinished(prop ? (M*)prop->self() : nullptr);
    const bool wasRunning = _daqRunning;
    const bool wasSaving = _daqSaving;
    const bool wasConnected = _daqConnected;
    const M* oldDaqTest = _daqTest;
    const M* oldCurrentProject = _currentProject;
    SubscribeAll(AgentServiceImpl, Index, prop);
    const bool contextChanged = oldDaqTest != _daqTest
        || oldCurrentProject != _currentProject
        || Index == "CurrentProject"
        || Index == "WorkSpace"
        || Index == "ChannelSettings";
    if (contextChanged && _diagnosticRun.state() == DiagnosticRunState::AnalysisPending) {
        invalidatePendingAnalysis("analysis_context_changed");
    } else if (contextChanged) {
        clearPendingAnalysisProcess();
    }
    if (contextChanged) _currentStandardPlanJson.clear();
    if (contextChanged && (_pendingConfirmationId.empty() == false || _confirmDecision != 0))
        invalidatePendingConfirmation();
    // Some device implementations publish only the bound state properties and
    // not the richer NetworkStateChange/Current_Info event matrix.  Promote those
    // transitions into the same bounded companion event stream so the UI, MCP and
    // evidence artifact do not disagree about a disconnect or storage change.
    if (Index == "IsConnected" && wasConnected != _daqConnected
        && _companionConfig.enabled && _companionRunStartedMs > 0)
        handleCompanionDeviceEvent("NetworkStateChange", prop);
    if (Index == "IsSaveStart" && wasSaving != _daqSaving
        && _companionConfig.enabled && _companionRunStartedMs > 0) {
        publishCompanionEvent(_daqSaving ? "info" : "warning",
            _daqSaving ? "storage_started" : "storage_stopped",
            _daqSaving ? "数据存储已开始" : "数据存储已停止",
            _daqSaving ? "采集数据正在写入存储。" : "采集仍可能运行，但当前未检测到存储写入。",
            false);
    }
    if (Index == "IsDaqStart" && wasRunning && !_daqRunning
        && _companionConfig.enabled && _companionRunStartedMs > 0)
        publishCompanionEvent("info", "acquisition_stopped", "采集已停止",
                              "采集状态已停止，陪试监控将结束本次运行。", false);
    if (Index == "Current_Error" || Index == "Current_Info"
        || Index == "NetworkStateChange" || Index == "CLOSEHOST"
        || Index == "DISKFULL" || Index == "DEVICEDAQSTOPED"
        || Index == "LimitStateChange") {
        handleCompanionDeviceEvent(Index, prop);
    }
    if (oldDaqTest != _daqTest || Index == "WorkSpace" || Index == "ChannelSettings")
        refreshCompanionRuntime(true);
    else if (Index == "IsDaqStart" && wasRunning != _daqRunning)
        refreshCompanionRuntime(true);
    else if (Index == "IsSaveStart" || Index == "IsConnected")
        publishCompanionStatus();
}
void AgentServiceImpl::onAnalysisFinished(M* process) {
    if (!process || process != _pendingAnalysisProcess.data()
        || _pendingAnalysisRunId.empty()
        || process->value<std::string>("AgentRunId") != _pendingAnalysisRunId
        || _diagnosticRun.state() != DiagnosticRunState::AnalysisPending) return;
    if (_analysisContextFingerprint != currentAnalysisContextFingerprint())
        return invalidatePendingAnalysis("analysis_context_changed");

    auto published = process->value<iData*>("PublishedResultData");
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool changed = published
        ? _diagnosticRun.apply(DiagnosticRunEvent::CompleteAnalysis, now)
        : _diagnosticRun.apply(DiagnosticRunEvent::Fail, now, "analysis_result_unpublished");
    clearPendingAnalysisProcess();
    if (published && _turnRunning.load() && _dpaTest
        && _dpaTest->value<M*>("PlotSet", "CurrentLayout", "CurrentCell", "Process") == process)
        refreshTurnContext();
    if (changed) publishDiagnosticRun();
    if (!published) return;

    // Proactive advice is opt-in. Publish it as a companion event so it cannot complete
    // or otherwise mutate the state of an unrelated Agent turn.
    if (!_advisoryConfig.enabled) return;
    ResultSummary rs = resultSummaryFromTest(_dpaTest);
    AdvisoryDecision d = evaluateAdvisory(rs, _advisoryConfig, QDateTime::currentMSecsSinceEpoch(), _advisoryState);
    if (d.fire)
        publishCompanionEvent("warning", "analysis_advisory",
            "分析结果超过主动建议阈值", d.message, false);
}
void AgentServiceImpl::confirmPendingAction(const std::string& confirmationId, bool approve) {
    QMutexLocker lk(&_confirmMutex);
    if (confirmationId.empty() || confirmationId != _pendingConfirmationId
        || _confirmDecision != 0) return;
    if (_pendingConfirmationExpiresAtMs <= QDateTime::currentMSecsSinceEpoch()) {
        _confirmDecision = -2;
        _confirmCond.wakeAll();
        return;
    }
    _confirmDecision = approve ? 1 : -1;
    _confirmCond.wakeAll();
}
void AgentServiceImpl::cancelCurrentTurn() {
    if (_turnRunning.load()) {
        _cancelRequested.store(true);
        if (_provider) _provider->cancel();
    }

    // Analysis cancellation is acknowledged only by the matching process's
    // ProcessAbort event. A completion that wins the race still completes.
    if (_diagnosticRun.state() == DiagnosticRunState::AnalysisPending
        && _pendingAnalysisProcess) {
        _analysisCancelRequested = true;
        const QPointer<M> process = _pendingAnalysisProcess;
        auto abortMatchingProcess = [process]() {
            if (process) ((iDataProcess*)process.data())->abortProcess();
        };
        if (process->thread() == QThread::currentThread()) abortMatchingProcess();
        else QMetaObject::invokeMethod(process.data(), abortMatchingProcess, Qt::QueuedConnection);
    } else if (_diagnosticRun.state() == DiagnosticRunState::ReportPending
               && _diagnosticRun.apply(DiagnosticRunEvent::Cancel,
                                       QDateTime::currentMSecsSinceEpoch())) {
        publishDiagnosticRun();
    }

    QMutexLocker lk(&_confirmMutex);
    if (_confirmDecision == 0 && !_pendingConfirmationId.empty()) _confirmDecision = -1;
    _pendingConfirmationId.clear();
    _pendingConfirmationTool.clear();
    _pendingConfirmationArgsSha256.clear();
    _pendingConfirmationExpiresAtMs = 0;
    _confirmCond.wakeAll();
}
bool AgentServiceImpl::startNewSession() {
    if (_turnRunning.load() || _diagnosticRun.state() == DiagnosticRunState::AnalysisPending) return false;
    if (flushMonitoringHistory(QDateTime::currentMSecsSinceEpoch(), true))
        persistMonitoringHistory();
    _session = SessionRecord{};
    _session.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    _session.title = "Agent Chat";
    _diagnosticRun = DiagnosticRun{};
    _currentStandardPlanJson.clear();
    _monitoringBuckets.clear();
    _monitoringHistory.clear();
    _channelTrends.clear();
    _feedbackEvents.clear();
    return true;
}

std::string AgentServiceImpl::listSessions() {
    QJsonArray sessions;
    auto dB = GetDefaultService2(iDataBaseService);
    if (!dB) return AgentJson::dump(QJsonObject{{"ok", false}, {"error", "database_unavailable"}, {"sessions", sessions}});

    MList rows;
    DataBaseCondition type;
    type._filed = "Type";
    type._value1 = "AgentSession";
    if (!dB->loadInfoList("AgentSession", &rows, {type}))
        return AgentJson::dump(QJsonObject{{"ok", false}, {"error", "session_list_failed"}, {"sessions", sessions}});
    for (utils::Property* row : rows.getChildren()) {
        if (!row) continue;
        QJsonObject item;
        item["id"] = QString::fromStdString(row->value<std::string>("Id"));
        item["title"] = QString::fromStdString(row->value<std::string>("Name"));
        item["updated_at"] = QString::fromStdString(row->value<std::string>("LastFixTime"));
        sessions.append(item);
    }
    return AgentJson::dump(QJsonObject{{"ok", true}, {"sessions", sessions}});
}

static bool validSessionId(const std::string& id) {
    return !id.empty() && id.size() <= 128
        && std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return std::isalnum(c) != 0 || c == '-' || c == '_';
        });
}

std::string AgentServiceImpl::resumeSession(const std::string& sessionId) {
    if (_turnRunning.load()) return errorResult("turn_running", "当前回复结束后才能恢复会话。").toJson();
    if (_diagnosticRun.state() == DiagnosticRunState::AnalysisPending)
        return errorResult("analysis_pending", "Analysis is still running; session switch is blocked.").toJson();
    if (!validSessionId(sessionId)) return errorResult("invalid_session_id", "会话标识无效。").toJson();
    auto dB = GetDefaultService2(iDataBaseService);
    if (!dB) return errorResult("database_unavailable", "本地会话数据库不可用。").toJson();

    AgentSessionProperty prop;
    DataBaseCondition id;
    id._filed = "Id";
    id._value1 = sessionId;
    if (!dB->load("AgentSession", &prop, {id})
        || prop.type != "AgentSession")
        return errorResult("session_not_found", "未找到该会话。").toJson();

    SessionRecord restored = parseSessionJson(prop.sessionJson);
    if (restored.id != sessionId)
        return errorResult("session_corrupt", "会话记录校验失败，未恢复。").toJson();
    DiagnosticRun restoredRun;
    if (!restored.diagnosticRunJson.empty()
        && !DiagnosticRun::fromJson(restored.diagnosticRunJson, restoredRun))
        return errorResult("session_run_state_corrupt", "诊断运行状态损坏，未恢复。").toJson();

    const bool interrupted = !restored.diagnosticRunJson.empty()
        && restoredRun.interruptPendingAfterRestart(QDateTime::currentMSecsSinceEpoch());
    if (interrupted) restored.diagnosticRunJson = restoredRun.toJson();

    QJsonObject pendingWrite;
    const bool writeOutcomeUnknown = !restored.pendingWriteJson.empty();
    if (writeOutcomeUnknown)
        pendingWrite = AgentJson::parseObject(restored.pendingWriteJson);

    if (flushMonitoringHistory(QDateTime::currentMSecsSinceEpoch(), true))
        persistMonitoringHistory();
    _session = std::move(restored);
    _diagnosticRun = _session.diagnosticRunJson.empty() ? DiagnosticRun{} : restoredRun;
    _currentStandardPlanJson.clear();
    _channelTrends.clear();
    _feedbackEvents.clear();
    for (const SessionTurn& turn : _session.turns) {
        if (turn.role != "operator_feedback") continue;
        const QJsonObject stored = AgentJson::parseObject(turn.content);
        if (stored.value("event").toString() != QStringLiteral("operator_feedback")
            || stored.value("feedbackType").toString().isEmpty()) continue;
        FeedbackEvent event;
        event.id = stored.value("feedbackId").toString().toStdString();
        event.timestampMs = static_cast<qint64>(stored.value("timestampMs").toDouble());
        event.sessionId = _session.id;
        event.correlationId = stored.value("correlationId").toString().toStdString();
        for (const QJsonValue& ref : stored.value("dpa.evidence_refs").toArray())
            event.evidenceRefs.push_back(ref.toString().toStdString());
        event.feedbackType = stored.value("feedbackType").toString().toStdString();
        event.severity = stored.value("severity").toString().toStdString();
        event.summary = stored.value("summary").toString().toStdString();
        event.operatorNote = stored.value("operatorNote").toString().toStdString();
        event.contextFingerprint = stored.value("contextFingerprint").toString().toStdString();
        event.toolCatalogSha256 = stored.value("toolCatalogSha256").toString().toStdString();
        _feedbackEvents.push_back(std::move(event));
    }
    if (_feedbackEvents.size() > 500)
        _feedbackEvents.erase(_feedbackEvents.begin(), _feedbackEvents.end() - 500);
    restoreMonitoringHistory();
    if (writeOutcomeUnknown) {
        QJsonObject recoveryAudit;
        recoveryAudit["event"] = "tool_outcome_unknown";
        for (const char* key : {"correlationId", "confirmationId", "tool", "argsSha256",
                                "contextFingerprint", "approvedAtMs", "startedMs"})
            recoveryAudit[key] = pendingWrite.value(key);
        recoveryAudit["toolCatalogSha256"] = pendingWrite.value("catalogSha256");
        recoveryAudit["source"] = "recovery";
        recoveryAudit["write"] = true;
        recoveryAudit["confirmation"] = "approved";
        recoveryAudit["recoveredAtMs"] = double(QDateTime::currentMSecsSinceEpoch());
        recoveryAudit["ok"] = false;
        recoveryAudit["error"] = "outcome_unknown_after_restart";
        _session.pendingWriteJson.clear();
        appendSessionTurn("tool", AgentJson::dump(recoveryAudit));
    }
    bool recoveryPersisted = true;
    if (interrupted || writeOutcomeUnknown) recoveryPersisted = persistSession();
    if (!_session.diagnosticRunJson.empty()) {
        emit agentStep(QStringLiteral("run_state"),
                       QString::fromLatin1(diagnosticRunStateName(_diagnosticRun.state())),
                       QString::fromStdString(_session.diagnosticRunJson));
    }
    QJsonObject result = AgentJson::parseObject(buildSessionJson(_session));
    result["ok"] = true;
    if (writeOutcomeUnknown) {
        result["recovery_warning"] = QJsonObject{
            {"code", "tool_outcome_unknown"},
            {"correlationId", pendingWrite.value("correlationId")},
            {"tool", pendingWrite.value("tool")},
            {"persisted", recoveryPersisted},
            {"message", "A previously approved write may have completed before restart; it was not replayed."}
        };
    }
    return AgentJson::dump(result);
}
std::string AgentServiceImpl::listTools(){ return _registry.listJson(); }
std::string AgentServiceImpl::callTool(const std::string& n, const std::string& a){
    return executeTool(n, a, ToolOrigin::PublicApi).toJson();
}
std::string AgentServiceImpl::listMcpTools(){ return _registry.listMcpJson(); }
std::string AgentServiceImpl::callMcpTool(const std::string& n, const std::string& a){
    return executeTool(n, a, ToolOrigin::Mcp, true).toJson();
}

const char* AgentServiceImpl::toolOriginName(ToolOrigin origin) {
    switch (origin) {
    case AgentServiceImpl::ToolOrigin::PublicApi: return "public_api";
    case AgentServiceImpl::ToolOrigin::AgentSync: return "agent_sync";
    case AgentServiceImpl::ToolOrigin::AgentInteractive: return "agent_interactive";
    case AgentServiceImpl::ToolOrigin::Autonomous: return "autonomous_executor";
    case AgentServiceImpl::ToolOrigin::Mcp: return "mcp";
    }
    return "unknown";
}

static std::string confirmationArgsSha256(const std::string& argsJson) {
    return QCryptographicHash::hash(QByteArray::fromStdString(argsJson),
                                    QCryptographicHash::Sha256).toHex().toStdString();
}

static bool isSensitiveConfirmationKey(const QString& key) {
    const QString lower = key.toLower();
    return lower == "apikey" || lower == "api_key" || lower == "password"
        || lower == "authorization" || lower == "credential"
        || lower == "credentials" || lower == "secret"
        || lower.endsWith("token") || lower.endsWith("secret")
        || lower.endsWith("password");
}

static QJsonValue redactedConfirmationValue(const QJsonValue& value) {
    if (value.isObject()) {
        QJsonObject redacted;
        const QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
            redacted.insert(it.key(), isSensitiveConfirmationKey(it.key())
                ? QJsonValue(QStringLiteral("[REDACTED]"))
                : redactedConfirmationValue(it.value()));
        return redacted;
    }
    if (value.isArray()) {
        QJsonArray redacted;
        for (const QJsonValue& item : value.toArray())
            redacted.append(redactedConfirmationValue(item));
        return redacted;
    }
    return value;
}

static std::string confirmationArgsSummary(const std::string& argsJson,
                                           const std::string& sha256) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromStdString(argsJson), &parseError);
    QString parameters;
    if (parseError.error == QJsonParseError::NoError && document.isObject()) {
        parameters = QString::fromUtf8(QJsonDocument(
            redactedConfirmationValue(document.object()).toObject())
            .toJson(QJsonDocument::Compact));
    } else {
        parameters = QStringLiteral("[invalid JSON]");
    }
    constexpr int maxVisibleParameters = 1024;
    if (parameters.size() > maxVisibleParameters)
        parameters = parameters.left(maxVisibleParameters) + QStringLiteral("...");
    return (QStringLiteral("Parameters: ") + parameters
            + QStringLiteral("\nArguments SHA-256: ")
            + QString::fromStdString(sha256)
            + QStringLiteral("\nConfirmation expires in 5 minutes.")).toStdString();
}

void AgentServiceImpl::recordToolAudit(const std::string& correlationId, const std::string& name,
                                       const std::string& argsJson, ToolOrigin origin, bool write,
                                       const std::string& confirmation, const std::string& confirmationId,
                                       const std::string& contextFingerprint,
                                       const std::string& toolCatalogSha256,
                                       qint64 startedMs,
                                       qint64 durationMs, const ToolResult& result) {
    QJsonObject audit;
    audit["event"] = "tool_call";
    audit["correlationId"] = QString::fromStdString(correlationId);
    audit["tool"] = QString::fromStdString(name);
    audit["source"] = toolOriginName(origin);
    audit["gen_ai.operation.name"] = "execute_tool";
    audit["gen_ai.tool.name"] = QString::fromStdString(name);
    audit["dpa.telemetry.schema"] = "dpa.telemetry.event";
    audit["dpa.telemetry.version"] = 1;
    audit["dpa.plan_first"] = _planFirstEnabled;
    audit["write"] = write;
    audit["confirmation"] = QString::fromStdString(confirmation);
    if (!confirmationId.empty())
        audit["confirmationId"] = QString::fromStdString(confirmationId);
    audit["contextFingerprint"] = QString::fromStdString(contextFingerprint);
    audit["toolCatalogSha256"] = QString::fromStdString(toolCatalogSha256);
    audit["startedMs"] = double(startedMs);
    audit["durationMs"] = double(durationMs);
    audit["ok"] = result.ok;
    if (!result.error.empty()) audit["error"] = QString::fromStdString(result.error);
    audit["argsSha256"] = QString::fromStdString(confirmationArgsSha256(argsJson));
    if (!result.evidenceRefs.empty()) {
        QJsonArray evidenceRefs;
        for (const auto& ref : result.evidenceRefs)
            evidenceRefs.append(QString::fromStdString(ref));
        audit["dpa.evidence_refs"] = evidenceRefs;
    }
    const std::string content = AgentJson::dump(audit);
    const bool sessionScoped = origin == ToolOrigin::AgentSync
        || origin == ToolOrigin::AgentInteractive;
    if (!sessionScoped) {
        auto appendExternal = [content]() {
            constexpr qint64 maxAuditBytes = 1024 * 1024;
            const QString directory = mcpDir();
            if (!QDir().mkpath(directory) || QFileInfo(directory).isSymLink()) return;
            const QString path = directory + "/external-tool-audit.ndjson";
            const QString rotated = directory + "/external-tool-audit.1.ndjson";
            if (QFileInfo(path).size() >= maxAuditBytes) {
                QFile::remove(rotated);
                QFile::rename(path, rotated);
            }
            QFile file(path);
            if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
                file.write(QByteArray::fromStdString(content));
                file.write("\n");
            }
        };
        if (QThread::currentThread() == thread()) appendExternal();
        else QMetaObject::invokeMethod(this, appendExternal, Qt::BlockingQueuedConnection);
        return;
    }
    auto append = [this, content, correlationId]() {
        std::string pendingBackup;
        if (!_session.pendingWriteJson.empty()) {
            const QJsonObject pending = AgentJson::parseObject(_session.pendingWriteJson);
            if (pending.value("correlationId").toString().toStdString() == correlationId) {
                pendingBackup = _session.pendingWriteJson;
                _session.pendingWriteJson.clear();
            }
        }
        appendSessionTurn("tool", content);
        if (!persistSession() && !pendingBackup.empty()) {
            if (!_session.turns.empty() && _session.turns.back().role == "tool"
                && _session.turns.back().content == content)
                _session.turns.pop_back();
            _session.pendingWriteJson = pendingBackup;
        }
    };
    if (QThread::currentThread() == thread()) append();
    else QMetaObject::invokeMethod(this, append, Qt::BlockingQueuedConnection);
}

ToolResult AgentServiceImpl::executeTool(const std::string& name, const std::string& argsJson,
                                         ToolOrigin origin, bool mcpName) {
    const qint64 startedMs = QDateTime::currentMSecsSinceEpoch();
    const std::string correlationId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    const bool known = mcpName ? _registry.hasMcp(name) : _registry.has(name);
    const bool write = mcpName ? _registry.isWriteMcp(name) : _registry.isWrite(name);
    const std::string toolCatalogSha256 = _registry.approvalCatalogSha256();
    const bool turnScopedOrigin = origin == ToolOrigin::AgentSync
        || origin == ToolOrigin::AgentInteractive
        || origin == ToolOrigin::Autonomous;
    std::string contextFingerprint;
    if (turnScopedOrigin) {
        QMutexLocker contextLock(&_turnContextMutex);
        contextFingerprint = _turnContextFingerprint;
    }
    std::string confirmation = write ? "unavailable" : "not_required";
    std::string confirmationId;
    std::string confirmationDigest;
    qint64 approvedAtMs = 0;
    ToolResult result;

    if (!known) {
        result.ok = false;
        result.error = "unknown_tool";
        result.summary = "The requested tool is not registered.";
    } else if (_diagnosticRun.state() == DiagnosticRunState::AnalysisPending
        && consumesPublishedAnalysisResult(name)) {
        result.ok = false;
        result.error = "analysis_pending";
        result.summary = "The matching analysis process is still running; no result is available yet.";
    } else if (turnScopedOrigin && _cancelRequested.load()) {
        result.ok = false;
        result.error = "cancelled";
        result.summary = "The current Agent turn was cancelled.";
    } else if (write && origin != ToolOrigin::AgentInteractive
               && !(origin == ToolOrigin::Autonomous && _autonomousExecutionActive)) {
        result.ok = false;
        result.error = "requires_in_app_confirmation";
        result.summary = "This write operation requires in-app confirmation.";
    } else {
        if (write) {
            if (origin == ToolOrigin::Autonomous) {
                confirmation = "autonomous_approved";
                approvedAtMs = _autonomousStartedMs;
            } else {
                QMutexLocker lock(&_confirmMutex);
                confirmationId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
                confirmationDigest = confirmationArgsSha256(argsJson);
                _confirmDecision = 0;
                _pendingConfirmationId = confirmationId;
                _pendingConfirmationTool = name;
                _pendingConfirmationArgsSha256 = confirmationDigest;
                _pendingConfirmationExpiresAtMs = QDateTime::currentMSecsSinceEpoch()
                    + kConfirmationTimeoutMs;
                const std::string visibleSummary = confirmationArgsSummary(argsJson, confirmationDigest);
                QMetaObject::invokeMethod(this, [this, confirmationId, name, visibleSummary]() {
                    emit confirmationRequired(QString::fromStdString(confirmationId),
                                              QString::fromStdString(name),
                                              QString::fromStdString(visibleSummary));
                }, Qt::QueuedConnection);
                while (_confirmDecision == 0 && !_cancelRequested.load()) {
                    const qint64 remaining = _pendingConfirmationExpiresAtMs
                        - QDateTime::currentMSecsSinceEpoch();
                    if (remaining <= 0) {
                        _confirmDecision = -2;
                        break;
                    }
                    _confirmCond.wait(&_confirmMutex, static_cast<unsigned long>(remaining));
                }
                const int decision = _confirmDecision;
                const bool contextMatches = _pendingConfirmationId == confirmationId
                    && _pendingConfirmationTool == name
                    && _pendingConfirmationArgsSha256 == confirmationDigest;
                if (_pendingConfirmationId == confirmationId) {
                    _pendingConfirmationId.clear();
                    _pendingConfirmationTool.clear();
                    _pendingConfirmationArgsSha256.clear();
                    _pendingConfirmationExpiresAtMs = 0;
                }
                if (_cancelRequested.load()) {
                    result.ok = false;
                    result.error = "cancelled";
                    result.summary = "The current Agent turn was cancelled.";
                    confirmation = "cancelled";
                } else if (!contextMatches) {
                    result.ok = false;
                    result.error = "confirmation_context_mismatch";
                    result.summary = "The confirmation is stale or does not match this tool invocation.";
                    confirmation = "invalid";
                } else if (decision == -2) {
                    result.ok = false;
                    result.error = "confirmation_expired";
                    result.summary = "The confirmation expired before approval was received.";
                    confirmation = "expired";
                } else if (decision < 0) {
                    result.ok = false;
                    result.error = "user_rejected";
                    result.summary = "The user rejected this operation.";
                    confirmation = "rejected";
                } else {
                    confirmation = "approved";
                    approvedAtMs = QDateTime::currentMSecsSinceEpoch();
                }
            }
        }

        if (result.error.empty()) {
            auto call = [this, &result, name, argsJson, mcpName, turnScopedOrigin, write,
                         correlationId, confirmationId, confirmationDigest, contextFingerprint,
                         toolCatalogSha256, approvedAtMs, startedMs]() {
                if (turnScopedOrigin && !turnContextMatches()) {
                    result = errorResult("context_changed",
                        "项目、试验、当前分析或来源数据在本回合中发生变化；已拒绝执行，请开始新回合重新绑定上下文。");
                    return;
                }
                if (write) {
                    _session.pendingWriteJson = AgentJson::dump(QJsonObject{
                        {"correlationId", QString::fromStdString(correlationId)},
                        {"confirmationId", QString::fromStdString(confirmationId)},
                        {"tool", QString::fromStdString(name)},
                        {"argsSha256", QString::fromStdString(confirmationDigest)},
                        {"contextFingerprint", QString::fromStdString(contextFingerprint)},
                        {"catalogSha256", QString::fromStdString(toolCatalogSha256)},
                        {"approvedAtMs", double(approvedAtMs)},
                        {"startedMs", double(startedMs)}
                    });
                    if (!persistSession()) {
                        _session.pendingWriteJson.clear();
                        result = errorResult("execution_intent_persist_failed",
                            "The approved write was not executed because its intent could not be persisted.");
                        return;
                    }
                }
                if (turnScopedOrigin) beginDiagnosticTool(name);
                result = mcpName ? _registry.callMcp(name, argsJson) : _registry.call(name, argsJson);
                if (turnScopedOrigin) finishDiagnosticTool(name, result);
                if (turnScopedOrigin && result.ok && isAnalysisRunTool(name))
                    refreshTurnContext();
            };
            if (QThread::currentThread() == thread()) call();
            else if (!QMetaObject::invokeMethod(this, call, Qt::BlockingQueuedConnection)) {
                result.ok = false;
                result.error = "tool_dispatch_failed";
                result.summary = "The tool could not be dispatched to the DPA main thread.";
            }
        }
    }

    const qint64 durationMs = QDateTime::currentMSecsSinceEpoch() - startedMs;
    recordToolAudit(correlationId, name, argsJson, origin, write, confirmation, confirmationId,
                    contextFingerprint, toolCatalogSha256, startedMs, durationMs, result);
    return result;
}

// Decode a text attachment: prefer UTF-8, fall back to GB18030 (GBK/ANSI) when UTF-8
// yields replacement chars — common for Excel-exported CSVs on Chinese Windows.
static std::string decodeAttachmentText(const QByteArray& bytes) {
    QString utf = QString::fromUtf8(bytes);
    if (!utf.contains(QChar(0xFFFD))) return utf.toStdString();
    if (QTextCodec* gbk = QTextCodec::codecForName("GB18030")) {
        QString g = gbk->toUnicode(bytes);
        if (!g.isEmpty() && !g.contains(QChar(0xFFFD))) return g.toStdString();
    }
    return utf.toStdString();
}

std::vector<LlmPart> AgentServiceImpl::buildUserParts(const std::string& userText, const std::vector<std::string>& attachmentPaths) const {
    static const qint64 kImageCap    = 10 * 1024 * 1024;   // never load an image larger than this
    static const qint64 kTextReadCap = 256 * 1024;         // read at most this many text bytes
    std::vector<LlmPart> parts;
    LlmPart text;
    text.kind = LlmPart::Text;
    text.text = userText.empty() ? std::string("请分析附件。") : userText;
    parts.push_back(text);

    QMimeDatabase mimeDb;
    for (const std::string& rawPath : attachmentPaths) {
        QFileInfo info(QString::fromStdString(rawPath));
        LlmPart note;
        note.kind = LlmPart::Text;
        if (!info.exists() || !info.isFile()) {
            const std::string displayName = info.fileName().isEmpty()
                ? std::string("unnamed") : info.fileName().toStdString();
            note.text = "\n[附件无法读取] " + displayName;
            parts.push_back(note);
            continue;
        }
        const qint64 sz = info.size();
        const QString mimeQ = mimeDb.mimeTypeForFile(info).name();
        const std::string mimeName = mimeQ.toStdString();
        const bool isImage = mimeQ.startsWith("image/");
        const bool isText  = mimeQ.startsWith("text/") ||
            info.suffix().compare("csv", Qt::CaseInsensitive) == 0 ||
            info.suffix().compare("json", Qt::CaseInsensitive) == 0 ||
            info.suffix().compare("md", Qt::CaseInsensitive) == 0;

        if (isImage) {
            if (sz > kImageCap) {   // check size BEFORE reading — never load a huge file into RAM
                note.text = "\n[图片过大未发送: " + info.fileName().toStdString() +
                    ", " + std::to_string(sz) + " bytes > 10MB]";
                parts.push_back(note);
                continue;
            }
            QFile f(info.absoluteFilePath());
            if (!f.open(QIODevice::ReadOnly)) {
                note.text = "\n[附件无法打开] " + info.fileName().toStdString();
                parts.push_back(note);
                continue;
            }
            QByteArray bytes = f.readAll();
            LlmPart img;
            img.kind = LlmPart::Image;
            img.mediaType = mimeName;
            img.imageBase64 = bytes.toBase64().toStdString();
            parts.push_back(img);
            continue;
        }
        if (isText) {
            QFile f(info.absoluteFilePath());
            if (!f.open(QIODevice::ReadOnly)) {
                note.text = "\n[附件无法打开] " + info.fileName().toStdString();
                parts.push_back(note);
                continue;
            }
            QByteArray bytes = f.read(kTextReadCap);   // read only the prefix, never the whole file
            note.text = "\n[文本附件: " + info.fileName().toStdString() + "]\n" + decodeAttachmentText(bytes);
            if (sz > bytes.size()) note.text += "\n[已截断到 256KB]";
            parts.push_back(note);
            continue;
        }
        // Unknown/binary: describe only (size from metadata), never read into memory.
        note.text = "\n[附件: " + info.fileName().toStdString() + ", 类型: " + mimeName +
            ", 大小: " + std::to_string(sz) + " bytes。当前模型仅直接读取图片和文本内容。]";
        parts.push_back(note);
    }
    return parts;
}

std::string AgentServiceImpl::sendMessage(const std::string& userText) {
    if (!_provider) return R"({"ok":false,"error":"no_provider"})";
    if (userText.size() > 64 * 1024)
        return R"({"ok":false,"error":"message_too_large","summary":"Message exceeds 64 KiB."})";
    if (_diagnosticRun.state() == DiagnosticRunState::AnalysisPending)
        return errorResult("analysis_pending", "Analysis is still running; a new Agent turn is blocked.").toJson();
    if (_turnRunning.exchange(true)) return R"({"ok":false,"error":"turn_running"})";
    _cancelRequested.store(false);
    _provider->resetCancellation();
    if (QThread::currentThread() == thread()) beginTurnContext();
    else QMetaObject::invokeMethod(this, [this]() { beginTurnContext(); }, Qt::BlockingQueuedConnection);
    std::vector<LlmMessage> history = compactedHistory(_session);
    appendSessionTurn("user", userText);
    AgentOrchestrator orch(_provider, &_registry);
    orch.setBudget(_llmBudget);
    orch.setPlanFirst(_planFirstEnabled);
    orch.setToolExecutor([this](const std::string& n, const std::string& a) {
        return executeTool(n, a, ToolOrigin::AgentSync);
    });
    orch.setStepCallback([this](const std::string& kind, const std::string&, const std::string& detail) {
        if (kind == "model_result") {
            appendSessionTurn("tool", detail);
            persistSession();
        }
    });
    orch.setHistory(std::move(history));
    {
        ToolResult sit = buildSituation(assembleSituationFromTest(_dpaTest, _currentProject,
                                                                  QDateTime::currentMSecsSinceEpoch()));
        if (sit.ok) {
            QJsonObject context = AgentJson::parseObject(sit.dataJson);
            context["context_fingerprint"] = QString::fromStdString(
                experimentContextSha256(_currentProject, _daqTest));
            context["tool_catalog_sha256"] = QString::fromStdString(
                _registry.approvalCatalogSha256());
            context["plan_first"] = _planFirstEnabled;
            const QJsonObject briefing = buildExperimentBriefing();
            for (auto it = briefing.begin(); it != briefing.end(); ++it)
                context.insert(it.key(), it.value());
            orch.setSystemContext(AgentJson::dump(context));
        }
    }
    std::string reply = orch.runParts(buildUserParts(userText, {}));
    if (!turnContextMatches()) {
        reply = kContextChangedReply;
        emit agentStep(QStringLiteral("turn_rejected"), QStringLiteral("context_changed"),
                       QString::fromUtf8(kContextChangedReply));
    }
    appendSessionTurn("assistant", reply);
    persistSession();
    if (QThread::currentThread() == thread()) clearTurnContext();
    else QMetaObject::invokeMethod(this, [this]() { clearTurnContext(); }, Qt::BlockingQueuedConnection);
    _cancelRequested.store(false);
    _turnRunning.store(false);
    return reply;
}
void AgentServiceImpl::sendMessageAsync(const std::string& userText) {
    sendMessageWithAttachmentsAsync(userText, {});
}

void AgentServiceImpl::sendMessageWithAttachmentsAsync(const std::string& userText, const std::vector<std::string>& attachmentPaths) {
    if (_turnRunning.exchange(true)) {
        emit agentStep(QStringLiteral("turn_rejected"), QStringLiteral("busy"),
                       QStringLiteral("A response is already running."));
        return;
    }
    if (!_provider) {
        _turnRunning.store(false);
        emit replyReady(QString::fromStdString(R"({"ok":false,"error":"no_provider"})"));
        return;
    }
    if (userText.size() > 64 * 1024 || attachmentPaths.size() > 8) {
        _turnRunning.store(false);
        emit replyReady(QString::fromUtf8(
            R"({"ok":false,"error":"input_quota_exceeded","summary":"Message exceeds 64 KiB or more than 8 attachments were supplied."})"));
        return;
    }
    if (_diagnosticRun.state() == DiagnosticRunState::AnalysisPending) {
        _turnRunning.store(false);
        emit agentStep(QStringLiteral("turn_rejected"), QStringLiteral("analysis_pending"),
                       QStringLiteral("Analysis is still running; a new Agent turn is blocked."));
        return;
    }
    _cancelRequested.store(false);
    _provider->resetCancellation();
    if (QThread::currentThread() == thread()) beginTurnContext();
    else QMetaObject::invokeMethod(this, [this]() { beginTurnContext(); }, Qt::BlockingQueuedConnection);
    // Assemble the situation header on the main thread (reads _dpaTest) BEFORE the worker
    // starts; pass the immutable string into the worker.
    std::string sysCtx;
    {
        ToolResult sit = buildSituation(assembleSituationFromTest(_dpaTest, _currentProject,
                                                                  QDateTime::currentMSecsSinceEpoch()));
        if (sit.ok) {
            QJsonObject context = AgentJson::parseObject(sit.dataJson);
            context["context_fingerprint"] = QString::fromStdString(
                experimentContextSha256(_currentProject, _daqTest));
            context["tool_catalog_sha256"] = QString::fromStdString(
                _registry.approvalCatalogSha256());
            context["plan_first"] = _planFirstEnabled;
            sysCtx = AgentJson::dump(context);
        }
    }
    std::vector<LlmMessage> history = compactedHistory(_session);
    appendSessionTurn("user", userText);
    _turnFuture = QtConcurrent::run([this, userText, sysCtx, attachmentPaths, history]() {
        // Read/encode attachments on the worker thread (file I/O off the UI thread).
        std::vector<LlmPart> userParts = buildUserParts(userText, attachmentPaths);
        AgentOrchestrator orch(_provider, &_registry);
        orch.setBudget(_llmBudget);
        orch.setPlanFirst(_planFirstEnabled);
        if (!sysCtx.empty()) orch.setSystemContext(sysCtx);
        orch.setHistory(history);
        orch.setCancelCallback([this]() { return _cancelRequested.load(); });
        // Relay each tool step to the UI (main thread) for the live workflow timeline.
        orch.setStepCallback([this](const std::string& k, const std::string& n, const std::string& d) {
            QMetaObject::invokeMethod(this, [this, k, n, d]() {
                if (k == "model_result") {
                    appendSessionTurn("tool", d);
                    persistSession();
                }
                emit agentStep(QString::fromStdString(k), QString::fromStdString(n), QString::fromStdString(d));
            }, Qt::QueuedConnection);
        });
        // Run every tool on the MAIN thread (tool handlers touch Qt model objects).
        orch.setToolExecutor([this](const std::string& n, const std::string& a) -> ToolResult {
            return executeTool(n, a, ToolOrigin::AgentInteractive);
        });
        std::string reply = orch.runParts(userParts);
        // Deliver the final reply back on the main thread via the signal (queued) and persist.
        QMetaObject::invokeMethod(this, [this, reply]() {
            const bool contextMatches = turnContextMatches();
            const std::string deliveredReply = contextMatches ? reply : kContextChangedReply;
            if (!contextMatches)
                emit agentStep(QStringLiteral("turn_rejected"), QStringLiteral("context_changed"),
                               QString::fromUtf8(kContextChangedReply));
            appendSessionTurn("assistant", deliveredReply);
            persistSession();
            clearTurnContext();
            _cancelRequested.store(false);
            _turnRunning.store(false);
            emit replyReady(QString::fromStdString(deliveredReply));
        }, Qt::QueuedConnection);
    });
}
