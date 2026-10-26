#include <QtTest/QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QSaveFile>
#include <QDir>
#include <QDateTime>
#include <QCryptographicHash>
#include "StandardsCatalog.h"
#include "StandardsEvidence.h"
#include "Diagnostics.h"
#include "BaselineMonitor.h"
#include "AgentJson.h"

using namespace Model;

class TstStandardsCatalog : public QObject { Q_OBJECT
private slots:
    void catalog_has_governed_core() {
        QCOMPARE(standardCatalog().size(), size_t(54));
        QVERIFY(isKnownStandardProfile("iso-20816-1:2016"));
        QVERIFY(isKnownStandardProfile("mil-std-810:2022"));
        QVERIFY(isKnownStandardProfile("gjb-150-series"));
        QVERIFY(isKnownStandardProfile("iec-60068-2-64:2008+a1:2019"));
        QVERIFY(isKnownStandardProfile("iso-16063-21:2003"));
        QVERIFY(isKnownStandardProfile("gb-t-9239.21:2019"));
    }
    void execution_is_explicitly_gated() {
        QVERIFY(isExecutionApprovedStandardProfile("legacy-iso10816-class-i-iv/1.0.0"));
        QVERIFY(!isExecutionApprovedStandardProfile("iso-20816-1:2016"));
        QVERIFY(!isExecutionApprovedStandardProfile("gjb-150-series"));
        QVERIFY(!isKnownStandardProfile("iso-10816-current"));
    }
    void json_contract_is_machine_readable() {
        const QJsonObject root = AgentJson::parseObject(standardsCatalogJson());
        QCOMPARE(root.value("schema").toString(), QString("dpa.standards.catalog"));
        QCOMPARE(root.value("records").toArray().size(), 54);
        QCOMPARE(root.value("execution_default").toString(), QString("catalog_only"));
    }
    void search_filters_without_losing_provenance() {
        const auto military = searchStandardCatalog("", "MIL-STD");
        QVERIFY(military.size() >= 4);
        for (const auto& entry : military) {
            QCOMPARE(QString::fromStdString(entry.authority), QString("MIL-STD"));
            QVERIFY(!entry.sourceUrl.empty());
        }
        const auto random = searchStandardCatalog("random", "", "catalog_only");
        QVERIFY(random.size() >= 2);
        const auto api = searchStandardCatalog("", "API", "purchase_required");
        QVERIFY(api.size() >= 4);
    }
    void applicability_is_context_and_authorization_gated() {
        StandardApplicabilityRequest request;
        request.standardProfile = "iso-20816-3:2022";
        request.machineType = "centrifugal_pump";
        request.ratedPowerKw = 75;
        request.rpm = 1800;
        request.platform = "industrial";
        request.testPurpose = "operational_vibration";
        request.physicalQuantity = "vibration_velocity";
        request.applicabilityBasis = "industrial pump in-situ monitoring";
        auto assessment = assessStandardApplicability(request);
        QCOMPARE(QString::fromStdString(assessment.disposition), QString("review_required"));
        QVERIFY(assessment.applicable);
        QVERIFY(!assessment.executionReady);

        request.authorizedCopy = true;
        request.ownerApproved = true;
        assessment = assessStandardApplicability(request);
        QVERIFY(!assessment.executionReady); // catalog-only profiles never become executable by flags alone

        request.standardProfile = "mil-std-167-1a:2022";
        request.platform = "industrial";
        assessment = assessStandardApplicability(request);
        QCOMPARE(QString::fromStdString(assessment.disposition), QString("context_missing"));
        QVERIFY(!assessment.applicable);
    }
    void test_plan_collects_blockers_and_evidence() {
        StandardTestPlanRequest request;
        request.machineType = "centrifugal_pump";
        request.ratedPowerKw = 75;
        request.rpm = 1800;
        request.platform = "industrial";
        request.testPurpose = "random_vibration";
        request.physicalQuantity = "vibration_acceleration";
        request.requestedStandardProfiles = {"gb-t-2423.56:2023", "gjb-150-series"};
        const auto plan = assessStandardTestPlan(request);
        QVERIFY(!plan.ready);
        QVERIFY(!plan.assessments.empty());
        QVERIFY(!plan.requiredEvidence.empty());
        QVERIFY(!plan.blockers.empty());

        request.platform = "oil_gas_process";
        request.testPurpose = "factory_acceptance";
        request.requestedStandardProfiles = {"api-610:2021"};
        const auto apiPlan = assessStandardTestPlan(request);
        QVERIFY(!apiPlan.ready);
        QVERIFY(!apiPlan.blockers.empty()); // no authorized API copy/owner approval
    }

