#include <QtTest/QtTest>
#include "ToolRegistry.h"
#include "AgentJson.h"
#include <QJsonDocument>
#include <utility>
using namespace Model;

static AgentTool readTool(const std::string& name, const std::string& description,
                          const std::string& schema,
                          std::function<ToolResult(const std::string&)> handler) {
    AgentTool tool{name, description, schema, std::move(handler)};
    tool.dangerLevel = "read";
    return tool;
}

class TstToolRegistry : public QObject { Q_OBJECT
private slots:
    void registers_and_calls() {
        ToolRegistry r;
        QVERIFY(r.add(readTool("echo", "echoes", "{}", [](const std::string& a){ ToolResult t; t.summary="echo"; t.dataJson=a; return t; })));
        QVERIFY(r.has("echo"));
        ToolResult res = r.call("echo", R"({"x":1})");
        QVERIFY(res.ok);
        QCOMPARE(AgentJson::parseObject(res.dataJson).value("x").toInt(), 1);
    }
    void unknown_tool_returns_error() {
        ToolRegistry r;
        ToolResult res = r.call("nope", "{}");
        QVERIFY(!res.ok);
        QCOMPARE(QString::fromStdString(res.error), QString("unknown_tool"));
    }
    void malformed_or_non_object_schemas_are_not_registered() {
        ToolRegistry r;
        auto handler = [](const std::string&){ return ToolResult{}; };

        QVERIFY(!r.add(readTool("bad-json", "bad", "{not-json", handler)));
        QVERIFY(!r.has("bad-json"));
        QVERIFY(!r.add(readTool("array-schema", "bad", "[]", handler)));
        QVERIFY(!r.has("array-schema"));

        AgentTool badOutput{"bad-output", "bad", R"({"type":"object"})", handler};
        badOutput.outputSchemaJson = "[]";
        badOutput.dangerLevel = "read";
        QVERIFY(!r.add(badOutput));
        QVERIFY(!r.has("bad-output"));

        // A deliberately empty object is still a valid schema; unlike parse failure it must
        // remain distinguishable and register normally.
        QVERIFY(r.add(readTool("empty-schema", "valid", "{}", handler)));
        QVERIFY(r.has("empty-schema"));
        QVERIFY(r.call("empty-schema", R"({"anything":1})").ok);
    }
    void rejects_invalid_json_and_schema_mismatches() {
        ToolRegistry r;
        int calls = 0;
        QVERIFY(r.add(readTool("probe", "probe", R"({"type":"object","properties":{"name":{"type":"string"},"count":{"type":"integer","minimum":1},"mode":{"type":"string","enum":["safe"]}},"required":["name","count"]})",
               [&](const std::string&){ ++calls; return ToolResult{}; })));
        QCOMPARE(QString::fromStdString(r.call("probe", "not-json").error), QString("invalid_arguments"));
        QCOMPARE(QString::fromStdString(r.call("probe", R"({"count":1})").error), QString("invalid_arguments"));
        QCOMPARE(QString::fromStdString(r.call("probe", R"({"name":"x","count":1.5})").error), QString("invalid_arguments"));
        QCOMPARE(QString::fromStdString(r.call("probe", R"({"name":"x","count":0})").error), QString("invalid_arguments"));
        QCOMPARE(QString::fromStdString(r.call("probe", R"({"name":"x","count":1,"mode":"unsafe"})").error), QString("invalid_arguments"));
        QVERIFY(r.call("probe", R"({"name":"x","count":1,"mode":"safe"})").ok);
        QCOMPARE(calls, 1);
    }
    void list_contains_registered() {
        ToolRegistry r;
        QVERIFY(r.add(readTool("echo", "echoes", "{}", [](const std::string&){ return ToolResult{}; })));
        auto arr = QJsonDocument::fromJson(QByteArray::fromStdString(r.listJson())).array();
        QCOMPARE(arr.size(), 1);
        QCOMPARE(arr.at(0).toObject().value("name").toString(), QString("echo"));
    }
    void validates_nested_objects_and_arrays() {
        ToolRegistry r;
        QVERIFY(r.add(readTool("nested", "nested", R"({"type":"object","properties":{"asset":{"type":"object","properties":{"id":{"type":"string","minLength":1}},"required":["id"],"additionalProperties":false},"channels":{"type":"array","minItems":1,"items":{"type":"integer","minimum":0}}},"required":["asset","channels"]})",
               [](const std::string&){ return ToolResult{}; })));
        QVERIFY(!r.call("nested", R"({"asset":{},"channels":[0]})").ok);
        QVERIFY(!r.call("nested", R"({"asset":{"id":"a","extra":1},"channels":[0]})").ok);
        QVERIFY(!r.call("nested", R"({"asset":{"id":"a"},"channels":[]})").ok);
        QVERIFY(!r.call("nested", R"({"asset":{"id":"a"},"channels":[-1]})").ok);
        QVERIFY(r.call("nested", R"({"asset":{"id":"a"},"channels":[0,2]})").ok);
    }
    void mcp_list_uses_prefixed_name() {
        ToolRegistry r;
        QVERIFY(r.add(readTool("echo", "echoes", R"({"type":"object"})", [](const std::string&){ return ToolResult{}; })));
        auto arr = QJsonDocument::fromJson(QByteArray::fromStdString(r.listMcpJson())).array();
        QCOMPARE(arr.at(0).toObject().value("name").toString(), QString("dpa.echo"));
        QVERIFY(arr.at(0).toObject().contains("inputSchema"));
    }
    void mcp_call_routes_to_internal_handler() {
        ToolRegistry r;
        QVERIFY(r.add(readTool("echo", "echoes", "{}", [](const std::string& a){ ToolResult t; t.summary="echo"; t.dataJson=a; return t; })));
        ToolResult res = r.callMcp("dpa.echo", R"({"x":2})");
        QVERIFY(res.ok);
        QCOMPARE(AgentJson::parseObject(res.dataJson).value("x").toInt(), 2);
    }
    void danger_level_gates_by_name_and_mcp_name() {
        ToolRegistry r;
        QVERIFY(r.add(readTool("safe", "reads", "{}", [](const std::string&){ return ToolResult{}; })));
        QVERIFY(r.add({"danger","mutates","{}",[](const std::string&){ return ToolResult{}; },
                       "dpa.danger","Danger",R"({"type":"object"})","analysis","write"}));
        QVERIFY(!r.isWrite("safe"));
        QVERIFY(r.isWrite("danger"));
        QVERIFY(!r.isWriteMcp("dpa.safe"));
        QVERIFY(r.isWriteMcp("dpa.danger"));
        QVERIFY(r.hasMcp("dpa.safe"));
        QVERIFY(r.hasMcp("dpa.danger"));
        QVERIFY(!r.hasMcp("dpa.unknown"));
        QVERIFY(r.isWrite("unknown"));
        QVERIFY(r.isWriteMcp("dpa.unknown"));
    }
    void approval_catalog_hash_is_stable_and_security_sensitive() {
        auto handler = [](const std::string&){ return ToolResult{}; };
        ToolRegistry first;
        ToolRegistry reversed;
        QVERIFY(first.add(readTool("alpha", "first description", R"({"type":"object","properties":{"value":{"type":"integer"}}})", handler)));
        QVERIFY(first.add(readTool("beta", "second description", "{}", handler)));
        QVERIFY(reversed.add(readTool("beta", "changed UI text", "{}", handler)));
        QVERIFY(reversed.add(readTool("alpha", "also changed", R"({"properties":{"value":{"type":"integer"}},"type":"object"})", handler)));
        QCOMPARE(first.approvalCatalogSha256(), reversed.approvalCatalogSha256());
        QCOMPARE(first.approvalCatalogSha256().size(), size_t(64));

        ToolRegistry changedSchema;
        QVERIFY(changedSchema.add(readTool("alpha", "first", R"({"type":"object","properties":{"value":{"type":"string"}}})", handler)));
        QVERIFY(changedSchema.add(readTool("beta", "second", "{}", handler)));
        QVERIFY(first.approvalCatalogSha256() != changedSchema.approvalCatalogSha256());

        ToolRegistry changedDanger;
        AgentTool writeAlpha{"alpha", "first", R"({"type":"object","properties":{"value":{"type":"integer"}}})", handler};
        writeAlpha.dangerLevel = "write";
        QVERIFY(changedDanger.add(writeAlpha));
        QVERIFY(changedDanger.add(readTool("beta", "second", "{}", handler)));
        QVERIFY(first.approvalCatalogSha256() != changedDanger.approvalCatalogSha256());
    }
    void unclassified_or_unknown_danger_levels_are_rejected() {
        ToolRegistry r;
        auto handler = [](const std::string&){ return ToolResult{}; };
        AgentTool unclassified{"unclassified", "missing classification", "{}", handler};
        QCOMPARE(QString::fromStdString(unclassified.dangerLevel), QString("unclassified"));
        QVERIFY(!r.add(unclassified));
        QVERIFY(!r.has("unclassified"));

        AgentTool unknown{"unknown-level", "bad classification", "{}", handler};
        unknown.dangerLevel = "READ";
        QVERIFY(!r.add(unknown));
        QVERIFY(!r.has("unknown-level"));
    }
    void mcp_publishes_and_executes_only_explicit_read_tools() {
        ToolRegistry r;
        int readCalls = 0;
        int writeCalls = 0;
        QVERIFY(r.add(readTool("reader", "reads", "{}", [&](const std::string&){ ++readCalls; return ToolResult{}; })));
        QVERIFY(r.add({"writer", "writes", "{}", [&](const std::string&){ ++writeCalls; return ToolResult{}; },
                       "dpa.writer", "Writer", R"({"type":"object"})", "analysis", "write"}));

        const QJsonArray tools = QJsonDocument::fromJson(QByteArray::fromStdString(r.listMcpJson())).array();
        QCOMPARE(tools.size(), 1);
        QCOMPARE(tools.at(0).toObject().value("name").toString(), QString("dpa.reader"));
        QVERIFY(r.callMcp("dpa.reader", "{}").ok);
        QCOMPARE(readCalls, 1);
        const ToolResult denied = r.callMcp("dpa.writer", "{}");
        QVERIFY(!denied.ok);
        QCOMPARE(QString::fromStdString(denied.error), QString("scope_forbidden"));
        QCOMPARE(writeCalls, 0);
    }
};
int runTstToolRegistry(int argc,char**argv){ TstToolRegistry t; return QTest::qExec(&t,argc,argv); }
#include "tst_toolregistry.moc"
