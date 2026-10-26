#pragma once
#ifndef MODEL_ILLMPROVIDER_H
#define MODEL_ILLMPROVIDER_H
#include <string>
#include <vector>
#include <utility>
namespace Model {
    // 一段消息内容：文本或内联图像。
    struct LlmPart {
        enum Kind { Text, Image } kind = Text;
        std::string text;                     // Text 时
        std::string imageBase64, mediaType;   // Image 时；mediaType 如 "image/png"
    };
    // 一条会话消息。文本/图像内容在 parts。工具链字段描述 assistant 的 tool_use 轮
    // 或 tool 结果：
    //  - assistant tool_use: role="assistant"，toolCallId+toolName+toolArgsJson 有值，parts 可选
    //  - tool 结果:          role="tool"，     toolCallId 有值，parts = 结果内容(文本/图像)
    struct LlmMessage {
        std::string role;                     // "system" | "user" | "assistant" | "tool"
        std::vector<LlmPart> parts;
        std::string toolCallId;               // tool_use id（配对 assistant tool_use <-> tool 结果）
        std::string toolName;                 // assistant tool_use 名
        std::string toolArgsJson;             // assistant tool_use 入参（JSON 对象字符串）
        LlmMessage() = default;
        // 便利构造：纯文本消息（保持 {"user","hi"} 等调用点可用）。
        LlmMessage(std::string r, std::string t) : role(std::move(r)) {
            LlmPart p; p.kind = LlmPart::Text; p.text = std::move(t); parts.push_back(p);
        }
    };
    struct LlmReply {
        enum Kind { Text, ToolCall, Error };
        Kind kind = Text;
        std::string text;                                   // Text 时
        std::string toolName, toolArgsJson, toolCallId;     // ToolCall 时
        std::string error;                                  // Error 时
        int inputTokens = -1, outputTokens = -1;            // provider usage；未知时为 -1
        // Stable, content-free routing/operations metadata. Never place prompts,
        // responses, tool arguments, or credentials in these fields.
        std::string errorCode;
        bool retryable = false;
        int httpStatus = -1;
        long long retryAfterMs = -1;
        std::string routeId, providerName, model;
        int attemptCount = 1;
        bool estimatedCostKnown = false;
        long long estimatedCostMicros = 0;                  // one USD = 1,000,000 micros
        std::string routingJson;                            // redacted route decisions only
    };
    struct LlmTurnBudget {
        // Zero disables a token/cost limit. maxModelCalls is a hard pre-call cap;
        // usage-based limits stop continuation when the provider reports usage.
        int maxModelCalls = 0;
        long long maxInputTokens = 0;
        long long maxOutputTokens = 0;
        long long maxEstimatedCostMicros = 0;
    };
    class iLLMProvider {
    public:
        virtual ~iLLMProvider() = default;
        // messages so far + the tool catalog JSON (ToolRegistry::listJson) -> ONE step reply
        virtual LlmReply chat(const std::vector<LlmMessage>& messages, const std::string& toolsJson) = 0;
        // Cooperative cancellation for a currently blocking network request.
        virtual void cancel() {}
        virtual void resetCancellation() {}
        // Limits physical provider attempts made by the next chat() call. A
        // router uses this to keep fallback attempts inside the turn budget.
        virtual void setCallAttemptLimit(int) {}
    };
}
#endif // MODEL_ILLMPROVIDER_H
