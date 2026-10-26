#include <QtTest/QtTest>

#include "ExperimentCampaign.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <set>
#include <string>
#include <vector>

using namespace Model;

namespace {

int outcomeCounter = 0;

std::string baseContract() {
    return R"JSON({
      "schema":"dpa.experiment.campaign","version":1,
      "campaign_id":"camp-1",
      "hypothesis":"analysis_type 与 average_num 共同影响频谱幅值稳定性",
      "factors":[{"name":"analysis_type","levels":["FFT","PSD"]},
                 {"name":"average_num","levels":[1,4]}],
      "replicates":2,
      "randomization":{"enabled":false,"seed":42},
      "run_template":{"duration_ms":1000,"frame_size":1024},
      "policy":{"on_run_failure":"abort","max_runs":16}
    })JSON";
}

std::string withSeed(int seed) {
    return std::string(R"JSON({
      "schema":"dpa.experiment.campaign","version":1,
      "campaign_id":"camp-rand",
      "hypothesis":"随机化顺序下重复同一因子设计以检验可复现性",
      "factors":[{"name":"analysis_type","levels":["FFT","PSD"]},
                 {"name":"average_num","levels":[1,4]}],
      "replicates":2,
      "randomization":{"enabled":true,"seed":)JSON")
        + std::to_string(seed) + R"JSON(},
      "run_template":{"duration_ms":1000},
      "policy":{"on_run_failure":"abort","max_runs":16}
    })JSON";
}

std::string baseObjective() {
    return R"JSON({
      "schema":"dpa.experiment.objective","version":1,
      "objective_id":"objective-rms-stability",
      "context_fingerprint":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "tool_catalog_sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
      "hypothesis":"analysis_type 与 average_num 影响 RMS 稳定性",
      "response":{"metric":"rms","unit":"g","direction":"minimize"},
      "constraints":{"fixed_plan":{"duration_ms":1000,"frame_size":1024},
                     "randomize":true,"min_replicates":2},
      "allowed_factors":[{"name":"analysis_type","levels":["FFT","PSD"]},
                         {"name":"average_num","levels":[1,4]}],
      "budget":{"max_runs":16,"max_run_duration_ms":2000,
                "max_total_duration_ms":16000},
      "stop_conditions":{"stop_on_warning":true,"on_run_failure":"abort",
                         "max_failed_runs":0,"interim_after":0},
      "evidence_requirements":{"save_raw_data":true,
        "create_per_run_evidence":true,"verify_current_catalog":true,
        "auto_analyze":true,"auto_refine":true}
    })JSON";
}

std::string objectiveBoundContract(const std::string& objectiveSha256,
                                   int replicates = 2) {
    return std::string(R"JSON({
      "schema":"dpa.experiment.campaign","version":1,
      "objective_sha256":")JSON") + objectiveSha256 + R"JSON(",
      "campaign_id":"camp-objective-rms",
      "hypothesis":"analysis_type 与 average_num 影响 RMS 稳定性",
      "factors":[{"name":"analysis_type","levels":["FFT","PSD"]},
                 {"name":"average_num","levels":[1,4]}],
      "replicates":)JSON" + std::to_string(replicates) + R"JSON(,
      "randomization":{"enabled":true,"seed":42},
      "run_template":{"duration_ms":1000,"frame_size":1024,
                      "stop_on_warning":true},
      "policy":{"on_run_failure":"abort","max_runs":16,
                "max_failed_runs":0,"interim_after":0},
      "response":{"metric":"rms","unit":"g","direction":"minimize"}
    })JSON";
}

std::string replaceFirst(std::string text, const std::string& from, const std::string& to) {
    const size_t position = text.find(from);
    if (position != std::string::npos)
        text.replace(position, from.size(), to);
    return text;
}

CampaignRunOutcome goodOutcome() {
    CampaignRunOutcome outcome;
    outcome.ok = true;
    ++outcomeCounter;
    outcome.runId = "run-" + std::to_string(outcomeCounter);
    outcome.evidenceArtifactId = "evidence_artifact_" + std::to_string(outcomeCounter);
    return outcome;
}

QJsonObject planAt(const ExperimentCampaign& campaign, int position) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(
        campaign.runPlanJsons[static_cast<size_t>(position)])).object();
}

} // namespace

