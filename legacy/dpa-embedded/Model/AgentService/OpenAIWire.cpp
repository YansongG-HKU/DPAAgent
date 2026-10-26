#include "OpenAIWire.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QByteArray>
#include <QString>
#include <QUrl>

namespace Model {

namespace {

static QString toolListFromJson(const std::string& toolsJson, bool responsesMode) {
    QJsonArray inTools = QJsonDocument::fromJson(QByteArray::fromStdString(toolsJson)).array();
    if (inTools.isEmpty()) return {};

    QJsonArray outTools;
    for (const QJsonValue& v : inTools) {
        QJsonObject src = v.toObject();
        if (responsesMode) {
            QJsonObject fn;
            fn["name"] = src.value("name").toString();
            fn["description"] = src.value("description").toString();
            fn["parameters"] = src.value("input_schema").toObject();
            outTools.append(fn);
            continue;
        }

        QJsonObject fn;
        fn["name"] = src.value("name").toString();
        fn["description"] = src.value("description").toString();
        fn["parameters"] = src.value("input_schema").toObject();
        QJsonObject tool;
        tool["type"] = "function";
        tool["function"] = fn;
        outTools.append(tool);
    }
    return QJsonDocument(outTools).toJson(QJsonDocument::Compact);
}

static QJsonValue openaiContent(const std::vector<LlmPart>& parts) {
    bool anyImage = false;
    for (const auto& p : parts) if (p.kind == LlmPart::Image) anyImage = true;
    if (!anyImage) {
        QString s;
        for (const auto& p : parts) if (p.kind == LlmPart::Text) s += QString::fromStdString(p.text);
        return QJsonValue(s);
    }
    QJsonArray arr;
    for (const auto& p : parts) {
        QJsonObject b;
        if (p.kind == LlmPart::Image) {
            b["type"] = "image_url";
            QJsonObject u;
            u["url"] = QString("data:")
                     + QString::fromStdString(p.mediaType.empty() ? std::string("image/png") : p.mediaType)
                     + ";base64," + QString::fromStdString(p.imageBase64);
            b["image_url"] = u;
        } else {
            b["type"] = "text";
            b["text"] = QString::fromStdString(p.text);
        }
        arr.append(b);
    }
    return arr;
}

static QString responsesInputFromMessages(const std::vector<LlmMessage>& messages) {
    QStringList lines;
    for (const auto& m : messages) {
        const QString role = QString::fromStdString(m.role.empty() ? "user" : m.role);

        if (role == "tool") {
            QString content;
            for (const auto& p : m.parts)
                if (p.kind == LlmPart::Text) content += QString::fromStdString(p.text);
            QString marker = role.toUpper();
            if (!m.toolCallId.empty()) marker += QLatin1String(" ") + QString::fromStdString(m.toolCallId);
            lines << marker + QLatin1String(": ") + content;
            continue;
        }

        if (role == "assistant" && !m.toolName.empty()) {
            lines << QLatin1String("[assistant-tool-call] ")
                  << QString::fromStdString(m.toolName)
                  << QLatin1String(" ") << QString::fromStdString(m.toolCallId)
                  << QLatin1String(" args=")
                  << QString::fromStdString(m.toolArgsJson.empty() ? std::string("{}") : m.toolArgsJson);
            continue;
        }

        QString content;
        for (const auto& p : m.parts) if (p.kind == LlmPart::Text) content += QString::fromStdString(p.text);
        lines << QLatin1Char('[') + role + QLatin1String("] ") + content;
    }
    return lines.join(QLatin1Char('\n'));
}

static QString jsonValueToString(const QJsonValue& value) {
    if (value.isString()) return value.toString();
    if (value.isBool()) return value.toBool() ? QLatin1String("true") : QLatin1String("false");
    if (value.isDouble()) return QString::number(value.toDouble());
    if (value.isObject()) return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    if (value.isArray()) return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    return QLatin1String("{}");
}

static QString extractResponsesText(const QJsonObject& outputItem) {
    const QString type = outputItem.value("type").toString();
    if (type == QLatin1String("message")) {
        QJsonValue content = outputItem.value("content");
        if (content.isString()) return content.toString().trimmed();
        if (content.isArray()) {
            for (const QJsonValue& v : content.toArray()) {
                QJsonObject block = v.toObject();
                if (block.value("type").toString() == QLatin1String("output_text")) {
                    const QString text = block.value("text").toString();
                    if (!text.isEmpty()) return text.trimmed();
                }
            }
        }
    }
    if (type == QLatin1String("output_text")) {
        return outputItem.value("text").toString().trimmed();
    }
    return {};
}

} // namespace

std::string buildOpenAIRequestBody(const std::vector<LlmMessage>& messages,
                                  const std::string& toolsJson, const LlmConfig& cfg) {
    QJsonObject root;
    root["model"] = QString::fromStdString(cfg.model);
    root["max_tokens"] = cfg.maxTokens;
    if (cfg.enableThinking.has_value())
        root["chat_template_kwargs"] = QJsonObject{{"enable_thinking", *cfg.enableThinking}};

    QJsonArray msgArr;
    for (const auto& m : messages) {
        QJsonObject mo;
        if (m.role == "tool") {
            mo["role"] = "tool";
            mo["tool_call_id"] = QString::fromStdString(m.toolCallId);
            QString s;
            for (const auto& p : m.parts) if (p.kind == LlmPart::Text) s += QString::fromStdString(p.text);
            mo["content"] = s;
        } else if (m.role == "assistant" && !m.toolName.empty()) {
            mo["role"] = "assistant";
            QJsonObject fn;
            fn["name"] = QString::fromStdString(m.toolName);
            fn["arguments"] = QString::fromStdString(m.toolArgsJson.empty() ? std::string("{}") : m.toolArgsJson);
            QJsonObject tc;
            tc["id"] = QString::fromStdString(m.toolCallId);
            tc["type"] = "function";
            tc["function"] = fn;
            QJsonArray tcs; tcs.append(tc);
            mo["tool_calls"] = tcs;
        } else {
            mo["role"] = (m.role == "assistant")
                            ? "assistant"
                            : (m.role == "system" ? "system" : "user");
            mo["content"] = openaiContent(m.parts);
        }
        msgArr.append(mo);
    }
    root["messages"] = msgArr;

    QString toolsValue = toolListFromJson(toolsJson, false);
    if (!toolsValue.isEmpty()) {
        QJsonDocument toolsDoc = QJsonDocument::fromJson(toolsValue.toUtf8());
        if (toolsDoc.isArray()) {
            root["tools"] = toolsDoc.array();
            root["tool_choice"] = "auto";
            root["parallel_tool_calls"] = false; // LlmReply carries one tool call.
        }
    }

    return QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
}

std::string buildOpenAIResponsesRequestBody(const std::vector<LlmMessage>& messages,
                                          const std::string& toolsJson, const LlmConfig& cfg) {
    QJsonObject root;
    root["model"] = QString::fromStdString(cfg.model);
    root["max_output_tokens"] = cfg.maxTokens;
    root["input"] = responsesInputFromMessages(messages);

    QString toolsValue = toolListFromJson(toolsJson, true);
    if (!toolsValue.isEmpty()) {
        QJsonDocument toolsDoc = QJsonDocument::fromJson(toolsValue.toUtf8());
        if (toolsDoc.isArray()) {
            root["tools"] = toolsDoc.array();
            root["tool_choice"] = "auto";
            root["parallel_tool_calls"] = false;
        }
    }

    return QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
}

std::string resolveOpenAICompatEndpoint(const std::string& baseUrl) {
    const QString base = QString::fromStdString(baseUrl).trimmed();
    if (base.isEmpty()) return {};
    const QUrl parsed(base);
    if (!parsed.isValid()) return baseUrl;

    const QString path = parsed.path().toLower();
    if (path.endsWith(QLatin1String("/chat/completions"), Qt::CaseInsensitive) ||
        path.endsWith(QLatin1String("/chat/completions/"), Qt::CaseInsensitive)) {
        return baseUrl;
    }
    if (path.endsWith(QLatin1String("/responses"), Qt::CaseInsensitive) ||
        path.endsWith(QLatin1String("/responses/"), Qt::CaseInsensitive) ||
        path.endsWith(QLatin1String("/v1/responses"), Qt::CaseInsensitive) ||
        path.endsWith(QLatin1String("/v1/responses/"), Qt::CaseInsensitive)) {
        return baseUrl;
    }

    if (path.endsWith(QLatin1String("/v1"), Qt::CaseInsensitive) ||
        path.endsWith(QLatin1String("/v1/"), Qt::CaseInsensitive)) {
        return (base.endsWith(QLatin1Char('/'))
            ? base.toStdString() + "responses"
            : base.toStdString() + "/responses");
    }
    if (path.isEmpty() || path == QLatin1String("/")) {
        return (base.endsWith(QLatin1Char('/'))
            ? base.toStdString() + "v1/responses"
            : base.toStdString() + "/v1/responses");
    }
    return baseUrl;
}

LlmReply parseOpenAIReply(const std::string& responseJson) {
    LlmReply reply;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(responseJson));
    if (!doc.isObject()) {
        reply.kind = LlmReply::Error;
        reply.error = "parse_error";
        return reply;
    }
    QJsonObject obj = doc.object();

