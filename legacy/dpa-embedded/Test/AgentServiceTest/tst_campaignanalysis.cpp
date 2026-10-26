#include <QtTest/QtTest>

#include "CampaignAnalysis.h"
#include "ExperimentCampaign.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
#include <string>
#include <vector>

using namespace Model;

namespace {

std::string contractJson() {
    return R"JSON({
      "schema":"dpa.experiment.campaign","version":1,
      "campaign_id":"camp-an",
      "hypothesis":"analysis_type 与 average_num 共同影响响应幅值",
      "factors":[{"name":"analysis_type","levels":["FFT","PSD"]},
                 {"name":"average_num","levels":[1,4]}],
      "replicates":2,
      "randomization":{"enabled":false,"seed":42},
      "run_template":{"duration_ms":1000,"frame_size":1024},
      "policy":{"on_run_failure":"abort","max_runs":16}
    })JSON";
}

std::string observationsJson(const std::vector<int>& runIndices,
                             const std::vector<double>& values) {
    QJsonArray array;
    for (size_t i = 0; i < runIndices.size(); ++i)
        array.append(QJsonObject{{"run_index", runIndices[i]},
                                  {"value", values[i]}});
    return QJsonDocument(QJsonArray(array)).toJson(QJsonDocument::Compact).toStdString();
}

std::string requestJson(const std::string& observations, int seed,
                        const std::string& permutations = std::string()) {
    std::string request = std::string(R"JSON({"contract":)JSON") + contractJson()
        + R"JSON(,"observations":)JSON" + observations
        + R"JSON(,"seed":)JSON" + std::to_string(seed);
    if (!permutations.empty())
        request += R"(,"permutations":)" + permutations;
    request += "}";
    return request;
}

std::string requestJsonForContract(const std::string& contract,
                                   const std::string& observations, int seed) {
    return std::string(R"JSON({"contract":)JSON") + contract
        + R"JSON(,"observations":)JSON" + observations
        + R"JSON(,"seed":)JSON" + std::to_string(seed) + "}";
}

QJsonObject analyzeOrDie(const std::string& request) {
    const CampaignAnalysisOutput output = analyzeExperimentCampaign(request);
    if (!output.ok) return QJsonObject();
    return QJsonDocument::fromJson(QByteArray::fromStdString(
        output.resultJson)).object();
}

QJsonObject factorAt(const QJsonObject& result, const char* name) {
    for (const QJsonValue& factor : result.value("factors").toArray())
        if (factor.toObject().value("name").toString() == QLatin1String(name))
            return factor.toObject();
    return QJsonObject();
}

} // namespace

class TstCampaignAnalysis : public QObject { Q_OBJECT
private slots:
    void computes_means_effects_and_pure_error_for_known_data() {
        const std::string observations = observationsJson(
            {0, 1, 2, 3, 4, 5, 6, 7}, {10, 12, 20, 22, 10, 12, 20, 22});
        const QJsonObject result = analyzeOrDie(requestJson(observations, 7));
        QVERIFY(!result.isEmpty());
        QCOMPARE(result.value("schema").toString(),
                 QString("dpa.experiment.campaign.analysis/v1"));
        QCOMPARE(result.value("analyzed_runs").toInt(), 8);
        QCOMPARE(result.value("planned_runs").toInt(), 8);
        QCOMPARE(result.value("coverage_ratio").toDouble(), 1.0);
        QCOMPARE(result.value("grand_mean").toDouble(), 16.0);
        QCOMPARE(result.value("residual_std").toDouble(), 0.0);
        QCOMPARE(result.value("residual_estimable").toBool(), true);

        const QJsonObject typeEffect = factorAt(result, "analysis_type");
        QVERIFY(!typeEffect.isEmpty());
        QVERIFY(typeEffect.value("testable").toBool());
        QCOMPARE(typeEffect.value("effect_range").toDouble(), 10.0);
        QVERIFY(typeEffect.value("perm_p").toDouble() <= 0.01);
        const QJsonArray typeLevels = typeEffect.value("levels").toArray();
        QCOMPARE(typeLevels.at(0).toObject().value("mean").toDouble(), 11.0);
        QCOMPARE(typeLevels.at(1).toObject().value("mean").toDouble(), 21.0);

        const QJsonObject averageEffect = factorAt(result, "average_num");
        QVERIFY(averageEffect.value("testable").toBool());
        QCOMPARE(averageEffect.value("effect_range").toDouble(), 2.0);
        QVERIFY(averageEffect.value("perm_p").toDouble() < 0.05);

        QCOMPARE(result.value("best_cell_index").toInt(), 3);
        QCOMPARE(result.value("worst_cell_index").toInt(), 0);
    }