    void plan_envelope_is_context_bound_and_recomputed() {
        StandardTestPlanRequest request;
        request.machineType = "centrifugal_pump";
        request.ratedPowerKw = 75;
        request.rpm = 1800;
        request.platform = "industrial";
        request.testPurpose = "operational_vibration";
        request.physicalQuantity = "vibration_velocity";
        request.applicabilityBasis = "domain-owned pump scope";
        request.experimentContextSha256 = std::string(64, 'e');
        request.requestedStandardProfiles = {"iso-20816-3:2022"};

        const std::string json = buildStandardTestPlanJson(request);
        QVERIFY(!json.empty());
        const QJsonObject root = AgentJson::parseObject(json);
        QCOMPARE(root.value("schema").toString(), QString("dpa.standards.test-plan"));
        QCOMPARE(root.value("context").toObject().value("experiment_context_sha256").toString(),
                 QString(64, 'e'));
        const auto valid = validateStandardTestPlanJson(json, std::string(64, 'e'));
        QVERIFY(valid.ok);
        QVERIFY(!valid.planSha256.empty());
        QCOMPARE(valid.profileCount, 1);

        QJsonObject tampered = root;
        QJsonObject context = tampered.value("context").toObject();
        context["platform"] = "shipboard";
        tampered["context"] = context;
        const auto rejected = validateStandardTestPlanJson(AgentJson::dump(tampered),
                                                            std::string(64, 'e'));
        QVERIFY(!rejected.ok);
        QCOMPARE(QString::fromStdString(rejected.error), QString("standard_plan_digest_mismatch"));
        const auto stale = validateStandardTestPlanJson(json, std::string(64, 'f'));
        QVERIFY(!stale.ok);
        QCOMPARE(QString::fromStdString(stale.error), QString("standard_plan_context_mismatch"));

        request.requestedStandardProfiles = {"legacy-iso10816-class-i-iv/1.0.0"};
        request.authorizedCopy = true;
        request.ownerApproved = true;
        request.authorizedEvidenceValidated = true;
        request.authorizedEvidenceManifestSha256 = std::string(64, 'a');
        const auto approved = assessStandardTestPlan(request);
        QVERIFY(approved.ready);
        QVERIFY(approved.assessments.front().executionReady);
    }

    void authorized_evidence_requires_clause_level_provenance() {
        const QJsonObject empty{{"schema", "dpa.standards.authorized-evidence"},
                                {"schema_version", 1}, {"policy", "test"},
                                {"records", QJsonArray{}}};
        const auto emptyResult = validateAuthorizedEvidenceManifestJson(AgentJson::dump(empty));
        QVERIFY(emptyResult.ok);
        QCOMPARE(emptyResult.recordCount, 0);

        const QJsonObject record{
            {"standard_id", "iso-20816-1:2016"},
            {"edition_or_date", "2016"},
            {"access_authority", "licensed-owner"},
            {"owner", "domain-owner"},
            {"local_authorized_copy_ref", "artifacts/standards-authorized/iso-20816-1/source.pdf"},
            {"sha256", QString(64, 'a')},
            {"imported_utc", "2026-08-24T00:00:00Z"},
            {"review_due", QJsonValue(QJsonValue::Null)},
            {"clause_references", QJsonArray{QJsonObject{
                {"clause_id", "5.1"}, {"page_or_section", "p.12"},
                {"title", "measurement scope"}, {"purpose", "scope"},
                {"evidence_ref", "plan-1"}}}}
        };
        QJsonObject manifest = empty;
        manifest["records"] = QJsonArray{record};
        const auto valid = validateAuthorizedEvidenceManifestJson(AgentJson::dump(manifest));
        QVERIFY(valid.ok);
        QCOMPARE(valid.recordCount, 1);
        QCOMPARE(valid.clauseReferenceCount, 1);

        QJsonObject incomplete = record;
        incomplete["clause_references"] = QJsonArray{};
        manifest["records"] = QJsonArray{incomplete};
        const auto rejected = validateAuthorizedEvidenceManifestJson(AgentJson::dump(manifest));
        QVERIFY(!rejected.ok);
        QCOMPARE(QString::fromStdString(rejected.error),
                 QString("authorized_evidence_clause_references_required"));
    }

