#include <QtTest/QtTest>
#include "AgentOrchestrator.h"
#include "AgentJson.h"
#include "LlmRouting.h"
#include "iLLMProvider.h"
#include "ToolRegistry.h"
using namespace Model;
// Scripted mock: returns replies[min(call, size-1)] each chat() (so a single ToolCall repeats).
class MockProvider : public iLLMProvider {
public:
    std::vector<LlmReply> replies; int calls=0;
    std::vector<LlmMessage> lastMessages;
    std::string lastTools;
    LlmReply chat(const std::vector<LlmMessage>& msgs, const std::string& tools) override {
        lastMessages = msgs;
        lastTools = tools;
        LlmReply r = replies.empty() ? LlmReply{} : replies[std::min<size_t>(calls, replies.size()-1)];
        ++calls; return r;
    }
};
static bool addReadTool(ToolRegistry& registry, AgentTool tool) {
    tool.dangerLevel = "read";
    return registry.add(tool);
}
class TstOrchestrator : public QObject { Q_OBJECT
private slots:
    void single_text_reply() {
        MockProvider p; p.replies = { LlmReply{LlmReply::Text, "hello"} };
        ToolRegistry reg;
        AgentOrchestrator orch(&p, &reg);
        QCOMPARE(QString::fromStdString(orch.run("hi")), QString("hello"));
    }
    void cancellation_stops_before_provider_call() {
        MockProvider p; p.replies = { LlmReply{LlmReply::Text, "should not run"} };
        ToolRegistry reg; AgentOrchestrator orch(&p, &reg);
        orch.setCancelCallback([]() { return true; });
        QString out = QString::fromStdString(orch.run("stop"));
        QVERIFY(out.contains(QString::fromUtf8("已停止")));
        QCOMPARE(p.calls, 0);
    }
    void carries_session_history_before_current_turn() {
        MockProvider p; p.replies = { LlmReply{LlmReply::Text, "ok"} };
        ToolRegistry reg; AgentOrchestrator orch(&p, &reg);
        orch.setHistory({{"user", "previous question"}, {"assistant", "previous answer"}});
        orch.run("current question");
        QCOMPARE((int)p.lastMessages.size(), 4);
        QCOMPARE(QString::fromStdString(p.lastMessages[0].role), QString("system"));
        QCOMPARE(QString::fromStdString(p.lastMessages[1].parts[0].text), QString("previous question"));
        QCOMPARE(QString::fromStdString(p.lastMessages[2].parts[0].text), QString("previous answer"));
        QCOMPARE(QString::fromStdString(p.lastMessages[3].parts[0].text), QString("current question"));
    }
    void one_tool_then_text() {
        MockProvider p;
        LlmReply call; call.kind=LlmReply::ToolCall; call.toolName="probe"; call.toolArgsJson="{}";
        p.replies = { call, LlmReply{LlmReply::Text, "done"} };
        ToolRegistry reg; bool probed=false;
        QVERIFY(addReadTool(reg, {"probe","p","{}",[&](const std::string&){ probed=true; ToolResult t; t.summary="ok"; return t; }}));
        AgentOrchestrator orch(&p, &reg);
        QString out = QString::fromStdString(orch.run("go"));
        QVERIFY(probed);
        QCOMPARE(out, QString("done"));
    }
    void plan_first_rejects_direct_tool_without_execution() {
        MockProvider p;
        LlmReply direct; direct.kind = LlmReply::ToolCall; direct.toolName = "probe";
        direct.toolArgsJson = R"({"secret":"must-not-run"})";
        p.replies = {direct, LlmReply{LlmReply::Text, "replanned"}};
        ToolRegistry reg; bool probed = false;
        QVERIFY(addReadTool(reg, {"probe", "p", "{}", [&](const std::string&) {
            probed = true; ToolResult t; t.summary = "unexpected"; return t;
        }}));
        AgentOrchestrator orch(&p, &reg);
        orch.setPlanFirst(true);
        const std::string out = orch.run("go");
        QCOMPARE(QString::fromStdString(out), QString("replanned"));
        QVERIFY(!probed);
        QVERIFY(p.lastMessages.back().parts[0].text.find("plan_first_required") != std::string::npos);
    }
    void plan_first_allows_plan_executor() {
        MockProvider p;
        LlmReply plan; plan.kind = LlmReply::ToolCall; plan.toolName = "execute_analysis_plan";
        plan.toolArgsJson = R"({"plan":{}})";
        p.replies = {plan, LlmReply{LlmReply::Text, "done"}};
        ToolRegistry reg; bool executed = false;
        QVERIFY(addReadTool(reg, {"execute_analysis_plan", "plan", R"({"type":"object"})",
            [&](const std::string&) { executed = true; ToolResult t; t.summary = "ok"; return t; }}));
        AgentOrchestrator orch(&p, &reg);
        orch.setPlanFirst(true);
        QCOMPARE(QString::fromStdString(orch.run("go")), QString("done"));
        QVERIFY(executed);
    }
    void plan_first_allows_governed_campaign_executor() {
        MockProvider p;
        LlmReply call; call.kind = LlmReply::ToolCall;
        call.toolName = "execute_autonomous_experiment_campaign";
        call.toolArgsJson = R"({"objective":{},"contract":{}})";
        p.replies = {call, LlmReply{LlmReply::Text, "done"}};
        ToolRegistry reg; bool executed = false;
        AgentTool tool{"execute_autonomous_experiment_campaign", "campaign",
            R"({"type":"object"})", [&](const std::string&) {
                executed = true; ToolResult result; result.ok = true; return result;
            }};
        tool.dangerLevel = "write";
        QVERIFY(reg.add(tool));
        AgentOrchestrator orch(&p, &reg);
        orch.setPlanFirst(true);
        QCOMPARE(QString::fromStdString(orch.run("go")), QString("done"));
        QVERIFY(executed);
    }
    void advertised_tools_match_plan_first_gate_without_losing_step_schemas() {
        MockProvider provider;
        provider.replies = {LlmReply{LlmReply::Text, "no execution"}};
        ToolRegistry registry;
        for (const char* name : {"execute_analysis_plan", "execute_autonomous_experiment",
                "execute_autonomous_experiment_campaign", "resume_autonomous_experiment_campaign", "probe"}) {
            AgentTool tool{name, "registered schema", R"({"type":"object"})",
                [](const std::string&) { return ToolResult{}; }};
            tool.dangerLevel = "write";
            QVERIFY(registry.add(tool));
        }
        AgentOrchestrator orchestrator(&provider, &registry);
        orchestrator.setPlanFirst(true);
        orchestrator.run("inspect available actions");
        const auto exposed = QJsonDocument::fromJson(QByteArray::fromStdString(provider.lastTools)).array();
        QCOMPARE(exposed.size(), 4);
        for (const auto& tool : exposed) QVERIFY(tool.toObject().value("name").toString() != "probe");
        const auto& prompt = provider.lastMessages.front().parts.front().text;
        QVERIFY(prompt.find("\"name\":\"probe\"") != std::string::npos);
        QVERIFY(prompt.find("\"requires_confirmation\":true") != std::string::npos);
        orchestrator.setPlanFirst(false);
        orchestrator.run("legacy mode");
        QCOMPARE(provider.lastTools, registry.listJson());
    }
    void steps_reported_in_order() {
        MockProvider p;
        LlmReply call; call.kind=LlmReply::ToolCall; call.toolName="diag"; call.toolArgsJson=R"({"rpm":3000})";
        call.inputTokens=20; call.outputTokens=5;
        LlmReply done{LlmReply::Text, "done"}; done.inputTokens=31; done.outputTokens=7;
        p.replies = { call, done };
        ToolRegistry reg;
        QVERIFY(addReadTool(reg, {"diag","d","{}",[&](const std::string&){ ToolResult t; t.ok=true; t.summary="2 findings"; return t; }}));
        AgentOrchestrator orch(&p, &reg);
        struct Step { std::string kind, name, detail; };
        std::vector<Step> steps;
        orch.setStepCallback([&](const std::string& k,const std::string& n,const std::string& d){ steps.push_back({k,n,d}); });
        orch.run("go");
        QCOMPARE((int)steps.size(), 4);
        QCOMPARE(QString::fromStdString(steps[0].kind), QString("model_result"));
        const QJsonObject firstModel = AgentJson::parseObject(steps[0].detail);
        QCOMPARE(firstModel.value("event").toString(), QString("model_call"));
        QCOMPARE(firstModel.value("iteration").toInt(), 1);
        QCOMPARE(firstModel.value("replyKind").toString(), QString("tool_call"));
        QCOMPARE(firstModel.value("inputTokens").toInt(), 20);
        QCOMPARE(firstModel.value("outputTokens").toInt(), 5);
        QVERIFY(steps[0].detail.find("rpm") == std::string::npos);
        QCOMPARE(QString::fromStdString(steps[1].kind), QString("tool_start"));
        QCOMPARE(QString::fromStdString(steps[1].name), QString("diag"));
        QCOMPARE(QString::fromStdString(steps[2].kind), QString("tool_result"));
        QCOMPARE(QString::fromStdString(steps[2].name), QString("diag"));
        QCOMPARE(QString::fromStdString(steps[3].kind), QString("model_result"));
        const QJsonObject secondModel = AgentJson::parseObject(steps[3].detail);
        QCOMPARE(secondModel.value("iteration").toInt(), 2);
        QCOMPARE(secondModel.value("replyKind").toString(), QString("text"));
        QCOMPARE(secondModel.value("inputTokens").toInt(), 31);
        QCOMPARE(secondModel.value("outputTokens").toInt(), 7);
        QVERIFY(steps[3].detail.find("done") == std::string::npos);
    }
    void retryable_failure_falls_back_and_circuit_recovers() {
        auto* primaryProvider = new MockProvider;
        LlmReply throttled;
        throttled.kind = LlmReply::Error;
        throttled.error = "throttled";
        throttled.errorCode = "http_429";
        throttled.retryable = true;
        throttled.httpStatus = 429;
        throttled.inputTokens = 2;
        throttled.outputTokens = 1;
        LlmReply recovered{LlmReply::Text, "primary-recovered"};
        recovered.inputTokens = 3;
        recovered.outputTokens = 1;
        primaryProvider->replies = {throttled, recovered};

        auto* fallbackProvider = new MockProvider;
        LlmReply fallbackOne{LlmReply::Text, "fallback-one"};
        fallbackOne.inputTokens = 5;
        fallbackOne.outputTokens = 2;
        LlmReply fallbackTwo{LlmReply::Text, "fallback-two"};
        fallbackTwo.inputTokens = 4;
        fallbackTwo.outputTokens = 1;
        fallbackProvider->replies = {fallbackOne, fallbackTwo};

        RoutedLlmRoute primary;
        primary.id = "primary";
        primary.providerName = "openai_compat";
        primary.model = "primary-test";
        primary.pricingConfigured = true;
        primary.inputCostMicrosPerMillionTokens = 1000000;
        primary.outputCostMicrosPerMillionTokens = 2000000;
        primary.provider.reset(primaryProvider);
        RoutedLlmRoute fallback;
        fallback.id = "fallback";
        fallback.providerName = "openai_compat";
        fallback.model = "fallback-test";
        fallback.pricingConfigured = true;
        fallback.provider.reset(fallbackProvider);
        std::vector<RoutedLlmRoute> routes;
        routes.push_back(std::move(primary));
        routes.push_back(std::move(fallback));

        long long now = 0;
        RoutedLlmProvider router(std::move(routes), 1, 1000, [&] { return now; });
        const LlmReply first = router.chat({}, "[]");
        QCOMPARE((int)first.kind, (int)LlmReply::Text);
        QCOMPARE(first.text, std::string("fallback-one"));
        QCOMPARE(first.routeId, std::string("fallback"));
        QCOMPARE(first.attemptCount, 2);
        QCOMPARE(first.inputTokens, 7);
        QCOMPARE(first.outputTokens, 3);
        QVERIFY(first.estimatedCostKnown);
        QCOMPARE(first.estimatedCostMicros, 4LL);
        QCOMPARE(primaryProvider->calls, 1);
        QCOMPARE(fallbackProvider->calls, 1);
        const QJsonObject firstTrace = AgentJson::parseObject(first.routingJson);
        const QJsonArray firstAttempts = firstTrace.value("attempts").toArray();
        QCOMPARE(firstAttempts.size(), 2);
        QCOMPARE(firstAttempts[0].toObject().value("status").toString(), QString("failed"));
        QCOMPARE(firstAttempts[1].toObject().value("status").toString(), QString("selected"));
        QVERIFY(first.routingJson.find("throttled") == std::string::npos);

        const LlmReply second = router.chat({}, "[]");
        QCOMPARE(second.text, std::string("fallback-two"));
        QCOMPARE(second.attemptCount, 1);
        QCOMPARE(primaryProvider->calls, 1);
        QCOMPARE(fallbackProvider->calls, 2);
        const QJsonArray secondAttempts = AgentJson::parseObject(second.routingJson)
            .value("attempts").toArray();
        QCOMPARE(secondAttempts[0].toObject().value("status").toString(), QString("cooldown"));

        now = 1000;
        const LlmReply third = router.chat({}, "[]");
        QCOMPARE(third.text, std::string("primary-recovered"));
        QCOMPARE(third.routeId, std::string("primary"));
        QCOMPARE(third.attemptCount, 1);
        QCOMPARE(primaryProvider->calls, 2);
        QCOMPARE(fallbackProvider->calls, 2);
        const QJsonArray thirdAttempts = AgentJson::parseObject(third.routingJson)
            .value("attempts").toArray();
        QVERIFY(thirdAttempts[0].toObject().value("half_open_probe").toBool());
    }
    void model_call_budget_includes_fallback_attempts() {
        auto* primaryProvider = new MockProvider;
        LlmReply retryable;
        retryable.kind = LlmReply::Error;
        retryable.error = "temporary";
        retryable.errorCode = "http_503";
        retryable.retryable = true;
        primaryProvider->replies = {retryable};
        auto* fallbackProvider = new MockProvider;
        fallbackProvider->replies = {LlmReply{LlmReply::Text, "must-not-run"}};

        RoutedLlmRoute primary;
        primary.id = "primary";
        primary.provider.reset(primaryProvider);
        RoutedLlmRoute fallback;
        fallback.id = "fallback";
        fallback.provider.reset(fallbackProvider);
        std::vector<RoutedLlmRoute> routes;
        routes.push_back(std::move(primary));
        routes.push_back(std::move(fallback));
        RoutedLlmProvider router(std::move(routes), 1, 1000);

        ToolRegistry registry;
        AgentOrchestrator orchestrator(&router, &registry);
        LlmTurnBudget budget;
        budget.maxModelCalls = 1;
        orchestrator.setBudget(budget);

        const std::string output = orchestrator.run("go");
        QVERIFY(output.find("temporary") != std::string::npos);
        QCOMPARE(primaryProvider->calls, 1);
        QCOMPARE(fallbackProvider->calls, 0);
    }
    void token_budget_blocks_tool_execution_and_redacts_arguments() {
        MockProvider provider;
        LlmReply call;
        call.kind = LlmReply::ToolCall;
        call.toolName = "probe";
        call.toolArgsJson = R"({"sensitive":"must-not-leak"})";
        call.inputTokens = 11;
        call.outputTokens = 1;
        provider.replies = {call};
        ToolRegistry registry;
        bool probed = false;
        QVERIFY(addReadTool(registry, {"probe", "p", "{}", [&](const std::string&) {
            probed = true;
            return ToolResult{};
        }}));
        AgentOrchestrator orchestrator(&provider, &registry);
        LlmTurnBudget budget;
        budget.maxModelCalls = 3;
        budget.maxInputTokens = 10;
        orchestrator.setBudget(budget);
        std::vector<std::string> modelEvents;
        orchestrator.setStepCallback([&](const std::string& kind, const std::string&,
                                          const std::string& detail) {
            if (kind == "model_result") modelEvents.push_back(detail);
        });

        const std::string output = orchestrator.run("go");
        QVERIFY(output.find("input_token_budget_exceeded") != std::string::npos);
        QVERIFY(!probed);
        QCOMPARE(provider.calls, 1);
        QCOMPARE((int)modelEvents.size(), 1);
        const QJsonObject event = AgentJson::parseObject(modelEvents[0]);
        QCOMPARE(event.value("budgetStatus").toString(),
                 QString("input_token_budget_exceeded"));
        QVERIFY(modelEvents[0].find("must-not-leak") == std::string::npos);
    }
    void empty_router_fails_safely() {
        std::vector<RoutedLlmRoute> routes;
        RoutedLlmProvider router(std::move(routes), 2, 1000);
        const LlmReply reply = router.chat({}, "[]");
        QCOMPARE((int)reply.kind, (int)LlmReply::Error);
        QCOMPARE(reply.errorCode, std::string("all_routes_unavailable"));
        QVERIFY(reply.retryable);
        QCOMPARE(reply.attemptCount, 0);
    }
    void caps_iterations() {
        MockProvider p;
        LlmReply call; call.kind=LlmReply::ToolCall; call.toolName="noop"; call.toolArgsJson="{}";
        p.replies = { call };  // always ToolCall -> would loop forever without a cap
        ToolRegistry reg;
        QVERIFY(addReadTool(reg, {"noop","n","{}",[](const std::string&){ return ToolResult{}; }}));
        AgentOrchestrator orch(&p, &reg, 8);
        QString out = QString::fromStdString(orch.run("go"));
        QVERIFY(out.contains("最大步数"));
        QCOMPARE(p.calls, 8);   // provider called exactly maxIters times
    }
    void provider_error_surfaces() {
        MockProvider p; p.replies = { LlmReply{LlmReply::Error, "", "", "", "", "boom"} };
        ToolRegistry reg; AgentOrchestrator orch(&p, &reg);
        QVERIFY(QString::fromStdString(orch.run("x")).contains("boom"));
    }
    void tool_turn_emits_paired_messages() {
        MockProvider p;
        LlmReply call; call.kind=LlmReply::ToolCall; call.toolName="probe";
        call.toolArgsJson=R"({"x":1})"; call.toolCallId="id7";
        p.replies = { call, LlmReply{LlmReply::Text, "done"} };
        ToolRegistry reg;
        QVERIFY(addReadTool(reg, {"probe","p","{}",[&](const std::string&){ ToolResult t; t.summary="ok"; return t; }}));
        AgentOrchestrator orch(&p, &reg);
        orch.run("go");
        const auto& ms = p.lastMessages;
        QVERIFY(ms.size() >= 3);
        const LlmMessage& asst = ms[ms.size()-2];
        const LlmMessage& tool = ms[ms.size()-1];
        QCOMPARE(QString::fromStdString(asst.role), QString("assistant"));
        QCOMPARE(QString::fromStdString(asst.toolName), QString("probe"));
        QCOMPARE(QString::fromStdString(asst.toolCallId), QString("id7"));
        QCOMPARE(QString::fromStdString(tool.role), QString("tool"));
        QCOMPARE(QString::fromStdString(tool.toolCallId), QString("id7"));
        QVERIFY(!tool.parts.empty());
        QCOMPARE((int)tool.parts[0].kind, (int)LlmPart::Text);
    }
    void prepends_system_context_when_set() {
        MockProvider p; p.replies = { LlmReply{LlmReply::Text, "ok"} };
        ToolRegistry reg;
        AgentOrchestrator orch(&p, &reg);
        orch.setSystemContext(R"({"test":"分析1"})");
        orch.run("你好");
        QVERIFY(p.lastMessages.size() >= 2);
        QCOMPARE(QString::fromStdString(p.lastMessages[0].role), QString("system"));
        QVERIFY(!p.lastMessages[0].parts.empty());
        QVERIFY(QString::fromStdString(p.lastMessages[0].parts[0].text).contains(
            QString("DPA_AGENT_POLICY_VERSION=%1").arg(kAgentPolicyVersion)));
        QVERIFY(QString::fromStdString(p.lastMessages[0].parts[0].text).contains(QString("分析1")));
        QCOMPARE(QString::fromStdString(p.lastMessages[1].parts[0].text), QString("你好"));
    }
    void tool_result_images_become_image_parts() {
        MockProvider p;
        LlmReply call; call.kind=LlmReply::ToolCall; call.toolName="shot";
        call.toolArgsJson="{}"; call.toolCallId="c1";
        p.replies = { call, LlmReply{LlmReply::Text, "seen"} };
        ToolRegistry reg;
        QVERIFY(addReadTool(reg, {"shot","p","{}",[&](const std::string&){
            ToolResult t; t.summary="ok";
            ImageArtifact im; im.base64="QUJD"; im.mediaType="image/png"; t.images.push_back(im);
            return t; }}));
        AgentOrchestrator orch(&p, &reg);
        orch.run("go");
        const auto& ms = p.lastMessages;
        QVERIFY(ms.size() >= 3);
        const LlmMessage& tool = ms[ms.size()-1];
        QVERIFY(tool.parts.size() >= 2);
        QCOMPARE((int)tool.parts[0].kind, (int)LlmPart::Text);
        QCOMPARE((int)tool.parts[1].kind, (int)LlmPart::Image);
        QCOMPARE(QString::fromStdString(tool.parts[1].imageBase64), QString("QUJD"));
        QCOMPARE(QString::fromStdString(tool.parts[1].mediaType), QString("image/png"));
    }
};
int runTstOrchestrator(int argc,char**argv){ TstOrchestrator t; return QTest::qExec(&t,argc,argv); }
#include "tst_orchestrator.moc"