    void response_direction_selects_the_semantically_best_cell() {
        std::string contract = contractJson();
        const size_t position = contract.find("\"run_template\":");
        QVERIFY(position != std::string::npos);
        contract.insert(position,
            R"JSON("response":{"metric":"rms","unit":"g","direction":"minimize"},)JSON");
        const QJsonObject result = analyzeOrDie(requestJsonForContract(
            contract, observationsJson({0, 1, 2, 3, 4, 5, 6, 7},
                                       {10, 12, 20, 22, 10, 12, 20, 22}), 7));
        QVERIFY(!result.isEmpty());
        QCOMPARE(result.value("response_direction").toString(), QString("minimize"));
        QCOMPARE(result.value("best_cell_index").toInt(), 0);
        QCOMPARE(result.value("worst_cell_index").toInt(), 3);
    }

    void constant_null_data_returns_exact_one_p_values() {
        const std::string observations = observationsJson(
            {0, 1, 2, 3, 4, 5, 6, 7}, {5, 5, 5, 5, 5, 5, 5, 5});
        const QJsonObject result = analyzeOrDie(requestJson(observations, 7));
        QVERIFY(!result.isEmpty());
        QCOMPARE(factorAt(result, "analysis_type").value("perm_p").toDouble(), 1.0);
        QCOMPARE(factorAt(result, "average_num").value("perm_p").toDouble(), 1.0);
        QCOMPARE(factorAt(result, "analysis_type").value("effect_range").toDouble(), 0.0);
    }

    void partial_coverage_is_reported_honestly() {
        const std::string observations = observationsJson(
            {0, 1, 4, 5}, {10, 12, 10, 12});
        const QJsonObject result = analyzeOrDie(requestJson(observations, 7));
        QVERIFY(!result.isEmpty());
        QCOMPARE(result.value("analyzed_runs").toInt(), 4);
        QCOMPARE(result.value("coverage_ratio").toDouble(), 0.5);
        // The whole PSD group is missing, so that factor cannot be tested.
        const QJsonObject typeEffect = factorAt(result, "analysis_type");
        QVERIFY(!typeEffect.value("testable").toBool());
        QCOMPARE(typeEffect.value("perm_p").toDouble(), -1.0);
        const QJsonObject averageEffect = factorAt(result, "average_num");
        QVERIFY(averageEffect.value("testable").toBool());
        QCOMPARE(averageEffect.value("effect_range").toDouble(), 2.0);
    }

    void dropped_single_run_keeps_residual_estimable() {
        const std::string observations = observationsJson(
            {0, 1, 2, 3, 4, 5, 6}, {10, 12, 20, 22, 11, 13, 19});
        const QJsonObject result = analyzeOrDie(requestJson(observations, 7));
        QVERIFY(!result.isEmpty());
        QCOMPARE(result.value("analyzed_runs").toInt(), 7);
        QCOMPARE(result.value("residual_estimable").toBool(), true);
        // Three replicated cells each contribute one degree of freedom with
        // within-cell spread of +/-0.5 around their means.
        QVERIFY(qAbs(result.value("residual_std").toDouble() - std::sqrt(0.5)) < 1e-9);
    }

