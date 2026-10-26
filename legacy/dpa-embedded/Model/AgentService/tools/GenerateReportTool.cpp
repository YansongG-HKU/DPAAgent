#include "tools/GenerateReportTool.h"
#include "StandardsCatalog.h"
#include "AgentJson.h"
#include <QJsonObject>
#include <QString>
using namespace Model;

namespace {
std::string shown(const std::string& value) {
    return value.empty() ? "未提供" : value;
}

void enrichStandardProvenance(ReportProvenance& provenance) {
    if (provenance.standardProfile.empty() || provenance.standardProfile == "none") {
        provenance.standardDisposition = provenance.standardProfile == "none"
            ? "no_standard_threshold_applied" : "standard_profile_missing";
        return;
    }
    for (const auto& entry : standardCatalog()) {
        if (entry.id != provenance.standardProfile) continue;
        provenance.standardAuthority = entry.authority;
        provenance.standardTitle = entry.title;
        provenance.standardStatus = entry.status;
        provenance.standardSourceUrl = entry.sourceUrl;
        provenance.standardDisposition = entry.executionStatus == "execution-approved"
            ? "execution_profile_metadata_verified"
            : "catalog_only_requires_owner_and_authorized_evidence";
        return;
    }
    provenance.standardDisposition = "unknown_profile_not_governed";
}
}

// Pure core (unit-tested). Assembles a Markdown report from real ReportData:
//   - no current test -> not ok, error="no_current_test"
//   - otherwise builds "# title\n## section\nbody\n..." and returns it in dataJson
//     as {"format":"markdown","report":<markdown>} with r.summary in Chinese.
ToolResult Model::buildReportResult(const ReportData& d) {
    ToolResult r;
    if (!d.hasTest) {
        r.ok      = false;
        r.error   = "no_current_test";
        r.summary = "当前没有可生成报告的分析试验";
        return r;
    }

    ReportProvenance provenance = d.provenance;
    // Diagnostic versions are derived only from an assessed diagnostic section. This keeps
    // ordinary reports and unassessed attempts from claiming that diagnostic rules ran.
    provenance.algorithmVersion.clear();
    provenance.ruleVersion.clear();
    for (const auto& sec : d.sections) {
        if (!sec.diagnosticAssessed) continue;
        provenance.algorithmVersion = sec.diagnosticAlgorithmVersion;
        provenance.ruleVersion = sec.diagnosticRuleVersion;
        if (provenance.standardProfile.empty())
            provenance.standardProfile = sec.diagnosticStandardProfile;
        break;
    }
    enrichStandardProvenance(provenance);
    if (provenance.standardPlanDisposition.empty())
        provenance.standardPlanDisposition = "no_standard_plan_bound";

    // Build Markdown string. Provenance is always present; missing source fields stay explicit.
    std::string md = "# " + d.title + "\n\n";
    md += "## 证据溯源\n";
    md += "- 数据 ID：" + shown(provenance.dataId) + "\n";
    md += "- 通道：" + (provenance.channel >= 0 ? std::to_string(provenance.channel) : "未提供") + "\n";
    md += "- 分析名称：" + shown(provenance.analysisName) + "\n";
    md += "- 分析参数：" + shown(provenance.analysisParameters) + "\n";
    md += "- 报告生成器版本：" + shown(provenance.reportGeneratorVersion) + "\n";
    if (!provenance.algorithmVersion.empty())
        md += "- 诊断算法版本：" + provenance.algorithmVersion + "\n";
    if (!provenance.ruleVersion.empty())
        md += "- 诊断规则版本：" + provenance.ruleVersion + "\n";
    md += "- 标准配置：" + shown(provenance.standardProfile) + "\n\n";
    md += "- 标准机构：" + shown(provenance.standardAuthority) + "\n";
    md += "- 标准标题：" + shown(provenance.standardTitle) + "\n";
    md += "- 标准目录状态：" + shown(provenance.standardStatus) + "\n";
    md += "- 标准治理处置：" + shown(provenance.standardDisposition) + "\n";
    md += "- 官方来源：" + shown(provenance.standardSourceUrl) + "\n\n";
    md += "- 标准计划摘要哈希：" + shown(provenance.standardPlanSha256) + "\n";
    md += "- 标准计划绑定状态：" + shown(provenance.standardPlanDisposition) + "\n\n";
    for (const auto& sec : d.sections) {
        md += "## " + sec.title + "\n" + sec.body + "\n\n";
    }

    // Build dataJson: {"format":"markdown","report":<properly-escaped markdown>}
    QJsonObject obj;
    obj["format"] = QString("markdown");
    obj["report"] = QString::fromStdString(md);
    QJsonObject provenanceJson;
    provenanceJson["data_id"] = QString::fromStdString(provenance.dataId);
    provenanceJson["channel"] = provenance.channel >= 0
                               ? QJsonValue(provenance.channel) : QJsonValue(QJsonValue::Null);
    provenanceJson["analysis_name"] = QString::fromStdString(provenance.analysisName);
    provenanceJson["analysis_parameters"] = QString::fromStdString(provenance.analysisParameters);
    provenanceJson["report_generator_version"] = QString::fromStdString(provenance.reportGeneratorVersion);
    if (!provenance.algorithmVersion.empty())
        provenanceJson["algorithm_version"] = QString::fromStdString(provenance.algorithmVersion);
    if (!provenance.ruleVersion.empty())
        provenanceJson["rule_version"] = QString::fromStdString(provenance.ruleVersion);
    provenanceJson["standard_profile"] = QString::fromStdString(provenance.standardProfile);
    provenanceJson["standard_authority"] = QString::fromStdString(provenance.standardAuthority);
    provenanceJson["standard_title"] = QString::fromStdString(provenance.standardTitle);
    provenanceJson["standard_status"] = QString::fromStdString(provenance.standardStatus);
    provenanceJson["standard_source_url"] = QString::fromStdString(provenance.standardSourceUrl);
    provenanceJson["standard_disposition"] = QString::fromStdString(provenance.standardDisposition);
    provenanceJson["standard_plan_sha256"] = QString::fromStdString(provenance.standardPlanSha256);
    provenanceJson["standard_plan_disposition"] = QString::fromStdString(provenance.standardPlanDisposition);
    obj["provenance"] = provenanceJson;
    r.dataJson = Model::AgentJson::dump(obj);

    r.ok      = true;
    r.summary = "已生成报告：" + d.title;
    return r;
}

