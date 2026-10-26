#include "ExperimentEvidence.h"

#include "StandardsCatalog.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace Model {
namespace {

constexpr int kSchemaVersion = 1;
constexpr int kMaxEvidenceBytes = 1024 * 1024;
constexpr int kMaxTimelineEntries = 400;
constexpr double kMaxExactJsonInteger = 9007199254740991.0;

QByteArray compact(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QString sha256(const QByteArray& bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

bool exactKeys(const QJsonObject& object, const QStringList& keys) {
    if (object.size() != keys.size()) return false;
    for (const QString& key : keys)
        if (!object.contains(key)) return false;
    return true;
}

bool integerInRange(const QJsonValue& value, double minimum, double maximum) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    return std::isfinite(number) && std::floor(number) == number
        && number >= minimum && number <= maximum
        && number <= kMaxExactJsonInteger;
}

bool finiteInRange(const QJsonValue& value, double minimum, double maximum) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    return std::isfinite(number) && number >= minimum && number <= maximum;
}

bool boundedToken(const QJsonValue& value, int maximum, bool allowDot = false) {
    if (!value.isString()) return false;
    const QString text = value.toString();
    if (text.isEmpty() || text.size() > maximum) return false;
    for (const QChar character : text) {
        const ushort code = character.unicode();
        if ((code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z')
            || (code >= '0' && code <= '9') || code == '-' || code == '_'
            || (allowDot && code == '.')) continue;
        return false;
    }
    return true;
}

bool sha256Value(const QJsonValue& value) {
    if (!value.isString() || value.toString().size() != 64) return false;
    for (const QChar character : value.toString()) {
        const ushort code = character.unicode();
        if (!((code >= '0' && code <= '9') || (code >= 'a' && code <= 'f'))) return false;
    }
    return true;
}

bool enumValue(const QJsonValue& value, const QStringList& allowed) {
    return value.isString() && allowed.contains(value.toString());
}

bool boundedCodeArray(const QJsonValue& value, int maximumEntries) {
    if (!value.isArray() || value.toArray().size() > maximumEntries) return false;
    for (const QJsonValue& item : value.toArray())
        if (!boundedToken(item, 64)) return false;
    return true;
}

std::optional<QJsonObject> parsedObject(const std::string& json, int maximumBytes = 128 * 1024) {
    if (json.empty() || json.size() > static_cast<size_t>(maximumBytes)) return std::nullopt;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(json), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return std::nullopt;
    return document.object();
}

std::optional<QJsonObject> sanitizedPreflight(const QJsonObject& input) {
    const QStringList keys = {
        "overall", "ready_to_acquire", "ready_to_monitor", "has_project", "has_daq_test",
        "connected", "acquiring", "saving", "companion_enabled", "selected_channels",
        "configured_monitored_channels", "valid_limit_channels", "active_monitored_channels",
        "data_sources", "blockers", "warnings", "nodes"
    };
    if (!exactKeys(input, keys)
        || !enumValue(input.value("overall"), {"blocked", "ready", "degraded", "monitoring", "monitoring_with_warnings"})
        || !input.value("ready_to_acquire").isBool()
        || !input.value("ready_to_monitor").isBool()
        || !input.value("has_project").isBool()
        || !input.value("has_daq_test").isBool()
        || !input.value("connected").isBool()
        || !input.value("acquiring").isBool()
        || !input.value("saving").isBool()
        || !input.value("companion_enabled").isBool()
        || !integerInRange(input.value("selected_channels"), 0, 1000000)
        || !integerInRange(input.value("configured_monitored_channels"), 0, 1000000)
        || !integerInRange(input.value("valid_limit_channels"), 0, 1000000)
        || !integerInRange(input.value("active_monitored_channels"), 0, 1000000)
        || !integerInRange(input.value("data_sources"), 0, 1000000)
        || !boundedCodeArray(input.value("blockers"), 32)
        || !boundedCodeArray(input.value("warnings"), 32)
        || !input.value("nodes").isArray()
        || input.value("nodes").toArray().size() > 16) return std::nullopt;
    for (const QJsonValue& value : input.value("nodes").toArray()) {
        if (!value.isObject()) return std::nullopt;
        const QJsonObject node = value.toObject();
        if (!exactKeys(node, {"id", "state"}) || !boundedToken(node.value("id"), 64)
            || !enumValue(node.value("state"), {"ok", "active", "waiting", "warning", "blocked"}))
            return std::nullopt;
    }
    return input;
}

std::optional<QJsonObject> sanitizedCompanion(const QJsonObject& input) {
    const QStringList required = {"enabled", "state", "connected", "saving", "monitored_channels",
                                  "active_alerts", "run_started_ms", "elapsed_ms", "event_count"};
    for (const QString& key : required)
        if (!input.contains(key)) return std::nullopt;
    if (!input.value("enabled").isBool() || !input.value("connected").isBool()
        || !input.value("saving").isBool()
        || !enumValue(input.value("state"), {"off", "unavailable", "waiting", "monitoring"})
        || !integerInRange(input.value("monitored_channels"), 0, 1000000)
        || !integerInRange(input.value("active_alerts"), 0, 3000000)
        || !integerInRange(input.value("run_started_ms"), 0, kMaxExactJsonInteger)
        || !integerInRange(input.value("elapsed_ms"), 0, kMaxExactJsonInteger)
        || !integerInRange(input.value("event_count"), 0, kMaxExactJsonInteger)) return std::nullopt;
    QJsonObject result;
    for (const QString& key : required) result.insert(key, input.value(key));
    return result;
}

std::optional<QJsonObject> sanitizedDiagnostic(const QJsonObject& input) {
    if (!exactKeys(input, {"state", "current_step", "error", "updated_at"})
        || !enumValue(input.value("state"), {"context_ready", "analysis_pending", "analysis_complete",
            "diagnosis_complete", "report_pending", "complete", "failed", "cancelled"})
        || !enumValue(input.value("current_step"), {"context_ready", "analysis_pending", "analysis_complete",
            "diagnosis_complete", "report_pending", "complete"})
        || !input.value("error").isString()
        || input.value("error").toString().toUtf8().size() > 4096
        || !integerInRange(input.value("updated_at"), 0, kMaxExactJsonInteger)) return std::nullopt;
    const QByteArray error = input.value("error").toString().toUtf8();
    if ((input.value("state").toString() == "failed") != !error.isEmpty()) return std::nullopt;
    return QJsonObject{{"state", input.value("state")},
                       {"current_step", input.value("current_step")},
                       {"updated_at", input.value("updated_at")},
                       {"has_error", !error.isEmpty()},
                       {"error_sha256", sha256(error)}};
}

bool validRecipe(const QJsonObject& recipe) {
    if (!exactKeys(recipe, {"schema", "version", "scenario", "amplitude", "offset", "noise",
        "duration_ms", "trigger_ms", "fault_duration_ms", "seed", "speed", "sampling_rate_hz",
        "low_limit", "high_limit"})
        || recipe.value("schema").toString() != "dpa.simulation.recipe"
        || !integerInRange(recipe.value("version"), 1, 1)
        || !integerInRange(recipe.value("scenario"), 1, 6)
        || !finiteInRange(recipe.value("amplitude"), 0, 1000000)
        || !finiteInRange(recipe.value("offset"), -100000000, 100000000)
        || !finiteInRange(recipe.value("noise"), 0, 1000000)
        || !integerInRange(recipe.value("duration_ms"), 100, 24 * 60 * 60 * 1000)
        || !integerInRange(recipe.value("trigger_ms"), 0, 24 * 60 * 60 * 1000)
        || !integerInRange(recipe.value("fault_duration_ms"), 1, 24 * 60 * 60 * 1000)
        || !integerInRange(recipe.value("seed"), 1, 4294967295.0)
        || !finiteInRange(recipe.value("speed"), 0.1, 3600)
        || !integerInRange(recipe.value("sampling_rate_hz"), 1, 204800)
        || !finiteInRange(recipe.value("low_limit"), -1000000000, 1000000000)
        || !finiteInRange(recipe.value("high_limit"), -1000000000, 1000000000)) return false;
    const double trigger = recipe.value("trigger_ms").toDouble();
    const double duration = recipe.value("duration_ms").toDouble();
    const double fault = recipe.value("fault_duration_ms").toDouble();
    return trigger < duration && fault <= duration - trigger
        && recipe.value("low_limit").toDouble() < recipe.value("high_limit").toDouble();
}

std::optional<QJsonObject> sanitizedSimulator(const QJsonObject& input) {
    if (!input.value("present").isBool()) return std::nullopt;
    if (!input.value("present").toBool()) {
        if (!exactKeys(input, {"present"})) return std::nullopt;
        return input;
    }
    if (!exactKeys(input, {"present", "recipe", "recipe_sha256", "elapsed_ms", "sample_index",
                           "sampling", "saving", "paused"})
        || !input.value("recipe").isObject() || !validRecipe(input.value("recipe").toObject())
        || !sha256Value(input.value("recipe_sha256"))
        || sha256(compact(input.value("recipe").toObject())) != input.value("recipe_sha256").toString()
        || !integerInRange(input.value("elapsed_ms"), 0, 24 * 60 * 60 * 1000)
        || !integerInRange(input.value("sample_index"), 0, kMaxExactJsonInteger)
        || !input.value("sampling").isBool() || !input.value("saving").isBool()
        || !input.value("paused").isBool()) return std::nullopt;
    return input;
}

std::optional<QJsonObject> sanitizedStandardsPlan(const std::string& json,
                                                  const std::string& expectedContextSha256,
                                                  std::string* planSha256 = nullptr) {
    const auto parsed = parsedObject(json.empty() ? R"({"present":false})" : json, 128 * 1024);
    if (!parsed || !parsed->value("present").isBool()) return std::nullopt;
    if (!parsed->value("present").toBool()) {
        if (!exactKeys(*parsed, {"present"})) return std::nullopt;
        if (planSha256) planSha256->clear();
        return parsed;
    }
    if (!exactKeys(*parsed, {"present", "plan"}) || !parsed->value("plan").isObject())
        return std::nullopt;
    const std::string planJson = QJsonDocument(parsed->value("plan").toObject())
        .toJson(QJsonDocument::Compact).toStdString();
    const StandardTestPlanValidation validation = validateStandardTestPlanJson(
        planJson, expectedContextSha256);
    if (!validation.ok) return std::nullopt;
    if (planSha256) *planSha256 = validation.planSha256;
    return parsed;
}

struct TimelineCandidate {
    long long timestampMs = 0;
    int ordinal = 0;
    QJsonObject event;
};

std::optional<TimelineCandidate> auditCandidate(const std::string& json, int ordinal) {
    const auto parsed = parsedObject(json, 64 * 1024);
    if (!parsed || parsed->value("event").toString() != "tool_call"
        || !boundedToken(parsed->value("correlationId"), 128)
        || !boundedToken(parsed->value("tool"), 128, true)
        || !boundedToken(parsed->value("source"), 32)
        || !parsed->value("write").isBool()
        || !boundedToken(parsed->value("confirmation"), 32)
        || !sha256Value(parsed->value("contextFingerprint"))
        || !sha256Value(parsed->value("toolCatalogSha256"))
        || !integerInRange(parsed->value("startedMs"), 0, kMaxExactJsonInteger)
        || !integerInRange(parsed->value("durationMs"), 0, 24 * 60 * 60 * 1000)
        || !parsed->value("ok").isBool()
        || !sha256Value(parsed->value("argsSha256"))) return std::nullopt;
    const QString confirmationId = parsed->value("confirmationId").toString();
    const QString error = parsed->value("error").toString();
    if ((!confirmationId.isEmpty() && !boundedToken(parsed->value("confirmationId"), 128))
        || error.size() > 128) return std::nullopt;
    QJsonArray evidenceRefs;
    if (parsed->contains("dpa.evidence_refs")) {
        if (!parsed->value("dpa.evidence_refs").isArray()
            || parsed->value("dpa.evidence_refs").toArray().size() > 64) return std::nullopt;
        for (const QJsonValue& ref : parsed->value("dpa.evidence_refs").toArray()) {
            if (!boundedToken(ref, 256)) return std::nullopt;
            evidenceRefs.append(ref);
        }
    }
    QJsonObject event{
        {"source", "tool_audit"},
        {"event_ref_sha256", sha256(parsed->value("correlationId").toString().toUtf8())},
        {"timestamp_ms", parsed->value("startedMs")},
        {"kind", "tool_call"},
        {"tool", parsed->value("tool")},
        {"origin", parsed->value("source")},
        {"write", parsed->value("write")},
        {"confirmation", parsed->value("confirmation")},
        {"confirmation_ref_sha256", sha256(confirmationId.toUtf8())},
        {"context_fingerprint", parsed->value("contextFingerprint")},
        {"tool_catalog_sha256", parsed->value("toolCatalogSha256")},
        {"duration_ms", parsed->value("durationMs")},
        {"ok", parsed->value("ok")},
        {"error_code", error},
        {"args_sha256", parsed->value("argsSha256")},
        {"evidence_refs", evidenceRefs}
    };
    return TimelineCandidate{static_cast<long long>(parsed->value("startedMs").toDouble()), ordinal, event};
}

std::optional<TimelineCandidate> companionCandidate(const std::string& json, int ordinal) {
    const auto parsed = parsedObject(json, 64 * 1024);
    if (!parsed || !boundedToken(parsed->value("id"), 128)
        || !integerInRange(parsed->value("timestamp_ms"), 0, kMaxExactJsonInteger)
        || parsed->value("source").toString() != "local_monitor"
        || !boundedToken(parsed->value("severity"), 16)
        || !boundedToken(parsed->value("kind"), 64)
        || !parsed->value("title").isString() || parsed->value("title").toString().toUtf8().size() > 2048
        || !parsed->value("message").isString() || parsed->value("message").toString().toUtf8().size() > 8192
        || !parsed->value("auto_explain").isBool()) return std::nullopt;
    const bool measured = parsed->contains("channel_id");
    if (measured && (!integerInRange(parsed->value("channel_id"), 0, 1000000)
        || !finiteInRange(parsed->value("value"), -1e300, 1e300)
        || !finiteInRange(parsed->value("limit"), -1e300, 1e300)
        || !parsed->value("unit").isString() || parsed->value("unit").toString().size() > 64))
        return std::nullopt;
    QJsonObject event{
        {"source", "companion"},
        {"event_ref_sha256", sha256(parsed->value("id").toString().toUtf8())},
        {"timestamp_ms", parsed->value("timestamp_ms")},
        {"kind", parsed->value("kind")},
        {"severity", parsed->value("severity")},
        {"auto_explain", parsed->value("auto_explain")},
        {"has_measurement", measured},
        {"channel_id", measured ? parsed->value("channel_id") : QJsonValue(-1)},
        {"value", measured ? parsed->value("value") : QJsonValue(0)},
        {"limit", measured ? parsed->value("limit") : QJsonValue(0)},
        {"unit", measured ? parsed->value("unit") : QJsonValue("")},
        {"title_sha256", sha256(parsed->value("title").toString().toUtf8())},
        {"message_sha256", sha256(parsed->value("message").toString().toUtf8())}
    };
    return TimelineCandidate{static_cast<long long>(parsed->value("timestamp_ms").toDouble()), ordinal, event};
}

QJsonObject privacyPolicy() {
    return QJsonObject{{"raw_conversation_included", false},
                       {"raw_attachments_included", false},
                       {"raw_waveforms_included", false},
                       {"paths_included", false},
                       {"secrets_included", false},
                       {"event_text_included", false},
                       {"event_text_sha256_only", true}};
}

bool validOpaqueRefs(const QJsonValue& value) {
    if (!value.isArray() || value.toArray().size() > 64) return false;
    for (const QJsonValue& ref : value.toArray())
        if (!boundedToken(ref, 256)) return false;
    return true;
}

bool validTimelineEvent(const QJsonObject& event, int sequence) {
    if (!integerInRange(event.value("sequence"), sequence, sequence)
        || !integerInRange(event.value("timestamp_ms"), 0, kMaxExactJsonInteger)
        || !sha256Value(event.value("event_ref_sha256"))) return false;
    if (event.value("source").toString() == "tool_audit") {
        const bool currentKeys = exactKeys(event, {"sequence", "source", "event_ref_sha256", "timestamp_ms", "kind",
            "tool", "origin", "write", "confirmation", "confirmation_ref_sha256",
            "context_fingerprint", "tool_catalog_sha256", "duration_ms", "ok", "error_code",
            "args_sha256", "evidence_refs"});
        const bool legacyKeys = exactKeys(event, {"sequence", "source", "event_ref_sha256", "timestamp_ms", "kind",
            "tool", "origin", "write", "confirmation", "confirmation_ref_sha256",
            "context_fingerprint", "tool_catalog_sha256", "duration_ms", "ok", "error_code", "args_sha256"});
        const bool evidenceRefsValid = !currentKeys || validOpaqueRefs(event.value("evidence_refs"));
        return (currentKeys || legacyKeys)
            && event.value("kind").toString() == "tool_call"
            && boundedToken(event.value("tool"), 128, true)
            && boundedToken(event.value("origin"), 32)
            && event.value("write").isBool()
            && boundedToken(event.value("confirmation"), 32)
            && sha256Value(event.value("confirmation_ref_sha256"))
            && sha256Value(event.value("context_fingerprint"))
            && sha256Value(event.value("tool_catalog_sha256"))
            && integerInRange(event.value("duration_ms"), 0, 24 * 60 * 60 * 1000)
            && event.value("ok").isBool()
            && event.value("error_code").isString() && event.value("error_code").toString().size() <= 128
            && sha256Value(event.value("args_sha256"))
            && evidenceRefsValid;
    }
    if (event.value("source").toString() == "companion") {
        return exactKeys(event, {"sequence", "source", "event_ref_sha256", "timestamp_ms", "kind",
            "severity", "auto_explain", "has_measurement", "channel_id", "value", "limit", "unit",
            "title_sha256", "message_sha256"})
            && boundedToken(event.value("kind"), 64)
            && boundedToken(event.value("severity"), 16)
            && event.value("auto_explain").isBool() && event.value("has_measurement").isBool()
            && integerInRange(event.value("channel_id"), -1, 1000000)
            && finiteInRange(event.value("value"), -1e300, 1e300)
            && finiteInRange(event.value("limit"), -1e300, 1e300)
            && event.value("unit").isString() && event.value("unit").toString().size() <= 64
            && sha256Value(event.value("title_sha256")) && sha256Value(event.value("message_sha256"));
    }
    return false;
}

} // namespace

std::string buildExperimentEvidenceJson(const SessionRecord& session,
                                        const ExperimentEvidenceSnapshot& snapshot) {
    if (snapshot.generatedAtMs < 0 || snapshot.generatedAtMs > static_cast<long long>(kMaxExactJsonInteger)
        || snapshot.toolCatalogSha256.size() != 64
        || snapshot.experimentContextSha256.size() != 64) return {};
    const auto preflightInput = parsedObject(snapshot.preflightJson);
    const auto companionInput = parsedObject(snapshot.companionStatusJson);
    const auto diagnosticInput = parsedObject(snapshot.diagnosticRunJson);
    const auto standardsPlan = sanitizedStandardsPlan(snapshot.standardsPlanJson,
                                                       snapshot.experimentContextSha256);
    const auto simulatorInput = parsedObject(snapshot.simulatorJson);
    if (!preflightInput || !companionInput || !diagnosticInput || !standardsPlan || !simulatorInput)
        return {};
    const auto preflight = sanitizedPreflight(*preflightInput);
    const auto companion = sanitizedCompanion(*companionInput);
    const auto diagnostic = sanitizedDiagnostic(*diagnosticInput);
    const auto simulator = sanitizedSimulator(*simulatorInput);
    if (!preflight || !companion || !diagnostic || !standardsPlan || !simulator
        || !sha256Value(QJsonValue(QString::fromStdString(snapshot.toolCatalogSha256)))
        || !sha256Value(QJsonValue(QString::fromStdString(snapshot.experimentContextSha256)))) return {};

    std::vector<TimelineCandidate> candidates;
    candidates.reserve(session.turns.size() + session.companionEvents.size());
    int omitted = 0;
    int ordinal = 0;
    for (const SessionTurn& turn : session.turns) {
        if (turn.role != "tool") continue;
        const auto candidate = auditCandidate(turn.content, ordinal++);
        if (candidate && candidate->timestampMs <= snapshot.generatedAtMs)
            candidates.push_back(*candidate);
        else ++omitted;
    }
    for (const std::string& eventJson : session.companionEvents) {
        const auto candidate = companionCandidate(eventJson, ordinal++);
        if (candidate && candidate->timestampMs <= snapshot.generatedAtMs)
            candidates.push_back(*candidate);
        else ++omitted;
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        return left.timestampMs < right.timestampMs
            || (left.timestampMs == right.timestampMs && left.ordinal < right.ordinal);
    });
    if (candidates.size() > kMaxTimelineEntries) return {};

