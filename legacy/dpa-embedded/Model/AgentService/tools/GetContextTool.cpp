#include "tools/GetContextTool.h"
#include "AgentJson.h"
#include "iTest.h"
#include <QJsonObject>
#include <QJsonArray>
using namespace Model;
ToolResult Model::buildContextResult(const ContextSnapshot& s) {
    ToolResult r;
    if (!s.hasTest) { r.ok=false; r.error="no_current_test"; r.summary="当前没有打开的分析试验"; return r; }
    QJsonObject o;
    o["project"] = QString::fromStdString(s.projectName);
    o["test"]    = QString::fromStdString(s.testName);
    QJsonArray chs;
    for (auto& c : s.channels) {
        QJsonObject co;
        co["id"]=c.id; co["name"]=QString::fromStdString(c.name);
        co["unit"]=QString::fromStdString(c.unit); co["sampling_rate"]=c.samplingRate;
        co["data_id"] = QString::fromStdString(c.dataId);
        co["data_name"] = QString::fromStdString(c.dataName);
        co["physical_type"] = QString::fromStdString(c.physicalType);
        chs.append(co);
    }
    o["channels"] = chs;
    QJsonObject analysis;
    analysis["present"] = s.hasAnalysis;
    if (s.hasAnalysis) {
        analysis["type"] = QString::fromStdString(s.analysisType);
        analysis["category"] = QString::fromStdString(s.analysisCategory);
        analysis["result_function_type"] = QString::fromStdString(s.resultFunctionType);
        analysis["frame_size"] = s.frameSize;
        analysis["average_num"] = s.averageNum;
        analysis["overlap_index"] = s.overlapIndex;
    }
    o["current_analysis"] = analysis;
    r.dataJson = Model::AgentJson::dump(o);
    r.summary  = "试验「" + s.testName + "」共 " + std::to_string(s.channels.size()) + " 个通道";
    return r;
}
ContextSnapshot Model::snapshotFromTest(M* dpaTest, M* project) {
    ContextSnapshot s;
    if (!dpaTest) return s;       // hasTest stays false

    s.hasTest = true;
    s.testName = dpaTest->value<std::string>("Name");

    if (project) s.projectName = project->value<std::string>("Name");

    M* process = dpaTest->value<M*>("PlotSet", "CurrentLayout", "CurrentCell", "Process");
    if (process) {
        s.hasAnalysis = true;
        s.analysisType = process->value<std::string>("ProcessType");
        s.analysisCategory = process->value<std::string>("ProcessCategory");
        s.resultFunctionType = process->value<std::string>("ResultFunctionType");
        s.frameSize = process->value<int>("FrameSize");
        s.averageNum = process->value<int>("AverageNum");
        s.overlapIndex = process->value<int>("OverLapIndex");
    }

    const int dataCount = dpaTest->childrenCount("WorkSpace");
    for (int dataIndex = 0; dataIndex < dataCount; ++dataIndex) {
        const std::string dataId = dpaTest->value<std::string>("WorkSpace", dataIndex, "Id");
        const std::string dataName = dpaTest->value<std::string>("WorkSpace", dataIndex, "Name");
        const int channelCount = dpaTest->childrenCount("WorkSpace", dataIndex, "Channels");
        for (int channelIndex = 0; channelIndex < channelCount; ++channelIndex) {
            auto channel = dpaTest->childSelf("WorkSpace", dataIndex, "Channels", channelIndex);
            if (!channel) continue;

            ChannelInfo info;
            info.id = channel->value<int>("ChannelId");
            info.name = channel->value<std::string>("Name");
            info.unit = channel->value<std::string>("Unit");
            info.samplingRate = channel->value<double>("SamplingRate");
            info.dataId = dataId;
            info.dataName = dataName;
            info.physicalType = channel->value<std::string>("PhysicType");
            s.channels.push_back(info);
        }
    }
    return s;
}