class TstExperimentCampaign : public QObject { Q_OBJECT
private slots:
    void validates_and_expands_full_factorial_in_replicate_blocks() {
        const CampaignValidation validation = parseAndValidateExperimentCampaign(baseContract());
        QVERIFY2(validation.ok, qPrintable(QString::fromStdString(validation.error)));
        const ExperimentCampaign& campaign = validation.campaign;
        QVERIFY(campaign.runPlanJsons.size() == 8);
        QVERIFY(campaign.designLevelsJson.size() == 4);
        const std::vector<int> expected{0, 1, 2, 3, 0, 1, 2, 3};
        QVERIFY(campaign.designIndices == expected);
        const QJsonObject first = planAt(campaign, 0);
        QCOMPARE(first.value("analysis_type").toString(), QString("FFT"));
        QCOMPARE(first.value("average_num").toInt(), 1);
        QCOMPARE(first.value("duration_ms").toInt(), 1000);
        QCOMPARE(first.value("frame_size").toInt(), 1024);
        QCOMPARE(planAt(campaign, 1).value("average_num").toInt(), 4);
        QCOMPARE(planAt(campaign, 2).value("analysis_type").toString(), QString("PSD"));
        QCOMPARE(planAt(campaign, 3).value("analysis_type").toString(), QString("PSD"));
        QCOMPARE(planAt(campaign, 3).value("average_num").toInt(), 4);
        QVERIFY(campaign.campaignSha256.size() == 64);
    }

    void rejects_malformed_or_unbounded_contracts() {
        auto expectRejected = [](const std::string& json, const char* label) {
            const CampaignValidation validation = parseAndValidateExperimentCampaign(json);
            QVERIFY2(!validation.ok, qPrintable(QString::fromLatin1(label)));
        };
        expectRejected(replaceFirst(baseContract(),
            "\"dpa.experiment.campaign\"", "\"dpa.other\""), "wrong schema");
        expectRejected(replaceFirst(baseContract(),
            "\"factors\":[{\"name\":\"analysis_type\"",
            "\"factors\":[{\"name\":\"speed_hz\""), "unknown override key");
        expectRejected(replaceFirst(baseContract(),
            "\"replicates\":2", "\"replicates\":9"), "replicates unbounded");
        expectRejected(replaceFirst(baseContract(),
            "\"max_runs\":16", "\"max_runs\":4"), "max_runs below design");
        expectRejected(replaceFirst(baseContract(),
            "\"levels\":[1,4]", "\"levels\":[1,1]"), "duplicate level values");
        expectRejected(replaceFirst(baseContract(),
            "\"levels\":[\"FFT\",\"PSD\"]", "\"levels\":[\"FFT\",\"XXX\"]"),
            "level outside domain");
        expectRejected(replaceFirst(baseContract(),
            "\"duration_ms\":1000,\"frame_size\":1024}",
            "\"duration_ms\":1000,\"frame_size\":1024,\"create_evidence\":false}"),
            "evidence binding disabled");
        expectRejected(replaceFirst(baseContract(),
            "\"run_template\":{\"duration_ms\":1000,\"frame_size\":1024},",
            "\"run_template\":{\"frame_size\":1024},"), "required key uncovered");
        expectRejected(baseContract() + ",", "trailing garbage");
    }

    void seeded_randomization_is_reproducible_and_preserves_the_multiset() {
        const CampaignValidation first = parseAndValidateExperimentCampaign(withSeed(42));
        const CampaignValidation second = parseAndValidateExperimentCampaign(withSeed(42));
        const CampaignValidation other = parseAndValidateExperimentCampaign(withSeed(43));
        QVERIFY(first.ok && second.ok && other.ok);
        QVERIFY(first.campaign.runPlanJsons == second.campaign.runPlanJsons);
        QVERIFY(!(first.campaign.runPlanJsons == other.campaign.runPlanJsons));

        auto designs = [](const ExperimentCampaign& campaign) {
            std::multiset<int> values;
            for (const int design : campaign.designIndices) values.insert(design);
            return values;
        };
        QVERIFY(designs(first.campaign) == designs(other.campaign));
        std::set<int> positions;
        for (const int design : first.campaign.designIndices) positions.insert(design);
        QVERIFY(positions.size() == 4);
    }

