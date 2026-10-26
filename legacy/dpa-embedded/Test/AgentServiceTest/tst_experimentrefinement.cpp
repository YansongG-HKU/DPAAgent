#include <QtTest/QtTest>

#include "ExperimentRefinement.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <string>
#include <vector>

using namespace Model;

namespace {

std::string contractJson() {
    return R"JSON({
      "schema":"dpa.experiment.campaign","version":1,
      "campaign_id":"camp-ref",
      "hypothesis":"screening: analysis_type dominant, average_num unclear",
      "factors":[{"name":"analysis_type","levels":["FFT","PSD"]},
                 {"name":"average_num","levels":[1,4]}],
      "replicates":2,
      "randomization":{"enabled":false,"seed":42},
      "run_template":{"duration_ms":1000,"frame_size":1024},
      "policy":{"on_run_failure":"abort","max_runs":16}
    })JSON";
}

std::string minimizingContractJson() {
    std::string contract = contractJson();
    const size_t position = contract.find("\"run_template\":");
    contract.insert(position,
        R"JSON("response":{"metric":"rms","unit":"g","direction":"minimize"},)JSON");
    return contract;
}

std::string numericContract() {
    return R"JSON({
      "schema":"dpa.experiment.campaign","version":1,
      "campaign_id":"camp-num",
      "hypothesis":"numeric knob refinement stays inside the legal domain",
      "factors":[{"name":"average_num","levels":[10,50]}],
      "replicates":2,
      "randomization":{"enabled":false,"seed":7},
      "run_template":{"duration_ms":1000,"analysis_type":"FFT"},
      "policy":{"on_run_failure":"continue","max_runs":16}
    })JSON";
}

std::string enumContract() {
    return R"JSON({
      "schema":"dpa.experiment.campaign","version":1,
      "campaign_id":"camp-enum",
      "hypothesis":"ordered enum knob refines along its ladder",
      "factors":[{"name":"frame_size","levels":[1024,8192]}],
      "replicates":2,
      "randomization":{"enabled":false,"seed":7},
      "run_template":{"duration_ms":1000,"analysis_type":"FFT"},
      "policy":{"on_run_failure":"continue","max_runs":16}
    })JSON";
}

std::string observationsJson(const std::vector<double>& values) {
    QJsonArray array;
    for (int runIndex = 0; runIndex < static_cast<int>(values.size()); ++runIndex)
        array.append(QJsonObject{{"run_index", runIndex},
                                  {"value", values[static_cast<size_t>(runIndex)]}});
    return QJsonDocument(QJsonArray(array)).toJson(QJsonDocument::Compact).toStdString();
}

std::string requestJson(const std::string& contract, const std::string& observations,
                        int seed) {
    return std::string(R"JSON({"contract":)JSON") + contract
        + R"JSON(,"observations":)JSON" + observations
        + R"JSON(,"seed":)JSON" + std::to_string(seed) + "}";
}

QJsonObject refineOrDie(const std::string& request) {
    const CampaignRefinementOutput output = refineExperimentCampaign(request);
    if (!output.ok) return QJsonObject();
    return QJsonDocument::fromJson(QByteArray::fromStdString(
        output.resultJson)).object();
}

QJsonObject assessmentAt(const QJsonObject& result, const char* name) {
    for (const QJsonValue& entry : result.value("factor_assessment").toArray())
        if (entry.toObject().value("name").toString() == QLatin1String(name))
            return entry.toObject();
    return QJsonObject();
}

} // namespace

class TstExperimentRefinement : public QObject { Q_OBJECT
private slots:
    void strong_factor_refined_weak_factor_fixed_at_best() {
        const std::string request =
            requestJson(contractJson(), observationsJson(
                {10.5, 9.5, 30.5, 29.5, 10.0, 10.0, 30.0, 30.0}), 7);
        const QJsonObject result = refineOrDie(request);
        QVERIFY2(!result.isEmpty(), qPrintable(QString::fromStdString(
            refineExperimentCampaign(request).error)));
        QCOMPARE(assessmentAt(result, "analysis_type")
            .value("action").toString(), QString("refined"));
        QCOMPARE(assessmentAt(result, "average_num")
            .value("action").toString(), QString("fixed_at_best"));
        QVERIFY(assessmentAt(result, "analysis_type").value("snr").toDouble()
            > assessmentAt(result, "average_num").value("snr").toDouble());
        const QJsonObject proposed = result.value("proposed_contract").toObject();
        QCOMPARE(proposed.value("schema").toString(),
                 QString("dpa.experiment.campaign"));
        QCOMPARE(proposed.value("factors").toArray().size(), 1);
        QCOMPARE(proposed.value("factors").toArray().at(0).toObject()
            .value("name").toString(), QString("analysis_type"));
        // avg1 (mean 20.25) beats avg4 (mean 19.75), so the template pins 1.
        QCOMPARE(proposed.value("run_template").toObject()
            .value("average_num").toInt(), 1);
        // Replicates climb from 3 until the discrete p-floor clears alpha:
        // C(6,3)=20 arrangements cannot calibrate, C(8,4)=70 can.
        QCOMPARE(result.value("planned_runs").toInt(), 8);
        QVERIFY(result.value("proposed_contract_sha256").toString().size() == 64);
        QVERIFY(!result.value("power").toObject().isEmpty());
        QCOMPARE(result.value("power").toObject().value("calibrated").toBool(), true);
    }

