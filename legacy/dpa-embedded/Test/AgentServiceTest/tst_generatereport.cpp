#include <QtTest/QtTest>
#include "tools/GenerateReportTool.h"
#include "AgentJson.h"
#include "iTest.h"
using namespace Model;
class TstGenerateReport : public QObject { Q_OBJECT
private slots:
    // No current test -> not ok, error == "no_current_test".
    void no_test_reports_error() {
        ReportData d; d.hasTest = false;
        ToolResult r = buildReportResult(d);
        QVERIFY(!r.ok);
        QCOMPARE(QString::fromStdString(r.error), QString("no_current_test"));
    }
    // With a test and two sections -> ok, dataJson has format=="markdown" and
    // the report string contains the expected header and section titles/bodies.
    void assembles_markdown() {
        ReportData d;
        d.hasTest = true;
        d.title   = "分析试验1";
        d.provenance.dataId = "data-01";
        d.provenance.channel = 3;
        d.provenance.analysisName = "PSD";
        d.provenance.analysisParameters = "frameSize=4096";
        d.provenance.standardProfile = kNoStandardProfile;
        d.sections.push_back({"数据来源", "通道:4"});
        d.sections.push_back({"说明",     "无编造"});
        ToolResult r = buildReportResult(d);
        QVERIFY(r.ok);
        QJsonObject obj = AgentJson::parseObject(r.dataJson);
        QCOMPARE(obj.value("format").toString(), QString("markdown"));
        QString report = obj.value("report").toString();
        QVERIFY(report.contains("# 分析试验1"));
        QVERIFY(report.contains("## 证据溯源"));
        QVERIFY(report.contains("数据 ID：data-01"));
        QVERIFY(report.contains("通道：3"));
        QVERIFY(report.contains(kReportGeneratorVersion));
        QVERIFY(!report.contains(kDiagnosticRuleVersion));
        QVERIFY(!report.contains("诊断算法版本"));
        QVERIFY(!report.contains("诊断规则版本"));
        QVERIFY(report.contains("## 数据来源"));
        QVERIFY(report.contains("通道:4"));
        QVERIFY(report.contains("## 说明"));
        QVERIFY(report.contains("无编造"));
        QJsonObject provenance = obj.value("provenance").toObject();
        QCOMPARE(provenance.value("data_id").toString(), QString("data-01"));
        QCOMPARE(provenance.value("channel").toInt(), 3);
        QCOMPARE(provenance.value("report_generator_version").toString(), QString(kReportGeneratorVersion));
        QVERIFY(!provenance.contains("algorithm_version"));
        QVERIFY(!provenance.contains("rule_version"));
        QCOMPARE(provenance.value("standard_profile").toString(), QString(kNoStandardProfile));
    }
    void diagnostic_section_grounds_findings() {
        // No findings -> honest note, never a fabricated fault.
        ReportSection empty = diagnosticFindingsSection({});
        QCOMPARE(QString::fromStdString(empty.title), QString("诊断结论"));
        QVERIFY(empty.body.find("未获得已评估的故障特征") != std::string::npos);
        QVERIFY(!empty.diagnosticAssessed);
        // A finding -> its label, evidence, and grounded recommendation all appear.
        DiagnosticFinding finding{"bearing_outer","alarm",0.9,"BPFO @ 162 Hz"};
        finding.ruleId = "bearing.characteristic_frequency";
        finding.standardProfile = kNoStandardProfile;
        std::vector<DiagnosticFinding> f = {finding};
        ReportSection s = diagnosticFindingsSection(f);
        QVERIFY(s.diagnosticAssessed);
        QVERIFY(s.body.find("bearing_outer") != std::string::npos);
        QVERIFY(s.body.find("BPFO") != std::string::npos);
        QVERIFY(s.body.find("建议措施") != std::string::npos);
        QVERIFY(s.body.find("规则匹配度 90%") != std::string::npos);
        QVERIFY(s.body.find("置信") == std::string::npos);
        QVERIFY(s.body.find(kDiagnosticRuleVersion) != std::string::npos);
        QVERIFY(s.body.find(kNoStandardProfile) != std::string::npos);
        // Composed into a full report, the section is present under its heading.
        ReportData d; d.hasTest = true; d.title = "机组A";
        d.sections.push_back(s);
        QJsonObject reportObject = AgentJson::parseObject(buildReportResult(d).dataJson);
        QString report = reportObject.value("report").toString();
        QVERIFY(report.contains("## 诊断结论"));
        QJsonObject provenance = reportObject.value("provenance").toObject();
        QCOMPARE(provenance.value("algorithm_version").toString(), QString(kDiagnosticAlgorithmVersion));
        QCOMPARE(provenance.value("rule_version").toString(), QString(kDiagnosticRuleVersion));
        QCOMPARE(provenance.value("standard_profile").toString(), QString(kNoStandardProfile));
        QCOMPARE(provenance.value("standard_plan_disposition").toString(),
                 QString("no_standard_plan_bound"));
    }
    void report_carries_catalog_provenance_without_full_text() {
        ReportData d;
        d.hasTest = true;
        d.title = "标准目录溯源";
        d.provenance.standardProfile = "iso-20816-1:2016";
        const ToolResult result = buildReportResult(d);
        QVERIFY(result.ok);
        const QJsonObject root = AgentJson::parseObject(result.dataJson);
        const QJsonObject provenance = root.value("provenance").toObject();
        QCOMPARE(provenance.value("standard_authority").toString(), QString("ISO"));
        QCOMPARE(provenance.value("standard_status").toString(), QString("published"));
        QCOMPARE(provenance.value("standard_disposition").toString(),
                 QString("catalog_only_requires_owner_and_authorized_evidence"));
        QVERIFY(root.value("report").toString().contains("标准治理处置"));
        QVERIFY(!root.value("report").toString().contains("全文"));
        QCOMPARE(provenance.value("standard_plan_disposition").toString(),
                 QString("no_standard_plan_bound"));
    }
    void unassessed_finding_has_no_score_claim() {
        DiagnosticFinding finding{"severity","unassessed",0,"not assessed: applicability not confirmed"};
        finding.assessed = false;
        finding.ruleId = "severity.legacy_profile.applicability";
        finding.standardProfile = kLegacyIso10816ClassProfile;
        ReportSection section = diagnosticFindingsSection({finding});
        QVERIFY(!section.diagnosticAssessed);
        QVERIFY(section.body.find("未评估") != std::string::npos);
        QVERIFY(section.body.find("规则匹配度") == std::string::npos);
        QVERIFY(section.body.find(kLegacyIso10816ClassProfile) != std::string::npos);
        ReportData data; data.hasTest = true; data.title = "未评估报告";
        data.sections.push_back(section);
        QJsonObject provenance = AgentJson::parseObject(buildReportResult(data).dataJson)
                                     .value("provenance").toObject();
        QVERIFY(!provenance.contains("algorithm_version"));
        QVERIFY(!provenance.contains("rule_version"));
    }
    void snapshots_existing_process_parameters_only() {
        std::string processType = "FFT";
        std::string physicalType = "vibration";
        int resultDataForm = 17;
        int axisTypeX = 1;
        int frameSize = 4096;
        int averageNum = 8;
        int overlapIndex = 2;
        double overlapValue = 0.5;
        int windowType = 3;
        M process;
        process.setProperty("Process", &process, Property::TYPE_Class);
        process.addProperty("ProcessType", &processType, Property::TYPE_StdString);
        process.addProperty("PhysicType", &physicalType, Property::TYPE_StdString);
        process.addProperty("ResultDataForm", &resultDataForm, Property::TYPE_Int);
        process.addProperty("AxisTypeX", &axisTypeX, Property::TYPE_Int);
        process.addProperty("FrameSize", &frameSize, Property::TYPE_Int);
        process.addProperty("AverageNum", &averageNum, Property::TYPE_Int);
        process.addProperty("OverLapIndex", &overlapIndex, Property::TYPE_Int);
        process.addProperty("OverLapValue", &overlapValue, Property::TYPE_Double);
        process.addProperty("WinType", &windowType, Property::TYPE_Int);

        QJsonObject snapshot = AgentJson::parseObject(snapshotAnalysisParameters(&process));
        QCOMPARE(snapshot.value("process_type").toString(), QString("FFT"));
        QCOMPARE(snapshot.value("physical_type").toString(), QString("vibration"));
        QCOMPARE(snapshot.value("result_data_form").toInt(), 17);
        QCOMPARE(snapshot.value("axis_type_x").toInt(), 1);
        QCOMPARE(snapshot.value("frame_size").toInt(), 4096);
        QCOMPARE(snapshot.value("average_num").toInt(), 8);
        QCOMPARE(snapshot.value("overlap_index").toInt(), 2);
        QCOMPARE(snapshot.value("overlap_value").toDouble(), 0.5);
        QCOMPARE(snapshot.value("window_type").toInt(), 3);
        QVERIFY(!snapshot.contains("axis_type_y"));
    }
};
int runTstGenerateReport(int argc, char** argv) { TstGenerateReport t; return QTest::qExec(&t, argc, argv); }
#include "tst_generatereport.moc"
