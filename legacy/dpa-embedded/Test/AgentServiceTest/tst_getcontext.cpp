#include <QtTest/QtTest>
#include "tools/GetContextTool.h"
#include "AgentJson.h"
#include "iTest.h"
#include <QJsonArray>
using namespace Model;

class TstGetContext : public QObject { Q_OBJECT
private slots:
    void no_test_reports_missing() {
        ContextSnapshot s; s.hasTest=false;
        ToolResult r = buildContextResult(s);
        QVERIFY(!r.ok);
        QCOMPARE(QString::fromStdString(r.error), QString("no_current_test"));
    }
    void lists_channels() {
        ContextSnapshot s; s.hasTest=true; s.projectName="P"; s.testName="分析1";
        s.channels = { {1,"通道1","mV",2000.0,"data-1","采集1"}, {2,"通道2","g",2000.0,"data-1","采集1"} };
        s.hasAnalysis = true; s.analysisType = "FFT"; s.analysisCategory = "SpectrumDominProcess";
        s.resultFunctionType = "Spectrum"; s.frameSize = 4096; s.averageNum = 4; s.overlapIndex = 3;
        ToolResult r = buildContextResult(s);
        QVERIFY(r.ok);
        QJsonObject o = AgentJson::parseObject(r.dataJson);
        QCOMPARE(o.value("test").toString(), QString("分析1"));
        QCOMPARE(o.value("channels").toArray().size(), 2);
        QCOMPARE(o.value("channels").toArray().at(0).toObject().value("name").toString(), QString("通道1"));
        QCOMPARE(o.value("channels").toArray().at(0).toObject().value("data_id").toString(), QString("data-1"));
        QCOMPARE(o.value("current_analysis").toObject().value("type").toString(), QString("FFT"));
        QCOMPARE(o.value("current_analysis").toObject().value("frame_size").toInt(), 4096);
    }
    void snapshots_current_property_tree() {
        std::string projectName = "机组项目", testName = "分析试验A";
        int channelId = 7;
        std::string channelName = "振动X", unit = "g", physicalType = "Acceleration";
        double samplingRate = 51200.0;

        M channel;
        channel.setProperty("Channel", &channel, Property::TYPE_Class);
        channel.addProperty("ChannelId", &channelId, Property::TYPE_Int);
        channel.addProperty("Name", &channelName, Property::TYPE_StdString);
        channel.addProperty("Unit", &unit, Property::TYPE_StdString);
        channel.addProperty("PhysicType", &physicalType, Property::TYPE_StdString);
        channel.addProperty("SamplingRate", &samplingRate, Property::TYPE_Double);
        MList channels;
        channels.addChildRef(&channel);
        std::string dataId = "dataset-1", dataName = "泵振动";
        M data;
        data.setProperty("Data", &data, Property::TYPE_Class);
        data.addProperty("Id", &dataId, Property::TYPE_StdString);
        data.addProperty("Name", &dataName, Property::TYPE_StdString);
        data.addProperty("Channels", &channels, Property::TYPE_Class);
        MList workSpace;
        workSpace.addChildRef(&data);
        M test;
        test.setProperty("Test", &test, Property::TYPE_Class);
        test.addProperty("Name", &testName, Property::TYPE_StdString);
        test.addProperty("WorkSpace", &workSpace, Property::TYPE_Class);
        M project;
        project.setProperty("Project", &project, Property::TYPE_Class);
        project.addProperty("Name", &projectName, Property::TYPE_StdString);

        ContextSnapshot s = snapshotFromTest(&test, &project);

        QVERIFY(s.hasTest);
        QCOMPARE(QString::fromStdString(s.projectName), QString("机组项目"));
        QCOMPARE(QString::fromStdString(s.testName), QString("分析试验A"));
        QCOMPARE(s.channels.size(), size_t(1));
        QCOMPARE(s.channels[0].id, 7);
        QCOMPARE(QString::fromStdString(s.channels[0].name), QString("振动X"));
        QCOMPARE(QString::fromStdString(s.channels[0].unit), QString("g"));
        QCOMPARE(s.channels[0].samplingRate, 51200.0);
        QCOMPARE(QString::fromStdString(s.channels[0].dataId), QString("dataset-1"));
        QCOMPARE(QString::fromStdString(s.channels[0].dataName), QString("泵振动"));
        QCOMPARE(QString::fromStdString(s.channels[0].physicalType), QString("Acceleration"));
    }
};
int runTstGetContext(int argc,char**argv){ TstGetContext t; return QTest::qExec(&t,argc,argv); }
#include "tst_getcontext.moc"
