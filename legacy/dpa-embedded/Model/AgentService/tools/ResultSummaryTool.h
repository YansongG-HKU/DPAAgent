#pragma once
#include <string>
#include <map>
#include "ToolResult.h"
namespace Model {
    struct ResultSummary {
        bool found=false;
        int channelId=-1;
        std::string name;
        std::string xDomain;                    // frequency | time | other
        std::map<std::string,double> peaks;   // e.g. {"peak_freq_hz":..,"peak_value":..}
        std::map<std::string,double> stats;   // e.g. {"rms":..,"max":..}
    };
    // Pure core (unit-tested):
    ToolResult buildResultSummaryResult(const ResultSummary& s);
    // Adapter (framework-coupled, plugin-only, NOT unit-tested):
    ResultSummary resultSummaryFromTest(class M* dpaTest);
}