    void driver_requires_evidence_per_successful_run() {
        const CampaignValidation validation = parseAndValidateExperimentCampaign(baseContract());
        QVERIFY(validation.ok);
        int calls = 0;
        const CampaignExecution execution = executeExperimentCampaign(
            validation.campaign, std::string(),
            [&](int, int, const std::string&) {
                ++calls;
                CampaignRunOutcome outcome;
                outcome.ok = true;
                outcome.runId = "run-without-evidence";
                return outcome;
            },
            []() { return false; });
        QVERIFY(!execution.ok);
        QVERIFY(calls == 1);
        QCOMPARE(QString::fromStdString(execution.state.status), QString("aborted"));
        QCOMPARE(QString::fromStdString(execution.state.stopReason),
                 QString("evidence_missing"));
        QVERIFY(execution.state.records.size() == 1);
        QVERIFY(!execution.state.records.at(0).ok);
    }

    void driver_requires_run_id_per_successful_run() {
        const CampaignValidation validation = parseAndValidateExperimentCampaign(baseContract());
        QVERIFY(validation.ok);
        const CampaignExecution execution = executeExperimentCampaign(
            validation.campaign, std::string(),
            [](int, int, const std::string&) {
                CampaignRunOutcome outcome;
                outcome.ok = true;
                outcome.evidenceArtifactId = "evidence-without-run";
                return outcome;
            },
            []() { return false; });
        QVERIFY(!execution.ok);
        QCOMPARE(QString::fromStdString(execution.state.stopReason),
                 QString("run_id_missing"));
    }

    void response_declared_campaign_requires_derived_response() {
        const std::string json = replaceFirst(baseContract(), "\"run_template\":",
            "\"response\":{\"metric\":\"rms\"},\"run_template\":");
        const CampaignValidation validation = parseAndValidateExperimentCampaign(json);
        QVERIFY(validation.ok);
        const CampaignExecution missing = executeExperimentCampaign(
            validation.campaign, std::string(),
            [](int, int, const std::string&) { return goodOutcome(); },
            []() { return false; });
        QVERIFY(!missing.ok);
        QCOMPARE(QString::fromStdString(missing.state.stopReason),
                 QString("response_missing"));

        outcomeCounter = 0;
        const CampaignExecution complete = executeExperimentCampaign(
            validation.campaign, std::string(),
            [](int, int, const std::string&) {
                CampaignRunOutcome outcome = goodOutcome();
                outcome.responsePresent = true;
                outcome.responseValue = 1.25;
                outcome.responseChannelId = 7;
                outcome.responseSampleCount = 2;
                outcome.responseUnit = "g";
                outcome.responseSource = "monitoring_window_mean";
                outcome.responseEvidenceSha256 = std::string(64, 'a');
                return outcome;
            },
            []() { return false; });
        QVERIFY(complete.ok);
        const CampaignStateValidation roundTrip = parseCampaignState(
            serializeCampaignState(complete.state));
        QVERIFY2(roundTrip.ok, qPrintable(QString::fromStdString(roundTrip.error)));
        QVERIFY(roundTrip.state.records.front().responsePresent);
        QCOMPARE(roundTrip.state.records.front().responseValue, 1.25);
    }

    void driver_honors_abort_continue_and_cancel_policies() {
        auto contractWithPolicy = [](const char* policy) {
            return replaceFirst(baseContract(), "\"on_run_failure\":\"abort\"", policy);
        };
        int calls = 0;
        auto failingExecutor = [&](int runIndex, int, const std::string&) {
            ++calls;
            if (runIndex == 1) {
                CampaignRunOutcome outcome;
                outcome.errorCode = "daq_start_failed";
                return outcome;
            }
            return goodOutcome();
        };

        const CampaignValidation abortContract =
            parseAndValidateExperimentCampaign(
                contractWithPolicy("\"on_run_failure\":\"abort\""));
        QVERIFY(abortContract.ok);
        calls = 0;
        const CampaignExecution aborted = executeExperimentCampaign(
            abortContract.campaign, std::string(), failingExecutor, []() { return false; });
        QVERIFY(!aborted.ok);
        QVERIFY(calls == 2);
        QVERIFY(aborted.state.records.size() == 2);
        QCOMPARE(QString::fromStdString(aborted.state.stopReason), QString("daq_start_failed"));

        const CampaignValidation continueContract =
            parseAndValidateExperimentCampaign(
                contractWithPolicy("\"on_run_failure\":\"continue\""));
        QVERIFY(continueContract.ok);
        calls = 0;
        const CampaignExecution continued = executeExperimentCampaign(
            continueContract.campaign, std::string(), failingExecutor, []() { return false; });
        // Continue policy finishes the full randomized design; the failed run
        // stays recorded as missing data instead of truncating the experiment.
        QVERIFY(continued.ok);
        QVERIFY(calls == 8);
        QVERIFY(continued.state.records.size() == 8);
        QVERIFY(!continued.state.records.at(1).ok);
        QCOMPARE(QString::fromStdString(continued.state.status), QString("completed"));

        calls = 0;
        const CampaignExecution cancelled = executeExperimentCampaign(
            continueContract.campaign, std::string(),
            [&](int runIndex, int, const std::string&) {
                ++calls;
                if (runIndex == 0) {
                    CampaignRunOutcome outcome;
                    outcome.errorCode = "cancelled";
                    return outcome;
                }
                return goodOutcome();
            },
            []() { return false; });
        QVERIFY(calls == 1);
        QCOMPARE(QString::fromStdString(cancelled.state.stopReason), QString("cancelled"));
    }

