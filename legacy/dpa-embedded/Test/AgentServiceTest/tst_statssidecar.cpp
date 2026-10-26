#include <QtTest/QtTest>

#include "StatsSidecar.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QFile>
#include <string>

using namespace Model;

namespace {

std::string contractJson() {
    return R"JSON({
      "schema":"dpa.experiment.campaign","version":1,
      "campaign_id":"camp-sidecar",
      "hypothesis":"stats sidecar suggestion domain conformance",
      "factors":[{"name":"analysis_type","levels":["FFT","PSD"]},
                 {"name":"average_num","levels":[10,50]}],
      "replicates":2,
      "randomization":{"enabled":false,"seed":42},
      "run_template":{"duration_ms":1000,"frame_size":1024},
      "policy":{"on_run_failure":"abort","max_runs":16},
      "response":{"metric":"rms","unit":"g","direction":"minimize"}
    })JSON";
}

std::string requestJson(const std::string& observations, int seed) {
    return std::string(R"JSON({"contract":)JSON") + contractJson()
        + R"JSON(,"observations":)JSON" + observations
        + R"JSON(,"seed":)JSON" + std::to_string(seed) + "}";
}

} // namespace

class TstStatsSidecar : public QObject {
    Q_OBJECT

private slots:
    void knobDomainDescriptors();
    void suggestionDomainValidation();
    void transportFailsClosedWhenScriptMissing();
    void sidecarRoundTripWhenPythonAvailable();
    void suggestionDegradesHonestlyWithoutObservations();
    void suggestionUsesSidecarOrWarnsClearly();
    void suggestionRejectsOutOfRangeRunIndex();
    void sparseObservationsAreRemappedForSidecar();
    void incompleteSidecarLevelsAreRejected();
    void duplicateRunIndicesAreRejected();
};

void TstStatsSidecar::knobDomainDescriptors() {
    const QJsonObject numeric = QJsonDocument::fromJson(
        QByteArray::fromStdString(campaignKnobDomainJson("average_num"))).object();
    QCOMPARE(QString("linear_numeric"), numeric.value("kind").toString());
    QVERIFY(numeric.value("min").toDouble() > 0.0);
    QVERIFY(numeric.value("max").toDouble() >= 99.0);

    const QJsonObject ladder = QJsonDocument::fromJson(
        QByteArray::fromStdString(campaignKnobDomainJson("frame_size"))).object();
    QCOMPARE(QString("ordered_enum"), ladder.value("kind").toString());
    QVERIFY(ladder.value("ladder").toArray().size() >= 2);

    const QJsonObject categorical = QJsonDocument::fromJson(
        QByteArray::fromStdString(campaignKnobDomainJson("analysis_type"))).object();
    QCOMPARE(QString("categorical"), categorical.value("kind").toString());
    QVERIFY(categorical.value("levels").toArray().contains(QLatin1String("FFT")));

    QVERIFY(campaignKnobDomainJson("not_a_real_knob").empty());
}

void TstStatsSidecar::suggestionDomainValidation() {
    QVERIFY(campaignSuggestionWithinDomain("average_num", QJsonValue(50.0)));
    QVERIFY(!campaignSuggestionWithinDomain("average_num", QJsonValue(100.0)));
    QVERIFY(!campaignSuggestionWithinDomain("average_num",
                                            QJsonValue(QStringLiteral("50"))));
    QVERIFY(campaignSuggestionWithinDomain("frame_size", QJsonValue(2048.0)));
    QVERIFY(!campaignSuggestionWithinDomain("frame_size", QJsonValue(3000.0)));
    QVERIFY(campaignSuggestionWithinDomain("analysis_type",
                                           QJsonValue(QStringLiteral("FFT"))));
    QVERIFY(!campaignSuggestionWithinDomain("analysis_type",
                                            QJsonValue(QStringLiteral("WELCH"))));
    QVERIFY(!campaignSuggestionWithinDomain("analysis_type", QJsonValue(42.0)));
}

