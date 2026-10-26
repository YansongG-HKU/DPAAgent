#include "tools/SituationAssembler.h"
#include "AgentJson.h"
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
using namespace Model;

static QJsonObject mapToJson(const std::map<std::string,double>& m) {
    QJsonObject o;
    for (const auto& kv : m) o[QString::fromStdString(kv.first)] = kv.second;
    return o;
}

ToolResult Model::buildSituation(const SoftwareStateSnapshot& s) {
    ToolResult r;
    if (!s.hasTest) {
        r.ok = false; r.error = "no_current_test";
        r.summary = "当前没有打开的分析试验";
        return r;
    }
    QJsonObject o;
    o["project"] = QString::fromStdString(s.projectName);
    o["test"]    = QString::fromStdString(s.testName);
    QJsonArray ds;
    for (const auto& d : s.datasets) {
        QJsonObject dj;
        dj["id"]       = QString::fromStdString(d.id);
        dj["name"]     = QString::fromStdString(d.name);
        dj["channels"] = d.channelCount;
        ds.append(dj);
    }
    o["datasets"] = ds;
    o["current_analysis"] = QString::fromStdString(s.currentAnalysisType);
    if (s.hasResult) {
        QJsonObject res;
        res["name"]  = QString::fromStdString(s.resultName);
        res["peaks"] = mapToJson(s.resultPeaks);
        res["stats"] = mapToJson(s.resultStats);
        o["latest_result"] = res;
    }
    r.dataJson = AgentJson::dump(o);
    r.summary  = "试验「" + s.testName + "」，" + std::to_string(s.datasets.size()) + " 个数据集"
               + (s.currentAnalysisType.empty() ? std::string() : ("，当前分析: " + s.currentAnalysisType))
               + (s.hasResult ? std::string("，有分析结果") : std::string());
    return r;
}