    void max_failed_runs_policy_stops_after_threshold() {
        std::string json = baseContract();
        json = replaceFirst(json, "\"on_run_failure\":\"abort\"",
                            "\"on_run_failure\":\"continue\",\"max_failed_runs\":1");
        const CampaignValidation validation = parseAndValidateExperimentCampaign(json);
        QVERIFY2(validation.ok,
                 qPrintable(QString::fromStdString(validation.error)));
        int calls = 0;
        const CampaignExecution execution = executeExperimentCampaign(
            validation.campaign, std::string(),
            [&](int runIndex, int, const std::string&) {
                ++calls;
                if (runIndex == 1) {
                    CampaignRunOutcome outcome;
                    outcome.errorCode = "daq_start_failed";
                    return outcome;
                }
                return goodOutcome();
            },
            []() { return false; });
        QVERIFY(!execution.ok);
        QVERIFY(calls == 2);
        QVERIFY(execution.state.records.size() == 2);
        QCOMPARE(QString::fromStdString(execution.state.stopReason),
                 QString("max_failed_runs_reached"));
    }

    void rejects_unbounded_max_failed_runs_policy() {
        // Zero disables the cap (the canonical form emits it for omitted keys).
        const std::string zero = replaceFirst(baseContract(),
            "\"on_run_failure\":\"abort\"",
            "\"on_run_failure\":\"abort\",\"max_failed_runs\":0");
        QVERIFY(parseAndValidateExperimentCampaign(zero).ok);
        const std::string over = replaceFirst(baseContract(),
            "\"on_run_failure\":\"abort\"",
            "\"on_run_failure\":\"abort\",\"max_failed_runs\":65");
        QVERIFY(!parseAndValidateExperimentCampaign(over).ok);
    }

    void interim_checkpoint_pauses_then_resumes_once() {
        const std::string json = replaceFirst(baseContract(), "\"max_runs\":16",
            "\"max_runs\":16,\"interim_after\":2");
        const CampaignValidation validation = parseAndValidateExperimentCampaign(json);
        QVERIFY2(validation.ok, qPrintable(QString::fromStdString(validation.error)));
        int calls = 0;
        const CampaignExecution partial = executeExperimentCampaign(
            validation.campaign, std::string(),
            [&](int, int, const std::string&) { ++calls; return goodOutcome(); },
            []() { return false; });
        QVERIFY(calls == 2);
        QVERIFY(partial.awaitingInterim);
        QCOMPARE(QString::fromStdString(partial.state.status), QString("paused_interim"));
        QCOMPARE(QString::fromStdString(partial.state.stopReason),
                 QString("interim_checkpoint"));
        const std::string stateJson = serializeCampaignState(partial.state);
        QVERIFY(parseCampaignState(stateJson).ok);

        std::vector<int> resumedRuns;
        const CampaignExecution resumed = executeExperimentCampaign(
            validation.campaign, stateJson,
            [&](int runIndex, int, const std::string&) {
                resumedRuns.push_back(runIndex);
                return goodOutcome();
            },
            []() { return false; });
        QVERIFY(resumed.ok);
        QVERIFY(resumedRuns == (std::vector<int>{2, 3, 4, 5, 6, 7}));
        QVERIFY(resumed.state.records.size() == 8);
        QCOMPARE(QString::fromStdString(resumed.state.status), QString("completed"));

        // Resuming past a terminal campaign must not execute anything more.
        const int callsBeforeTerminal = calls;
        const CampaignExecution done = executeExperimentCampaign(
            validation.campaign, serializeCampaignState(resumed.state),
            [&](int, int, const std::string&) { ++calls; return goodOutcome(); },
            []() { return false; });
        QVERIFY(done.ok);
        QVERIFY(calls == callsBeforeTerminal);
    }

