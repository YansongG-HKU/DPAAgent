#include <QtTest/QtTest>
#include "tools/ResultSummaryTool.h"
#include "AgentJson.h"
using namespace Model;
class TstResultSummary : public QObject { Q_OBJECT
private slots:
    // No available result -> not ok, distinct "no_result" error code.
    void no_result_reports_error() {
        ResultSummary s; s.found = false;
        ToolResult r = buildResultSummaryResult(s);
        QVERIFY(!r.ok);
        QCOMPARE(QString::fromStdString(r.error), QString("no_result"));
    }
    // A found result formats name + peaks + stats into the result JSON.
    void formats_peaks_and_stats() {
        ResultSummary s;
        s.found = true;
        s.channelId = 7;
        s.name  = "PSD@通道1";
        s.xDomain = "frequency";
        s.peaks = { {"peak_freq_hz", 50.0}, {"peak_value", 1.2} };
        s.stats = { {"rms", 0.7} };
        ToolResult r = buildResultSummaryResult(s);
        QVERIFY(r.ok);
        QJsonObject d = AgentJson::parseObject(r.dataJson);
        QCOMPARE(d.value("channel_id").toInt(), 7);
        QCOMPARE(d.value("name").toString(), QString("PSD@通道1"));
        QCOMPARE(d.value("x_domain").toString(), QString("frequency"));
        QCOMPARE(d.value("peaks").toObject().value("peak_freq_hz").toDouble(), 50.0);
        QCOMPARE(d.value("stats").toObject().value("rms").toDouble(), 0.7);
    }
};
int runTstResultSummary(int argc,char**argv){ TstResultSummary t; return QTest::qExec(&t,argc,argv); }
#include "tst_resultsummary.moc"
