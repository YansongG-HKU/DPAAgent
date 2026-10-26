#include <QtTest/QtTest>
#include "tools/ReadChannelTool.h"
#include "AgentJson.h"
#include <QJsonArray>
using namespace Model;
class TstReadChannel : public QObject { Q_OBJECT
private slots:
    // Privacy/error contract: an unfound series must not produce a payload.
    void not_found_reports_error() {
        ChannelSeries s; s.found=false;
        ReadChannelArgs args; args.dataId="d"; args.channel=0;
        ToolResult r = buildReadChannelResult(s, args);
        QVERIFY(!r.ok);
        QCOMPARE(QString::fromStdString(r.error), QString("channel_not_found"));
    }
    // Privacy contract: a long series must be stride-downsampled to <= maxPoints.
    void caps_to_maxpoints() {
        ChannelSeries s; s.found=true;
        for (int i=0;i<1000;++i){ s.x.push_back(i); s.y.push_back(i*0.5); }
        ReadChannelArgs args; args.dataId="d"; args.channel=0; args.maxPoints=100;
        ToolResult r = buildReadChannelResult(s, args);
        QVERIFY(r.ok);
        QJsonObject o = AgentJson::parseObject(r.dataJson);
        QCOMPARE(o.value("total_points").toInt(), 1000);
        QVERIFY(o.value("returned_points").toInt() <= 100);
        QVERIFY(o.value("downsampled").toBool());
        // The emitted arrays must honor the cap (never the full raw signal).
        QVERIFY(o.value("y").toArray().size() <= 100);
        QCOMPARE(o.value("returned_points").toInt(), o.value("y").toArray().size());
    }
    // Statistics must always be surfaced.
    void passes_stats() {
        ChannelSeries s; s.found=true;
        s.x = {0,1,2}; s.y = {1,2,3};
        s.stats = { {"rms", 2.5} };
        ReadChannelArgs args; args.dataId="d"; args.channel=0;
        ToolResult r = buildReadChannelResult(s, args);
        QVERIFY(r.ok);
        QJsonObject o = AgentJson::parseObject(r.dataJson);
        QCOMPARE(o.value("stats").toObject().value("rms").toDouble(), 2.5);
    }
    // A short series is returned whole, not flagged downsampled.
    void short_series_not_downsampled() {
        ChannelSeries s; s.found=true;
        s.x = {0,1,2}; s.y = {10,20,30};
        ReadChannelArgs args; args.dataId="d"; args.channel=0; args.maxPoints=512;
        ToolResult r = buildReadChannelResult(s, args);
        QVERIFY(r.ok);
        QJsonObject o = AgentJson::parseObject(r.dataJson);
        QCOMPARE(o.value("total_points").toInt(), 3);
        QCOMPARE(o.value("returned_points").toInt(), 3);
        QVERIFY(!o.value("downsampled").toBool());
    }
    // Arg parsing defaults: maxPoints defaults to 512, start/len to 0.
    void parse_defaults() {
        ReadChannelArgs a = parseReadChannelArgs(R"({"dataId":"d","channel":3})");
        QCOMPARE(QString::fromStdString(a.dataId), QString("d"));
        QCOMPARE(a.channel, 3);
        QCOMPARE(a.maxPoints, 512);
        QCOMPARE(a.start, 0.0);
        QCOMPARE(a.len, 0.0);
    }
    // Arg parsing: explicit values are read; maxPoints clamps to a sane max.
    void parse_explicit_and_clamp() {
        ReadChannelArgs a = parseReadChannelArgs(
            R"({"dataId":"x","channel":2,"start":1.5,"len":3.0,"maxPoints":99999})");
        QCOMPARE(QString::fromStdString(a.dataId), QString("x"));
        QCOMPARE(a.channel, 2);
        QCOMPARE(a.start, 1.5);
        QCOMPARE(a.len, 3.0);
        QVERIFY(a.maxPoints <= 5000);
        QVERIFY(a.maxPoints > 0);
    }
};
int runTstReadChannel(int argc,char**argv){ TstReadChannel t; return QTest::qExec(&t,argc,argv); }
#include "tst_readchannel.moc"
