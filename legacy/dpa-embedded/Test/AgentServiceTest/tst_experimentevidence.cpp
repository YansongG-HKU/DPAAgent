#include <QtTest/QtTest>

#include "AgentArtifactPath.h"
#include "ExperimentEvidence.h"
#include "StandardsCatalog.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

using namespace Model;

namespace {

std::string dump(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

QJsonObject preflight() {
    return QJsonObject{{"overall", "monitoring"}, {"ready_to_acquire", true},
        {"ready_to_monitor", true}, {"has_project", true}, {"has_daq_test", true},
        {"connected", true}, {"acquiring", true}, {"saving", true},
        {"companion_enabled", true}, {"selected_channels", 4},
        {"configured_monitored_channels", 4}, {"valid_limit_channels", 4},
        {"active_monitored_channels", 4}, {"data_sources", 1},
        {"blockers", QJsonArray{}}, {"warnings", QJsonArray{}},
        {"nodes", QJsonArray{QJsonObject{{"id", "context"}, {"state", "ok"}},
                              QJsonObject{{"id", "data_flow"}, {"state", "active"}}}}};
}

QJsonObject companion() {
    return QJsonObject{{"enabled", true}, {"state", "monitoring"}, {"connected", true},
        {"saving", true}, {"monitored_channels", 4}, {"active_alerts", 1},
        {"run_started_ms", 900}, {"elapsed_ms", 100}, {"event_count", 1},
        {"preflight", preflight()}};
}

QJsonObject recipe() {
    return QJsonObject{{"schema", "dpa.simulation.recipe"}, {"version", 1},
        {"scenario", 2}, {"amplitude", 2.0}, {"offset", 0.0}, {"noise", 0.05},
        {"duration_ms", 30000}, {"trigger_ms", 5000}, {"fault_duration_ms", 5000},
        {"seed", 20260715.0}, {"speed", 1.0}, {"sampling_rate_hz", 1000},
        {"low_limit", -10.0}, {"high_limit", 10.0}};
}

QJsonObject simulator(bool present) {
    if (!present) return QJsonObject{{"present", false}};
    const QJsonObject value = recipe();
    const QByteArray canonical = QJsonDocument(value).toJson(QJsonDocument::Compact);
    const QString digest = QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex();
    return QJsonObject{{"present", true}, {"recipe", value}, {"recipe_sha256", digest},
        {"elapsed_ms", 1200}, {"sample_index", 1200}, {"sampling", true},
        {"saving", true}, {"paused", false}};
}

ExperimentEvidenceSnapshot snapshot(bool withSimulator = false) {
    ExperimentEvidenceSnapshot value;
    value.generatedAtMs = 2000;
    value.toolCatalogSha256 = std::string(64, 'a');
    value.experimentContextSha256 = std::string(64, 'd');
    value.preflightJson = dump(preflight());
    value.companionStatusJson = dump(companion());
    value.diagnosticRunJson = dump(QJsonObject{{"state", "complete"},
        {"current_step", "complete"}, {"error", ""}, {"updated_at", 1900}});
    value.simulatorJson = dump(simulator(withSimulator));
    return value;
}

std::string boundStandardsPlan() {
    StandardTestPlanRequest request;
    request.machineType = "centrifugal_pump";
    request.ratedPowerKw = 75;
    request.rpm = 1800;
    request.platform = "industrial";
    request.testPurpose = "operational_vibration";
    request.physicalQuantity = "vibration_velocity";
    request.applicabilityBasis = "evidence test scope";
    request.experimentContextSha256 = std::string(64, 'd');
    request.requestedStandardProfiles = {"iso-20816-3:2022"};
    const QJsonObject plan = QJsonDocument::fromJson(
        QByteArray::fromStdString(buildStandardTestPlanJson(request))).object();
    return dump(QJsonObject{{"present", true}, {"plan", plan}});
}

SessionRecord sessionWithEvidence() {
    SessionRecord session;
    session.id = "session-1";
    session.title = "private project";
    session.turns.push_back({"user", "TOP-SECRET API key api-key-never-export and C:/private/path"});
    session.turns.push_back({"assistant", "private answer"});
    session.turns.push_back({"tool", dump(QJsonObject{
        {"event", "tool_call"}, {"correlationId", "event-1"}, {"tool", "experiment_preflight"},
        {"source", "agent"}, {"write", false}, {"confirmation", "not_required"},
        {"contextFingerprint", QString(64, 'b')}, {"toolCatalogSha256", QString(64, 'a')},
        {"startedMs", 1000}, {"durationMs", 12}, {"ok", true},
        {"argsSha256", QString(64, 'c')}})});
    session.companionEvents.push_back(dump(QJsonObject{
        {"id", "event-2"}, {"timestamp_ms", 1100}, {"source", "local_monitor"},
        {"severity", "alarm"}, {"kind", "limit_high"}, {"title", "Private Motor A"},
        {"message", "Private Motor A exceeded its configured limit"}, {"auto_explain", true},
        {"channel_id", 1}, {"value", 12.5}, {"limit", 10.0}, {"unit", "g"}}));
    return session;
}

} // namespace

class TstExperimentEvidence : public QObject {
    Q_OBJECT
private slots:
    void builds_privacy_minimized_verifiable_timeline() {
        const std::string evidence = buildExperimentEvidenceJson(sessionWithEvidence(), snapshot());
        QVERIFY(!evidence.empty());
        const QByteArray bytes = QByteArray::fromStdString(evidence);
        QVERIFY(!bytes.contains("TOP-SECRET"));
        QVERIFY(!bytes.contains("api-key-never-export"));
        QVERIFY(!bytes.contains("C:/private/path"));
        QVERIFY(!bytes.contains("Private Motor A"));
        QVERIFY(!bytes.contains("private project"));

        const auto verification = verifyExperimentEvidenceJson(evidence, std::string(64, 'a'));
        QVERIFY(verification.ok);
        QVERIFY(verification.catalogMatches);
        QCOMPARE(verification.timelineEntries, 2);
        QCOMPARE(verification.toolEvents, 1);
        QCOMPARE(verification.companionEvents, 1);
        QCOMPARE(verification.omittedEvents, 0);
        QVERIFY(!verification.simulatorRecipePresent);

        const QJsonObject replay = QJsonDocument::fromJson(
            QByteArray::fromStdString(verification.replayJson)).object();
        QCOMPARE(replay.value("mode").toString(), QString("verification_only"));
        QCOMPARE(replay.value("timeline").toArray().size(), 2);
        const QJsonObject sideEffects = replay.value("side_effects").toObject();
        QCOMPARE(sideEffects.value("model_calls").toInt(), 0);
        QCOMPARE(sideEffects.value("tool_calls").toInt(), 0);
        QCOMPARE(sideEffects.value("device_reads").toInt(), 0);
        QCOMPARE(sideEffects.value("device_writes").toInt(), 0);
    }

