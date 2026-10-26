#include "tools/ResultSummaryTool.h"
#include "AgentJson.h"
#include <QJsonObject>
#include <QString>
using namespace Model;

// Pure core (unit-tested). Formats a ResultSummary into a ToolResult:
//   - no available result -> not ok, distinct "no_result" error code,
//   - otherwise a JSON { name, peaks:{...}, stats:{...} } plus a Chinese
//     one-line summary stitching the headline metrics.
ToolResult Model::buildResultSummaryResult(const ResultSummary& s) {
    ToolResult r;
    if (!s.found) {
        r.ok = false;
        r.error = "no_result";
        r.summary = "当前没有可用的分析结果";
        return r;
    }

    QJsonObject peaks;
    for (const auto& kv : s.peaks) peaks[QString::fromStdString(kv.first)] = kv.second;
    QJsonObject stats;
    for (const auto& kv : s.stats) stats[QString::fromStdString(kv.first)] = kv.second;

    QJsonObject d;
    d["channel_id"] = s.channelId;
    d["name"]  = QString::fromStdString(s.name);
    d["x_domain"] = QString::fromStdString(s.xDomain);
    d["peaks"] = peaks;
    d["stats"] = stats;
    r.dataJson = Model::AgentJson::dump(d);

    // One-line headline: prefer the main peak, then append a couple of stats.
    std::string metrics;
    auto append = [&metrics](const std::string& label, double v) {
        if (!metrics.empty()) metrics += "，";
        metrics += label + "=" + QString::number(v).toStdString();
    };
    auto pf = s.peaks.find("peak_freq_hz");
    auto px = s.peaks.find("peak_x");
    auto pv = s.peaks.find("peak_value");
    if (pf != s.peaks.end()) append("主峰频率(Hz)", pf->second);
    else if (px != s.peaks.end()) append("主峰X", px->second);
    if (pv != s.peaks.end()) append("主峰值", pv->second);
    for (const auto& kv : s.stats) append(kv.first, kv.second);

    r.ok = true;
    r.summary = "结果「" + s.name + "」：" + (metrics.empty() ? std::string("无关键指标") : metrics);
    return r;
}
