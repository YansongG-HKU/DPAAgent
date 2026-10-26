#include <QtTest/QtTest>
#include "tools/SituationAssembler.h"
#include "AgentJson.h"
#include <QJsonObject>
using namespace Model;
class TstSituation : public QObject { Q_OBJECT
private slots:
    void no_test_reports_missing() {
        SoftwareStateSnapshot s; s.hasTest = false;
        ToolResult r = buildSituation(s);
        QVERIFY(!r.ok);
        QCOMPARE(QString::fromStdString(r.error), QString("no_current_test"));
    }
    void assembles_compact_state() {
        SoftwareStateSnapshot s;
        s.hasTest = true; s.projectName = "P1"; s.testName = "分析1";
        s.datasets = { DataItem{"d1","数据A",8}, DataItem{"d2","数据B",16} };
        s.currentAnalysisType = "PSD";
        s.hasResult = true; s.resultName = "PSD结果";
        s.resultPeaks = { {"peak_freq_hz", 50.0}, {"peak_value", 1.2} };
        s.resultStats = { {"rms", 0.7} };
        ToolResult r = buildSituation(s);
        QVERIFY(r.ok);
        QJsonObject o = AgentJson::parseObject(r.dataJson);
        QCOMPARE(o.value("test").toString(), QString("分析1"));
        QCOMPARE(o.value("datasets").toArray().size(), 2);
        QCOMPARE(o.value("datasets").toArray().at(0).toObject().value("channels").toInt(), 8);
        QCOMPARE(o.value("current_analysis").toString(), QString("PSD"));
        QCOMPARE(o.value("latest_result").toObject().value("peaks").toObject().value("peak_freq_hz").toDouble(), 50.0);
        QVERIFY(QString::fromStdString(r.summary).contains(QString("分析1")));
    }
};
int runTstSituation(int argc,char**argv){ TstSituation t; return QTest::qExec(&t,argc,argv); }
#include "tst_situation.moc"
