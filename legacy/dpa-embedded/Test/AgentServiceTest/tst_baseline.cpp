#include <QtTest/QtTest>
#include "BaselineMonitor.h"
#include "AgentJson.h"
#include <QJsonObject>
using namespace Model;
static BaselineProvenance provenance(const std::string& dataId = "data-1", int frameSize = 4096) {
    BaselineProvenance p;
    p.sourceDataId = dataId; p.sourceChannel = 1; p.sourceUnit = "mm/s";
    p.sourcePhysicalType = "Velocity";
    p.samplingRateHz = 51200; p.processType = "FFT"; p.resultFunctionType = "Spectrum";
    p.resultDataForm = 0x212; p.axisTypeX = 1; p.frameSize = frameSize; p.averageNum = 4;
    p.overlapIndex = 3; p.overlapValue = 0.5; p.windowType = 1; p.resultUnit = "mm/s";
    p.configFingerprint = baselineConfigFingerprint(p);
    return p;
}
static MachineBaseline base() {
    MachineBaseline b; b.assetId = "pump-1"; b.measurementPoint = "DE";
    b.direction = "H"; b.operatingCondition = "rated"; b.valid = true;
    b.physicalQuantity = "vibration_velocity"; b.unit = "mm/s";
    b.standardProfile = "none"; b.provenance = provenance();
    b.configFingerprint = b.provenance.configFingerprint;
    b.peakFreqsHz = {50.0, 100.0}; b.isoZone = "A"; return b;
}
class TstBaseline : public QObject {
    Q_OBJECT
private slots:
    void no_baseline_prompts_capture();
    void stable_is_info();
    void new_peak_warns();
    void iso_worsened_two_zones_alarms();
    void iso_worsened_one_zone_warns();
    void zone_delta_ranks();
    void parse_args_reads_identity_and_confirmed_standard();
    void baseline_identity_is_required_and_round_trips();
    void config_fingerprint_is_stable_and_separates_incompatible_results();
    void legacy_schema_is_rejected_fail_closed();
    void deviation_result_json();
    void no_baseline_result_is_ok_info();
    void trend_compares_first_and_latest_confirmed_observations();
    void trend_rejects_non_chronological_history();
    void old_legacy_labels_are_preserved_but_not_evaluation_evidence();
};

void TstBaseline::no_baseline_prompts_capture() {
        MachineBaseline none;   // valid == false
        auto d = compareToBaseline(none, {50}, "C", 0.05);
        QVERIFY(!d.hasBaseline);
        QCOMPARE(QString::fromStdString(d.severity), QString("info"));
        QVERIFY(QString::fromStdString(d.summary).contains("基线"));
    }
void TstBaseline::stable_is_info() {
        auto d = compareToBaseline(base(), {50, 100}, "A", 0.05);
        QVERIFY(d.hasBaseline);
        QVERIFY(d.newPeaksHz.empty());
        QVERIFY(!d.isoWorsened);
        QCOMPARE(QString::fromStdString(d.severity), QString("info"));
        QVERIFY(QString::fromStdString(d.summary).contains("未进行跨频谱幅值 RMS 比较"));
    }
void TstBaseline::new_peak_warns() {
        auto d = compareToBaseline(base(), {50, 100, 162}, "A", 0.05);   // 162 Hz new (BPFO-like)
        QCOMPARE((int)d.newPeaksHz.size(), 1);
        QCOMPARE(d.newPeaksHz[0], 162.0);
        QCOMPARE(QString::fromStdString(d.severity), QString("warn"));
    }
void TstBaseline::iso_worsened_two_zones_alarms() {
        auto d = compareToBaseline(base(), {50, 100}, "C", 0.05);   // A -> C == delta 2
        QVERIFY(d.isoWorsened);
        QCOMPARE(QString::fromStdString(d.severity), QString("alarm"));
    }
void TstBaseline::iso_worsened_one_zone_warns() {
        auto d = compareToBaseline(base(), {50, 100}, "B", 0.05);   // A -> B == delta 1
        QVERIFY(d.isoWorsened);
        QCOMPARE(QString::fromStdString(d.severity), QString("warn"));
    }
