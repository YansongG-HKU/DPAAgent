#include <QtTest/QtTest>

#include "ExperimentKnowledge.h"
#include "AgentUiParse.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <string>

using namespace Model;

namespace {

std::string analysisResult() {
    return std::string(R"JSON({
      "schema":"dpa.experiment.campaign.analysis/v1",
      "campaign_id":"camp-k","campaign_sha256":")JSON")
        + std::string(64, 'a') + R"JSON(",
      "analyzed_runs":8,"planned_runs":8,"coverage_ratio":1.0,
      "seed":7,"permutations":999,"grand_mean":16.0,
      "residual_std":0.5,"residual_estimable":true,
      "best_cell_index":1,"worst_cell_index":0,
      "factors":[
        {"name":"analysis_type","effect_range":20.0,"perm_p":0.001,"testable":true,"levels":[]},
        {"name":"average_num","effect_range":0.5,"perm_p":0.4,"testable":true,"levels":[]}],
      "cells":[
        {"index":0,"levels":{"analysis_type":"FFT","average_num":1},"mean":10.0,"count":2},
        {"index":3,"levels":{"analysis_type":"PSD","average_num":4},"mean":22.0,"count":2}]
    })JSON";
}

std::string campaignResult() {
    return R"JSON({
      "schema":"dpa.experiment.campaign.result/v1",
      "campaign_id":"camp-r","status":"aborted","stop_reason":"daq_start_failed",
      "planned_runs":8,"runs_ok":1,"runs_failed":1,
      "runs":[
        {"run_index":0,"design_index":2,"levels":{"analysis_type":"PSD"},
         "ok":true,"error_code":"","run_id":"rid-1","evidence_artifact_id":"ev-1"},
        {"run_index":1,"design_index":3,"levels":{"analysis_type":"PSD"},
         "ok":false,"error_code":"daq_start_failed","run_id":"","evidence_artifact_id":""}
      ]
    })JSON";
}

} // namespace

class TstExperimentKnowledge : public QObject { Q_OBJECT
private slots:
    void builds_entry_from_analysis_and_round_trips() {
        const KnowledgeBuildOutput built =
            buildKnowledgeEntry(analysisResult(), "artifact_1", 1234567890);
        QVERIFY2(built.ok, qPrintable(QString::fromStdString(built.error)));
        const KnowledgeEntryParse parsed = parseKnowledgeEntry(built.entryJson);
        QVERIFY2(parsed.ok, qPrintable(QString::fromStdString(parsed.error)));
        QCOMPARE(parsed.entry.recordedAtMs, 1234567890LL);
        QCOMPARE(QString::fromStdString(parsed.entry.campaignId), QString("camp-k"));
        QCOMPARE(parsed.entry.residualStd, 0.5);
        QCOMPARE(parsed.entry.residualEstimable, true);
        QCOMPARE(QString::fromStdString(parsed.entry.sourceArtifactId),
                 QString("artifact_1"));
        QVERIFY(parsed.entry.factorsJson.find("\"effect_range\":20") != std::string::npos);
        QVERIFY(parsed.entry.bestCellJson.find("\"mean\":22") != std::string::npos);
        // Round-trip stability: reparsing the canonical form yields identity.
        const KnowledgeEntryParse again = parseKnowledgeEntry(parsed.entry.canonicalJson);
        QVERIFY(again.ok);
        QCOMPARE(again.entry.canonicalJson, parsed.entry.canonicalJson);
    }

    void rejects_sources_that_are_not_verified_analyses() {
        auto expectError = [](const std::string& source, const char* label) {
            const KnowledgeBuildOutput output =
                buildKnowledgeEntry(source, "artifact", 1);
            QVERIFY2(!output.ok, qPrintable(QString::fromLatin1(label)));
        };
        auto analysisWith = [](const std::string& factorsField) {
            return std::string("{\"schema\":\"dpa.experiment.campaign.analysis/v1\",")
                + "\"campaign_id\":\"c\",\"campaign_sha256\":\"" + std::string(64, 'b')
                + "\",\"cells\":[],\"best_cell_index\":-1," + factorsField + "}";
        };
        expectError("not json", "garbage");
        expectError(std::string(R"JSON({"schema":"dpa.experiment.status/v1"})JSON"),
                    "wrong schema");
        expectError(analysisWith("\"factors\":\"not-array\""), "missing fields");
    }

