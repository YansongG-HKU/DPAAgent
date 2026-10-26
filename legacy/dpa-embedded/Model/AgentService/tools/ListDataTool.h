#pragma once
#include <string>
#include <vector>
#include "ToolResult.h"
namespace Model {
    struct DataItem { std::string id; std::string name; int channelCount = 0; };
    struct DataListSnapshot { bool hasTest = false; std::vector<DataItem> items; };
    // Pure core (unit-tested):
    ToolResult buildListDataResult(const DataListSnapshot& s);
    // Adapter (framework-coupled, plugin-only, NOT unit-tested):
    DataListSnapshot snapshotDataListFromTest(class M* dpaTest);
}
