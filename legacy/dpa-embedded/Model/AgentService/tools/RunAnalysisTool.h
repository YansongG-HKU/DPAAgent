#pragma once
#include <functional>
#include <string>
#include "ToolResult.h"
namespace Model {
    class M;
    struct RunAnalysisArgs { std::string type; int frameSize=0; int averageNum=0; int overlapIndex=-1; };
    struct AnalysisValidation { bool ok=true; std::string error; };
    struct AnalysisOutcome {
        bool started=false;
        std::string resultName;
        std::string error;
        M* process=nullptr;       // non-owning; process cell owns it
        std::string runId;
    };
    using AnalysisProcessReady = std::function<void(M*, const std::string&)>;
    // Pure core (unit-tested):
    RunAnalysisArgs parseRunAnalysisArgs(const std::string& argsJson);
    AnalysisValidation validateAnalysisRequest(const RunAnalysisArgs& a);   // type whitelist
    ToolResult buildRunAnalysisResult(const AnalysisValidation& v, const AnalysisOutcome& o, const RunAnalysisArgs& a);
    // Adapter (framework-coupled, plugin-only, NOT unit-tested):
    AnalysisOutcome runAnalysisOnTest(M* dpaTest, const RunAnalysisArgs& a,
                                      const std::string& runId = {},
                                      const AnalysisProcessReady& onProcessReady = {});
}