    void detects_tampering_and_unsupported_schema() {
        const std::string evidence = buildExperimentEvidenceJson(sessionWithEvidence(), snapshot());
        QJsonObject root = QJsonDocument::fromJson(QByteArray::fromStdString(evidence)).object();
        QJsonObject payload = root.value("payload").toObject();
        QJsonObject counts = payload.value("counts").toObject();
        counts["tool_events"] = 0;
        payload["counts"] = counts;
        root["payload"] = payload;
        auto verification = verifyExperimentEvidenceJson(dump(root));
        QVERIFY(!verification.ok);
        QCOMPARE(QString::fromStdString(verification.error), QString("evidence_integrity_mismatch"));

        root = QJsonDocument::fromJson(QByteArray::fromStdString(evidence)).object();
        root["schema_version"] = 2;
        verification = verifyExperimentEvidenceJson(dump(root));
        QVERIFY(!verification.ok);
        QCOMPARE(QString::fromStdString(verification.error), QString("evidence_schema_unsupported"));
    }

    void reports_catalog_drift_without_reexecution() {
        const std::string evidence = buildExperimentEvidenceJson(sessionWithEvidence(), snapshot());
        const auto verification = verifyExperimentEvidenceJson(evidence, std::string(64, 'f'));
        QVERIFY(verification.ok);
        QVERIFY(!verification.catalogMatches);
        const QJsonObject replay = QJsonDocument::fromJson(
            QByteArray::fromStdString(verification.replayJson)).object();
        QVERIFY(!replay.value("catalog_matches_current").toBool(true));
        QCOMPARE(replay.value("side_effects").toObject().value("device_writes").toInt(), 0);
    }

    void preserves_opaque_evidence_refs_in_tool_timeline() {
        SessionRecord session = sessionWithEvidence();
        QJsonObject tool = QJsonDocument::fromJson(
            QByteArray::fromStdString(session.turns[2].content)).object();
        tool["dpa.evidence_refs"] = QJsonArray{"evidence-1", "evidence-2"};
        session.turns[2].content = dump(tool);
        const std::string evidence = buildExperimentEvidenceJson(session, snapshot());
        const auto verification = verifyExperimentEvidenceJson(evidence, std::string(64, 'a'));
        QVERIFY(verification.ok);
        const QJsonObject replay = QJsonDocument::fromJson(
            QByteArray::fromStdString(verification.replayJson)).object();
        const QJsonObject toolEvent = replay.value("timeline").toArray().at(0).toObject();
        QCOMPARE(toolEvent.value("evidence_refs").toArray().size(), 2);
        QCOMPARE(toolEvent.value("evidence_refs").toArray().at(0).toString(), QString("evidence-1"));
    }

