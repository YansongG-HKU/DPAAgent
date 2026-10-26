#pragma once
#include <string>
#include <vector>
#include <map>
#include "ToolResult.h"
#include "tools/ListDataTool.h"   // reuse struct DataItem
namespace Model {
    struct SoftwareStateSnapshot {
        bool hasTest = false;
        std::string projectName;
        std::string testName;
        std::vector<DataItem> datasets;          // workspace datasets (id/name/channelCount)
        std::string currentAnalysisType;         // ProcessCategory of the current cell (may be "")
        bool hasResult = false;
        std::string resultName;
        std::map<std::string,double> resultPeaks;
        std::map<std::string,double> resultStats;
        long long capturedAt = 0;
    };
    // Pure core: compose the snapshot into a compact situation JSON (unit-tested).
    ToolResult buildSituation(const SoftwareStateSnapshot& s);
    // Adapter: assemble the snapshot from the current test/project (framework-coupled,
    // plugin-only, build/smoke-verified; reuses list + result adapters).
    SoftwareStateSnapshot assembleSituationFromTest(class M* dpaTest, class M* project, long long nowMs);
}
