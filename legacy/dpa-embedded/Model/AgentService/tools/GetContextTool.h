#pragma once
#include <string>
#include <vector>
#include "ToolResult.h"
namespace Model {
    struct ChannelInfo {
        int id=0;
        std::string name;
        std::string unit;
        double samplingRate=0;
        std::string dataId;
        std::string dataName;
        std::string physicalType;
    };
    struct ContextSnapshot {
        bool hasTest=false;
        std::string projectName;
        std::string testName;
        std::vector<ChannelInfo> channels;
        bool hasAnalysis=false;
        std::string analysisType;
        std::string analysisCategory;
        std::string resultFunctionType;
        int frameSize=0;
        int averageNum=0;
        int overlapIndex=-1;
    };
    // Pure core — unit-testable, no Qt-service deps.
    ToolResult buildContextResult(const ContextSnapshot& s);
    // Adapter: fills a snapshot from the current analysis test/property tree.
    // Project is optional so missing bindings safely produce an empty project name.
    ContextSnapshot snapshotFromTest(class M* dpaTest, class M* project = nullptr);
}
