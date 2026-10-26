#include "tools/ReadChannelTool.h"
#include "AgentJson.h"
#include <QJsonObject>
#include <QJsonArray>
#include <algorithm>
#include <cmath>
using namespace Model;

ReadChannelArgs Model::parseReadChannelArgs(const std::string& argsJson) {
    ReadChannelArgs a;
    QJsonObject o = Model::AgentJson::parseObject(argsJson);
    if (o.contains("dataId"))    a.dataId    = o.value("dataId").toString().toStdString();
    if (o.contains("channel"))   a.channel   = o.value("channel").toInt(a.channel);
    if (o.contains("start"))     a.start     = o.value("start").toDouble(a.start);
    if (o.contains("len"))       a.len       = o.value("len").toDouble(a.len);
    if (o.contains("maxPoints")) a.maxPoints = o.value("maxPoints").toInt(a.maxPoints);
    // Clamp maxPoints to a sane range so a hostile/oversized request can never
    // force the full raw signal back through the privacy boundary.
    const int kMaxAllowed = 5000;
    if (a.maxPoints <= 0)          a.maxPoints = 512;
    if (a.maxPoints > kMaxAllowed) a.maxPoints = kMaxAllowed;
    return a;
}

ToolResult Model::buildReadChannelResult(const ChannelSeries& s, const ReadChannelArgs& args) {
    ToolResult r;
    if (!s.found) {
        r.ok = false;
        r.error = "channel_not_found";
        r.summary = "未找到通道 " + std::to_string(args.channel);
        return r;
    }

    const int total = static_cast<int>(s.y.size());
    int maxPoints = args.maxPoints > 0 ? args.maxPoints : 512;

    // Privacy contract: NEVER emit the full raw signal. If the series is longer
    // than maxPoints, stride-downsample to at most maxPoints points.
    bool downsampled = false;
    int stride = 1;
    if (total > maxPoints) {
        downsampled = true;
        // ceil(total / maxPoints) guarantees the strided count is <= maxPoints.
        stride = static_cast<int>(std::ceil(static_cast<double>(total) / maxPoints));
        if (stride < 1) stride = 1;
    }

    QJsonArray xs, ys;
    for (int i = 0; i < total; i += stride) {
        if (i < static_cast<int>(s.x.size())) xs.append(s.x[i]);
        ys.append(s.y[i]);
    }
    const int returned = ys.size();

    QJsonObject stats;
    for (const auto& kv : s.stats) {
        stats[QString::fromStdString(kv.first)] = kv.second;
    }

    QJsonObject o;
    o["channel"]         = args.channel;
    o["total_points"]    = total;
    o["returned_points"] = returned;
    o["downsampled"]     = downsampled;
    o["stats"]           = stats;
    o["x"]               = xs;
    o["y"]               = ys;
    r.dataJson = Model::AgentJson::dump(o);

    // Build a compact human summary; surface a couple of the most useful stats.
    std::string statText;
    auto appendStat = [&](const char* key) {
        auto it = s.stats.find(key);
        if (it != s.stats.end()) {
            if (!statText.empty()) statText += ", ";
            // Trim trailing zeros for readability.
            std::string v = std::to_string(it->second);
            v.erase(v.find_last_not_of('0') + 1, std::string::npos);
            if (!v.empty() && v.back() == '.') v.pop_back();
            statText += std::string(key) + "=" + v;
        }
    };
    appendStat("rms");
    appendStat("max");
    std::string summary = "通道 " + std::to_string(args.channel) + "：";
    if (!statText.empty()) summary += statText + "，";
    summary += "返回 " + std::to_string(returned) + "/总 " + std::to_string(total) + " 点";
    if (downsampled) summary += "（降采样）";
    r.summary = summary;
    return r;
}