    QJsonArray timeline;
    int toolEvents = 0;
    int companionEvents = 0;
    for (int index = 0; index < static_cast<int>(candidates.size()); ++index) {
        QJsonObject event = candidates[static_cast<size_t>(index)].event;
        event["sequence"] = index;
        timeline.append(event);
        if (event.value("source").toString() == "tool_audit") ++toolEvents;
        else ++companionEvents;
    }
    QJsonObject payload{
        {"generated_at_ms", static_cast<double>(snapshot.generatedAtMs)},
        {"capture_mode", "metadata_only"},
        {"session_ref_sha256", sha256(QByteArray::fromStdString(session.id))},
        {"experiment_context_sha256", QString::fromStdString(snapshot.experimentContextSha256)},
        {"tool_catalog_sha256", QString::fromStdString(snapshot.toolCatalogSha256)},
        {"preflight", *preflight},
        {"companion", *companion},
        {"diagnostic_run", *diagnostic},
        {"standards_plan", *standardsPlan},
        {"simulator", *simulator},
        {"timeline", timeline},
        {"counts", QJsonObject{{"timeline_entries", timeline.size()}, {"tool_events", toolEvents},
                                {"companion_events", companionEvents}, {"omitted_events", omitted}}},
        {"privacy", privacyPolicy()}
    };
    const QString digest = sha256(compact(payload));
    const QJsonObject root{{"schema", "dpa.experiment.evidence"},
                           {"schema_version", kSchemaVersion},
                           {"payload", payload},
                           {"integrity", QJsonObject{{"algorithm", "SHA-256"},
                                                     {"payload_sha256", digest}}}};
    const QByteArray serialized = compact(root);
    return serialized.size() <= kMaxEvidenceBytes ? serialized.toStdString() : std::string{};
}