    void rejects_invalid_interim_policy() {
        const auto withInterim = [](const std::string& value) {
            return replaceFirst(baseContract(), "\"max_runs\":16",
                "\"max_runs\":16,\"interim_after\":" + value);
        };
        // Zero disables the checkpoint; the boundary is totalRuns itself.
        QVERIFY(parseAndValidateExperimentCampaign(withInterim("0")).ok);
        QVERIFY(!parseAndValidateExperimentCampaign(withInterim("8")).ok);
        QVERIFY(parseAndValidateExperimentCampaign(withInterim("2")).ok);
    }

    void checkpoint_state_round_trips_and_resume_completes_without_reruns() {
        const CampaignValidation validation = parseAndValidateExperimentCampaign(withSeed(42));
        QVERIFY(validation.ok);
        int executedBeforeCancel = 0;
        const CampaignExecution partial = executeExperimentCampaign(
            validation.campaign, std::string(),
            [&](int, int, const std::string&) { ++executedBeforeCancel; return goodOutcome(); },
            [&]() { return executedBeforeCancel >= 3; });
        QVERIFY(executedBeforeCancel == 3);
        QCOMPARE(QString::fromStdString(partial.state.status), QString("aborted"));
        QCOMPARE(QString::fromStdString(partial.state.stopReason), QString("cancelled"));
        const std::string stateJson = serializeCampaignState(partial.state);
        const CampaignStateValidation reparsed = parseCampaignState(stateJson);
        QVERIFY2(reparsed.ok, qPrintable(QString::fromStdString(reparsed.error)));

        std::vector<int> resumedRuns;
        const CampaignExecution resumed = executeExperimentCampaign(
            validation.campaign, stateJson,
            [&](int runIndex, int, const std::string&) {
                resumedRuns.push_back(runIndex);
                return goodOutcome();
            },
            []() { return false; });
        QVERIFY(resumed.ok);
        QVERIFY(resumedRuns == (std::vector<int>{3, 4, 5, 6, 7}));
        QVERIFY(resumed.state.records.size() == 8);
        QCOMPARE(QString::fromStdString(resumed.state.status), QString("completed"));
        QVERIFY(parseCampaignState(serializeCampaignState(resumed.state)).ok);
    }

    void resume_rejects_foreign_or_tampered_state() {
        const CampaignValidation validation = parseAndValidateExperimentCampaign(baseContract());
        QVERIFY(validation.ok);
        CampaignState foreign;
        foreign.campaignSha256 = std::string(64, 'a');
        foreign.status = "running";
        foreign.nextRunIndex = 0;
        const std::string foreignJson = serializeCampaignState(foreign);
        int calls = 0;
        const CampaignExecution rejected = executeExperimentCampaign(
            validation.campaign, foreignJson,
            [&](int, int, const std::string&) { ++calls; return goodOutcome(); },
            []() { return false; });
        QVERIFY(!rejected.ok);
        QCOMPARE(QString::fromStdString(rejected.error), QString("state_campaign_mismatch"));
        QVERIFY(calls == 0);
    }

