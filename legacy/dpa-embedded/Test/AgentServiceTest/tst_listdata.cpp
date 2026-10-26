#include <QtTest/QtTest>
#include "tools/ListDataTool.h"
#include "AgentJson.h"
#include <QJsonArray>
using namespace Model;
class TstListData : public QObject { Q_OBJECT
private slots:
    void no_test_reports_missing() {
        DataListSnapshot s; s.hasTest=false;
        ToolResult r = buildListDataResult(s);
        QVERIFY(!r.ok); QCOMPARE(QString::fromStdString(r.error), QString("no_current_test"));
    }
    void lists_datasets() {
        DataListSnapshot s; s.hasTest=true;
        s.items = { {"d1","振动1",4}, {"d2","应变A",2} };
        ToolResult r = buildListDataResult(s);
        QVERIFY(r.ok);
        QJsonObject o = AgentJson::parseObject(r.dataJson);
        QCOMPARE(o.value("data").toArray().size(), 2);
        QCOMPARE(o.value("data").toArray().at(0).toObject().value("name").toString(), QString("振动1"));
        QCOMPARE(o.value("data").toArray().at(0).toObject().value("channel_count").toInt(), 4);
    }
};
int runTstListData(int argc,char**argv){ TstListData t; return QTest::qExec(&t,argc,argv); }
#include "tst_listdata.moc"
