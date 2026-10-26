#include <QtTest/QtTest>

#include "AnalysisPlan.h"
#include "ToolRegistry.h"

using namespace Model;

static AgentTool readTool(const std::string& name) {
    AgentTool tool{name, "test", R"({"type":"object","additionalProperties":false})",
                   [](const std::string&) { ToolResult result; result.ok = true; return result; }};
    tool.dangerLevel = "read";
    return tool;
}

static AgentTool writeTool() {
    AgentTool tool{"write_probe", "test", R"({"type":"object","additionalProperties":false})",
                   [](const std::string&) { ToolResult result; result.ok = true; return result; }};
    tool.dangerLevel = "write";
    return tool;
}

class TstAnalysisPlan : public QObject { Q_OBJECT
private slots:
    void validates_context_catalog_and_dependencies() {
        ToolRegistry registry;
        QVERIFY(registry.add(readTool("get_context")));
        QVERIFY(registry.add(readTool("read_channel")));
        const std::string context(64, 'a');
        const std::string catalog = registry.approvalCatalogSha256();
        const std::string json =
            std::string("{\"schema\":\"dpa.analysis.plan\",\"version\":1,\"plan_id\":\"p1\",")
            + "\"context_fingerprint\":\"" + context + "\",\"tool_catalog_sha256\":\""
            + catalog + "\",\"steps\":["
            + "{\"id\":\"context\",\"tool\":\"get_context\",\"args\":{},\"depends_on\":[],\"requires_confirmation\":false},"
            + "{\"id\":\"channel\",\"tool\":\"read_channel\",\"args\":{},\"depends_on\":[\"context\"],\"requires_confirmation\":false}]}";
        const AnalysisPlanValidation validation = parseAndValidateAnalysisPlan(
            json, registry, context);
        QVERIFY2(validation.ok, qPrintable(QString::fromStdString(validation.error)));
        QCOMPARE(validation.plan.steps.size(), size_t(2));
        QCOMPARE(validation.plan.steps.at(1).dependsOn.at(0), std::string("context"));
        QCOMPARE(validation.plan.planSha256.size(), size_t(64));
    }

    void rejects_context_catalog_unknown_and_write_bypass() {
        ToolRegistry registry;
        QVERIFY(registry.add(readTool("get_context")));
        QVERIFY(registry.add(writeTool()));
        const std::string context(64, 'b');
        const std::string catalog = registry.approvalCatalogSha256();
        auto plan = [&](const std::string& tool, bool confirmation, const std::string& ctx,
                        const std::string& cat) {
            return std::string("{\"schema\":\"dpa.analysis.plan\",\"version\":1,\"plan_id\":\"p1\",")
                + "\"context_fingerprint\":\"" + ctx + "\",\"tool_catalog_sha256\":\""
                + cat + "\",\"steps\":[{\"id\":\"s\",\"tool\":\"" + tool
                + "\",\"args\":{},\"depends_on\":[],\"requires_confirmation\":"
                + (confirmation ? "true" : "false") + "}]}";
        };
        QVERIFY(!parseAndValidateAnalysisPlan(plan("get_context", false, std::string(64, 'c'), catalog), registry, context).ok);
        QVERIFY(!parseAndValidateAnalysisPlan(plan("get_context", false, context, std::string(64, 'd')), registry, context).ok);
        QVERIFY(!parseAndValidateAnalysisPlan(plan("unknown", false, context, catalog), registry, context).ok);
        QVERIFY(!parseAndValidateAnalysisPlan(plan("write_probe", true, context, catalog), registry, context).ok);
        QVERIFY(parseAndValidateAnalysisPlan(plan("write_probe", true, context, catalog), registry, context, true).ok);
    }

    void executes_only_through_supplied_executor() {
        ToolRegistry registry;
        QVERIFY(registry.add(readTool("get_context")));
        const std::string context(64, 'e');
        const std::string catalog = registry.approvalCatalogSha256();
        const std::string json =
            std::string("{\"schema\":\"dpa.analysis.plan\",\"version\":1,\"plan_id\":\"p1\",")
            + "\"context_fingerprint\":\"" + context + "\",\"tool_catalog_sha256\":\""
            + catalog + "\",\"steps\":[{\"id\":\"s\",\"tool\":\"get_context\",\"args\":{},\"depends_on\":[],\"requires_confirmation\":false}]}";
        const AnalysisPlanValidation validation = parseAndValidateAnalysisPlan(json, registry, context);
        QVERIFY(validation.ok);
        int calls = 0;
        std::string observedTool;
        std::string observedArgs;
        const AnalysisPlanExecution execution = executeAnalysisPlan(
            validation.plan, [&](const std::string& tool, const std::string& args) -> ToolResult {
                ++calls;
                observedTool = tool;
                observedArgs = args;
                ToolResult result; result.ok = true; return result;
            });
        QVERIFY(execution.ok);
        QCOMPARE(calls, 1);
        QCOMPARE(observedTool, std::string("get_context"));
        QCOMPARE(observedArgs, std::string("{}"));
        QCOMPARE(execution.completedSteps, 1);
    }

    void refuses_programmatic_out_of_order_dependencies() {
        AnalysisPlan plan;
        AnalysisPlanStep step;
        step.id = "dependent";
        step.tool = "read_channel";
        step.argsJson = "{}";
        step.dependsOn = {"missing"};
        plan.steps.push_back(step);

        int calls = 0;
        const AnalysisPlanExecution execution = executeAnalysisPlan(
            plan, [&](const std::string&, const std::string&) {
                ++calls;
                ToolResult result;
                result.ok = true;
                return result;
            });
        QVERIFY(!execution.ok);
        QCOMPARE(QString::fromStdString(execution.error),
                 QString("dependency_not_completed:dependent"));
        QCOMPARE(calls, 0);
        QCOMPARE(execution.completedSteps, 0);
    }
};

int runTstAnalysisPlan(int argc, char** argv) {
    TstAnalysisPlan test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_analysisplan.moc"
