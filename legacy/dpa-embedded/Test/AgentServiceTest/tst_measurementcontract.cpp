#include <QtTest/QtTest>

#include "MeasurementContract.h"
#include "AgentJson.h"

using namespace Model;

namespace {

CanonicalMeasurement measurement() {
    CanonicalMeasurement value;
    value.measurementId = "measurement-1";
    value.sourceDataId = "data-1";
    value.assetId = "asset-1";
    value.testId = "test-1";
    value.channelId = 3;
    value.measurementPoint = "bearing-drive-end";
    value.direction = "radial-x";
    value.physicalQuantity = "acceleration";
    value.unit = "m/s2";
    value.samplingRateHz = 25600;
    value.startTimeMs = 1000;
    value.endTimeMs = 2000;
    value.sampleCount = 100;
    value.operatingCondition = "1500 rpm / no-load";
    value.contextFingerprint = std::string(64, 'a');
    value.sourceSha256 = std::string(64, 'b');
    value.calibrationProfile = "calibration-2026-01";
    return value;
}

QualityReport quality() {
    QualityReport value;
    value.status = "pass";
    value.finiteSamples = 100;
    value.monotonicTime = true;
    value.nyquistValid = true;
    value.calibrationValid = true;
    return value;
}

EvidenceRef evidence() {
    EvidenceRef value;
    value.evidenceId = "evidence-1";
    value.kind = "fft";
    value.measurementId = "measurement-1";
    value.sourceSha256 = std::string(64, 'b');
    value.algorithmVersion = "fft-1";
    value.ruleVersion = "rules-1";
    value.createdAtMs = 2000;
    value.summarySha256 = std::string(64, 'c');
    value.artifactRefs = {"artifact-1"};
    return value;
}

} // namespace

class TstMeasurementContract : public QObject {
    Q_OBJECT
private slots:
    void valid_bundle_round_trips() {
        const std::string measurementJson = canonicalMeasurementJson(measurement());
        const std::string qualityJson = qualityReportJson(quality());
        const std::string evidenceJson = evidenceRefJson(evidence());
        QVERIFY(validateCanonicalMeasurementJson(measurementJson).ok);
        QVERIFY(validateQualityReportJson(qualityJson, 100).ok);
        QVERIFY(validateEvidenceRefJson(evidenceJson, "measurement-1").ok);
        QVERIFY(validateMeasurementBundleJson(measurementJson, qualityJson, evidenceJson).ok);
    }

    void quality_cannot_claim_more_samples_than_measurement() {
        QualityReport value = quality();
        value.finiteSamples = 101;
        const auto result = validateQualityReportJson(qualityReportJson(value), 100);
        QVERIFY(!result.ok);
        QCOMPARE(QString::fromStdString(result.error), QString("quality_sample_count_invalid"));
    }

    void evidence_must_bind_to_measurement() {
        EvidenceRef value = evidence();
        value.measurementId = "other-measurement";
        const auto result = validateEvidenceRefJson(evidenceRefJson(value), "measurement-1");
        QVERIFY(!result.ok);
        QCOMPARE(QString::fromStdString(result.error), QString("evidence_ref_measurement_mismatch"));
    }

    void measurement_rejects_invalid_time_range() {
        CanonicalMeasurement value = measurement();
        value.endTimeMs = value.startTimeMs - 1;
        const auto result = validateCanonicalMeasurementJson(canonicalMeasurementJson(value));
        QVERIFY(!result.ok);
        QCOMPARE(QString::fromStdString(result.error), QString("measurement_time_range_invalid"));
    }
};

int runTstMeasurementContract(int argc, char** argv) {
    TstMeasurementContract test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_measurementcontract.moc"
