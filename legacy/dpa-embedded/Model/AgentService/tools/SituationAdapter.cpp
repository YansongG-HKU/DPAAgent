#include "tools/SituationAssembler.h"
#include "tools/ListDataTool.h"       // snapshotDataListFromTest / DataListSnapshot / DataItem
#include "tools/ResultSummaryTool.h"  // resultSummaryFromTest / ResultSummary
#include "iTest.h"
using namespace Model;

// Adapter: assemble the situation snapshot from the current analysis test (+ project).
// Framework-coupled (iTest::value<T>), plugin-only, NOT unit-tested (build/smoke-verified).
// Reuses the already-tested list + result adapters; the only new navigation is the
// current cell's ProcessCategory (same PlotSet/CurrentLayout/CurrentCell/Process path
// ResultSummaryAdapter uses).
SoftwareStateSnapshot Model::assembleSituationFromTest(M* dpaTest, M* project, long long nowMs) {
    SoftwareStateSnapshot s;
    s.capturedAt = nowMs;
    if (!dpaTest) return s;                         // hasTest stays false
    s.hasTest = true;

    iTest* test = (iTest*)dpaTest;
    s.testName = test->value<std::string>("Name");
    if (project) s.projectName = project->value<std::string>("Name");

    // Workspace datasets (reuse the tested list adapter).
    DataListSnapshot dl = snapshotDataListFromTest(dpaTest);
    s.datasets = dl.items;

    // Current analysis type: the ProcessCategory of the current cell's process (empty if none).
    s.currentAnalysisType =
        test->value<std::string>("PlotSet", "CurrentLayout", "CurrentCell", "Process", "ProcessCategory");

    // Latest result (reuse the tested result adapter).
    ResultSummary rs = resultSummaryFromTest(dpaTest);
    s.hasResult   = rs.found;
    s.resultName  = rs.name;
    s.resultPeaks = rs.peaks;
    s.resultStats = rs.stats;
    return s;
}
