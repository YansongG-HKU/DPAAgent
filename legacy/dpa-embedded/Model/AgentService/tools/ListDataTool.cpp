#include "tools/ListDataTool.h"
#include "AgentJson.h"
#include <QJsonObject>
#include <QJsonArray>
using namespace Model;
ToolResult Model::buildListDataResult(const DataListSnapshot& s) {
    ToolResult r;
    if (!s.hasTest) { r.ok=false; r.error="no_current_test"; r.summary="当前没有打开的分析试验"; return r; }
    QJsonObject o; QJsonArray arr;
    for (auto& it : s.items) {
        QJsonObject jo; jo["id"]=QString::fromStdString(it.id);
        jo["name"]=QString::fromStdString(it.name); jo["channel_count"]=it.channelCount;
        arr.append(jo);
    }
    o["data"]=arr;
    r.dataJson = Model::AgentJson::dump(o);
    r.summary  = "工作区共 " + std::to_string(s.items.size()) + " 个数据集";
    return r;
}