void TstStatsSidecar::transportFailsClosedWhenScriptMissing() {
    qputenv("DPA_STATS_SIDECAR_SCRIPT", "Z:/definitely/missing/sidecar.py");
    const StatsSidecarOutput output =
        runStatsSidecarRequest(R"({"method":"nope"})");
    QVERIFY(!output.ok);
    QCOMPARE(QString::fromStdString(output.error),
             QString("stats_sidecar_script_missing"));
    qunsetenv("DPA_STATS_SIDECAR_SCRIPT");
}

void TstStatsSidecar::sidecarRoundTripWhenPythonAvailable() {
    if (QStandardPaths::findExecutable(QStringLiteral("python")).isEmpty())
        QSKIP("python is not available on this machine");
    const StatsSidecarOutput boundary = runStatsSidecarRequest(
        R"({"method":"seq_boundary","design":"obrien_fleming",)"
        R"("max_interims":3,"alpha":0.05})");
    QVERIFY(boundary.ok);
    const QJsonObject response =
        QJsonDocument::fromJson(QByteArray::fromStdString(
            boundary.resultJson)).object();
    const QJsonArray entries = response.value("boundary_p").toArray();
    QCOMPARE(entries.size(), 3);
    double previousZ = 1e9;
    for (const QJsonValue& entry : entries) {
        const double z = entry.toObject().value("z").toDouble();
        QVERIFY(z < previousZ);
        previousZ = z;
    }
    QVERIFY(!response.value("source").toString().isEmpty());

    const StatsSidecarOutput unknownMethod = runStatsSidecarRequest(
        R"({"method":"teleport"})");
    QVERIFY(!unknownMethod.ok);
    QCOMPARE(QString::fromStdString(unknownMethod.error),
             QString("unknown_method"));
}

void TstStatsSidecar::suggestionDegradesHonestlyWithoutObservations() {
    const StatsSuggestionOutput output =
        buildExperimentRefinementSuggestion(requestJson("[]", 7));
    QVERIFY(output.ok);
    const QJsonObject payload = QJsonDocument::fromJson(
        QByteArray::fromStdString(output.resultJson)).object();
    QCOMPARE(payload.value("source").toString(),
             QLatin1String("builtin"));
    QCOMPARE(payload.value("surrogate").toString(), QLatin1String("builtin_grid"));
    QCOMPARE(payload.value("objective_direction").toString(),
             QLatin1String("minimize"));
    const QJsonArray warnings = payload.value("warnings").toArray();
    bool foundGridWarning = false;
    for (const QJsonValue& warning : warnings)
        if (warning.toString().contains(
                QLatin1String("no_observations_deterministic_grid")))
            foundGridWarning = true;
    QVERIFY(foundGridWarning);

    const QJsonObject levels = payload.value("next_levels").toObject();
    QCOMPARE(levels.value("average_num").toInt(), 50);
    QCOMPARE(levels.value("analysis_type").toString(),
             QStringLiteral("RMSPRO"));
}

void TstStatsSidecar::suggestionUsesSidecarOrWarnsClearly() {
    const std::string observations =
        R"([{"run_index":0,"value":3.5},{"run_index":1,"value":9.25},)"
        R"({"run_index":2,"value":4.0},{"run_index":3,"value":12.75},)"
        R"({"run_index":4,"value":3.0},{"run_index":5,"value":8.5}])";
    const StatsSuggestionOutput output =
        buildExperimentRefinementSuggestion(requestJson(observations, 99));
    QVERIFY(output.ok);
    const QJsonObject payload = QJsonDocument::fromJson(
        QByteArray::fromStdString(output.resultJson)).object();

    const QString surrogate = payload.value("surrogate").toString();
    QCOMPARE(payload.value("objective_direction").toString(),
             QLatin1String("minimize"));
    if (surrogate == QLatin1String("gp_matern52")) {
        QCOMPARE(payload.value("source").toString(),
                 QLatin1String("stats_sidecar"));
        QCOMPARE(payload.value("warnings").toArray().size(), 0);
    } else {
        QCOMPARE(surrogate, QLatin1String("builtin_grid"));
        QCOMPARE(payload.value("source").toString(), QLatin1String("builtin"));
        bool foundUnavailableWarning = false;
        for (const QJsonValue& warning : payload.value("warnings").toArray())
            if (warning.toString().contains(
                    QLatin1String("stats_sidecar_unavailable")))
                foundUnavailableWarning = true;
        QVERIFY(foundUnavailableWarning);
    }

    // Whatever produced the levels, they must sit inside the legal domains.
    const QJsonObject levels = payload.value("next_levels").toObject();
    QVERIFY(campaignSuggestionWithinDomain("average_num",
                                           levels.value("average_num")));
    QVERIFY(campaignSuggestionWithinDomain("analysis_type",
                                           levels.value("analysis_type")));
}

