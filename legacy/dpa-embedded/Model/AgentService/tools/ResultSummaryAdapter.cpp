#include "tools/ResultSummaryTool.h"
#include "iTest.h"          // Model::M / Model::iTest / Model::iData (via HeaderDefine + iData.h)
#include "iDataProcess.h"   // Model::iDataProcess result publication properties
#include <algorithm>
#include <cmath>
#include <limits>
using namespace Model;

// Adapter: framework-coupled, plugin-only, NOT unit-tested (only build-verified).
// Reads the current plot cell's analysis RESULT and distills it into a peak +
// statistics summary for get_result_summary.
//
// Result resolution: after excuteCurrentProcess()/onProcessDone(), the current
// cell's analysis is an iDataProcess* reachable via the same property path the
// Ribbon ViewModels and RunAnalysisAdapter use:
//     PlotSet -> CurrentLayout -> CurrentCell -> Process
// Prefer the publication alias set only after the test workspace accepts the exact result;
// ResultData/LastResultData remain compatibility fallbacks for non-Agent legacy analyses.
//
// M2: marshal to UI/event-loop thread via
//     QMetaObject::invokeMethod(..., Qt::BlockingQueuedConnection)
// before touching iData/iDataProcess (Qt objects + recompute mutexes).
// For M1 this runs directly on the calling thread.
ResultSummary Model::resultSummaryFromTest(M* dpaTest) {
    ResultSummary s;
    if (!dpaTest) return s;                       // found stays false

    iTest* test = (iTest*)dpaTest;

    // Walk to the current cell's analysis process. value<T> returns null if any
    // hop on the path is missing (no current cell / cell holds no process).
    auto proc = test->value<iDataProcess*>("PlotSet", "CurrentLayout", "CurrentCell", "Process");
    if (!proc) return s;                          // no analysis on the current cell

    iData* result = proc->value<iData*>("PublishedResultData");
    if (!result) result = proc->value<iData*>("ResultData");
    if (!result) result = proc->value<iData*>("LastResultData");
    if (!result) return s;                        // not computed yet

    s.name = result->value<std::string>("Name");
    const int dataForm = result->value<int>("DataForm");
    s.xDomain = (dataForm & iData::FreqForm) != 0 ? "frequency"
        : (dataForm & iData::TimeForm) != 0 ? "time" : "other";

    // Statistics: getStatistics is non-pure (only some iData subclasses override
    // it; the base returns false). Only fill stats when it actually succeeds.
    std::vector<std::string> names;
    std::vector<double> vals;
    if (result->getStatistics(names, vals, "Value")) {
        const size_t n = std::min(names.size(), vals.size());
        for (size_t i = 0; i < n; ++i) s.stats[names[i]] = vals[i];
    }

    // Peak: scan the result's first channel for the max |y| via getThinXY
    // (already volume-reduced at the source). argmax -> peak_value + its x as
    // peak_freq_hz (the result X axis is frequency for PSD/FFT). Best-effort:
    // skip silently if the series is unavailable.
    const int chCount = result->childrenCount("Channels");
    if (chCount > 0) {
        const int channelId = result->value<int>("Channels", 0, "ChannelId");
        s.channelId = channelId;
        std::vector<double> x, y;
        if (result->getThinXY(channelId, 0.0, 0.0, x, y) && !y.empty()) {
            size_t bestI = 0;
            double bestV = std::fabs(y[0]);
            for (size_t i = 1; i < y.size(); ++i) {
                const double magnitude = std::fabs(y[i]);
                if (magnitude > bestV) { bestV = magnitude; bestI = i; }
            }
            s.peaks["peak_value"] = bestV;
            if (bestI < x.size()) {
                if (s.xDomain == "frequency") s.peaks["peak_freq_hz"] = x[bestI];
                else s.peaks["peak_x"] = x[bestI];
            }
        }
    }

    s.found = true;
    return s;
}
