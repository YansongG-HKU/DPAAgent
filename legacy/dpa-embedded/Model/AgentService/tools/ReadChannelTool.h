#pragma once
#include <string>
#include <vector>
#include <map>
#include "ToolResult.h"
namespace Model {
    struct ReadChannelArgs { std::string dataId; int channel=0; double start=0; double len=0; int maxPoints=512; };
    struct ChannelSeries {
        bool found=false;
        std::vector<double> x, y;                 // full series as read by the adapter
        std::map<std::string,double> stats;       // e.g. {"rms":..,"max":..}
    };
    // Pure core (unit-tested):
    ReadChannelArgs parseReadChannelArgs(const std::string& argsJson);
    ToolResult buildReadChannelResult(const ChannelSeries& s, const ReadChannelArgs& args);
    // Adapter (framework-coupled, plugin-only, NOT unit-tested):
    ChannelSeries readChannelFromTest(class M* dpaTest, const ReadChannelArgs& args);
}
