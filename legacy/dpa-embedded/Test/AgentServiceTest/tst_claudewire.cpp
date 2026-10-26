#include <QtTest/QtTest>
#include "ClaudeWire.h"
#include "AgentJson.h"
#include <QJsonDocument>
using namespace Model;
class TstClaudeWire : public QObject { Q_OBJECT
private slots:
    void parses_text_reply() {
        std::string j = R"({"content":[{"type":"text","text":"hi there"}],"stop_reason":"end_turn","usage":{"input_tokens":9,"output_tokens":4}})";
        LlmReply r = parseClaudeReply(j);
        QCOMPARE((int)r.kind, (int)LlmReply::Text);
        QCOMPARE(QString::fromStdString(r.text), QString("hi there"));
        QCOMPARE(r.inputTokens, 9);
        QCOMPARE(r.outputTokens, 4);
    }
    void parses_tool_use() {
        std::string j = R"({"content":[{"type":"tool_use","id":"toolu_1","name":"get_context","input":{"a":1}}],"stop_reason":"tool_use"})";
        LlmReply r = parseClaudeReply(j);
        QCOMPARE((int)r.kind, (int)LlmReply::ToolCall);
        QCOMPARE(QString::fromStdString(r.toolName), QString("get_context"));
        QCOMPARE(QString::fromStdString(r.toolCallId), QString("toolu_1"));
        QCOMPARE(AgentJson::parseObject(r.toolArgsJson).value("a").toInt(), 1);
        QCOMPARE(r.inputTokens, -1);
        QCOMPARE(r.outputTokens, -1);
    }
    void parses_error() {
        std::string j = R"({"type":"error","error":{"type":"x","message":"bad key"}})";
        LlmReply r = parseClaudeReply(j);
        QCOMPARE((int)r.kind, (int)LlmReply::Error);
        QVERIFY(QString::fromStdString(r.error).contains("bad key"));
    }
    void builds_request_with_model_and_messages() {
        LlmConfig cfg; cfg.model="claude-opus-4-8"; cfg.maxTokens=1234;
        std::vector<LlmMessage> msgs = { {"user","hello"} };
        std::string body = buildClaudeRequestBody(msgs, "[]", cfg);
        QJsonObject o = AgentJson::parseObject(body);
        QCOMPARE(o.value("model").toString(), QString("claude-opus-4-8"));
        QCOMPARE(o.value("max_tokens").toInt(), 1234);
        QCOMPARE(o.value("messages").toArray().size(), 1);
        QCOMPARE(o.value("messages").toArray().at(0).toObject().value("content").toString(), QString("hello"));
    }
    void builds_top_level_system_prompt() {
        LlmConfig cfg;
        std::vector<LlmMessage> msgs = { {"system", "policy-v1"}, {"user", "hello"} };
        QJsonObject o = AgentJson::parseObject(buildClaudeRequestBody(msgs, "[]", cfg));
        QCOMPARE(o.value("system").toString(), QString("policy-v1"));
        QCOMPARE(o.value("messages").toArray().size(), 1);
        QCOMPARE(o.value("messages").toArray().at(0).toObject().value("role").toString(), QString("user"));
    }
    void llmmessage_text_ctor_populates_one_text_part() {
        LlmMessage m("user", "hi");
        QCOMPARE((int)m.parts.size(), 1);
        QCOMPARE((int)m.parts[0].kind, (int)LlmPart::Text);
        QCOMPARE(QString::fromStdString(m.parts[0].text), QString("hi"));
    }
    void builds_tool_use_and_tool_result_pairing() {
        LlmConfig cfg;
        std::vector<LlmMessage> msgs;
        msgs.push_back(LlmMessage("user", "run probe"));
        LlmMessage asst; asst.role="assistant"; asst.toolCallId="abc";
        asst.toolName="probe"; asst.toolArgsJson=R"({"x":1})";
        msgs.push_back(asst);
        LlmMessage tool; tool.role="tool"; tool.toolCallId="abc";
        LlmPart tp; tp.kind=LlmPart::Text; tp.text="result-text"; tool.parts.push_back(tp);
        msgs.push_back(tool);

        QJsonObject o = AgentJson::parseObject(buildClaudeRequestBody(msgs, "[]", cfg));
        QJsonArray ms = o.value("messages").toArray();
        QCOMPARE(ms.size(), 3);
        QJsonObject a = ms.at(1).toObject();
        QCOMPARE(a.value("role").toString(), QString("assistant"));
        QJsonObject tu = a.value("content").toArray().at(0).toObject();
        QCOMPARE(tu.value("type").toString(), QString("tool_use"));
        QCOMPARE(tu.value("id").toString(), QString("abc"));
        QCOMPARE(tu.value("name").toString(), QString("probe"));
        QCOMPARE(tu.value("input").toObject().value("x").toInt(), 1);
        QJsonObject u = ms.at(2).toObject();
        QCOMPARE(u.value("role").toString(), QString("user"));
        QJsonObject tr = u.value("content").toArray().at(0).toObject();
        QCOMPARE(tr.value("type").toString(), QString("tool_result"));
        QCOMPARE(tr.value("tool_use_id").toString(), QString("abc"));
        QCOMPARE(tr.value("content").toArray().at(0).toObject().value("text").toString(), QString("result-text"));
    }
    void builds_image_block_when_part_is_image() {
        LlmConfig cfg;
        LlmMessage m; m.role="user";
        LlmPart t; t.kind=LlmPart::Text; t.text="look";
        LlmPart img; img.kind=LlmPart::Image; img.mediaType="image/png"; img.imageBase64="QUJD";
        m.parts.push_back(t); m.parts.push_back(img);
        std::vector<LlmMessage> msgs{ m };

        QJsonObject o = AgentJson::parseObject(buildClaudeRequestBody(msgs, "[]", cfg));
        QJsonArray content = o.value("messages").toArray().at(0).toObject().value("content").toArray();
        QCOMPARE(content.size(), 2);
        QCOMPARE(content.at(0).toObject().value("type").toString(), QString("text"));
        QJsonObject ib = content.at(1).toObject();
        QCOMPARE(ib.value("type").toString(), QString("image"));
        QCOMPARE(ib.value("source").toObject().value("type").toString(), QString("base64"));
        QCOMPARE(ib.value("source").toObject().value("media_type").toString(), QString("image/png"));
        QCOMPARE(ib.value("source").toObject().value("data").toString(), QString("QUJD"));
    }
};
int runTstClaudeWire(int argc,char**argv){ TstClaudeWire t; return QTest::qExec(&t,argc,argv); }
#include "tst_claudewire.moc"
