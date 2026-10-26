#include <QtTest/QtTest>
#include "OpenAIWire.h"
#include "AgentJson.h"
#include <QJsonDocument>
using namespace Model;
class TstOpenAIWire : public QObject
{
    Q_OBJECT
private slots:
    void parses_text() {
        std::string j = R"({"choices":[{"message":{"role":"assistant","content":"hi"},"finish_reason":"stop"}],"usage":{"prompt_tokens":12,"completion_tokens":3}})";
        LlmReply r = parseOpenAIReply(j);
        QCOMPARE((int)r.kind, (int)LlmReply::Text);
        QCOMPARE(QString::fromStdString(r.text), QString("hi"));
        QCOMPARE(r.inputTokens, 12);
        QCOMPARE(r.outputTokens, 3);
    }
    void parses_tool_call() {
        std::string j = R"({"choices":[{"message":{"tool_calls":[{"id":"c1","type":"function","function":{"name":"get_context","arguments":"{}"}}]},"finish_reason":"tool_calls"}]})";
        LlmReply r = parseOpenAIReply(j);
        QCOMPARE((int)r.kind, (int)LlmReply::ToolCall);
        QCOMPARE(QString::fromStdString(r.toolName), QString("get_context"));
        QCOMPARE(QString::fromStdString(r.toolCallId), QString("c1"));
        QCOMPARE(r.inputTokens, -1);
        QCOMPARE(r.outputTokens, -1);
    }
    void parses_responses_tool_call() {
        std::string j = "{\"output\":[{\"type\":\"function_call\",\"id\":\"fc_1\",\"call_id\":\"call_1\",\"name\":\"get_context\",\"arguments\":\"{\\\"x\\\":1}\"}],\"usage\":{\"input_tokens\":12,\"output_tokens\":3}}";
        LlmReply r = parseOpenAIReply(j);
        QCOMPARE((int)r.kind, (int)LlmReply::ToolCall);
        QCOMPARE(QString::fromStdString(r.toolName), QString("get_context"));
        QCOMPARE(QString::fromStdString(r.toolCallId), QString("call_1"));
        QCOMPARE(QString::fromStdString(r.toolArgsJson), QString("{\"x\":1}"));
        QCOMPARE(r.inputTokens, 12);
        QCOMPARE(r.outputTokens, 3);
    }
    void rejects_multiple_tool_calls_without_partial_execution() {
        const std::vector<std::string> replies{
            R"({"choices":[{"message":{"tool_calls":[{"id":"c1","type":"function","function":{"name":"get_context","arguments":"{}"}},{"id":"c2","type":"function","function":{"name":"dangerous_write","arguments":"{}"}}]}}]})",
            R"({"output":[{"type":"message","content":[{"type":"output_text","text":"Do not hide later calls"}]},{"type":"function_call","call_id":"c1","name":"get_context","arguments":"{}"},{"type":"tool_call","call_id":"c2","name":"dangerous_write","arguments":"{}"}]})"
        };
        for (const auto& json : replies) {
            const auto reply = parseOpenAIReply(json);
            QCOMPARE(int(reply.kind), int(LlmReply::Error));
            QCOMPARE(reply.error, std::string("multiple_tool_calls_not_supported"));
            QVERIFY(reply.toolName.empty() && reply.toolCallId.empty() && reply.text.empty());
        }
    }
    void parses_responses_text() {
        std::string j = R"({"output":[{"type":"message","content":[{"type":"output_text","text":"resp hi"}]}],"usage":{"input_tokens":4,"output_tokens":2}})";
        LlmReply r = parseOpenAIReply(j);
        QCOMPARE((int)r.kind, (int)LlmReply::Text);
        QCOMPARE(QString::fromStdString(r.text), QString("resp hi"));
        QCOMPARE(r.inputTokens, 4);
        QCOMPARE(r.outputTokens, 2);
    }
    void parses_error() {
        std::string j = R"({"error":{"message":"bad model"}})";
        LlmReply r = parseOpenAIReply(j);
        QCOMPARE((int)r.kind, (int)LlmReply::Error);
        QVERIFY(QString::fromStdString(r.error).contains("bad model"));
    }
    void builds_request_openai_shape() {
        LlmConfig cfg; cfg.model="m"; cfg.maxTokens=64;
        std::vector<LlmMessage> msgs = { {"user","hi"} };
        std::string toolsJson = R"([{"name":"t","description":"d","input_schema":{"type":"object"}}])";
        std::string body = buildOpenAIRequestBody(msgs, toolsJson, cfg);
        QJsonObject o = AgentJson::parseObject(body);
        QCOMPARE(o.value("model").toString(), QString("m"));
        QCOMPARE(o.value("messages").toArray().at(0).toObject().value("content").toString(), QString("hi"));
        QJsonObject tool0 = o.value("tools").toArray().at(0).toObject();
        QCOMPARE(tool0.value("type").toString(), QString("function"));
        QCOMPARE(tool0.value("function").toObject().value("name").toString(), QString("t"));
        QCOMPARE(o.value("tool_choice").toString(), QString("auto"));
        QCOMPARE(o.value("parallel_tool_calls"), QJsonValue(false));
    }
    void builds_real_system_message() {
        LlmConfig cfg;
        std::vector<LlmMessage> msgs = { {"system", "policy-v1"}, {"user", "hi"} };
        QJsonObject o = AgentJson::parseObject(buildOpenAIRequestBody(msgs, "[]", cfg));
        QVERIFY(!o.contains("parallel_tool_calls"));
        QVERIFY(!o.contains("chat_template_kwargs"));
        QJsonArray ms = o.value("messages").toArray();
        QCOMPARE(ms.size(), 2);
        QCOMPARE(ms.at(0).toObject().value("role").toString(), QString("system"));
        QCOMPARE(ms.at(0).toObject().value("content").toString(), QString("policy-v1"));
    }
    void serializes_only_explicit_chat_thinking_option() {
        LlmConfig cfg;
        for (bool enabled : {false, true}) {
            cfg.enableThinking = enabled;
            const auto chat = AgentJson::parseObject(buildOpenAIRequestBody({{"user", "hi"}}, "[]", cfg));
            QCOMPARE(chat.value("chat_template_kwargs").toObject(),
                     QJsonObject({{"enable_thinking", enabled}}));
            QVERIFY(!chat.contains("extra_body")); // Native HTTP JSON, not an SDK wrapper.
            const auto responses = AgentJson::parseObject(buildOpenAIResponsesRequestBody({{"user", "hi"}}, "[]", cfg));
            QVERIFY(!responses.contains("chat_template_kwargs"));
        }
    }
    void builds_openai_tool_call_and_tool_role() {
        LlmConfig cfg;
        std::vector<LlmMessage> msgs;
        LlmMessage asst; asst.role="assistant"; asst.toolCallId="c1";
        asst.toolName="probe"; asst.toolArgsJson=R"({"x":1})";
        msgs.push_back(asst);
        LlmMessage tool; tool.role="tool"; tool.toolCallId="c1";
        LlmPart tp; tp.kind=LlmPart::Text; tp.text="res"; tool.parts.push_back(tp);
        msgs.push_back(tool);

        QJsonObject o = AgentJson::parseObject(buildOpenAIRequestBody(msgs, "[]", cfg));
        QJsonArray ms = o.value("messages").toArray();
        QJsonObject a = ms.at(0).toObject();
        QCOMPARE(a.value("role").toString(), QString("assistant"));
        QJsonObject tc = a.value("tool_calls").toArray().at(0).toObject();
        QCOMPARE(tc.value("id").toString(), QString("c1"));
        QCOMPARE(tc.value("function").toObject().value("name").toString(), QString("probe"));
        QCOMPARE(tc.value("function").toObject().value("arguments").toString(), QString(R"({"x":1})"));
        QJsonObject tr = ms.at(1).toObject();
        QCOMPARE(tr.value("role").toString(), QString("tool"));
        QCOMPARE(tr.value("tool_call_id").toString(), QString("c1"));
        QCOMPARE(tr.value("content").toString(), QString("res"));
    }
    void builds_openai_image_url_when_image() {
        LlmConfig cfg;
        LlmMessage m; m.role="user";
        LlmPart img; img.kind=LlmPart::Image; img.mediaType="image/png"; img.imageBase64="QUJD";
        m.parts.push_back(img);
        std::vector<LlmMessage> msgs{ m };
        QJsonObject o = AgentJson::parseObject(buildOpenAIRequestBody(msgs, "[]", cfg));
        QJsonArray content = o.value("messages").toArray().at(0).toObject().value("content").toArray();
        QJsonObject ib = content.at(0).toObject();
        QCOMPARE(ib.value("type").toString(), QString("image_url"));
        QCOMPARE(ib.value("image_url").toObject().value("url").toString(),
                 QString("data:image/png;base64,QUJD"));
    }
    void builds_responses_request_shape() {
        LlmConfig cfg; cfg.model="m"; cfg.maxTokens=64;
        std::vector<LlmMessage> msgs = { {"system", "policy-v1"}, {"user","hi"} };
        std::string toolsJson = R"([{"name":"t","description":"d","input_schema":{"type":"object"}}])";
        QJsonObject o = AgentJson::parseObject(buildOpenAIResponsesRequestBody(msgs, toolsJson, cfg));
        QCOMPARE(o.value("model").toString(), QString("m"));
        QCOMPARE(o.value("input").toString(), QString("[system] policy-v1\n[user] hi"));
        QCOMPARE(o.value("max_output_tokens").toInt(), 64);
        QCOMPARE(o.value("tools").toArray().at(0).toObject().value("name").toString(), QString("t"));
        QCOMPARE(o.value("tool_choice").toString(), QString("auto"));
        QCOMPARE(o.value("parallel_tool_calls"), QJsonValue(false));
    }
    void resolves_responses_endpoint_paths() {
        QCOMPARE(QString::fromStdString(resolveOpenAICompatEndpoint("https://max.jojocode.com/v1")),
                 QString("https://max.jojocode.com/v1/responses"));
        QCOMPARE(QString::fromStdString(resolveOpenAICompatEndpoint("https://max.jojocode.com/v1/responses")),
                 QString("https://max.jojocode.com/v1/responses"));
        QCOMPARE(QString::fromStdString(resolveOpenAICompatEndpoint("https://max.jojocode.com/v1/chat/completions")),
                 QString("https://max.jojocode.com/v1/chat/completions"));
        QCOMPARE(QString::fromStdString(resolveOpenAICompatEndpoint("http://127.0.0.1:11434")),
                 QString("http://127.0.0.1:11434/v1/responses"));
    }
};
int runTstOpenAIWire(int argc,char**argv){ TstOpenAIWire t; return QTest::qExec(&t,argc,argv); }
#include "tst_openaiwire.moc"
