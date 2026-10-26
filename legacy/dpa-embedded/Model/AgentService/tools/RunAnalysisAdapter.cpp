#include "tools/RunAnalysisTool.h"
#include "iTest.h"          // Model::M / Model::iTest (via HeaderDefine + iData.h)
#include "iDataProcess.h"   // Model::iDataProcess (updateFrameSize/updateAverageNum/updateOverLap)
using namespace Model;

// Adapter: framework-coupled, plugin-only, NOT unit-tested (only build-verified).
// Builds & runs a DSP analysis process on the current analysis test:
//   1. close whatever process the current plot cell holds,
//   2. create the requested analysis (switches the current cell to it),
//   3. apply optional frame/average/overlap params (coupled recompute calls),
//   4. execute the current cell's process.
//
// Process resolution: createEmptyProcess(type) delegates to switchCurrentProcess
// (TestDPAImpl), which switches the *current plot cell* to the new analysis. The
// freshly-created iDataProcess* is then the current cell's "Process" property,
// reachable via PlotSet -> CurrentLayout -> CurrentCell -> Process (the same path
// the Ribbon ViewModels subscribe to). We read it back that way rather than
// trusting the raw return value, since switchCurrentProcess returns nullptr when
// the cell still holds a process (hence the explicit close in step 1).
//
// M2: marshal to the UI/event-loop thread via
//     QMetaObject::invokeMethod(..., Qt::BlockingQueuedConnection)
// before touching iData/iTest/iDataProcess (Qt objects + recompute mutexes).
// For M1 this runs directly on the calling thread.
AnalysisOutcome Model::runAnalysisOnTest(M* dpaTest, const RunAnalysisArgs& a,
                                         const std::string& runId,
                                         const AnalysisProcessReady& onProcessReady) {
    AnalysisOutcome out;
    if (!dpaTest) { out.error = "no_current_test"; return out; }

    iTest* test = (iTest*)dpaTest;

    // (1) Close/exit any analysis the current cell already holds. An empty type
    // resets the current cell to a plain display cell, freeing it so the new
    // analysis can take it over (switchCurrentProcess refuses an occupied cell).
    test->switchCurrentProcess("");

    // (2) Create the requested analysis on the current cell.
    test->createEmptyProcess(a.type);

    // Read the process back from the current cell (PlotSet -> CurrentLayout ->
    // CurrentCell -> Process). value<T> walks the property path and returns null
    // if any hop is missing.
    auto proc = test->value<iDataProcess*>("PlotSet", "CurrentLayout", "CurrentCell", "Process");
    if (!proc) { out.error = "process_create_failed"; return out; }

    // Correlate completion to this exact process/run before execution starts.
    // PublishedResultData is cleared again by startProcess() and remains null
    // until TestDPAImpl has transferred ownership to WorkSpace.
    proc->setValue("AgentRunId", runId);
    proc->setValue("PublishedResultData", static_cast<iData*>(nullptr));
    out.process = proc;
    out.runId = runId;
    if (onProcessReady) onProcessReady(proc, runId);

    // (3) Apply optional parameters. Each is a coupled recompute call; only touch
    // a knob when the caller asked for it (defaults leave the engine's own value).
    if (a.frameSize > 0)     proc->updateFrameSize(a.frameSize);
    if (a.averageNum > 0)    proc->updateAverageNum(a.averageNum);
    if (a.overlapIndex >= 0) proc->updateOverLap(a.overlapIndex);   // takes an INDEX

    // (4) Execute the current cell's process. Returns nonzero on success.
    int ret = test->excuteCurrentProcess();

    out.started    = (ret != 0);
    out.resultName = a.type;
    if (!out.started) out.error = "analysis_failed";
    return out;
}
