#include <QtTest/QtTest>
#include "ToolResult.h"
#include "AgentJson.h"
using namespace Model;
class TstToolResult : public QObject { Q_OBJECT
private slots:
    void ok_result_serializes() {
        ToolResult r; r.ok = true; r.summary = "done"; r.dataJson = R"({"rows":3})";
        QJsonObject o = AgentJson::parseObject(r.toJson());
        QCOMPARE(o.value("ok").toBool(), true);
        QCOMPARE(o.value("summary").toString(), QString("done"));
        QCOMPARE(o.value("data").toObject().value("rows").toInt(), 3);
    }
    void error_result_serializes() {
        ToolResult r; r.ok = false; r.error = "no_test";
        QJsonObject o = AgentJson::parseObject(r.toJson());
        QCOMPARE(o.value("ok").toBool(), false);
        QCOMPARE(o.value("error").toString(), QString("no_test"));
    }
    void images_serialize_as_metadata_without_base64() {
        ToolResult r; r.ok = true; r.summary = "shot";
        ImageArtifact im; im.base64 = "QUJD"; im.mediaType = "image/png"; im.caption = "PSD";
        r.images.push_back(im);
        std::string js = r.toJson();
        QVERIFY(!QString::fromStdString(js).contains("QUJD"));   // base64 must NOT be in the text channel
        QJsonObject o = AgentJson::parseObject(js);
        QCOMPARE(o.value("images").toArray().size(), 1);
        QCOMPARE(o.value("images").toArray().at(0).toObject().value("media_type").toString(), QString("image/png"));
        QCOMPARE(o.value("images").toArray().at(0).toObject().value("caption").toString(), QString("PSD"));
    }
    void evidence_refs_serialize_as_opaque_ids() {
        ToolResult r; r.evidenceRefs = {"evidence-1", "evidence-2"};
        const QJsonObject o = AgentJson::parseObject(r.toJson());
        QCOMPARE(o.value("evidence_refs").toArray().size(), 2);
        QCOMPARE(o.value("evidence_refs").toArray().at(0).toString(), QString("evidence-1"));
    }
};
int runTstToolResult(int argc, char** argv){ TstToolResult t; return QTest::qExec(&t, argc, argv); }
#include "tst_toolresult.moc"