    void rejects_invalid_requests() {
        auto expectError = [](const std::string& request, const char* label) {
            const CampaignAnalysisOutput output = analyzeExperimentCampaign(request);
            QVERIFY2(!output.ok, qPrintable(QString::fromLatin1(label)));
        };
        const std::string full = observationsJson(
            {0, 1, 2, 3, 4, 5, 6, 7}, {10, 12, 20, 22, 10, 12, 20, 22});
        expectError(requestJson(full, 7) + "x", "trailing garbage");
        expectError(std::string(R"JSON({"contract":{"schema":"x"},"observations":[],"seed":1})JSON"),
                    "invalid contract");
        expectError(std::string(R"JSON({"contract":)JSON") + contractJson()
                    + R"JSON(,"observations":)JSON" + full + R"JSON(})JSON",
                    "missing seed");
        expectError(requestJson(observationsJson({0, 0}, {1, 2}), 7), "duplicate run");
        expectError(requestJson(observationsJson({8}, {1}), 7), "run out of range");
        const std::vector<int> noRuns;
        const std::vector<double> noValues;
        expectError(requestJson(observationsJson(noRuns, noValues), 7),
                    "empty observations");
        expectError(requestJson(observationsJson({0}, {1e13}), 7), "magnitude unbounded");
        expectError(requestJson(full, 7, "198"), "permutations below floor");
        expectError(requestJson(full, 7, "5000"), "permutations above ceiling");
    }

    void deterministic_given_identical_requests() {
        const std::string observations = observationsJson(
            {0, 1, 2, 3, 4, 5, 6, 7}, {10, 12, 20, 21, 11, 13, 19, 23});
        const double first = analyzeOrDie(requestJson(observations, 12345))
            .value("factors").toArray().at(0).toObject().value("perm_p").toDouble();
        const double second = analyzeOrDie(requestJson(observations, 12345))
            .value("factors").toArray().at(0).toObject().value("perm_p").toDouble();
        QVERIFY(first > 0.0 && first < 1.0);
        QCOMPARE(first, second);
    }

    void default_permutations_applied_when_omitted() {
        const std::string observations = observationsJson(
            {0, 1, 2, 3, 4, 5, 6, 7}, {10, 12, 20, 22, 10, 12, 20, 22});
        const QJsonObject result = analyzeOrDie(requestJson(observations, 7));
        QCOMPARE(result.value("permutations").toInt(), 999);
    }

    void evidence_binding_gates_observations() {
        const CampaignValidation validation =
            parseAndValidateExperimentCampaign(contractJson());
        QVERIFY(validation.ok);
        const QString sha =
            QString::fromStdString(validation.campaign.campaignSha256);
        QJsonArray runs;
        for (int i = 0; i < 8; ++i)
            runs.append(QJsonObject{
                {"run_index", i}, {"design_index", i % 4}, {"ok", i != 7},
                {"evidence_artifact_id",
                 i == 7 ? QString() : QStringLiteral("ev%1").arg(i)}});
        const QByteArray artifact = QJsonDocument(QJsonObject{
            {"schema", "dpa.experiment.campaign.result/v1"},
            {"campaign_sha256", sha}, {"status", "completed"},
            {"planned_runs", 8}, {"runs", runs}})
            .toJson(QJsonDocument::Compact);
        auto bind = [&](const std::string& observations, double coverage) {
            return bindObservationsToEvidence(artifact.toStdString(),
                validation.campaign.campaignSha256, observations, coverage);
        };

        const EvidenceBoundObservations ok6 = bind(
            observationsJson({0, 1, 2, 3, 4, 5}, {10, 12, 20, 22, 10, 12}), 0.75);
        QVERIFY2(ok6.ok, qPrintable(QString::fromStdString(ok6.error)));
        QCOMPARE(ok6.verifiedRuns, 6);
        QVERIFY(!ok6.observationsJson.empty());

        QVERIFY(!bind(observationsJson({0, 7}, {1, 2}), 0.0).ok);      // no evidence
        QVERIFY(!bind(observationsJson({0, 9}, {1, 2}), 0.0).ok);      // unknown run
        QVERIFY(!bind(observationsJson({0}, {1}), 0.75).ok);           // coverage
        const EvidenceBoundObservations foreign = bindObservationsToEvidence(
            artifact.toStdString(), std::string(64, 'f'),
            observationsJson({0, 1}, {1, 2}), 0.75);
        QVERIFY(!foreign.ok);                                          // sha mismatch
    }