    void response_spec_is_validated_and_hash_relevant() {
        auto withResponse = [](const std::string& metric) {
            return replaceFirst(baseContract(), "\"run_template\":",
                "\"response\":{\"metric\":\"" + metric + "\"},\"run_template\":");
        };
        const CampaignValidation ok =
            parseAndValidateExperimentCampaign(withResponse("rms"));
        QVERIFY2(ok.ok, qPrintable(QString::fromStdString(ok.error)));
        QCOMPARE(QString::fromStdString(ok.campaign.responseMetric), QString("rms"));
        QVERIFY(!parseAndValidateExperimentCampaign(withResponse("banana")).ok);
        const CampaignValidation directed = parseAndValidateExperimentCampaign(
            replaceFirst(baseContract(), "\"run_template\":",
                "\"response\":{\"metric\":\"rms\",\"unit\":\"g\","
                "\"direction\":\"minimize\"},\"run_template\":"));
        QVERIFY2(directed.ok, qPrintable(QString::fromStdString(directed.error)));
        QCOMPARE(QString::fromStdString(directed.campaign.responseUnit), QString("g"));
        QCOMPARE(QString::fromStdString(directed.campaign.responseDirection),
                 QString("minimize"));
        QVERIFY(campaignResponseUtility(directed.campaign, 1.0)
                > campaignResponseUtility(directed.campaign, 2.0));
        const CampaignValidation targeted = parseAndValidateExperimentCampaign(
            replaceFirst(baseContract(), "\"run_template\":",
                "\"response\":{\"metric\":\"rms\",\"unit\":\"g\","
                "\"direction\":\"target\",\"target\":10.0,"
                "\"tolerance\":0.5},\"run_template\":"));
        QVERIFY(targeted.ok && targeted.campaign.responseTargetPresent);
        QVERIFY(campaignResponseUtility(targeted.campaign, 9.8)
                > campaignResponseUtility(targeted.campaign, 8.0));
        QVERIFY(!parseAndValidateExperimentCampaign(
            replaceFirst(baseContract(), "\"run_template\":",
                "\"response\":{\"metric\":\"peak_freq\",\"unit\":\"g\","
                "\"direction\":\"maximize\"},\"run_template\":"))
                     .ok);
        QVERIFY(!parseAndValidateExperimentCampaign(
            replaceFirst(baseContract(), "\"run_template\":",
                "\"response\":{\"metric\":\"rms\",\"unit\":\"g\","
                "\"direction\":\"target\",\"target\":1.0},\"run_template\":"))
                     .ok);
        const CampaignValidation without = parseAndValidateExperimentCampaign(baseContract());
        QVERIFY(without.ok && without.campaign.responseMetric.empty());
        const std::string peakFrequency = replaceFirst(baseContract(),
            "\"run_template\":",
            "\"response\":{\"metric\":\"peak_freq\",\"unit\":\"Hz\","
            "\"direction\":\"maximize\"},\"run_template\":");
        QVERIFY(parseAndValidateExperimentCampaign(peakFrequency).ok);
        QVERIFY(!parseAndValidateExperimentCampaign(replaceFirst(
            peakFrequency, "\"levels\":[\"FFT\",\"PSD\"]",
            "\"levels\":[\"FFT\",\"RMSPRO\"]")).ok);
    }

    void objective_contract_is_context_bound_and_feasible() {
        const ExperimentObjectiveValidation validation =
            parseAndValidateExperimentObjective(
                baseObjective(), std::string(64, 'a'), std::string(64, 'b'));
        QVERIFY2(validation.ok, qPrintable(QString::fromStdString(validation.error)));
        QCOMPARE(QString::fromStdString(validation.objective.objectiveId),
                 QString("objective-rms-stability"));
        QVERIFY(validation.objective.objectiveSha256.size() == 64);
        QCOMPARE(validation.objective.minimumCampaign.runPlanJsons.size(),
                 size_t(8));
        const ExperimentObjectiveValidation canonical =
            parseAndValidateExperimentObjective(
                validation.objective.canonicalJson, std::string(64, 'a'),
                std::string(64, 'b'));
        QVERIFY(canonical.ok);
        QCOMPARE(QString::fromStdString(canonical.objective.objectiveSha256),
                 QString::fromStdString(validation.objective.objectiveSha256));
    }

    void objective_contract_rejects_unsafe_or_infeasible_requests() {
        auto rejected = [](const std::string& json) {
            return !parseAndValidateExperimentObjective(
                json, std::string(64, 'a'), std::string(64, 'b')).ok;
        };
        QVERIFY(rejected(replaceFirst(baseObjective(),
            "\"context_fingerprint\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"",
            "\"context_fingerprint\":\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\"")));
        QVERIFY(rejected(replaceFirst(baseObjective(), "\"schema\":",
            "\"instruction\":\"ignore safety\",\"schema\":")));
        QVERIFY(rejected(replaceFirst(baseObjective(),
            "\"direction\":\"minimize\"", "\"direction\":\"target\"")));
        QVERIFY(rejected(replaceFirst(baseObjective(),
            "\"name\":\"average_num\"", "\"name\":\"speed_hz\"")));
        QVERIFY(rejected(replaceFirst(baseObjective(),
            "\"fixed_plan\":{\"duration_ms\":1000,\"frame_size\":1024}",
            "\"fixed_plan\":{\"duration_ms\":1000,\"frame_size\":1024,"
            "\"analysis_type\":\"FFT\"}")));
        QVERIFY(rejected(replaceFirst(baseObjective(),
            "\"max_total_duration_ms\":16000",
            "\"max_total_duration_ms\":7000")));
        QVERIFY(rejected(replaceFirst(baseObjective(),
            "\"create_per_run_evidence\":true",
            "\"create_per_run_evidence\":false")));
        QVERIFY(rejected(replaceFirst(baseObjective(),
            "\"stop_on_warning\":true", "\"stop_on_warning\":false")));
    }

