#include "ClaudeWire.h"
#include "AgentJson.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QByteArray>
#include <QString>

namespace Model {

// 单个 part -> Anthropic 内容块
static QJsonObject claudePartBlock(const LlmPart& p) {
    QJsonObject b;
    if (p.kind == LlmPart::Image) {
        b["type"] = "image";
        QJsonObject src;
        src["type"] = "base64";
        src["media_type"] = QString::fromStdString(p.mediaType.empty() ? std::string("image/png") : p.mediaType);
        src["data"] = QString::fromStdString(p.imageBase64);
        b["source"] = src;
    } else {
        b["type"] = "text";
        b["text"] = QString::fromStdString(p.text);
    }
    return b;
}

// 纯文本 -> 字符串 content（保持旧行为）；含图像 -> 块数组
static QJsonValue claudeContent(const std::vector<LlmPart>& parts) {
    bool anyImage = false;
    for (const auto& p : parts) if (p.kind == LlmPart::Image) anyImage = true;
    if (!anyImage) {
        QString s;
        for (const auto& p : parts) if (p.kind == LlmPart::Text) s += QString::fromStdString(p.text);
        return QJsonValue(s);
    }
    QJsonArray arr;
    for (const auto& p : parts) arr.append(claudePartBlock(p));
    return arr;
}

std::string buildClaudeRequestBody(const std::vector<LlmMessage>& messages,
                                   const std::string& toolsJson, const LlmConfig& cfg) {
    QJsonObject root;
    root["model"] = QString::fromStdString(cfg.model);
    root["max_tokens"] = cfg.maxTokens;
    // Embed the tool catalog as-is: ToolRegistry::listJson() already emits an array of
    // {name, description, input_schema}, which matches Anthropic's tool shape.
    QJsonArray toolsArr =
        QJsonDocument::fromJson(QByteArray::fromStdString(toolsJson)).array();
    root["tools"] = toolsArr;

    QString systemText;
    QJsonArray msgArr;
    for (const auto& m : messages) {
        if (m.role == "system") {
            for (const auto& p : m.parts)
                if (p.kind == LlmPart::Text) {
                    if (!systemText.isEmpty()) systemText += "\n\n";
                    systemText += QString::fromStdString(p.text);
                }
            continue;
        }
        QJsonObject mo;
        if (m.role == "tool") {                                   // tool 结果 -> user 里的 tool_result 块
            mo["role"] = "user";
            QJsonObject tr;
            tr["type"] = "tool_result";
            tr["tool_use_id"] = QString::fromStdString(m.toolCallId);
            QJsonArray inner;
            for (const auto& p : m.parts) inner.append(claudePartBlock(p));
            tr["content"] = inner;
            QJsonArray content; content.append(tr);
            mo["content"] = content;
        } else if (m.role == "assistant" && !m.toolName.empty()) { // assistant tool_use
            mo["role"] = "assistant";
            QJsonArray content;
            for (const auto& p : m.parts) content.append(claudePartBlock(p)); // 可选前置文本
            QJsonObject tu;
            tu["type"] = "tool_use";
            tu["id"] = QString::fromStdString(m.toolCallId);
            tu["name"] = QString::fromStdString(m.toolName);
            tu["input"] = AgentJson::parseObject(m.toolArgsJson.empty() ? std::string("{}") : m.toolArgsJson);
            content.append(tu);
            mo["content"] = content;
        } else {                                                   // 普通文本/图像消息
            mo["role"] = (m.role == "assistant") ? "assistant" : "user";
            mo["content"] = claudeContent(m.parts);
        }
        msgArr.append(mo);
    }
    if (!systemText.isEmpty()) root["system"] = systemText;
    root["messages"] = msgArr;

    return QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
}

LlmReply parseClaudeReply(const std::string& responseJson) {
    LlmReply reply;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(responseJson));
    if (!doc.isObject()) {
        reply.kind = LlmReply::Error;
        reply.error = "parse_error";
        return reply;
    }
    QJsonObject obj = doc.object();

    // Top-level error object -> Error reply.
    if (obj.contains("error") && obj.value("error").isObject()) {
        QJsonObject err = obj.value("error").toObject();
        QString msg = err.value("message").toString();
        reply.kind = LlmReply::Error;
        reply.error = msg.isEmpty() ? std::string("parse_error") : msg.toStdString();
        return reply;
    }

    const QJsonObject usage = obj.value("usage").toObject();
    reply.inputTokens = usage.value("input_tokens").toInt(-1);
    reply.outputTokens = usage.value("output_tokens").toInt(-1);
    if (reply.inputTokens < 0) reply.inputTokens = -1;
    if (reply.outputTokens < 0) reply.outputTokens = -1;

    QJsonArray content = obj.value("content").toArray();
    QString textAccum;
    bool sawText = false;
    for (const QJsonValue& v : content) {
        QJsonObject block = v.toObject();
        QString type = block.value("type").toString();
        if (type == "tool_use") {
            reply.kind = LlmReply::ToolCall;
            reply.toolName = block.value("name").toString().toStdString();
            reply.toolCallId = block.value("id").toString().toStdString();
            QJsonValue input = block.value("input");
            QJsonDocument inDoc = input.isArray()
                ? QJsonDocument(input.toArray())
                : QJsonDocument(input.toObject());
            reply.toolArgsJson = inDoc.toJson(QJsonDocument::Compact).toStdString();
            return reply; // tool_use takes priority
        } else if (type == "text") {
            sawText = true;
            textAccum += block.value("text").toString();
        }
    }

    if (sawText) {
        reply.kind = LlmReply::Text;
        reply.text = textAccum.toStdString();
        return reply;
    }

    // No usable content.
    reply.kind = LlmReply::Error;
    reply.error = "parse_error";
    return reply;
}

}