    void extracts_only_executor_recorded_responses() {
        const CampaignValidation validation =
            parseAndValidateExperimentCampaign(contractJson());
        QVERIFY(validation.ok);
        QJsonArray runs;
        for (int i = 0; i < 8; ++i) {
            runs.append(QJsonObject{
                {"run_index", i}, {"design_index", i % 4}, {"ok", true},
                {"evidence_artifact_id", QStringLiteral("ev%1").arg(i)},
                {"response", QJsonObject{
                    {"metric", "rms"}, {"value", 10.0 + i},
                    {"channel_id", 7}, {"sample_count", 2}, {"unit", "g"},
                    {"source", "monitoring_window_mean"},
                    {"evidence_payload_sha256", QString(64, 'a')}}}});
        }
        auto artifact = [&]() {
            QJsonObject result{
                {"schema", "dpa.experiment.campaign.result/v1"},
                {"campaign_sha256",
                 QString::fromStdString(validation.campaign.campaignSha256)},
                {"status", "completed"}, {"planned_runs", 8}, {"runs", runs}};
            result["payload_sha256"] = QString::fromLatin1(QCryptographicHash::hash(
                QJsonDocument(result).toJson(QJsonDocument::Compact),
                QCryptographicHash::Sha256).toHex());
            return QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString();
        };
        const EvidenceBoundObservations extracted = extractObservationsFromCampaignResult(
            artifact(), validation.campaign.campaignSha256, "rms", 1.0, "g");
        QVERIFY2(extracted.ok, qPrintable(QString::fromStdString(extracted.error)));
        QCOMPARE(extracted.verifiedRuns, 8);
        QCOMPARE(QJsonDocument::fromJson(QByteArray::fromStdString(
            extracted.observationsJson)).array().at(7).toObject()
            .value("value").toDouble(), 17.0);

        QJsonObject tampered = QJsonDocument::fromJson(
            QByteArray::fromStdString(artifact())).object();
        QJsonArray tamperedRuns = tampered.value("runs").toArray();
        QJsonObject tamperedRun = tamperedRuns.at(0).toObject();
        QJsonObject tamperedResponse = tamperedRun.value("response").toObject();
        tamperedResponse["value"] = 999.0;
        tamperedRun["response"] = tamperedResponse;
        tamperedRuns[0] = tamperedRun;
        tampered["runs"] = tamperedRuns;
        QCOMPARE(QString::fromStdString(extractObservationsFromCampaignResult(
            QJsonDocument(tampered).toJson(QJsonDocument::Compact).toStdString(),
            validation.campaign.campaignSha256, "rms", 1.0).error),
            QString("campaign_artifact_integrity_mismatch"));

        QJsonObject mismatched = runs.at(7).toObject();
        QJsonObject response = mismatched.value("response").toObject();
        response["unit"] = "m/s2";
        mismatched["response"] = response;
        runs[7] = mismatched;
        QVERIFY(!extractObservationsFromCampaignResult(
            artifact(), validation.campaign.campaignSha256, "rms", 1.0, "g").ok);
    }

    void detects_pure_interaction_and_exact_additive_null() {
        // Pure interaction: main effects cancel entirely while the diagonal
        // cell pattern carries all signal.
        const std::string interactionValues = observationsJson(
            {0, 1, 2, 3, 4, 5, 6, 7}, {10, 0, 0, 10, 10, 0, 0, 10});
        const QJsonObject result = analyzeOrDie(requestJson(interactionValues, 7));
        const QJsonArray interactions = result.value("interactions").toArray();
        QCOMPARE(interactions.size(), 1);
        const QJsonObject pair = interactions.at(0).toObject();
        QVERIFY(pair.value("testable").toBool());
        QCOMPARE(pair.value("method").toString(), QString("freedman_lane"));
        QVERIFY(pair.value("perm_p").toDouble() <= 0.05);

        // Perfectly additive data yields exactly zero interaction statistic,
        // so the permutation p-value is exactly one.
        const std::string additiveValues = observationsJson(
            {0, 1, 2, 3, 4, 5, 6, 7}, {11, 12, 21, 22, 11, 12, 21, 22});
        const QJsonObject nullResult = analyzeOrDie(requestJson(additiveValues, 7));
        const QJsonObject nullPair = nullResult.value("interactions")
            .toArray().at(0).toObject();
        QVERIFY(nullPair.value("testable").toBool());
        QCOMPARE(nullPair.value("perm_p").toDouble(), 1.0);
    }
};

int runTstCampaignAnalysis(int argc, char** argv) {
    TstCampaignAnalysis test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_campaignanalysis.moc"