void TstBaseline::zone_delta_ranks() {
        QCOMPARE(isoZoneDelta("A", "D"), 3);
        QCOMPARE(isoZoneDelta("C", "A"), -2);   // improved
        QCOMPARE(isoZoneDelta("A", ""), 0);     // unknown -> 0
    }
void TstBaseline::old_legacy_labels_are_preserved_but_not_evaluation_evidence() {
    MachineBaseline legacy = base();
    legacy.standardProfile = "legacy-iso10816-class-i-iv/1.0.0";
    legacy.isoZone = "A";
    const auto deviation = compareToBaseline(legacy, legacy.peakFreqsHz, "C", 0.05);
    QVERIFY(!deviation.isoWorsened);
    const MachineBaseline restored = parseBaseline(serializeBaseline(legacy));
    QCOMPARE(QString::fromStdString(restored.isoZone), QString("A"));
    const QJsonArray history = AgentJson::parseObject(buildBaselineTrendResult(restored).dataJson)
        .value("observations").toArray();
    QCOMPARE(history.size(), 1);
    QCOMPARE(history.first().toObject().value("iso_zone").toString(), QString("A"));
    QVERIFY(!history.first().toObject().value("legacy_severity_assessed").toBool(true));
}
void TstBaseline::parse_args_reads_identity_and_confirmed_standard() {
        auto a = parseBaselineArgs(R"({"assetId":"pump-1","measurementPoint":"DE","direction":"H","operatingCondition":"rated","dataId":"data-1","channel":1,"physicalQuantity":"vibration_velocity","unit":"mm/s RMS","samplingRateHz":51200,"standardProfile":"legacy-iso10816-class-i-iv/1.0.0","standard":{"applicabilityConfirmed":true,"applicabilityBasis":"domain owner approved fixture","velocityRmsMmS":3.0,"velocityRmsSource":"calibrated meter","velocityRmsDataRef":"fixture://rms/1","legacyMachineClass":1},"topN":5})");
        QCOMPARE(QString::fromStdString(a.assetId), QString("pump-1"));
        QCOMPARE(a.legacyIso10816MachineClass, 1);
        QCOMPARE(a.topN, 5);
        QVERIFY(!baselineKey(a, provenance().configFingerprint).empty());
        QVERIFY(baselineSeverityZone(a).empty()); // flags/text are not local authorization
        a.standardApplicabilityConfirmed = false;
        QVERIFY(baselineSeverityZone(a).empty());
    }
void TstBaseline::baseline_identity_is_required_and_round_trips() {
        BaselineArgs missing;
        missing.assetId = "pump-1";
        QVERIFY(baselineKey(missing, provenance().configFingerprint).empty());

        MachineBaseline input = base();
        input.capturedMs = 123456789;
        BaselineObservation observation;
        observation.capturedMs = 123456789; observation.isoZone = "A";
        observation.peakFreqsHz = {50.0, 100.0}; observation.provenance = input.provenance;
        observation.standardProfile = input.standardProfile;
        input.history.push_back(observation);
        MachineBaseline output = parseBaseline(serializeBaseline(input));
        QVERIFY(output.valid);
        QCOMPARE(QString::fromStdString(output.assetId), QString("pump-1"));
        QCOMPARE(QString::fromStdString(output.measurementPoint), QString("DE"));
        QCOMPARE(QString::fromStdString(output.direction), QString("H"));
        QCOMPARE(QString::fromStdString(output.operatingCondition), QString("rated"));
        QCOMPARE(output.peakFreqsHz.size(), size_t(2));
        QCOMPARE(output.capturedMs, 123456789LL);
        QCOMPARE(output.history.size(), size_t(1));
        QCOMPARE(QString::fromStdString(output.configFingerprint), QString::fromStdString(input.configFingerprint));
        QCOMPARE(QString::fromStdString(output.history[0].provenance.sourceDataId), QString("data-1"));
        ToolResult trend = buildBaselineTrendResult(output);
        QVERIFY(trend.ok);
        QCOMPARE(AgentJson::parseObject(trend.dataJson).value("observations").toArray().size(), 1);
        QCOMPARE(AgentJson::parseObject(trend.dataJson).value("trend").toObject()
                     .value("evaluation_status").toString(),
                 QString("insufficient_history"));
    }
void TstBaseline::config_fingerprint_is_stable_and_separates_incompatible_results() {
        BaselineProvenance first = provenance("capture-a", 4096);
        BaselineProvenance sameConfig = provenance("capture-b", 4096);
        BaselineProvenance otherFrame = provenance("capture-c", 8192);
        QCOMPARE(QString::fromStdString(first.configFingerprint), QString::fromStdString(sameConfig.configFingerprint));
        QVERIFY(first.configFingerprint != otherFrame.configFingerprint);

        BaselineArgs args = parseBaselineArgs(R"({"assetId":"pump-1","measurementPoint":"DE","direction":"H","operatingCondition":"rated","dataId":"capture-a","channel":1,"physicalQuantity":"vibration_velocity","unit":"mm/s","samplingRateHz":51200,"standardProfile":"none"})");
        QVERIFY(baselineKey(args, first.configFingerprint) != baselineKey(args, otherFrame.configFingerprint));
    }
void TstBaseline::legacy_schema_is_rejected_fail_closed() {
        MachineBaseline old = parseBaseline(R"({"schema_version":1,"asset_id":"pump-1","valid":true})");
        QVERIFY(!old.valid);
    }
void TstBaseline::deviation_result_json() {
        auto d = compareToBaseline(base(), {50, 100, 162}, "A", 0.05);
        ToolResult r = buildDeviationResult(d);
        QVERIFY(r.ok);
        auto o = AgentJson::parseObject(r.dataJson);
        QCOMPARE(o.value("severity").toString(), QString("warn"));
        QVERIFY(o.value("has_baseline").toBool());
        QVERIFY(!o.contains("rms_ratio"));
    }
void TstBaseline::no_baseline_result_is_ok_info() {
        MachineBaseline none;
        ToolResult r = buildDeviationResult(compareToBaseline(none, {50}, "C", 0.05));
        QVERIFY(r.ok);   // absence of a baseline is a valid, non-error outcome
        auto o = AgentJson::parseObject(r.dataJson);
        QVERIFY(!o.value("has_baseline").toBool());
    }
void TstBaseline::trend_compares_first_and_latest_confirmed_observations() {
        MachineBaseline input = base();
        BaselineObservation first;
        first.capturedMs = 100; first.isoZone = "A"; first.peakFreqsHz = {50.0, 100.0};
        first.provenance = provenance("capture-a"); first.standardProfile = "none";
        BaselineObservation latest;
        latest.capturedMs = 200; latest.isoZone = "C"; latest.peakFreqsHz = {50.0, 162.0};
        latest.provenance = provenance("capture-b"); latest.standardProfile = "none";
        input.history = {first, latest};

        const QJsonObject trend = AgentJson::parseObject(
            buildBaselineTrendResult(input).dataJson).value("trend").toObject();
        QVERIFY(trend.value("evaluated").toBool());
        QCOMPARE(trend.value("evaluation_status").toString(), QString("evaluated"));
        QCOMPARE(trend.value("severity").toString(), QString("alarm"));
        QVERIFY(trend.value("iso_worsened").toBool());
        QCOMPARE(trend.value("new_peaks_hz").toArray().at(0).toDouble(), 162.0);
        QVERIFY(!trend.value("rms_evaluated").toBool());
    }
void TstBaseline::trend_rejects_non_chronological_history() {
        MachineBaseline input = base();
        BaselineObservation first;
        first.capturedMs = 200; first.provenance = provenance("capture-a"); first.standardProfile = "none";
        BaselineObservation latest = first;
        latest.capturedMs = 100; latest.provenance = provenance("capture-b");
        input.history = {first, latest};

        const QJsonObject trend = AgentJson::parseObject(
            buildBaselineTrendResult(input).dataJson).value("trend").toObject();
        QVERIFY(!trend.value("evaluated").toBool());
        QCOMPARE(trend.value("evaluation_status").toString(), QString("invalid_chronology"));
    }
int runTstBaseline(int argc,char**argv){ TstBaseline t; return QTest::qExec(&t,argc,argv); }
#include "tst_baseline.moc"