ExperimentEvidenceVerification verifyExperimentEvidenceJson(
    const std::string& json, const std::string& expectedToolCatalogSha256) {
    ExperimentEvidenceVerification result;
    auto fail = [&result](const char* error) {
        result.error = error;
        return result;
    };
    if (json.empty() || json.size() > kMaxEvidenceBytes) return fail("evidence_size_invalid");
    const auto rootInput = parsedObject(json, kMaxEvidenceBytes);
    if (!rootInput || !exactKeys(*rootInput, {"schema", "schema_version", "payload", "integrity"}))
        return fail("evidence_schema_invalid");
    if (rootInput->value("schema").toString() != "dpa.experiment.evidence"
        || !integerInRange(rootInput->value("schema_version"), kSchemaVersion, kSchemaVersion)
        || !rootInput->value("payload").isObject() || !rootInput->value("integrity").isObject())
        return fail("evidence_schema_unsupported");
    const QJsonObject integrity = rootInput->value("integrity").toObject();
    if (!exactKeys(integrity, {"algorithm", "payload_sha256"})
        || integrity.value("algorithm").toString() != "SHA-256"
        || !sha256Value(integrity.value("payload_sha256"))) return fail("evidence_integrity_invalid");
    const QJsonObject payload = rootInput->value("payload").toObject();
    const QString digest = sha256(compact(payload));
    if (digest != integrity.value("payload_sha256").toString()) return fail("evidence_integrity_mismatch");
    result.payloadSha256 = digest.toStdString();
    const QStringList legacyPayloadKeys = {"generated_at_ms", "capture_mode", "session_ref_sha256",
        "experiment_context_sha256", "tool_catalog_sha256", "preflight", "companion",
        "diagnostic_run", "simulator", "timeline", "counts", "privacy"};
    const QStringList currentPayloadKeys = {"generated_at_ms", "capture_mode", "session_ref_sha256",
        "experiment_context_sha256", "tool_catalog_sha256", "preflight", "companion",
        "diagnostic_run", "standards_plan", "simulator", "timeline", "counts", "privacy"};
    const bool hasStandardsPlan = payload.contains("standards_plan");
    if ((!exactKeys(payload, legacyPayloadKeys) && !exactKeys(payload, currentPayloadKeys))
        || !integerInRange(payload.value("generated_at_ms"), 0, kMaxExactJsonInteger)
        || payload.value("capture_mode").toString() != "metadata_only"
        || !sha256Value(payload.value("session_ref_sha256"))
        || !sha256Value(payload.value("experiment_context_sha256"))
        || !sha256Value(payload.value("tool_catalog_sha256"))
        || !payload.value("preflight").isObject() || !payload.value("companion").isObject()
        || !payload.value("diagnostic_run").isObject() || !payload.value("simulator").isObject()
        || (hasStandardsPlan && !payload.value("standards_plan").isObject())
        || !payload.value("timeline").isArray() || !payload.value("counts").isObject()
        || payload.value("privacy").toObject() != privacyPolicy()) return fail("evidence_payload_invalid");
    const auto preflight = sanitizedPreflight(payload.value("preflight").toObject());
    const auto companion = sanitizedCompanion(payload.value("companion").toObject());
    std::string standardsPlanSha256;
    const auto standardsPlan = hasStandardsPlan
        ? sanitizedStandardsPlan(QJsonDocument(payload.value("standards_plan").toObject())
                                     .toJson(QJsonDocument::Compact).toStdString(),
                                 payload.value("experiment_context_sha256").toString().toStdString(),
                                 &standardsPlanSha256)
        : std::optional<QJsonObject>(QJsonObject{{"present", false}});
    const auto simulator = sanitizedSimulator(payload.value("simulator").toObject());
    const QJsonObject diagnostic = payload.value("diagnostic_run").toObject();
    if (!preflight || *preflight != payload.value("preflight").toObject()
        || !companion || *companion != payload.value("companion").toObject()
        || !standardsPlan
        || (hasStandardsPlan && *standardsPlan != payload.value("standards_plan").toObject())
        || !simulator || *simulator != payload.value("simulator").toObject()
        || !exactKeys(diagnostic, {"state", "current_step", "updated_at", "has_error", "error_sha256"})
        || !enumValue(diagnostic.value("state"), {"context_ready", "analysis_pending", "analysis_complete",
            "diagnosis_complete", "report_pending", "complete", "failed", "cancelled"})
        || !enumValue(diagnostic.value("current_step"), {"context_ready", "analysis_pending", "analysis_complete",
            "diagnosis_complete", "report_pending", "complete"})
        || !integerInRange(diagnostic.value("updated_at"), 0, kMaxExactJsonInteger)
        || !diagnostic.value("has_error").isBool() || !sha256Value(diagnostic.value("error_sha256")))
        return fail("evidence_snapshot_invalid");
    const bool hasDiagnosticError = diagnostic.value("has_error").toBool();
    if ((diagnostic.value("state").toString() == "failed") != hasDiagnosticError
        || (!hasDiagnosticError
            && diagnostic.value("error_sha256").toString() != sha256(QByteArray())))
        return fail("evidence_snapshot_invalid");

    const QJsonArray timeline = payload.value("timeline").toArray();
    if (timeline.size() > kMaxTimelineEntries) return fail("evidence_timeline_too_large");
    long long previousTimestamp = -1;
    for (int index = 0; index < timeline.size(); ++index) {
        if (!timeline.at(index).isObject() || !validTimelineEvent(timeline.at(index).toObject(), index))
            return fail("evidence_timeline_invalid");
        const QJsonObject event = timeline.at(index).toObject();
        const long long timestamp = static_cast<long long>(event.value("timestamp_ms").toDouble());
        if (timestamp < previousTimestamp
            || timestamp > static_cast<long long>(payload.value("generated_at_ms").toDouble()))
            return fail("evidence_timeline_unordered");
        previousTimestamp = timestamp;
        if (event.value("source").toString() == "tool_audit") ++result.toolEvents;
        else ++result.companionEvents;
    }
    const QJsonObject counts = payload.value("counts").toObject();
    if (!exactKeys(counts, {"timeline_entries", "tool_events", "companion_events", "omitted_events"})
        || !integerInRange(counts.value("timeline_entries"), timeline.size(), timeline.size())
        || !integerInRange(counts.value("tool_events"), result.toolEvents, result.toolEvents)
        || !integerInRange(counts.value("companion_events"), result.companionEvents, result.companionEvents)
        || !integerInRange(counts.value("omitted_events"), 0, 400)) return fail("evidence_counts_invalid");
    result.timelineEntries = timeline.size();
    result.omittedEvents = static_cast<int>(counts.value("omitted_events").toDouble());
    result.simulatorRecipePresent = simulator->value("present").toBool();
    result.catalogMatches = expectedToolCatalogSha256.empty()
        || expectedToolCatalogSha256 == payload.value("tool_catalog_sha256").toString().toStdString();
    result.standardsPlanPresent = standardsPlan->value("present").toBool();
    result.standardsPlanSha256 = standardsPlanSha256;
    result.standardsPlanContextMatches = true;
    const QJsonObject replay{
        {"mode", "verification_only"},
        {"integrity_verified", true},
        {"schema_supported", true},
        {"catalog_matches_current", result.catalogMatches},
        {"timeline", timeline},
        {"simulator_recipe", result.simulatorRecipePresent
            ? simulator->value("recipe") : QJsonValue(QJsonValue::Null)},
        {"standards_plan", result.standardsPlanPresent
            ? standardsPlan->value("plan") : QJsonValue(QJsonValue::Null)},
        {"side_effects", QJsonObject{{"model_calls", 0}, {"tool_calls", 0},
                                      {"device_reads", 0}, {"device_writes", 0}}}
    };
    result.replayJson = compact(replay).toStdString();
    result.ok = true;
    return result;
}

} // namespace Model