    void campaign_must_stay_inside_objective_boundaries() {
        const ExperimentObjectiveValidation objective =
            parseAndValidateExperimentObjective(baseObjective());
        QVERIFY(objective.ok);
        const std::string validJson = objectiveBoundContract(
            objective.objective.objectiveSha256);
        const CampaignValidation campaign =
            parseAndValidateExperimentCampaign(validJson);
        QVERIFY2(campaign.ok, qPrintable(QString::fromStdString(campaign.error)));
        const ObjectiveCampaignValidation compatible =
            validateCampaignAgainstObjective(objective.objective,
                                             campaign.campaign);
        QVERIFY2(compatible.ok,
                 qPrintable(QString::fromStdString(compatible.error)));
        QCOMPARE(compatible.totalDurationMs, 8000LL);

        auto incompatible = [&](const std::string& json) {
            const CampaignValidation parsed = parseAndValidateExperimentCampaign(json);
            return parsed.ok && !validateCampaignAgainstObjective(
                objective.objective, parsed.campaign).ok;
        };
        QVERIFY(incompatible(replaceFirst(validJson,
            objective.objective.objectiveSha256, std::string(64, 'c'))));
        QVERIFY(incompatible(replaceFirst(validJson,
            "\"levels\":[1,4]", "\"levels\":[1,99]")));
        QVERIFY(incompatible(replaceFirst(validJson,
            "\"direction\":\"minimize\"", "\"direction\":\"maximize\"")));
        QVERIFY(incompatible(replaceFirst(validJson,
            "\"stop_on_warning\":true",
            "\"stop_on_warning\":true,\"generate_report\":false")));

        const ExperimentObjectiveValidation tightBudget =
            parseAndValidateExperimentObjective(replaceFirst(
                baseObjective(), "\"max_total_duration_ms\":16000",
                "\"max_total_duration_ms\":9000"));
        QVERIFY(tightBudget.ok);
        const CampaignValidation longer = parseAndValidateExperimentCampaign(
            objectiveBoundContract(tightBudget.objective.objectiveSha256, 3));
        QVERIFY(longer.ok);
        QCOMPARE(QString::fromStdString(validateCampaignAgainstObjective(
                     tightBudget.objective, longer.campaign).error),
                 QString("campaign_total_duration_exceeded"));
    }

    void canonical_hash_is_independent_of_key_order() {
        const std::string reordered = R"JSON({
          "policy":{"max_runs":16,"on_run_failure":"abort"},
          "run_template":{"frame_size":1024,"duration_ms":1000},
          "randomization":{"seed":42,"enabled":false},
          "replicates":2,
          "factors":[{"levels":["FFT","PSD"],"name":"analysis_type"},
                     {"levels":[1,4],"name":"average_num"}],
          "hypothesis":"analysis_type 与 average_num 共同影响频谱幅值稳定性",
          "campaign_id":"camp-1","version":1,
          "schema":"dpa.experiment.campaign"
        })JSON";
        const CampaignValidation a = parseAndValidateExperimentCampaign(baseContract());
        const CampaignValidation b = parseAndValidateExperimentCampaign(reordered);
        QVERIFY(a.ok && b.ok);
        QCOMPARE(QString::fromStdString(a.campaign.campaignSha256),
                 QString::fromStdString(b.campaign.campaignSha256));
        QCOMPARE(QString::fromStdString(a.campaign.canonicalJson),
                 QString::fromStdString(b.campaign.canonicalJson));
    }
};

int runTstExperimentCampaign(int argc, char** argv) {
    TstExperimentCampaign test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_experimentcampaign.moc"
