#include <QtTest/QtTest>

#include "CampaignPower.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <string>

using namespace Model;

namespace {

std::string contractJson(bool randomize) {
    return std::string(R"JSON({
      "schema":"dpa.experiment.campaign","version":1,
      "campaign_id":"camp-pwr",
      "hypothesis":"analysis_type 主效应的功效规划验证",
      "factors":[{"name":"analysis_type","levels":["FFT","PSD"]},
                 {"name":"average_num","levels":[1,4]}],
      "replicates":2,
      "randomization":{"enabled":)JSON")
        + (randomize ? "true" : "false") + R"JSON(,"seed":42},
      "run_template":{"duration_ms":1000},
      "policy":{"on_run_failure":"continue","max_runs":16}
    })JSON";
}

std::string requestJson(const std::string& contract, double effectRange,
                        double noiseStd, int seed,
                        const std::string& extra = std::string()) {
    std::string request = std::string(R"JSON({
      "contract":)JSON") + contract
        + R"JSON(,
      "factor":"analysis_type",
      "effect_range":)JSON" + std::to_string(effectRange)
        + R"JSON(,
      "noise_std":)JSON" + std::to_string(noiseStd)
        + R"JSON(,
      "seed":)JSON" + std::to_string(seed);
    if (!extra.empty()) request += "," + extra;
    request += "}";
    return request;
}

QJsonObject estimateOrDie(const std::string& request) {
    const CampaignPowerOutput output = estimateCampaignPower(request);
    if (!output.ok) return QJsonObject();
    return QJsonDocument::fromJson(QByteArray::fromStdString(
        output.resultJson)).object();
}

} // namespace

class TstCampaignPower : public QObject { Q_OBJECT
private slots:
    void strong_effect_yields_high_power_and_calibrated_null() {
        const QJsonObject result =
            estimateOrDie(requestJson(contractJson(false), 10.0, 1.0, 7));
        QVERIFY(!result.isEmpty());
        QCOMPARE(result.value("schema").toString(),
                 QString("dpa.experiment.campaign.power/v1"));
        QCOMPARE(result.value("planned_runs").toInt(), 8);
        QVERIFY(result.value("calibrated").toBool());
        // Perfect separation carries mirrored arrangements with identical BSS,
        // so the discrete p-floor sits near 2/C(8,4); power stays bounded below
        // one but must clear any conventional planning target comfortably.
        QVERIFY(result.value("estimated_power").toDouble() >= 0.90);
        QVERIFY(result.value("null_false_positive_rate").toDouble() <= 0.12);
    }

    void negligible_effect_in_noise_yields_low_power() {
        const QJsonObject result =
            estimateOrDie(requestJson(contractJson(false), 0.01, 10.0, 7));
        QVERIFY(!result.isEmpty());
        QVERIFY(result.value("estimated_power").toDouble() <= 0.35);
        QVERIFY(result.value("calibrated").toBool());
    }

    void randomized_design_is_supported_and_deterministic() {
        const std::string request = requestJson(contractJson(true), 10.0, 1.0, 99,
                                                R"("simulations":100)");
        const double first = estimateOrDie(request).value("estimated_power").toDouble();
        const double second = estimateOrDie(request).value("estimated_power").toDouble();
        QVERIFY(first >= 0.0 && first <= 1.0);
        QCOMPARE(first, second);
    }

    void rejects_invalid_requests() {
        auto expectError = [](const std::string& request, const char* label) {
            const CampaignPowerOutput output = estimateCampaignPower(request);
            QVERIFY2(!output.ok, qPrintable(QString::fromLatin1(label)));
        };
        const std::string goodContract = contractJson(false);
        auto noSeedRequest = [&](const std::string& factor, int seed) {
            std::string request = std::string("{\"contract\":") + goodContract
                + ",\"factor\":\"" + factor
                + "\",\"effect_range\":1,\"noise_std\":1";
            if (seed >= 0) request += ",\"seed\":" + std::to_string(seed);
            return request + "}";
        };
        expectError(requestJson(contractJson(false), 10.0, 1.0, 7) + "x",
                    "trailing garbage");
        expectError(std::string("{\"contract\":{\"schema\":\"nope\"},"
                                "\"factor\":\"analysis_type\",\"effect_range\":1,"
                                "\"noise_std\":1,\"seed\":1}"),
                    "invalid contract");
        std::string tampered = requestJson(contractJson(false), 10.0, 1.0, 7);
        tampered.insert(tampered.size() - 1, ",\"factor_name\":\"x\"");
        expectError(tampered, "unknown request key");
        expectError(requestJson(contractJson(false), -1.0, 1.0, 7),
                    "negative effect range");
        expectError(requestJson(contractJson(false), 0.0, 0.0, 7), "empty question");
        expectError(noSeedRequest("average_num", -1), "missing seed");
        expectError(noSeedRequest("speed_hz", 7), "factor unknown");
        expectError(requestJson(contractJson(false), 10.0, 1.0, 7, "\"alpha\":0"),
                    "alpha zero");
        expectError(requestJson(contractJson(false), 10.0, 1.0, 7,
                                "\"simulations\":49"), "simulations below floor");
        expectError(requestJson(contractJson(false), 10.0, 1.0, 7,
                                "\"permutations\":98"), "permutations below floor");
    }
};

int runTstCampaignPower(int argc, char** argv) {
    TstCampaignPower test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_campaignpower.moc"