    if (obj.contains("error") && obj.value("error").isObject()) {
        QJsonObject err = obj.value("error").toObject();
        QString msg = err.value("message").toString();
        reply.kind = LlmReply::Error;
        reply.error = msg.isEmpty() ? std::string("parse_error") : msg.toStdString();
        return reply;
    }

    const QJsonObject usage = obj.value("usage").toObject();
    reply.inputTokens = usage.value("prompt_tokens").toInt(-1);
    if (reply.inputTokens < 0) {
        reply.inputTokens = usage.value("input_tokens").toInt(-1);
    }
    reply.outputTokens = usage.value("completion_tokens").toInt(-1);
    if (reply.outputTokens < 0) {
        reply.outputTokens = usage.value("output_tokens").toInt(-1);
    }
    if (reply.inputTokens < 0) reply.inputTokens = -1;
    if (reply.outputTokens < 0) reply.outputTokens = -1;

    QJsonArray choices = obj.value("choices").toArray();
    if (!choices.isEmpty()) {
        QJsonObject message = choices.at(0).toObject().value("message").toObject();
        QJsonArray toolCalls = message.value("tool_calls").toArray();
        if (toolCalls.size() > 1) {
            reply.kind = LlmReply::Error;
            reply.error = "multiple_tool_calls_not_supported";
            return reply;
        }
        if (!toolCalls.isEmpty()) {
            QJsonObject tc = toolCalls.at(0).toObject();
            QJsonObject fn = tc.value("function").toObject();
            reply.kind = LlmReply::ToolCall;
            reply.toolName = fn.value("name").toString().toStdString();
            reply.toolCallId = tc.value("id").toString().toStdString();
            QString args = fn.value("arguments").toString();
            reply.toolArgsJson = args.isEmpty() ? std::string("{}") : args.toStdString();
            return reply;
        }
        QJsonValue content = message.value("content");
        if (content.isString()) {
            reply.kind = LlmReply::Text;
            reply.text = content.toString().toStdString();
            return reply;
        }
    }

