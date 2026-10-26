#include "tools/GenerateReportTool.h"
#include "tools/ResultSummaryTool.h"
#include "iTest.h"   // Model::M / Model::iTest via HeaderDefine + iData.h
#include "iDataProcess.h"
#include <QJsonDocument>
#include <QJsonObject>
using namespace Model;

namespace {
bool hasProperty(M* object, const char* name) {
    return object && object->child(name) != nullptr;
}

void addStringProperty(QJsonObject& out, M* object, const char* property, const char* jsonName) {
    if (hasProperty(object, property))
        out[QString::fromLatin1(jsonName)] = QString::fromStdString(object->value<std::string>(property));
}

void addIntProperty(QJsonObject& out, M* object, const char* property, const char* jsonName) {
    if (hasProperty(object, property))
        out[QString::fromLatin1(jsonName)] = object->value<int>(property);
}

void addDoubleProperty(QJsonObject& out, M* object, const char* property, const char* jsonName) {
    if (hasProperty(object, property))
        out[QString::fromLatin1(jsonName)] = object->value<double>(property);
}
}

std::string Model::snapshotAnalysisParameters(M* process) {
    if (!process) return {};
    QJsonObject parameters;
    addStringProperty(parameters, process, "ProcessType", "process_type");
    addStringProperty(parameters, process, "PhysicType", "physical_type");
    addStringProperty(parameters, process, "ResultDataType", "result_data_type");
    addStringProperty(parameters, process, "ResultFunctionType", "result_function_type");
    addStringProperty(parameters, process, "SourceFunctionType", "source_function_type");
    addIntProperty(parameters, process, "ResultDataForm", "result_data_form");
    addIntProperty(parameters, process, "AxisTypeX", "axis_type_x");
    addIntProperty(parameters, process, "AxisTypeY", "axis_type_y");
    addIntProperty(parameters, process, "FrameSize", "frame_size");
    addIntProperty(parameters, process, "FrameNumIndex", "frame_num_index");
    addIntProperty(parameters, process, "AverageNum", "average_num");
    addIntProperty(parameters, process, "OverLapIndex", "overlap_index");
    addDoubleProperty(parameters, process, "OverLapValue", "overlap_value");
    addIntProperty(parameters, process, "WinType", "window_type");
    if (parameters.isEmpty()) return {};
    return QJsonDocument(parameters).toJson(QJsonDocument::Compact).toStdString();
}

// Adapter: framework-coupled, plugin-only, NOT unit-tested (only build-verified).
// Gathers real data from the current analysis test to populate a ReportData.
// Null-guards every access. Uses honest placeholders when data is unavailable.
ReportData Model::gatherReportData(M* dpaTest) {
    ReportData d;
    if (!dpaTest) return d;   // hasTest stays false

    d.hasTest = true;

    // Report title: the test name
    d.title = dpaTest->value<std::string>("Name");
    if (d.title.empty()) d.title = "分析试验";

    // Provenance comes from the current process's actual first source selection. Do not
    // guess a source from the workspace when the process has no resolvable selection.
    iTest* test = static_cast<iTest*>(dpaTest);
    iDataProcess* process = test->value<iDataProcess*>("PlotSet", "CurrentLayout", "CurrentCell", "Process");
    if (process) {
        d.provenance.analysisName = process->value<std::string>("Name");
        if (d.provenance.analysisName.empty())
            d.provenance.analysisName = process->value<std::string>("ProcessType");
        d.provenance.analysisParameters = snapshotAnalysisParameters(process);
        if (process->childrenCount("Units") > 0) {
            d.provenance.dataId = process->value<std::string>("Units", 0, "SourceSelect", "Data", "Id");
            if (!d.provenance.dataId.empty())
                d.provenance.channel = process->value<int>("Units", 0, "SourceSelect", "ChannelId");
        }
    }

    // Section 1: 数据来源 — enumerate workspace datasets
    {
        ReportSection sec;
        sec.title = "数据来源";
        const int count = dpaTest->childrenCount("WorkSpace");
        if (count == 0) {
            sec.body = "（工作区暂无数据集）";
        } else {
            for (int i = 0; i < count; ++i) {
                std::string name = dpaTest->value<std::string>("WorkSpace", i, "Name");
                std::string id = dpaTest->value<std::string>("WorkSpace", i, "Id");
                int chCount = dpaTest->childrenCount("WorkSpace", i, "Channels");
                if (!sec.body.empty()) sec.body += "\n";
                sec.body += "- " + (name.empty() ? std::string("数据集") + std::to_string(i + 1) : name)
                          + "（ID: " + (id.empty() ? std::string("未提供") : id)
                          + "，" + std::to_string(chCount) + " 通道）";
            }
        }
        d.sections.push_back(std::move(sec));
    }

    // Section 2: 分析结果 — use ResultSummary if available
    {
        ReportSection sec;
        sec.title = "分析结果";
        ResultSummary rs = resultSummaryFromTest(dpaTest);
        if (rs.found) {
            sec.body = "结果名称：" + rs.name;
            if (!rs.peaks.empty()) {
                auto pf = rs.peaks.find("peak_freq_hz");
                auto pv = rs.peaks.find("peak_value");
                if (pf != rs.peaks.end())
                    sec.body += "\n主峰频率：" + std::to_string(pf->second) + " Hz";
                else {
                    auto px = rs.peaks.find("peak_x");
                    if (px != rs.peaks.end())
                        sec.body += "\n主峰 X（" + (rs.xDomain.empty() ? std::string("other") : rs.xDomain)
                                  + "）：" + std::to_string(px->second);
                }
                if (pv != rs.peaks.end())
                    sec.body += "\n主峰值：" + std::to_string(pv->second);
            }
            for (const auto& kv : rs.stats)
                sec.body += "\n" + kv.first + "：" + std::to_string(kv.second);
        } else {
            sec.body = "（尚无分析结果）";
        }
        d.sections.push_back(std::move(sec));
    }

    // Section 3: 说明 — fixed honest note
    {
        ReportSection sec;
        sec.title = "说明";
        sec.body  = "本报告由当前分析试验的真实数据生成，不包含任何编造数值。";
        d.sections.push_back(std::move(sec));
    }

    return d;
}