void TstStatsSidecar::suggestionRejectsOutOfRangeRunIndex() {
    const StatsSuggestionOutput output = buildExperimentRefinementSuggestion(
        requestJson(R"([{"run_index":999,"value":1.0}])", 1));
    QVERIFY(!output.ok);
    QCOMPARE(QString::fromStdString(output.error),
             QString("run_index_out_of_range"));
}

void TstStatsSidecar::sparseObservationsAreRemappedForSidecar() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath("pairing_fixture.py");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("import json,sys\n"
               "r=json.load(open(sys.argv[1],encoding='utf-8-sig'))\n"
               "assert [o['run_index'] for o in r['observations']]==[0,1]\n"
               "assert [o['value'] for o in r['observations']]==[4,9]\n"
               "assert len(r['design'])==2\n"
               "print(json.dumps({'ok':True,'direction':r['direction'],"
               "'next_levels':r['design'][0],'surrogate':'gp_matern52'}))\n");
    file.close();
    const QByteArray saved = qgetenv("DPA_STATS_SIDECAR_SCRIPT");
    qputenv("DPA_STATS_SIDECAR_SCRIPT", path.toLocal8Bit());
    const auto output = buildExperimentRefinementSuggestion(
        requestJson(R"([{"run_index":3,"value":4},{"run_index":7,"value":9}])", 7));
    if (saved.isNull()) qunsetenv("DPA_STATS_SIDECAR_SCRIPT");
    else qputenv("DPA_STATS_SIDECAR_SCRIPT", saved);
    QVERIFY(output.ok);
    const auto payload = QJsonDocument::fromJson(QByteArray::fromStdString(output.resultJson)).object();
    QCOMPARE(payload.value("source").toString(), QString("stats_sidecar"));
    QCOMPARE(payload.value("paired_observations").toInt(), 2);
}

void TstStatsSidecar::incompleteSidecarLevelsAreRejected() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath("incomplete_fixture.py");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("import json\nprint(json.dumps({'ok':True,'direction':'minimize',"
               "'next_levels':{'average_num':50},'surrogate':'gp_matern52'}))\n");
    file.close();
    const QByteArray saved = qgetenv("DPA_STATS_SIDECAR_SCRIPT");
    qputenv("DPA_STATS_SIDECAR_SCRIPT", path.toLocal8Bit());
    const auto output = buildExperimentRefinementSuggestion(
        requestJson(R"([{"run_index":0,"value":4},{"run_index":1,"value":9}])", 7));
    if (saved.isNull()) qunsetenv("DPA_STATS_SIDECAR_SCRIPT");
    else qputenv("DPA_STATS_SIDECAR_SCRIPT", saved);
    QVERIFY(output.ok);
    const auto payload = QJsonDocument::fromJson(QByteArray::fromStdString(output.resultJson)).object();
    QCOMPARE(payload.value("source").toString(), QString("builtin"));
    QCOMPARE(payload.value("next_levels").toObject().size(), 2);
}

void TstStatsSidecar::duplicateRunIndicesAreRejected() {
    const auto output = buildExperimentRefinementSuggestion(
        requestJson(R"([{"run_index":0,"value":4},{"run_index":0,"value":9}])", 7));
    QVERIFY(!output.ok);
    QCOMPARE(QString::fromStdString(output.error), QString("duplicate_run_index"));
}

#include "tst_statssidecar.moc"

int runTstStatsSidecar(int argc, char** argv) {
    TstStatsSidecar test;
    return QTest::qExec(&test, argc, argv);
}