    void includes_integrity_checked_simulator_recipe() {
        const std::string evidence = buildExperimentEvidenceJson(sessionWithEvidence(), snapshot(true));
        auto verification = verifyExperimentEvidenceJson(evidence, std::string(64, 'a'));
        QVERIFY(verification.ok);
        QVERIFY(verification.simulatorRecipePresent);
        const QJsonObject replay = QJsonDocument::fromJson(
            QByteArray::fromStdString(verification.replayJson)).object();
        QCOMPARE(replay.value("simulator_recipe").toObject().value("seed").toDouble(), 20260715.0);

        ExperimentEvidenceSnapshot invalid = snapshot(true);
        QJsonObject changed = simulator(true);
        QJsonObject changedRecipe = changed.value("recipe").toObject();
        changedRecipe["seed"] = 20260716.0;
        changed["recipe"] = changedRecipe; // stale recipe_sha256 must fail closed
        invalid.simulatorJson = dump(changed);
        QVERIFY(buildExperimentEvidenceJson(sessionWithEvidence(), invalid).empty());
    }

    void binds_and_verifies_standard_plan_metadata() {
        ExperimentEvidenceSnapshot withPlan = snapshot();
        withPlan.standardsPlanJson = boundStandardsPlan();
        const std::string evidence = buildExperimentEvidenceJson(sessionWithEvidence(), withPlan);
        QVERIFY(!evidence.empty());
        const auto verification = verifyExperimentEvidenceJson(evidence, std::string(64, 'a'));
        QVERIFY(verification.ok);
        QVERIFY(verification.standardsPlanPresent);
        QVERIFY(!verification.standardsPlanSha256.empty());
        const QJsonObject replay = QJsonDocument::fromJson(
            QByteArray::fromStdString(verification.replayJson)).object();
        QCOMPARE(replay.value("standards_plan").toObject().value("schema").toString(),
                 QString("dpa.standards.test-plan"));

        QJsonObject envelope = QJsonDocument::fromJson(QByteArray::fromStdString(
            withPlan.standardsPlanJson)).object();
        QJsonObject plan = envelope.value("plan").toObject();
        QJsonObject context = plan.value("context").toObject();
        context["experiment_context_sha256"] = QString(64, 'f');
        plan["context"] = context;
        envelope["plan"] = plan;
        withPlan.standardsPlanJson = dump(envelope);
        QVERIFY(buildExperimentEvidenceJson(sessionWithEvidence(), withPlan).empty());
    }

    void omits_malformed_events_and_rejects_oversize_input() {
        SessionRecord session = sessionWithEvidence();
        session.turns.push_back({"tool", "{not-json"});
        session.companionEvents.push_back("[]");
        const std::string evidence = buildExperimentEvidenceJson(session, snapshot());
        const auto verification = verifyExperimentEvidenceJson(evidence);
        QVERIFY(verification.ok);
        QCOMPARE(verification.omittedEvents, 2);

        const auto oversized = verifyExperimentEvidenceJson(std::string(1024 * 1024 + 1, 'x'));
        QVERIFY(!oversized.ok);
        QCOMPARE(QString::fromStdString(oversized.error), QString("evidence_size_invalid"));
    }

    void canonical_digest_is_independent_of_input_key_order() {
        ExperimentEvidenceSnapshot first = snapshot();
        ExperimentEvidenceSnapshot second = first;
        QJsonObject reversed;
        const QJsonObject original = preflight();
        const QStringList keys = original.keys();
        for (auto it = keys.crbegin(); it != keys.crend(); ++it) reversed.insert(*it, original.value(*it));
        second.preflightJson = dump(reversed);
        QCOMPARE(QByteArray::fromStdString(buildExperimentEvidenceJson(sessionWithEvidence(), first)),
                 QByteArray::fromStdString(buildExperimentEvidenceJson(sessionWithEvidence(), second)));
    }

    void resolves_same_session_artifact_across_native_separators() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString scope = temp.path() + "/session";
        QVERIFY(QDir().mkpath(scope));
        const QString evidencePath = scope + "/evidence.json";
        QFile evidence(evidencePath);
        QVERIFY(evidence.open(QIODevice::WriteOnly));
        QCOMPARE(evidence.write("{}"), qint64(2));
        evidence.close();

        const QString resolved = AgentArtifactPath::existingRegularFileWithinScope(
            QDir::toNativeSeparators(scope), QDir::toNativeSeparators(evidencePath));
        QCOMPARE(QFileInfo(resolved).canonicalFilePath(), QFileInfo(evidencePath).canonicalFilePath());

        const QString sibling = temp.path() + "/session-other";
        QVERIFY(QDir().mkpath(sibling));
        const QString siblingPath = sibling + "/evidence.json";
        QFile siblingFile(siblingPath);
        QVERIFY(siblingFile.open(QIODevice::WriteOnly));
        QCOMPARE(siblingFile.write("{}"), qint64(2));
        siblingFile.close();
        QVERIFY(AgentArtifactPath::existingRegularFileWithinScope(scope, siblingPath).isEmpty());
        QVERIFY(AgentArtifactPath::existingRegularFileWithinScope(
            scope, scope + "/missing.json").isEmpty());
    }
};

int runTstExperimentEvidence(int argc, char** argv) {
    TstExperimentEvidence test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_experimentevidence.moc"