    void categorical_levels_ranked_by_mean_desc() {
        const std::string request =
            requestJson(contractJson(), observationsJson(
                {10.5, 9.5, 30.5, 29.5, 10.0, 10.0, 30.0, 30.0}), 7);
        const QJsonObject result = refineOrDie(request);
        QVERIFY(!result.isEmpty());
        const QJsonArray levels = result.value("proposed_contract").toObject()
            .value("factors").toArray().at(0).toObject()
            .value("levels").toArray();
        QCOMPARE(levels.size(), 2);
        QCOMPARE(levels.at(0).toString(), QString("PSD"));
        QCOMPARE(levels.at(1).toString(), QString("FFT"));
    }

    void refinement_honors_minimize_and_preserves_response_goal() {
        const QJsonObject result = refineOrDie(requestJson(
            minimizingContractJson(), observationsJson(
                {10.5, 9.5, 30.5, 29.5, 10.0, 10.0, 30.0, 30.0}), 7));
        QVERIFY(!result.isEmpty());
        const QJsonObject proposed = result.value("proposed_contract").toObject();
        const QJsonObject response = proposed.value("response").toObject();
        QCOMPARE(response.value("metric").toString(), QString("rms"));
        QCOMPARE(response.value("unit").toString(), QString("g"));
        QCOMPARE(response.value("direction").toString(), QString("minimize"));
        const QJsonArray levels = proposed.value("factors").toArray().at(0)
                                      .toObject().value("levels").toArray();
        QCOMPARE(levels.at(0).toString(), QString("FFT"));
        QCOMPARE(levels.at(1).toString(), QString("PSD"));
    }

    void numeric_levels_stay_in_domain_around_best() {
        // average_num [10,50], best at 50: midpoint refinement yields {30,50}.
        const std::string request = requestJson(numericContract(),
            observationsJson({1.0, 3.0, 5.0, 7.0}), 7);
        const QJsonObject result = refineOrDie(request);
        QVERIFY2(!result.isEmpty(), qPrintable(QString::fromStdString(
            refineExperimentCampaign(request).error)));
        const QJsonArray levels = result.value("proposed_contract").toObject()
            .value("factors").toArray().at(0).toObject().value("levels").toArray();
        QCOMPARE(levels.size(), 2);
        QCOMPARE(levels.at(0).toInt(), 30);
        QCOMPARE(levels.at(1).toInt(), 50);
    }

    void ordered_enum_levels_use_ladder_neighbors() {
        // Ladder around 1024 is {512, 1024, 2048}; run 0 holds the best mean.
        const std::string request = requestJson(enumContract(),
            observationsJson({9.0, 1.0, 2.0, 3.0}), 7);
        const QJsonObject result = refineOrDie(request);
        QVERIFY2(!result.isEmpty(), qPrintable(QString::fromStdString(
            refineExperimentCampaign(request).error)));
        const QJsonArray levels = result.value("proposed_contract").toObject()
            .value("factors").toArray().at(0).toObject().value("levels").toArray();
        QCOMPARE(levels.size(), 3);
        QCOMPARE(levels.at(0).toInt(), 512);
        QCOMPARE(levels.at(1).toInt(), 1024);
        QCOMPARE(levels.at(2).toInt(), 2048);
    }

    void weak_signal_promotes_dominant_factor_with_warning() {
        const std::string request = requestJson(contractJson(),
            observationsJson({1, 3, 2, 4, 3, 1, 4, 2}), 7);
        const QJsonObject result = refineOrDie(request);
        QVERIFY2(!result.isEmpty(), qPrintable(QString::fromStdString(
            refineExperimentCampaign(request).error)));
        QCOMPARE(assessmentAt(result, "analysis_type")
            .value("action").toString(), QString("promoted"));
        QCOMPARE(assessmentAt(result, "average_num")
            .value("action").toString(), QString("fixed_at_best"));
        bool foundWarning = false;
        for (const QJsonValue& warning : result.value("warnings").toArray())
            if (warning.toString().contains(QStringLiteral("promoted_dominant")))
                foundWarning = true;
        QVERIFY(foundWarning);
    }

    void deterministic_and_rejects_invalid_requests() {
        const std::string observations = observationsJson(
            {10.5, 9.5, 30.5, 29.5, 10.0, 10.0, 30.0, 30.0});
        const std::string request = requestJson(contractJson(), observations, 7);
        const QString first = refineOrDie(request)
            .value("proposed_contract_sha256").toString();
        const QString second = refineOrDie(request)
            .value("proposed_contract_sha256").toString();
        QVERIFY(first.size() == 64);
        QCOMPARE(first, second);

        auto expectError = [](const std::string& bad, const char* label) {
            const CampaignRefinementOutput output = refineExperimentCampaign(bad);
            QVERIFY2(!output.ok, qPrintable(QString::fromLatin1(label)));
        };
        auto analysisStyleRequest = [&](const std::string& extra) {
            return std::string(R"JSON({"contract":)JSON") + contractJson()
                + R"JSON(,"observations":[],"seed":1,)JSON" + extra + "}";
        };
        expectError(request + "x", "trailing garbage");
        expectError(std::string(R"JSON({"contract":{"schema":"nope"},
            "observations":[],"seed":1})JSON"), "invalid contract");
        expectError(analysisStyleRequest("\"bogus\":true"), "unknown key");
        expectError(requestJson(contractJson(), observationsJson({}), 7),
                    "empty observations");
        expectError(analysisStyleRequest("\"threshold\":0"), "threshold zero");
        const std::string constantValues = observationsJson(
            {5, 5, 5, 5, 5, 5, 5, 5});
        expectError(requestJson(contractJson(), constantValues, 7),
                    "no signal to refine");
    }
};

int runTstExperimentRefinement(int argc, char** argv) {
    TstExperimentRefinement test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_experimentrefinement.moc"
