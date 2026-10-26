#include "tools/ListDataTool.h"
#include "iTest.h"   // brings in Model::M / Model::iData via HeaderDefine + iData.h
using namespace Model;

// Adapter: framework-coupled, plugin-only, NOT unit-tested (only build-verified).
// Enumerates the current analysis test's workspace datasets.
// TestDPAImpl registers its workspace MList under property "WorkSpace"
// (addPropertyClass<iData>("WorkSpace", &_workSpace, ...)), and each dataset
// exposes "Id"/"Name" plus a "Channels" child list (see iData ctor).
DataListSnapshot Model::snapshotDataListFromTest(M* dpaTest) {
    if (!dpaTest) return {};            // hasTest stays false
    DataListSnapshot s;
    s.hasTest = true;
    const int count = dpaTest->childrenCount("WorkSpace");
    for (int i = 0; i < count; ++i) {
        DataItem item;
        item.id           = dpaTest->value<std::string>("WorkSpace", i, "Id");
        item.name         = dpaTest->value<std::string>("WorkSpace", i, "Name");
        item.channelCount = dpaTest->childrenCount("WorkSpace", i, "Channels");
        s.items.push_back(std::move(item));
    }
    return s;
}
