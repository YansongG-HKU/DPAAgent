#include <QtTest/QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include "SessionRecord.h"
using namespace Model;
class TstSessionRecord : public QObject { Q_OBJECT
private slots:
    void round_trip() {
        SessionRecord s;
        s.id    = "sess-001";
        s.title = "test session";
        s.diagnosticRunJson = R"({"state":"analysis_complete","current_step":"analysis_complete","error":"","updated_at":42})";
        s.pendingWriteJson = R"({"correlationId":"call-001","confirmationId":"confirm-001","tool":"save_baseline","argsSha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","contextFingerprint":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","catalogSha256":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","approvedAtMs":41,"startedMs":40})";
        s.companionEvents.push_back(R"({"kind":"limit_high","severity":"alarm"})");
        s.turns.push_back({"user",      "hello"});
        s.turns.push_back({"assistant", "hi there"});
        s.turns.push_back({"operator_feedback", R"({"event":"operator_feedback","feedbackType":"correct"})"});
        std::string json = buildSessionJson(s);
        SessionRecord r  = parseSessionJson(json);
        QCOMPARE(r.id,    s.id);
        QCOMPARE(r.title, s.title);
        QVERIFY(r.diagnosticRunJson.find("\"state\":\"analysis_complete\"") != std::string::npos);
        QVERIFY(r.pendingWriteJson.find("\"correlationId\":\"call-001\"") != std::string::npos);
        QCOMPARE((int)r.companionEvents.size(), 1);
        QVERIFY(r.companionEvents[0].find("limit_high") != std::string::npos);
        QCOMPARE((int)r.turns.size(), (int)s.turns.size());
        QCOMPARE(r.turns[0].role,    std::string("user"));
        QCOMPARE(r.turns[0].content, std::string("hello"));
        QCOMPARE(r.turns[2].role, std::string("operator_feedback"));
    }
    void empty_session() {
        SessionRecord s;
        std::string json = buildSessionJson(s);
        // must contain the three keys
        QVERIFY(json.find("\"id\"")    != std::string::npos);
        QVERIFY(json.find("\"title\"") != std::string::npos);
        QVERIFY(json.find("\"turns\"") != std::string::npos);
        SessionRecord r = parseSessionJson(json);
        QCOMPARE(r.id,    std::string(""));
        QCOMPARE(r.title, std::string(""));
        QCOMPARE((int)r.turns.size(), 0);
    }
    void rejects_unbounded_or_invalid_persisted_sessions() {
        SessionRecord malformedPending;
        malformedPending.id = "sess-invalid-pending";
        malformedPending.title = "invalid pending";
        malformedPending.pendingWriteJson = "{}";
        QVERIFY(buildSessionJson(malformedPending).empty());

        SessionRecord tooMany;
        tooMany.id = "sess-many";
        tooMany.title = "too many";
        for (int i = 0; i < 201; ++i) tooMany.turns.push_back({"user", "x"});
        QVERIFY(parseSessionJson(buildSessionJson(tooMany)).id.empty());

        SessionRecord tooManyEvents;
        tooManyEvents.id = "sess-events";
        tooManyEvents.title = "too many events";
        for (int i = 0; i < 201; ++i)
            tooManyEvents.companionEvents.push_back(R"({"kind":"data_stale"})");
        QVERIFY(parseSessionJson(buildSessionJson(tooManyEvents)).id.empty());

        SessionRecord oversized;
        oversized.id = "sess-large";
        oversized.title = "oversized";
        oversized.turns.push_back({"assistant", std::string(513 * 1024 + 1, 'x')});
        QVERIFY(parseSessionJson(buildSessionJson(oversized)).id.empty());

        SessionRecord invalidRole;
        invalidRole.id = "sess-role";
        invalidRole.title = "invalid role";
        invalidRole.turns.push_back({"system", "must not be restored"});
        QVERIFY(parseSessionJson(buildSessionJson(invalidRole)).id.empty());

        const std::string invalidPending =
            R"({"id":"sess-pending","title":"invalid pending","pending_write":[],"turns":[]})";
        QVERIFY(parseSessionJson(invalidPending).id.empty());

        const QJsonObject oversizedPending{
            {"correlationId", "call-001"},
            {"confirmationId", "confirm-001"},
            {"tool", QString(9 * 1024, 'x')},
            {"argsSha256", QString(64, 'a')},
            {"contextFingerprint", QString(64, 'b')},
            {"catalogSha256", QString(64, 'c')},
            {"approvedAtMs", 41},
            {"startedMs", 40}
        };
        const QJsonObject oversizedRoot{
            {"id", "sess-pending"}, {"title", "oversized pending"},
            {"pending_write", oversizedPending}, {"turns", QJsonArray{}}
        };
        QVERIFY(parseSessionJson(QJsonDocument(oversizedRoot).toJson(
            QJsonDocument::Compact).toStdString()).id.empty());

        QJsonObject fractionalPending = oversizedPending;
        fractionalPending["tool"] = "save_baseline";
        fractionalPending["approvedAtMs"] = 41.5;
        QJsonObject fractionalRoot = oversizedRoot;
        fractionalRoot["pending_write"] = fractionalPending;
        QVERIFY(parseSessionJson(QJsonDocument(fractionalRoot).toJson(
            QJsonDocument::Compact).toStdString()).id.empty());
    }
};
int runTstSessionRecord(int argc, char** argv){ TstSessionRecord t; return QTest::qExec(&t, argc, argv); }
#include "tst_sessionrecord.moc"
