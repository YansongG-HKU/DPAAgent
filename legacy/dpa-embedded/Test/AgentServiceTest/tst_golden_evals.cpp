#include <QtTest/QtTest>
#include "Diagnostics.h"
#include "EnvelopeAnalysis.h"
#include "AgentJson.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <cmath>

using namespace Model;

static QJsonObject loadGoldenSuite(QString* error)
{
    const QString path = QFINDTESTDATA("../../TestData/AgentEval/golden-evals.json");
    if (path.isEmpty()) {
        *error = "golden-evals.json not found";
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = parseError.errorString();
        return {};
    }
    return document.object();
}

static bool isNonEmptyStringArray(const QJsonValue& value)
{
    const QJsonArray array = value.toArray();
    if (array.isEmpty()) return false;
    for (const QJsonValue& item : array)
        if (!item.isString() || item.toString().trimmed().isEmpty()) return false;
    return true;
}

static QSet<QString> stringSet(const QJsonValue& value)
{
    QSet<QString> out;
    for (const QJsonValue& item : value.toArray())
        if (item.isString()) out.insert(item.toString());
    return out;
}

class TstGoldenEvals : public QObject {
    Q_OBJECT
private slots:
    void manifest_has_required_binary_cases()
    {
        QString error;
        const QJsonObject root = loadGoldenSuite(&error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(root.value("schema_version").toInt(), 2);
        QCOMPARE(root.value("scoring").toObject().value("type").toString(), QString("binary"));
        QVERIFY2(!root.value("suite_version").toString().isEmpty(), "suite_version is required");
        const QJsonObject contract = root.value("diagnostic_contract").toObject();
        QCOMPARE(contract.value("result_contract_version").toString(),
                 QString(kDiagnosticResultContractVersion));
        QCOMPARE(contract.value("algorithm_version").toString(),
                 QString(kDiagnosticAlgorithmVersion));
        QCOMPARE(contract.value("rule_version").toString(),
                 QString(kDiagnosticRuleVersion));
        const QSet<QString> requiredScopeDomains = {
            "profile", "rotor_order", "bearing_frequency", "resonance", "severity"
        };
        const QSet<QString> declaredScopeDomains =
            stringSet(contract.value("required_scope_domains"));
        QCOMPARE(declaredScopeDomains, requiredScopeDomains);
        QVERIFY2(!root.value("scope_profiles").toObject().isEmpty(),
                 "scope_profiles is required");

        const QSet<QString> requiredCategories = {
            "healthy", "imbalance_1x", "misalignment_2x", "looseness_harmonics",
            "bearing_bpfo", "bearing_bpfi", "bearing_bsf", "bearing_ftf", "envelope_am",
            "missing_rpm", "wrong_unit", "wrong_sample_rate", "empty_result",
            "bearing_corroboration", "resonance_abstention", "partial_assessment",
            "standard_governance", "metadata_prompt_injection",
            "objective_planning"
        };
        const QSet<QString> allowedRequirements = {"pure_core", "authorized_machine", "real_model"};
        QSet<QString> categories;
        QSet<QString> ids;

        const QJsonArray cases = root.value("cases").toArray();
        QVERIFY(!cases.isEmpty());
        for (const QJsonValue& value : cases) {
            const QJsonObject evalCase = value.toObject();
            const QString id = evalCase.value("id").toString();
            QVERIFY2(!id.isEmpty(), "eval case id is required");
            QVERIFY2(!ids.contains(id), qPrintable("duplicate eval id: " + id));
            ids.insert(id);

            const QString category = evalCase.value("category").toString();
            QVERIFY2(!category.isEmpty(), qPrintable(id + ": category is required"));
            categories.insert(category);
            QVERIFY2(!evalCase.value("input").toObject().isEmpty(), qPrintable(id + ": input is required"));
            QVERIFY2(!evalCase.value("ground_truth").toObject().isEmpty(), qPrintable(id + ": ground_truth is required"));

            const QJsonObject execution = evalCase.value("execution").toObject();
            const QJsonArray requirements = execution.value("requires").toArray();
            QVERIFY2(!requirements.isEmpty(), qPrintable(id + ": execution.requires is required"));
            for (const QJsonValue& requirement : requirements)
                QVERIFY2(allowedRequirements.contains(requirement.toString()), qPrintable(id + ": unknown execution requirement"));
            if (execution.value("automated").toBool()) {
                QVERIFY2(requirements.contains("pure_core"), qPrintable(id + ": automated cases must require pure_core"));
                QVERIFY2(isNonEmptyStringArray(execution.value("automated_by")), qPrintable(id + ": automated_by is required"));
                QVERIFY2(execution.value("automated_by").toArray().contains(
                             QString("TstGoldenEvals::automated_cases_match_ground_truth")),
                         qPrintable(id + ": every automated case must be executed by the manifest runner"));
            } else {
                QVERIFY2(isNonEmptyStringArray(execution.value("procedure")), qPrintable(id + ": manual procedure is required"));
            }

            const QJsonObject rubric = evalCase.value("rubric").toObject();
            QCOMPARE(rubric.value("type").toString(), QString("binary"));
            QVERIFY2(isNonEmptyStringArray(rubric.value("pass_if")), qPrintable(id + ": pass_if is required"));
            QVERIFY2(isNonEmptyStringArray(rubric.value("fail_if")), qPrintable(id + ": fail_if is required"));
            if (execution.value("automated").toBool()
                && evalCase.value("input").toObject().value("kind").toString() == "diagnostic_peaks") {
                const QJsonObject truth = evalCase.value("ground_truth").toObject();
                for (const char* key : {"expected_faults", "forbidden_faults",
                                        "expected_rule_ids", "expected_unassessed_rule_ids",
                                        "forbidden_rule_ids"}) {
                    QVERIFY2(truth.value(key).isArray(),
                             qPrintable(id + ": " + QString::fromUtf8(key) + " must be an array"));
                }
                QVERIFY2(truth.value("expected_ok").isBool(),
                         qPrintable(id + ": expected_ok is required"));
                QVERIFY2(!truth.value("expected_assessment_status").toString().isEmpty(),
                         qPrintable(id + ": expected_assessment_status is required"));
                const QString scopeProfile = truth.value("expected_scope_profile").toString();
                QVERIFY2(root.value("scope_profiles").toObject().contains(scopeProfile),
                         qPrintable(id + ": unknown expected_scope_profile " + scopeProfile));
            }

        }

        for (const QString& required : requiredCategories)
            QVERIFY2(categories.contains(required), qPrintable("missing required category: " + required));

        const QSet<QString> requiredObjectiveIds = {
            "objective_valid_requirement", "objective_missing_parameters",
            "objective_contradictory_direction", "objective_unauthorized_factor",
            "objective_prompt_injection", "objective_infeasible_budget"
        };
        QCOMPARE(stringSet(root.value("objective_planning_gate").toObject()
                               .value("required_case_ids")),
                 requiredObjectiveIds);
        QCOMPARE(root.value("objective_planning_gate").toObject()
                     .value("pass_rule").toString(),
                 QString("all_required_cases_pass_on_the_same_model_configuration"));
        for (const QString& id : requiredObjectiveIds)
            QVERIFY2(ids.contains(id), qPrintable("missing objective eval: " + id));
        for (const QJsonValue& value : cases) {
            const QJsonObject evalCase = value.toObject();
            if (!requiredObjectiveIds.contains(evalCase.value("id").toString()))
                continue;
            const QJsonObject execution = evalCase.value("execution").toObject();
            QVERIFY(!execution.value("automated").toBool());
            QVERIFY(execution.value("requires").toArray().contains("real_model"));
        }
    }

    void automated_cases_match_ground_truth()
    {
        QString error;
        const QJsonObject root = loadGoldenSuite(&error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        int automatedCount = 0;
        QSet<QString> executedIds;

        for (const QJsonValue& value : root.value("cases").toArray()) {
            const QJsonObject evalCase = value.toObject();
            if (!evalCase.value("execution").toObject().value("automated").toBool()) continue;
            ++automatedCount;
            const QString id = evalCase.value("id").toString();
            const QJsonObject input = evalCase.value("input").toObject();
            const QString kind = input.value("kind").toString();
            const QJsonObject truth = evalCase.value("ground_truth").toObject();

            if (kind == "diagnostic_peaks") {
                DiagnoseConfig config;
                config.asset.assetId = ("golden-" + id).toStdString();
                config.asset.machineType = "centrifugal_pump";
                config.asset.rpm = input.value("rpm").toDouble();
                config.asset.rpmSource = input.value("rpm_source").toString().toStdString();
                config.asset.operatingCondition = "golden steady-state fixture";
                config.freqTolRatio = input.value("freq_tolerance_ratio").toDouble(0.05);
                const QJsonObject bearing = input.value("bearing_geometry").toObject();
                config.asset.bearing.rollingElements = bearing.value("rolling_elements").toInt();
                config.asset.bearing.ballDiaMm = bearing.value("ball_diameter_mm").toDouble();
                config.asset.bearing.pitchDiaMm = bearing.value("pitch_diameter_mm").toDouble();
                config.asset.bearing.contactAngleDeg = bearing.value("contact_angle_deg").toDouble();
                config.asset.bearing.contactAngleKnown = bearing.contains("contact_angle_deg")
                    && bearing.value("contact_angle_deg").isDouble();
                config.measurement.dataId = ("golden-data-" + id).toStdString();
                config.measurement.channel = 1;
                config.measurement.measurementPoint = "drive-end bearing";
                config.measurement.direction = "radial-horizontal";
                config.measurement.physicalQuantity = "vibration_velocity";
                config.measurement.unit = "mm/s";
                config.measurement.samplingRateHz = 51200;
                config.measurement.standardProfile =
                    input.value("standard_profile").toString(kNoStandardProfile).toStdString();

                const QJsonObject standard = input.value("standard").toObject();
                config.standardApplicabilityConfirmed =
                    standard.value("applicability_confirmed").toBool(false);
                config.standardApplicabilityBasis =
                    standard.value("applicability_basis").toString().toStdString();
                config.severityVelocityRmsMmS =
                    standard.value("velocity_rms_mm_s").toDouble(0);
                config.legacyIso10816MachineClass =
                    standard.value("legacy_machine_class").toInt(0);
                config.velocityRmsSource =
                    standard.value("velocity_rms_source").toString().toStdString();
                config.velocityRmsDataRef =
                    standard.value("velocity_rms_data_ref").toString().toStdString();

                const ProfileValidation profile =
                    validateDiagnosticProfile(config.asset, config.measurement);
                const bool expectedProfileValid = input.contains("profile_valid")
                    ? input.value("profile_valid").toBool() : config.asset.rpm > 0;
                QCOMPARE(profile.valid(), expectedProfileValid);

                std::vector<SpectralPeak> peaks;
                double maxFrequencyHz = 0;
                for (const QJsonValue& peakValue : input.value("peaks").toArray()) {
                    const QJsonObject peak = peakValue.toObject();
                    const double amplitude = peak.value("value").toDouble();
                    const double frequency = peak.value("freq_hz").toDouble();
                    if (frequency > maxFrequencyHz) maxFrequencyHz = frequency;
                    peaks.push_back({frequency, amplitude, amplitude});
                }

                const ToolResult result = diagnoseSpectrum(peaks, config);
                QCOMPARE(result.ok, truth.value("expected_ok").toBool());
                if (!truth.value("expected_error").toString().isEmpty())
                    QCOMPARE(QString::fromStdString(result.error),
                             truth.value("expected_error").toString());

                const QJsonObject data = AgentJson::parseObject(result.dataJson);
                QVERIFY2(!data.isEmpty(), qPrintable(id + ": diagnostic result contract is required"));
                QCOMPARE(data.value("result_contract_version").toString(),
                         QString(kDiagnosticResultContractVersion));
                QCOMPARE(data.value("algorithm_version").toString(),
                         QString(kDiagnosticAlgorithmVersion));
                QCOMPARE(data.value("rule_version").toString(),
                         QString(kDiagnosticRuleVersion));
                QCOMPARE(data.value("assessment_status").toString(),
                         truth.value("expected_assessment_status").toString());

                const QJsonObject provenance = data.value("input_provenance").toObject();
                const QJsonObject provenanceAsset = provenance.value("asset").toObject();
                QCOMPARE(provenanceAsset.value("asset_id").toString(), "golden-" + id);
                QCOMPARE(provenanceAsset.value("rpm").toDouble(), config.asset.rpm);
                QCOMPARE(provenanceAsset.value("rpm_source").toString(),
                         QString::fromStdString(config.asset.rpmSource));
                const QJsonObject provenanceBearing =
                    provenanceAsset.value("bearing_geometry").toObject();
                QCOMPARE(provenanceBearing.value("rolling_elements").toInt(),
                         config.asset.bearing.rollingElements);
                QCOMPARE(provenanceBearing.value("contact_angle_known").toBool(),
                         config.asset.bearing.contactAngleKnown);

                const QJsonObject provenanceMeasurement =
                    provenance.value("measurement").toObject();
                QCOMPARE(provenanceMeasurement.value("data_id").toString(),
                         "golden-data-" + id);
                QCOMPARE(provenanceMeasurement.value("channel").toInt(),
                         config.measurement.channel);
                QCOMPARE(provenanceMeasurement.value("unit").toString(),
                         QString::fromStdString(config.measurement.unit));
                QCOMPARE(provenanceMeasurement.value("sampling_rate_hz").toDouble(),
                         config.measurement.samplingRateHz);
                QCOMPARE(provenanceMeasurement.value("standard_profile").toString(),
                         QString::fromStdString(config.measurement.standardProfile));

                const QJsonObject provenanceStandard =
                    provenance.value("standard").toObject();
                QCOMPARE(provenanceStandard.value("applicability_confirmed").toBool(),
                         config.standardApplicabilityConfirmed);
                QCOMPARE(provenanceStandard.value("applicability_basis").toString(),
                         QString::fromStdString(config.standardApplicabilityBasis));
                QCOMPARE(provenanceStandard.value("velocity_rms_mm_s").toDouble(),
                         config.severityVelocityRmsMmS);
                QCOMPARE(provenanceStandard.value("legacy_machine_class").toInt(),
                         config.legacyIso10816MachineClass);
                QCOMPARE(provenanceStandard.value("velocity_rms_source").toString(),
                         QString::fromStdString(config.velocityRmsSource));
                QCOMPARE(provenanceStandard.value("velocity_rms_data_ref").toString(),
                         QString::fromStdString(config.velocityRmsDataRef));


                const QJsonObject features = provenance.value("features").toObject();
                QCOMPARE(features.value("peak_count").toInt(), static_cast<int>(peaks.size()));
                QCOMPARE(features.value("max_frequency_hz").toDouble(), maxFrequencyHz);
                QCOMPARE(features.value("raw_values_included").toBool(true), false);

                QSet<QString> actualFaults;
                QSet<QString> actualRuleIds;
                QSet<QString> actualUnassessedRuleIds;
                for (const QJsonValue& findingValue : data.value("findings").toArray()) {
                    const QJsonObject finding = findingValue.toObject();
                    QVERIFY2(!finding.value("evidence").toString().isEmpty(),
                             qPrintable(id + ": finding evidence is required"));
                    QCOMPARE(finding.value("algorithm_version").toString(),
                             QString(kDiagnosticAlgorithmVersion));
                    QCOMPARE(finding.value("rule_version").toString(),
                             QString(kDiagnosticRuleVersion));
                    if (finding.value("assessed").toBool()) {
                        actualFaults.insert(finding.value("fault").toString());
                        actualRuleIds.insert(finding.value("rule_id").toString());
                    } else {
                        actualUnassessedRuleIds.insert(finding.value("rule_id").toString());
                    }
                }

                const QSet<QString> expectedFaults = stringSet(truth.value("expected_faults"));
                const QSet<QString> expectedRuleIds = stringSet(truth.value("expected_rule_ids"));
                const QSet<QString> expectedUnassessedRuleIds =
                    stringSet(truth.value("expected_unassessed_rule_ids"));
                QVERIFY2(actualFaults == expectedFaults, qPrintable(
                    id + ": actual faults={" + QStringList(actualFaults.values()).join(",")
                    + "}, expected={" + QStringList(expectedFaults.values()).join(",") + "}"));
                QVERIFY2(actualRuleIds == expectedRuleIds, qPrintable(
                    id + ": actual rule ids={" + QStringList(actualRuleIds.values()).join(",")
                    + "}, expected={" + QStringList(expectedRuleIds.values()).join(",") + "}"));
                QVERIFY2(actualUnassessedRuleIds == expectedUnassessedRuleIds, qPrintable(
                    id + ": actual unassessed rule ids={"
                    + QStringList(actualUnassessedRuleIds.values()).join(",")
                    + "}, expected={" + QStringList(expectedUnassessedRuleIds.values()).join(",") + "}"));
                for (const QString& forbidden : stringSet(truth.value("forbidden_faults")))
                    QVERIFY2(!actualFaults.contains(forbidden),
                             qPrintable(id + ": emitted forbidden fault " + forbidden));
                for (const QString& forbidden : stringSet(truth.value("forbidden_rule_ids")))
                    QVERIFY2(!actualRuleIds.contains(forbidden),
                             qPrintable(id + ": emitted forbidden rule " + forbidden));

                const QString expectedScopeProfile =
                    truth.value("expected_scope_profile").toString();
                const QJsonObject expectedScope = root.value("scope_profiles").toObject()
                    .value(expectedScopeProfile).toObject();
                for (const QString& domain : stringSet(root.value("diagnostic_contract")
                                                       .toObject()
                                                       .value("required_scope_domains"))) {
                    const QJsonObject scopeItem = data.value("assessment_scope").toObject()
                        .value(domain).toObject();
                    QCOMPARE(scopeItem.value("status").toString(),
                             expectedScope.value(domain).toString());
                    QVERIFY2(!scopeItem.value("reason").toString().isEmpty(),
                             qPrintable(id + ": scope reason is required for " + domain));
                }
            } else if (kind == "am_waveform") {
                const double fs = input.value("sample_rate_hz").toDouble();
                const int sampleCount = input.value("samples").toInt();
                const double carrier = input.value("carrier_hz").toDouble();
                const double modulation = input.value("modulation_hz").toDouble();
                const double depth = input.value("modulation_depth").toDouble();
                const QJsonArray band = input.value("band_hz").toArray();
                std::vector<double> waveform(sampleCount);
                constexpr double pi = 3.14159265358979323846;
                for (int i = 0; i < sampleCount; ++i) {
                    const double t = i / fs;
                    waveform[i] = (1.0 + depth * std::cos(2 * pi * modulation * t))
                        * std::cos(2 * pi * carrier * t);
                }
                const EnvSpectrum spectrum = envelopeSpectrum(waveform, fs, band.at(0).toDouble(), band.at(1).toDouble());
                double bestFrequency = 0, bestMagnitude = -1;
                for (size_t i = 0; i < spectrum.freqHz.size(); ++i) {
                    if (spectrum.freqHz[i] > 10 && spectrum.mag[i] > bestMagnitude) {
                        bestMagnitude = spectrum.mag[i];
                        bestFrequency = spectrum.freqHz[i];
                    }
                }
                QVERIFY(std::fabs(bestFrequency - truth.value("expected_frequency_hz").toDouble())
                        <= truth.value("frequency_tolerance_hz").toDouble());
                QVERIFY(std::fabs(bestMagnitude - truth.value("expected_amplitude").toDouble())
                        <= truth.value("amplitude_tolerance").toDouble());
            } else if (kind == "channel_metadata") {
                DiagnoseConfig config;
                config.asset.assetId = "golden-unit";
                config.asset.machineType = "centrifugal_pump";
                config.asset.rpm = 1800;
                config.asset.rpmSource = "tachometer";
                config.asset.operatingCondition = "steady";
                config.measurement.dataId = "golden-unit-data";
                config.measurement.channel = 1;
                config.measurement.measurementPoint = "DE";
                config.measurement.direction = "radial";
                config.measurement.physicalQuantity = "vibration_velocity";
                config.measurement.unit = input.value("declared_unit").toString().toStdString();
                config.measurement.samplingRateHz = 51200;
                config.measurement.standardProfile = kNoStandardProfile;
                const ProfileValidation validation = validateDiagnosticProfile(config.asset, config.measurement);
                QVERIFY2(!validation.valid(), qPrintable(id + ": invalid unit must be rejected"));
                const ToolResult result = diagnoseSpectrum({{50.0, 1.0, 0.9}}, config);
                QVERIFY(!result.ok);
                QCOMPARE(QString::fromStdString(result.error), truth.value("expected_error").toString());
            } else if (kind == "analysis_request") {
                const QJsonArray band = input.value("requested_band_hz").toArray();
                const std::string rangeError = validateFrequencyRange(
                    input.value("sample_rate_hz").toDouble(), band.at(1).toDouble());
                QCOMPARE(QString::fromStdString(rangeError), truth.value("expected_error").toString());
            } else if (kind == "empty_peaks") {
                DiagnoseConfig config;
                config.asset.assetId = "golden-empty";
                config.asset.machineType = "centrifugal_pump";
                config.asset.rpm = 1800;
                config.asset.rpmSource = "tachometer";
                config.asset.operatingCondition = "steady";
                config.measurement.dataId = "golden-empty-data";
                config.measurement.channel = 1;
                config.measurement.measurementPoint = "DE";
                config.measurement.direction = "radial";
                config.measurement.physicalQuantity = "vibration_velocity";
                config.measurement.unit = "mm/s";
                config.measurement.samplingRateHz = 51200;
                config.measurement.standardProfile = kNoStandardProfile;
                const ToolResult result = diagnoseSpectrum({}, config);
                QVERIFY(!result.ok);
                QCOMPARE(QString::fromStdString(result.error), truth.value("expected_error").toString());
            } else {
                QFAIL(qPrintable(id + ": automated input kind has no manifest runner"));
            }
            QVERIFY2(!executedIds.contains(id), qPrintable(id + ": executed twice"));
            executedIds.insert(id);
        }

        QCOMPARE(executedIds.size(), automatedCount);
        QVERIFY2(automatedCount >= 12, "expected at least twelve manifest-driven automated cases");
    }
};

int runTstGoldenEvals(int argc, char** argv)
{
    TstGoldenEvals test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_golden_evals.moc"
