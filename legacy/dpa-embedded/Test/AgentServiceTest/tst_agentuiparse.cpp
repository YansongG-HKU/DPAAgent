#include <QtTest/QtTest>
#include "AgentUiParse.h"
#include "Diagnostics.h"   // buildDiagnoseResult, DiagnosticFinding
#include "ToolResult.h"
using namespace Model;
class TstAgentUiParse : public QObject { Q_OBJECT
private slots:
    void parses_real_diagnose_result() {
        // Round-trip the actual producer: buildDiagnoseResult -> ToolResult::toJson -> parse.
        std::vector<DiagnosticFinding> f = {{"bearing_outer","alarm",0.9,"peak BPFO @ 95.6 Hz"}};
        std::string json = buildDiagnoseResult(f).toJson();
        auto ui = parseDiagnoseFindings(json);
        QCOMPARE((int)ui.size(), 1);
        QCOMPARE(QString::fromStdString(ui[0].fault), QString("bearing_outer"));
        QCOMPARE(QString::fromStdString(ui[0].severity), QString("alarm"));
        QVERIFY(QString::fromStdString(ui[0].evidence).contains("BPFO"));
        QCOMPARE(ui[0].ruleMatchScore, 0.9);
        QVERIFY(ui[0].assessed);
        QVERIFY(!ui[0].recommendations.empty());   // faultAdvice folded in
        QVERIFY(!ui[0].rootCauses.empty());
    }
    void parses_unassessed_without_probability_claim() {
        DiagnosticFinding finding{"profile","unassessed",0,"not assessed: missing profile"};
        finding.assessed = false;
        auto ui = parseDiagnoseFindings(buildDiagnoseResult({finding}).toJson());
        QCOMPARE((int)ui.size(), 1);
        QVERIFY(!ui[0].assessed);
        QCOMPARE(ui[0].ruleMatchScore, 0.0);
    }
    void builds_feedback_contract_without_false_confidence_semantics() {
        const std::string json = buildDiagnosticFeedbackJson(
            "needs_more_information", "bearing_outer", "alarm",
            "bearing.characteristic_frequency_corroborated", {"evidence-1"});
        const QJsonObject payload = QJsonDocument::fromJson(
            QByteArray::fromStdString(json)).object();
        QCOMPARE(payload.value("feedbackType").toString(), QString("needs_more_information"));
        QCOMPARE(payload.value("severity").toString(), QString("critical"));
        QCOMPARE(payload.value("correlationId").toString(),
                 QString("bearing.characteristic_frequency_corroborated"));
        QCOMPARE(payload.value("evidenceRefs").toArray().size(), 1);
        QVERIFY(!payload.value("summary").toString().isEmpty());
        QVERIFY(!payload.contains("feedback_type"));
    }
    void empty_for_non_diagnostic() {
        ToolResult r; r.ok = true; r.summary = "done"; r.dataJson = R"({"type":"PSD","started":true})";
        QVERIFY(parseDiagnoseFindings(r.toJson()).empty());
    }
    void empty_for_garbage() {
        QVERIFY(parseDiagnoseFindings("not json").empty());
        QVERIFY(parseDiagnoseFindings("{}").empty());
    }
};
int runTstAgentUiParse(int argc,char**argv){ TstAgentUiParse t; return QTest::qExec(&t,argc,argv); }
#include "tst_agentuiparse.moc"