// Pure core (unit-tested): render grounded diagnostic findings as a Markdown report section.
// Every finding carries its computed evidence plus faultAdvice() root-cause + recommendation,
// so the report's conclusions stay tied to the engine, never free-invented.
ReportSection Model::diagnosticFindingsSection(const std::vector<DiagnosticFinding>& findings) {
    ReportSection sec;
    sec.title = "诊断结论";
    std::string standardProfile;
    for (const auto& finding : findings) {
        if (standardProfile.empty() && !finding.standardProfile.empty())
            standardProfile = finding.standardProfile;
        if (!sec.diagnosticAssessed && finding.assessed) {
            sec.diagnosticAssessed = true;
            sec.diagnosticAlgorithmVersion = finding.algorithmVersion;
            sec.diagnosticRuleVersion = finding.ruleVersion;
            sec.diagnosticStandardProfile = finding.standardProfile;
        }
    }
    sec.body = "- 算法版本：" + shown(sec.diagnosticAlgorithmVersion)
             + "\n- 规则版本：" + shown(sec.diagnosticRuleVersion)
             + "\n- 标准配置：" + shown(standardProfile) + "\n\n";
    if (findings.empty()) {
        sec.body += "未获得已评估的故障特征；这不等同于设备健康保证。";
        return sec;
    }
    auto join = [](const std::vector<std::string>& v) {
        std::string s;
        for (size_t i = 0; i < v.size(); ++i) { if (i) s += "；"; s += v[i]; }
        return s;
    };
    for (const auto& f : findings) {
        if (f.assessed) {
            sec.body += "### " + f.fault + "（" + f.severity + "，规则匹配度 "
                      + std::to_string((int)(f.ruleMatchScore * 100 + 0.5)) + "%）\n";
        } else {
            sec.body += "### " + f.fault + "（未评估）\n";
        }
        sec.body += "- 规则 ID：" + shown(f.ruleId) + "\n";
        sec.body += "- 依据：" + f.evidence + "\n";
        FaultAdvice adv = faultAdvice(f.fault, f.severity);
        if (!adv.rootCauses.empty())      sec.body += "- 可能原因：" + join(adv.rootCauses) + "\n";
        if (!adv.recommendations.empty()) sec.body += "- 建议措施：" + join(adv.recommendations) + "\n";
        sec.body += "\n";
    }
    return sec;
}
