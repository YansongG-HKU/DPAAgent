#include "tools/ReadChannelTool.h"
#include "iTest.h"   // brings in Model::M / Model::iTest / Model::iData via HeaderDefine + iData.h
using namespace Model;

// Adapter: framework-coupled, plugin-only, NOT unit-tested (only build-verified).
// Reads ONE channel's DOWNSAMPLED waveform + statistics from the current
// analysis test's workspace dataset identified by args.dataId.
//
// Resolution path: the bound "分析试验" pointer is an iTest*, which exposes
// getData(uuid) -> M* (the workspace iData matched by its "Id" property; see
// TestDPAImpl::getData). The returned M* is the iData for that dataset.
ChannelSeries Model::readChannelFromTest(M* dpaTest, const ReadChannelArgs& args) {
    ChannelSeries s;
    if (!dpaTest) return s;                       // found stays false

    // The bound "分析试验" pointer is an iTest*; getData(uuid) returns the
    // workspace iData (matched by "Id"), cast the same way as TestDPAImpl.
    iTest* test = (iTest*)dpaTest;
    iData* data = (iData*)test->getData(args.dataId);
    if (!data) return s;                          // unknown dataId

    // getThinXY reads a stride/period-thinned XY pair for the channel over
    // [start, start+len) (len<=0 => whole record), already reducing volume at
    // the source; the pure core then re-caps to maxPoints for the privacy bound.
    // getThinXY honors the [start, start+len) window. getXY() reads the WHOLE record and
    // cannot honor a window, so only fall back to it when the whole record was requested
    // (start<=0 && len<=0). Otherwise a windowed request would silently get the full record.
    const bool wholeRecord = (args.start <= 0.0 && args.len <= 0.0);
    if (!data->getThinXY(args.channel, args.start, args.len, s.x, s.y)) {
        if (!wholeRecord || !data->getXY(args.channel, s.x, s.y)) {
            return s;                                 // found stays false (no data for window)
        }
    }

    // getStatistics is non-pure: it returns false on data types that do not
    // override it (only MemoryData/RealTimeData do). Only fill stats when true.
    std::vector<std::string> names;
    std::vector<double> vals;
    if (data->getStatistics(names, vals, "Value")) {
        const size_t n = std::min(names.size(), vals.size());
        for (size_t i = 0; i < n; ++i) s.stats[names[i]] = vals[i];
    }

    s.found = true;
    return s;
}