    void execution_reads_local_bytes_and_context_approval_fail_closed() {
        QTemporaryDir directory(QDir::tempPath() + QStringLiteral("/dpa-\u6807\u51c6-XXXXXX"));
        QVERIFY(directory.isValid());
        struct RestoreEnvironment {
            bool existed = qEnvironmentVariableIsSet("DPA_STANDARDS_ROOT");
            QByteArray previous = qgetenv("DPA_STANDARDS_ROOT");
            ~RestoreEnvironment() {
                if (existed) qputenv("DPA_STANDARDS_ROOT", previous);
                else qunsetenv("DPA_STANDARDS_ROOT");
            }
        } restore;
        // qputenv uses the platform environment encoding, not JSON's UTF-8.
        qputenv("DPA_STANDARDS_ROOT", directory.path().toLocal8Bit());
        QCOMPARE(qEnvironmentVariable("DPA_STANDARDS_ROOT"), directory.path());
        const auto write = [&](const QString& relative, const QByteArray& bytes) {
            const QString path = QDir(directory.path()).filePath(relative);
            if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
            QSaveFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
        };
        const auto jsonBytes = [](const QJsonObject& object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); };
        const QByteArray source("TECHNICAL SYNTHETIC FIXTURE; NOT A LICENSED STANDARD OR DOMAIN ACCEPTANCE\n");
        const QString sourceRef = "artifacts/standards-authorized/fixture/source.txt";
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const QString reviewDue = now.date().addDays(30).toString(Qt::ISODate);
        QJsonArray clauses;
        for (const QString& purpose : {QString("scope"), QString("method"), QString("acceptance")})
            clauses.append(QJsonObject{{"clause_id", purpose}, {"page_or_section", "fixture"},
                {"title", "synthetic gate check"}, {"purpose", purpose}, {"evidence_ref", ""}});
        QJsonObject record{{"standard_id", kLegacyIso10816ClassProfile}, {"edition_or_date", "fixture"},
            {"access_authority", "test only"}, {"owner", "test only"}, {"local_authorized_copy_ref", sourceRef},
            {"sha256", QString::fromLatin1(QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex())},
            {"imported_utc", now.addSecs(-120).toString(Qt::ISODateWithMs)}, {"review_due", reviewDue},
            {"clause_references", clauses}};
        const auto writeManifest = [&](const QJsonObject& value) {
            return write("docs/standards/authorized-evidence-manifest.json", jsonBytes(QJsonObject{
                {"schema", "dpa.standards.authorized-evidence"}, {"schema_version", 1},
                {"policy", "synthetic technical test only"}, {"records", QJsonArray{value}}}));
        };
        QVERIFY(write(sourceRef, source));
        QVERIFY(writeManifest(record));
        DiagnoseConfig cfg = parseDiagnoseArgs(R"({
            "asset":{"assetId":"fixture-pump","machineType":"pump","ratedPowerKw":75,"rpm":3000,"rpmSource":"fixture","operatingCondition":"steady"},
            "measurement":{"dataId":"fixture-data","channel":0,"measurementPoint":"DE","direction":"radial","physicalQuantity":"vibration_velocity","unit":"mm/s","samplingRateHz":51200,"standardProfile":"legacy-iso10816-class-i-iv/1.0.0"},
            "standard":{"applicabilityConfirmed":true,"applicabilityBasis":"synthetic test only","velocityRmsMmS":9,"velocityRmsSource":"fixture","velocityRmsDataRef":"fixture-only","legacyMachineClass":1}})");
        QVERIFY(validateDiagnosticProfile(cfg.asset, cfg.measurement).valid());
        const std::string scope = diagnosticStandardScopeJson(cfg);
        const auto validate = [&]() { return validateLocalStandardExecutionEvidence(kLegacyIso10816ClassProfile, scope, kDiagnosticRuleVersion); };
        const auto missing = validate();
        QVERIFY(!missing.ok);
        QCOMPARE(QString::fromStdString(missing.error), QString("standard_domain_approval_missing_or_unavailable"));
        const auto approvalFor = [&](const StandardExecutionEvidence& evidence, const std::string& scopeJson) {
            return QJsonObject{{"schema", "dpa.standards.execution-approval"}, {"schema_version", 1},
                {"standard_id", kLegacyIso10816ClassProfile}, {"record_sha256", QString::fromStdString(evidence.recordSha256)},
                {"scope", AgentJson::parseObject(scopeJson)},
                {"scope_sha256", QString::fromStdString(evidence.scopeSha256)}, {"rule_version", kDiagnosticRuleVersion},
                {"decision", "approved"}, {"reviewer", "synthetic test reviewer"},
                {"reviewed_utc", now.addSecs(-60).toString(Qt::ISODateWithMs)}, {"review_due", reviewDue},
                {"rationale", "Synthetic authorization-gate regression; not domain expertise evidence."}};
        };
        const QJsonObject approval = approvalFor(missing, scope);
        const QString approvalRef = QString::fromStdString(missing.approvalRef);
        QVERIFY(write(approvalRef, jsonBytes(approval)));
        cfg.standardExecutionEvidence = validate();
        QVERIFY2(cfg.standardExecutionEvidence.ok, cfg.standardExecutionEvidence.error.c_str());
        const auto assessed = diagnoseVibration({}, cfg);
        bool severityAssessed = false;
        for (const auto& finding : assessed) if (finding.fault == "severity") {
            severityAssessed = finding.assessed;
            QCOMPARE(QString::fromStdString(finding.severity), QString("alarm"));
            QVERIFY(QString::fromStdString(finding.evidence).contains("approval_sha256="));
        }
        QVERIFY(severityAssessed);
        cfg.asset.assetId = "another-asset";
        for (const auto& finding : diagnoseVibration({}, cfg))
            if (finding.fault == "severity") QVERIFY(!finding.assessed);
        cfg.asset.assetId = "fixture-pump";

        for (const auto& mutation : std::vector<std::pair<QString, QJsonValue>>{
            {"decision", "revoked"}, {"scope_sha256", QString(64, '0')},
            {"record_sha256", QString(64, '0')}, {"scope", QJsonObject{{"asset_id", "other"}}},
            {"rule_version", "old-rule"}, {"reviewer", "   "},
            {"review_due", now.date().addDays(-1).toString(Qt::ISODate)},
            {"reviewed_utc", now.addDays(1).toString(Qt::ISODateWithMs)}, {"schema_version", 1.5}}) {
            QJsonObject changed = approval;
            changed[mutation.first] = mutation.second;
            QVERIFY(write(approvalRef, jsonBytes(changed)));
            QVERIFY2(!validate().ok, qPrintable(mutation.first));
        }
        QVERIFY(write(approvalRef, jsonBytes(approval)));
        QVERIFY(write(sourceRef, source + "tampered"));
        QCOMPARE(QString::fromStdString(validate().error), QString("standard_source_hash_mismatch_or_unavailable"));
        QVERIFY(write(sourceRef, source));
        for (const auto& mutation : std::vector<std::pair<QString, QJsonValue>>{
            {"review_due", QJsonValue(QJsonValue::Null)}, {"review_due", "2026-02-30"},
            {"imported_utc", "not-a-time"}, {"local_authorized_copy_ref", "artifacts/standards-authorized/../source.txt"},
            {"local_authorized_copy_ref", "artifacts/standards-authorized//source.txt"},
            {"local_authorized_copy_ref", "artifacts/standards-authorized/fixture/source.txt:stream"},
            {"clause_references", QJsonArray{clauses[0]}}}) {
            QJsonObject changed = record;
            changed[mutation.first] = mutation.second;
            QVERIFY(writeManifest(changed));
            QVERIFY2(!validate().ok, qPrintable(mutation.first));
        }
        QVERIFY(writeManifest(record));
        QVERIFY(validate().ok);

        BaselineArgs args = parseBaselineArgs(R"({"assetId":"fixture-pump","measurementPoint":"DE","direction":"radial","operatingCondition":"steady","physicalQuantity":"vibration_velocity","unit":"mm/s","samplingRateHz":51200,"standardProfile":"legacy-iso10816-class-i-iv/1.0.0","standard":{"applicabilityConfirmed":true,"applicabilityBasis":"synthetic test only","velocityRmsMmS":3,"velocityRmsSource":"fixture","velocityRmsDataRef":"fixture-only","legacyMachineClass":1}})");
        QVERIFY(baselineSeverityZone(args).empty());
        const auto baselineMissing = validateLocalStandardExecutionEvidence(args.standardProfile, baselineStandardScopeJson(args), kDiagnosticRuleVersion);
        QVERIFY(!baselineMissing.ok); // a diagnostic approval cannot authorize a different operation
        QVERIFY(write(QString::fromStdString(baselineMissing.approvalRef), jsonBytes(approvalFor(baselineMissing, baselineStandardScopeJson(args)))));
        args.standardExecutionEvidence = validateLocalStandardExecutionEvidence(args.standardProfile, baselineStandardScopeJson(args), kDiagnosticRuleVersion);
        QVERIFY(args.standardExecutionEvidence.ok);
        QCOMPARE(QString::fromStdString(baselineSeverityZone(args)), QString("C"));
        args.legacyIso10816MachineClass = 2;
        QVERIFY(baselineSeverityZone(args).empty());
        qunsetenv("DPA_STANDARDS_ROOT");
        QVERIFY(!validate().ok);
    }
};

int runTstStandardsCatalog(int argc, char** argv) {
    TstStandardsCatalog test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_standards_catalog.moc"