    const QJsonArray outputs = obj.value("output").toArray();
    int toolCount = 0;
    for (const QJsonValue& each : outputs) {
        const QString type = each.toObject().value("type").toString();
        if ((type == QLatin1String("function_call") || type == QLatin1String("tool_call"))
            && ++toolCount > 1) {
            reply.kind = LlmReply::Error;
            reply.error = "multiple_tool_calls_not_supported";
            return reply;
        }
    }
    for (const QJsonValue& each : outputs) {
        if (!each.isObject()) continue;
        const QJsonObject item = each.toObject();
        const QString type = item.value("type").toString();

        if (type == QLatin1String("function_call") || type == QLatin1String("tool_call")) {
            reply.kind = LlmReply::ToolCall;
            const QJsonObject fn = item.value("function").toObject();
            reply.toolName = fn.value("name").toString().toStdString();
            if (reply.toolName.empty())
                reply.toolName = item.value("name").toString().toStdString();
            reply.toolCallId = item.value("call_id").toString().toStdString();
            if (reply.toolCallId.empty())
                reply.toolCallId = item.value("id").toString().toStdString();

            QJsonValue args = fn.isEmpty() ? item.value("arguments") : fn.value("arguments");
            reply.toolArgsJson = jsonValueToString(args).toStdString();
            if (reply.toolArgsJson.empty()) reply.toolArgsJson = "{}";
            return reply;
        }

        const QString text = extractResponsesText(item);
        if (!text.isEmpty()) {
            reply.kind = LlmReply::Text;
            reply.text = text.toStdString();
            return reply;
        }
    }

    reply.kind = LlmReply::Error;
    reply.error = "parse_error";
    return reply;
}

}
