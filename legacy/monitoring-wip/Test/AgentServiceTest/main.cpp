#include <QApplication>
#include <QTemporaryDir>
#include <cstdio>

int runTstSmoke(int argc, char** argv);
int runTstSimpleString(int argc, char** argv);
int runTstSessionRecord(int argc, char** argv);
int runTstAgentConfig(int argc, char** argv);
int runTstDiagnosticRun(int argc, char** argv);
int runTstAgentServiceAsyncOps(int argc, char** argv);
int runTstExperimentEvidence(int argc, char** argv);
int runTstAnalysisPlan(int argc, char** argv);
int runTstToolResult(int argc, char** argv);
int runTstToolRegistry(int argc, char** argv);
int runTstGetContext(int argc, char** argv);
int runTstListData(int argc, char** argv);
int runTstReadChannel(int argc, char** argv);
int runTstRunAnalysis(int argc, char** argv);
int runTstResultSummary(int argc, char** argv);
int runTstOrchestrator(int argc, char** argv);
int runTstClaudeWire(int argc, char** argv);
int runTstOpenAIWire(int argc, char** argv);
int runTstGenerateReport(int argc, char** argv);
int runTstSituation(int argc, char** argv);
int runTstAdvisory(int argc, char** argv);
int runTstSimulationScenario(int argc, char** argv);
int runTstDiagnostics(int argc, char** argv);
int runTstGoldenEvals(int argc, char** argv);
int runTstEnvelope(int argc, char** argv);
int runTstBaseline(int argc, char** argv);
int runTstRateLimiter(int argc, char** argv);
int runTstMcpBridge(int argc, char** argv);
int runTstMarkdownLite(int argc, char** argv);
int runTstAgentUiParse(int argc, char** argv);
int runTstLiveDgx(int argc, char** argv);
int runTstTlsRuntime(int argc, char** argv);
int runTstStandardsCatalog(int argc, char** argv);
int runTstMeasurementContract(int argc, char** argv);
int runTstMeasurementAdapterContract(int argc, char** argv);
int runTstExperimentCampaign(int argc, char** argv);
int runTstCampaignAnalysis(int argc, char** argv);
int runTstCampaignPower(int argc, char** argv);
int runTstExperimentRefinement(int argc, char** argv);
int runTstExperimentKnowledge(int argc, char** argv);
int runTstStatsSidecar(int argc, char** argv);
int runTstAgentChatDock(int argc, char** argv);
int runTstStreamingMonitor(int argc, char** argv);

int main(int argc, char** argv)
{
	QTemporaryDir localAppData;
	if (!localAppData.isValid()) {
		std::fprintf(stderr, "FAILED: could not isolate LOCALAPPDATA\n");
		return 3;
	}
	qputenv("LOCALAPPDATA", localAppData.path().toLocal8Bit());
    QApplication app(argc, argv);
	// Existing scripted service tests intentionally exercise the legacy direct
	// tool loop. Production defaults to plan-first; migration coverage for the
	// new mode lives in the orchestrator tests.
	qputenv("DPA_PLAN_FIRST", "0");
	int status = 0;
	const QByteArray requestedSuite = qgetenv("DPA_TEST_SUITE");
	if (requestedSuite == "TlsRuntime")
		return runTstTlsRuntime(argc, argv);
	bool matchedSuite = requestedSuite.isEmpty();
	auto run = [&](const char* name, int (*suite)(int, char**)) {
		if (!requestedSuite.isEmpty() && requestedSuite != name) return;
		matchedSuite = true;
		const int result = suite(argc, argv);
        if (result != 0) std::fprintf(stderr, "FAILED SUITE: %s (exit %d)\n", name, result);
        status |= result;
    };
    run("Smoke", runTstSmoke);
    run("SimpleString", runTstSimpleString);
    run("SessionRecord", runTstSessionRecord);
    run("AgentConfig", runTstAgentConfig);
    run("DiagnosticRun", runTstDiagnosticRun);
    run("AgentServiceAsyncOps", runTstAgentServiceAsyncOps);
    run("ExperimentEvidence", runTstExperimentEvidence);
    run("AnalysisPlan", runTstAnalysisPlan);
    run("ToolResult", runTstToolResult);
    run("ToolRegistry", runTstToolRegistry);
    run("GetContext", runTstGetContext);
    run("ListData", runTstListData);
    run("ReadChannel", runTstReadChannel);
    run("RunAnalysis", runTstRunAnalysis);
    run("ResultSummary", runTstResultSummary);
    run("Orchestrator", runTstOrchestrator);
    run("ClaudeWire", runTstClaudeWire);
    run("OpenAIWire", runTstOpenAIWire);
    run("GenerateReport", runTstGenerateReport);
    run("Situation", runTstSituation);
    run("Advisory", runTstAdvisory);
    run("SimulationScenario", runTstSimulationScenario);
    run("Diagnostics", runTstDiagnostics);
    run("GoldenEvals", runTstGoldenEvals);
    run("Envelope", runTstEnvelope);
    run("Baseline", runTstBaseline);
    run("RateLimiter", runTstRateLimiter);
    run("McpBridge", runTstMcpBridge);
    run("MarkdownLite", runTstMarkdownLite);
    run("AgentUiParse", runTstAgentUiParse);
	run("LiveDgx", runTstLiveDgx);
	run("StandardsCatalog", runTstStandardsCatalog);
	run("MeasurementContract", runTstMeasurementContract);
	run("MeasurementAdapterContract", runTstMeasurementAdapterContract);
	run("ExperimentCampaign", runTstExperimentCampaign);
	run("CampaignAnalysis", runTstCampaignAnalysis);
	run("CampaignPower", runTstCampaignPower);
	run("ExperimentRefinement", runTstExperimentRefinement);
	run("ExperimentKnowledge", runTstExperimentKnowledge);
	run("StatsSidecar", runTstStatsSidecar);
	run("AgentChatDock", runTstAgentChatDock);
	run("StreamingMonitor", runTstStreamingMonitor);
	if (!matchedSuite) {
		std::fprintf(stderr, "UNKNOWN TEST SUITE: %s\n", requestedSuite.constData());
		return 2;
	}
	return status;
}