    void parses_campaign_run_rows_for_the_ui_panel() {
        UiCampaignSnapshot snapshot;
        QVERIFY(parseCampaignRunRows(campaignResult(), snapshot));
        QCOMPARE(QString::fromStdString(snapshot.status), QString("aborted"));
        QCOMPARE(snapshot.plannedRuns, 8);
        QVERIFY(snapshot.runs.size() == 2);
        QVERIFY(snapshot.runs.at(0).ok);
        QCOMPARE(QString::fromStdString(snapshot.runs.at(0).evidenceId),
                 QString("ev-1"));
        QVERIFY(!snapshot.runs.at(1).ok);
        QCOMPARE(QString::fromStdString(snapshot.runs.at(1).errorCode),
                 QString("daq_start_failed"));
        QVERIFY(!snapshot.runs.at(1).evidenceId.empty() == false);

        // Tolerates the ToolResult envelope and rejects foreign payloads.
        UiCampaignSnapshot enveloped;
        QVERIFY(parseCampaignRunRows(
            std::string(R"JSON({"ok":true,"data":)JSON") + campaignResult() + "}",
            enveloped));
        QVERIFY(enveloped.runs.size() == snapshot.runs.size());
        UiCampaignSnapshot rejected;
        QVERIFY(!parseCampaignRunRows(std::string(R"JSON({"schema":"x"})JSON"), rejected));
        QVERIFY(!parseCampaignRunRows("garbage", rejected));
    }

    void parses_interim_checkpoint_hint() {
        UiCheckpointHint hint;
        QVERIFY(parseCheckpointHint(campaignResult(), hint));
        QVERIFY(!hint.awaiting);
        const std::string paused =
            std::string("{\"schema\":\"dpa.experiment.campaign.result/v1\",")
            + "\"campaign_sha256\":\"" + std::string(64, 'd') + "\","
              "\"status\":\"paused_interim\",\"stop_reason\":\"interim_checkpoint\","
              "\"awaiting_interim\":true,\"checkpoint_artifact_id\":\"cp_1\","
              "\"planned_runs\":8,\"runs_ok\":2,\"runs_failed\":0,"
              "\"runs\":[{\"run_index\":0,\"ok\":true},{\"run_index\":1,\"ok\":true}]}";
        QVERIFY(parseCheckpointHint(paused, hint));
        QVERIFY(hint.awaiting);
        QCOMPARE(hint.completedRuns, 2);
        QCOMPARE(hint.plannedRuns, 8);
        QCOMPARE(QString::fromStdString(hint.checkpointArtifactId), QString("cp_1"));
        QVERIFY(hint.campaignSha256.size() == 64);
        QVERIFY(!parseCheckpointHint("garbage", hint));
    }

