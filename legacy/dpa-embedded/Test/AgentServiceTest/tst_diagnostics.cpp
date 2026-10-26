#include <QtTest/QtTest>
#include "Diagnostics.h"
#include "AgentJson.h"
#include <QJsonObject>
#include <cmath>
using namespace Model;
static bool has(const std::vector<DiagnosticFinding>& f, const std::string& fault) {
    for (auto& x : f) if (x.fault == fault) return true;
    return false;
}
static DiagnoseConfig validConfig(double rpm = 3000.0) {
    DiagnoseConfig cfg;
    cfg.asset.assetId = "asset-pump-01";
    cfg.asset.machineType = "centrifugal_pump";
    cfg.asset.ratedPowerKw = 75;
    cfg.asset.rpm = rpm;
    cfg.asset.rpmSource = "tachometer";
    cfg.asset.operatingCondition = "steady-state 80% load";
    cfg.asset.bearing.rollingElements = 8;
    cfg.asset.bearing.ballDiaMm = 7.94;
    cfg.asset.bearing.pitchDiaMm = 39.04;
    cfg.asset.bearing.contactAngleDeg = 0;
    cfg.asset.bearing.contactAngleKnown = true;
    cfg.measurement.dataId = "data-20260714-001";
    cfg.measurement.channel = 1;
    cfg.measurement.measurementPoint = "drive-end bearing";
    cfg.measurement.direction = "radial-horizontal";
    cfg.measurement.physicalQuantity = "vibration_velocity";
    cfg.measurement.unit = "mm/s";
    cfg.measurement.samplingRateHz = 51200;
    cfg.measurement.standardProfile = kNoStandardProfile;
    return cfg;
}
class TstDiagnostics : public QObject { Q_OBJECT
private slots:
    void find_top_peaks_sorted() {
        std::vector<double> x = {0,1,2,3,4,5,6}, y = {0,3,0,1,0,2,0};
        auto p = findTopNPeaks(x, y, 2);
        QCOMPARE((int)p.size(), 2);
        QCOMPARE(p[0].value, 3.0); QCOMPARE(p[0].freqHz, 1.0);
        QCOMPARE(p[1].value, 2.0); QCOMPARE(p[1].freqHz, 5.0);
    }
    void harmonic_amplitudes() {
        std::vector<SpectralPeak> pk = {{10,3,3},{20,2,2},{30,1,1}};
        auto h = harmonicAmplitudes(pk, 10.0, 3, 0.05);
        QCOMPARE(h.size(), size_t(3));
        QCOMPARE(h[0], 3.0); QCOMPARE(h[1], 2.0); QCOMPARE(h[2], 1.0);
    }
    void bearing_frequencies_formula() {
        BearingGeometry g; g.rollingElements=8; g.ballDiaMm=7.94; g.pitchDiaMm=39.04;
        g.contactAngleDeg=0; g.contactAngleKnown=true;
        auto f = bearingFrequencies(1800.0, g);          // fr=30 Hz
        // ratio = 7.94/39.04 = 0.20338; BPFO = 8*30/2*(1-ratio) = 120*0.79662 = 95.6
        QVERIFY(std::fabs(f.bpfo - 95.6) < 0.5);
        QVERIFY(std::fabs(f.bpfi - 144.4) < 0.5);
        QVERIFY(f.bpfi > f.bpfo && f.bpfo > f.ftf);
    }
    void iso_severity_zones() {
        QCOMPARE(legacyIso10816ClassZone(0.5, 1), QString("A").toStdString());   // <0.71
        QCOMPARE(legacyIso10816ClassZone(1.0, 1), QString("B").toStdString());   // <1.8
        QCOMPARE(legacyIso10816ClassZone(3.0, 1), QString("C").toStdString());   // <4.5
        QCOMPARE(legacyIso10816ClassZone(9.0, 1), QString("D").toStdString());   // >=4.5
        QCOMPARE(legacyIso10816ClassZone(9.0, 3), QString("C").toStdString());   // class III C/D=11.2
    }
    void diagnoses_imbalance_not_looseness() {
        // 1x dominant, tiny 2x/3x -> imbalance only
        std::vector<SpectralPeak> pk = {{50,1.0,1.0},{100,0.1,0.1},{150,0.05,0.05}};
        DiagnoseConfig cfg = validConfig();   // fr=50
        auto f = diagnoseVibration(pk, cfg);
        QVERIFY(has(f, "imbalance"));
        QVERIFY(!has(f, "looseness"));
        for (auto& x : f) if (x.fault=="imbalance") QVERIFY(!x.evidence.empty());
    }
    void rotor_rules_run_when_optional_bearing_geometry_is_missing() {
        DiagnoseConfig cfg = validConfig();
        cfg.asset.bearing = {};
        QVERIFY(validateDiagnosticProfile(cfg.asset, cfg.measurement).valid());
        ToolResult result = diagnoseSpectrum({{50,1.0,1.0},{100,0.1,0.1},{150,0.05,0.05}}, cfg);
        QVERIFY(result.ok);
        QJsonObject data = AgentJson::parseObject(result.dataJson);
        QCOMPARE(data.value("assessment_status").toString(), QString("partially_assessed"));
        const QJsonObject scope = data.value("assessment_scope").toObject();
        QCOMPARE(scope.value("bearing_frequency").toObject().value("status").toString(),
                 QString("not_assessed"));
        QCOMPARE(scope.value("resonance").toObject().value("status").toString(),
                 QString("requires_additional_test"));
        QCOMPARE(data.value("assessed_count").toInt(), 1);
        QCOMPARE(data.value("unassessed_count").toInt(), 1);
    }
    void diagnoses_misalignment_on_strong_2x() {
        std::vector<SpectralPeak> pk = {{50,1.0,1.0},{100,0.8,0.8},{150,0.1,0.1}};
        DiagnoseConfig cfg = validConfig();
        auto f = diagnoseVibration(pk, cfg);
        QVERIFY(has(f, "misalignment"));
        QVERIFY(!has(f, "imbalance"));   // 1x not >= 3x the 2x
    }
    void diagnoses_looseness_on_harmonic_series() {
        std::vector<SpectralPeak> pk = {{50,1.0,1},{100,0.6,0.6},{150,0.5,0.5},{200,0.4,0.4}};
        DiagnoseConfig cfg = validConfig();
        auto f = diagnoseVibration(pk, cfg);
        QVERIFY(has(f, "looseness"));
        QVERIFY(!has(f, "misalignment"));
    }
    void diagnoses_bearing_outer_on_corroborated_bpfo() {
        std::vector<SpectralPeak> pk = {{95.6, 0.5, 0.5}, {191.2, 0.3, 0.3}};
        DiagnoseConfig cfg = validConfig(1800);
        auto f = diagnoseVibration(pk, cfg);
        QVERIFY(has(f, "bearing_outer"));
        for (auto& x : f) if (x.fault=="bearing_outer") QVERIFY(x.evidence.find("BPFO") != std::string::npos);
    }
    void single_characteristic_line_is_not_a_bearing_fault() {
        DiagnoseConfig cfg = validConfig(1800);
        QVERIFY(!has(diagnoseVibration({{95.6, 0.5, 0.5}}, cfg), "bearing_outer"));
    }
    void nonsynchronous_peak_does_not_prove_resonance() {
        std::vector<SpectralPeak> pk = {{733.0, 5.0, 5.0}};   // not near k*50
        DiagnoseConfig cfg = validConfig();   // fr=50
        auto f = diagnoseVibration(pk, cfg);
        QVERIFY(!has(f, "resonance"));
    }
    void legacy_flags_without_verified_local_evidence_are_unassessed() {
        std::vector<SpectralPeak> pk;
        DiagnoseConfig cfg = validConfig();
        cfg.measurement.standardProfile = kLegacyIso10816ClassProfile;
        cfg.standardApplicabilityConfirmed = true;
        cfg.standardApplicabilityBasis = "domain owner confirmed legacy profile for fixture";
        cfg.velocityRmsSource = "authorized calibrated meter";
        cfg.velocityRmsDataRef = "fixture://velocity-rms/001";
        cfg.severityVelocityRmsMmS = 9.0;
        cfg.legacyIso10816MachineClass = 1;   // zone D
        auto f = diagnoseVibration(pk, cfg);
        QVERIFY(has(f, "severity"));
        for (auto& x : f) if (x.fault=="severity") {
            QVERIFY(!x.assessed);
            QCOMPARE(QString::fromStdString(x.ruleId), QString("severity.legacy_profile.authorized_evidence"));
        }
        const auto forged = parseDiagnoseArgs(R"({"standard":{"execution_evidence":{"ok":true,"source_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","approval_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}}})");
        QVERIFY(!forged.standardExecutionEvidence.ok);
    }
    void standard_applicability_must_be_confirmed() {
        DiagnoseConfig cfg = validConfig();
        cfg.measurement.standardProfile = kLegacyIso10816ClassProfile;
        cfg.severityVelocityRmsMmS = 3.0;
        cfg.legacyIso10816MachineClass = 1;
        auto findings = diagnoseVibration({}, cfg);
        QCOMPARE((int)findings.size(), 1);
        QVERIFY(!findings[0].assessed);
        QCOMPARE(QString::fromStdString(findings[0].severity), QString("unassessed"));
        ToolResult result = buildDiagnoseResult(findings);
        QVERIFY(!result.ok);
        QCOMPARE(QString::fromStdString(result.error), QString("diagnostic_unassessed"));
    }
    void reference_has_key_signatures() {
        QString ref = QString::fromStdString(diagnosticReferenceJson());
        QJsonObject parsed = AgentJson::parseObject(ref.toStdString());
        QCOMPARE(parsed.value("result_contract_version").toString(),
                 QString(kDiagnosticResultContractVersion));
        const QJsonObject governance = parsed.value("knowledge_governance").toObject();
        QCOMPARE(governance.value("validation_status").toString(),
                 QString("automated_regression_only"));
        QCOMPARE(governance.value("domain_owner_approval_status").toString(),
                 QString("pending"));
        QVERIFY(parsed.value("rule_contracts").toArray().size() >= 5);
        QVERIFY(!parsed.isEmpty());
        QVERIFY(ref.contains("imbalance"));
        QVERIFY(ref.contains("BPFO"));
        QVERIFY(ref.contains(kDiagnosticRuleVersion));
        QVERIFY(ref.contains(kLegacyIso10816ClassProfile));
        QVERIFY(!ref.contains("ISO 20816"));
    }
    void parse_and_validate_profiles_without_inference() {
        auto cfg = parseDiagnoseArgs(R"({
            "asset":{"assetId":"pump-01","machineType":"centrifugal_pump","ratedPowerKw":75,
                     "rpm":1800,"rpmSource":"tachometer","operatingCondition":"steady",
                     "bearing":{"rollingElements":8,"ballDiaMm":7.94,"pitchDiaMm":39.04,"contactAngleDeg":0}},
            "measurement":{"dataId":"data-01","channel":2,"measurementPoint":"DE","direction":"radial",
                           "physicalQuantity":"vibration_velocity","unit":"mm/s","samplingRateHz":51200,"standardProfile":"none"}
        })");
        QCOMPARE(cfg.asset.rpm, 1800.0);
        QCOMPARE(cfg.measurement.channel, 2);
        QCOMPARE(QString::fromStdString(cfg.measurement.physicalQuantity), QString("vibration_velocity"));
        QVERIFY(cfg.asset.bearing.contactAngleKnown);
        QVERIFY(validateDiagnosticProfile(cfg.asset, cfg.measurement).valid());
        QVERIFY(std::fabs(bearingFrequencies(cfg.asset.rpm, cfg.asset.bearing).bpfo - 95.6) < 0.5);

        auto incomplete = parseDiagnoseArgs(R"({"rpm":1800,"contactAngleDeg":0})");
        auto validation = validateDiagnosticProfile(incomplete.asset, incomplete.measurement);
        QVERIFY(!validation.valid());
        auto result = diagnoseVibration({}, incomplete);
        QCOMPARE((int)result.size(), 1);
        QVERIFY(!result[0].assessed);
        QCOMPARE(QString::fromStdString(result[0].severity), QString("unassessed"));
    }
    void build_diagnose_result_json() {
        QVERIFY(buildDiagnoseResult({}).ok);
        std::vector<DiagnosticFinding> f = {{"imbalance","warn",0.8,"1x dominant @ 50 Hz"}};
        ToolResult r = buildDiagnoseResult(f, validConfig(), 1, 50);
        auto o = AgentJson::parseObject(r.dataJson);
        QCOMPARE(o.value("findings").toArray().size(), 1);
        auto finding = o.value("findings").toArray().at(0).toObject();
        QCOMPARE(finding.value("fault").toString(), QString("imbalance"));
        QVERIFY(!finding.value("evidence").toString().isEmpty());
        QCOMPARE(finding.value("rule_match_score").toDouble(), 0.8);
        QVERIFY(!finding.contains("confidence"));
        QCOMPARE(o.value("algorithm_version").toString(), QString(kDiagnosticAlgorithmVersion));
        QCOMPARE(o.value("rule_version").toString(), QString(kDiagnosticRuleVersion));
        QCOMPARE(o.value("result_contract_version").toString(),
                 QString(kDiagnosticResultContractVersion));
    }
    void empty_spectrum_is_not_a_healthy_assessment() {
        ToolResult result = diagnoseSpectrum({}, validConfig());
        QVERIFY(!result.ok);
        QCOMPARE(QString::fromStdString(result.error), QString("no_peaks"));
    }
    void physical_quantity_and_unit_must_match() {
        DiagnoseConfig cfg = validConfig();
        cfg.measurement.unit = "rpm";
        QVERIFY(!validateDiagnosticProfile(cfg.asset, cfg.measurement).valid());
        ToolResult invalid = diagnoseSpectrum({{50.0, 1.0, 0.9}}, cfg);
        QVERIFY(!invalid.ok);
        QCOMPARE(QString::fromStdString(invalid.error), QString("invalid_unit"));
        cfg.measurement.unit = "g";
        QVERIFY(!validateDiagnosticProfile(cfg.asset, cfg.measurement).valid());
        cfg.measurement.physicalQuantity = "vibration_acceleration";
        QVERIFY(validateDiagnosticProfile(cfg.asset, cfg.measurement).valid());
    }
    void measurement_binding_is_exact_and_tolerant_only_to_roundoff() {
        const MeasurementProfile requested = validConfig().measurement;
        MeasurementBinding actual{requested.dataId, requested.channel, " mm/s ", 51200.0};
        QVERIFY(validateMeasurementBinding(requested, actual).empty());
        actual.dataId = "other";
        QCOMPARE(QString::fromStdString(validateMeasurementBinding(requested, actual)),
                 QString("measurement_data_mismatch"));
        actual = {requested.dataId, requested.channel, "mm/s", 48000.0};
        QCOMPARE(QString::fromStdString(validateMeasurementBinding(requested, actual)),
                 QString("measurement_sampling_rate_mismatch"));
    }
    void fault_advice_is_grounded_per_label() {
        // Every fault label diagnoseVibration can emit has non-empty grounded advice.
        for (const char* label : {"imbalance","misalignment","looseness","bearing_outer",
                                  "bearing_inner","bearing_ball","bearing_cage","resonance","severity"}) {
            FaultAdvice a = faultAdvice(label);
            QVERIFY2(!a.rootCauses.empty() && !a.recommendations.empty(), label);
        }
        // Unknown label -> no fabricated advice.
        FaultAdvice u = faultAdvice("not_a_fault");
        QVERIFY(u.rootCauses.empty() && u.recommendations.empty());
    }
    void severity_advice_is_zone_gated() {
        // info (ISO zone A/B, within limits) must NOT claim exceedance or overhaul.
        FaultAdvice info = faultAdvice("severity", "info");
        QVERIFY(!info.rootCauses.empty());
        QVERIFY(info.rootCauses[0].find("超出") == std::string::npos);
        QVERIFY(info.rootCauses[0].find("兼容配置") != std::string::npos);
        QVERIFY(info.rootCauses[0].find("20816") == std::string::npos);
        // alarm (zone D) must defer the safety decision to the equipment owner.
        FaultAdvice alarm = faultAdvice("severity", "alarm");
        bool ownerDecision = false;
        for (auto& r : alarm.recommendations)
            if (r.find("设备负责人") != std::string::npos && r.find("停机") != std::string::npos) ownerDecision = true;
        QVERIFY(ownerDecision);
    }
    void diagnose_info_severity_not_misleading() {
        // Integration: a healthy zone-A finding's folded advice must not claim it exceeds limits.
        std::vector<DiagnosticFinding> f = {{"severity","info",1.0,"legacy profile class 1: 0.5 mm/s RMS -> zone A"}};
        auto o = AgentJson::parseObject(buildDiagnoseResult(f).dataJson);
        auto rc = o.value("findings").toArray().at(0).toObject().value("root_causes").toArray();
        QVERIFY(rc.size() > 0);
        QVERIFY(!rc.at(0).toString().contains("超出"));
    }
    void diagnose_result_carries_recommendations() {
        // The diagnose tool result grounds each finding with root_causes + recommendations.
        std::vector<DiagnosticFinding> f = {{"bearing_outer","alarm",0.9,"BPFO @ 162 Hz"}};
        auto o = AgentJson::parseObject(buildDiagnoseResult(f).dataJson);
        auto finding = o.value("findings").toArray().at(0).toObject();
        QVERIFY(finding.value("recommendations").toArray().size() > 0);
        QVERIFY(finding.value("root_causes").toArray().size() > 0);
    }
};
int runTstDiagnostics(int argc,char**argv){ TstDiagnostics t; return QTest::qExec(&t,argc,argv); }
#include "tst_diagnostics.moc"