    void parses_refinement_proposals_for_the_authorization_card() {
        // Escaped strings instead of raw strings: moc's lexer is stricter
        // than MSVC and skipped the class when raw strings appeared here.
        const std::string sha(64, 'c');
        const std::string payload =
            std::string("{\"schema\":\"dpa.experiment.campaign.refinement/v1\",")
            + "\"source_campaign_id\":\"camp-k\","
              "\"proposed_campaign_id\":\"camp-k-r2\","
              "\"proposed_contract_sha256\":\"" + sha + "\","
              "\"refinement_artifact_id\":\"artifact_9\","
              "\"planned_runs\":8,\"replicates\":4,"
              "\"power\":{\"estimated_power\":0.97,\"calibrated\":true},"
              "\"warnings\":[],"
              "\"factor_assessment\":["
                "{\"name\":\"analysis_type\",\"action\":\"refined\",\"snr\":56.0,"
                 "\"effect_range\":20.0,"
                 "\"best_level\":{\"value\":\"PSD\",\"mean\":30.0,\"count\":4}},"
                "{\"name\":\"average_num\",\"action\":\"fixed_at_best\",\"snr\":1.4,"
                 "\"effect_range\":0.5,"
                 "\"best_level\":{\"value\":1,\"mean\":20.25,\"count\":4}}]}";
        UiRefinementProposal proposal;
        QVERIFY(parseRefinementProposal(payload, proposal));
        QCOMPARE(QString::fromStdString(proposal.sourceCampaignId), QString("camp-k"));
        QCOMPARE(QString::fromStdString(proposal.proposedCampaignId),
                 QString("camp-k-r2"));
        QVERIFY(proposal.proposedSha256.size() == 64);
        QCOMPARE(QString::fromStdString(proposal.refinementArtifactId),
                 QString("artifact_9"));
        QCOMPARE(proposal.plannedRuns, 8);
        QCOMPARE(proposal.replicates, 4);
        QVERIFY(proposal.hasPower);
        QCOMPARE(proposal.power, 0.97);
        QCOMPARE(proposal.powerCalibrated, true);
        QVERIFY(proposal.refinedFactors.size() == 1
                && proposal.refinedFactors.at(0) == "analysis_type");
        QVERIFY(proposal.fixedFactors.size() == 1
                && proposal.fixedFactors.at(0) == "average_num");

        // Envelope tolerance and rejection of foreign payloads. Raw strings
        // stay outside QTest macros (MSVC traditional preprocessor quirk).
        const std::string envelope =
            std::string(R"JSON({"ok":true,"data":)JSON") + payload + "}";
        UiRefinementProposal enveloped;
        QVERIFY(parseRefinementProposal(envelope, enveloped));
        QVERIFY(enveloped.plannedRuns == 8);
        UiRefinementProposal rejected;
        QVERIFY(!parseRefinementProposal(
            std::string(R"JSON({"schema":"x"})JSON"), rejected));
    }

    void parses_knowledge_rows_with_dominant_factor_selection() {
        const KnowledgeBuildOutput first =
            buildKnowledgeEntry(analysisResult(), "artifact_1", 1000);
        QVERIFY(first.ok);
        const QString listPayload = QString(R"JSON({
          "schema":"dpa.experiment.knowledge.list/v1",
          "total_entries":1,"corrupt_lines":0,"returned":1,
          "entries":[%1]})JSON").arg(QString::fromStdString(first.entryJson));
        std::vector<UiKnowledgeRow> rows;
        QVERIFY(parseKnowledgeRows(listPayload.toStdString(), rows));
        QVERIFY(rows.size() == 1);
        QCOMPARE(QString::fromStdString(rows.at(0).dominantFactor),
                 QString("analysis_type"));
        QCOMPARE(rows.at(0).effectRange, 20.0);
        QCOMPARE(rows.at(0).permP, 0.001);
        QVERIFY(rows.at(0).testable);
        QVERIFY(rows.at(0).bestLevels.find("\"average_num\"") != std::string::npos);

        // Works through the ToolResult envelope too.
        std::vector<UiKnowledgeRow> enveloped;
        QVERIFY(parseKnowledgeRows(
            std::string(R"JSON({"ok":true,"data":)JSON")
                + listPayload.toStdString() + "}", enveloped));
        QVERIFY(enveloped.size() == 1);
        std::vector<UiKnowledgeRow> rejected;
        QVERIFY(!parseKnowledgeRows("garbage", rejected));
        QVERIFY(rejected.empty());
    }
};

int runTstExperimentKnowledge(int argc, char** argv) {
    TstExperimentKnowledge test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_experimentknowledge.moc"
