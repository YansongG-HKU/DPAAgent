#include "ToolRegistry.h"
#include "AgentJson.h"
#include <QJsonArray>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <cmath>
using namespace Model;

static ToolResult invalidArguments(const QString& detail) {
    ToolResult r;
    r.ok = false;
    r.error = "invalid_arguments";
    r.summary = detail.toStdString();
    return r;
}

static ToolResult scopeForbidden() {
    ToolResult r;
    r.ok = false;
    r.error = "scope_forbidden";
    r.summary = "MCP may execute only explicitly classified read tools.";
    return r;
}

static bool hasExplicitDangerLevel(const AgentTool& tool) {
    return tool.dangerLevel == "read" || tool.dangerLevel == "write";
}

static bool parseSchemaObject(const std::string& json, QJsonObject& out) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(json), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return false;
    out = document.object();
    return true;
}

static bool matchesType(const QJsonValue& value, const QString& type) {
    if (type == "string") return value.isString();
    if (type == "number") return value.isDouble();
    if (type == "integer") return value.isDouble() && std::floor(value.toDouble()) == value.toDouble();
    if (type == "boolean") return value.isBool();
    if (type == "object") return value.isObject();
    if (type == "array") return value.isArray();
    if (type == "null") return value.isNull();
    return true;
}

static QString validateValue(const QJsonValue& value, const QJsonObject& schema,
                             const QString& path) {
    const QString type = schema.value("type").toString();
    if (!type.isEmpty() && !matchesType(value, type))
        return path + " has wrong type";

    const QJsonArray allowed = schema.value("enum").toArray();
    if (!allowed.isEmpty() && !allowed.contains(value))
        return path + " is not an allowed value";

    if (value.isDouble()) {
        if (schema.contains("minimum") && value.toDouble() < schema.value("minimum").toDouble())
            return path + " is below minimum";
        if (schema.contains("maximum") && value.toDouble() > schema.value("maximum").toDouble())
            return path + " is above maximum";
    }
    if (value.isString()) {
        const int length = value.toString().size();
        if (schema.contains("minLength") && length < schema.value("minLength").toInt())
            return path + " is shorter than minLength";
        if (schema.contains("maxLength") && length > schema.value("maxLength").toInt())
            return path + " is longer than maxLength";
    }
    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        if (schema.contains("minItems") && array.size() < schema.value("minItems").toInt())
            return path + " has fewer than minItems";
        if (schema.contains("maxItems") && array.size() > schema.value("maxItems").toInt())
            return path + " has more than maxItems";
        const QJsonObject itemSchema = schema.value("items").toObject();
        if (!itemSchema.isEmpty()) {
            for (int i = 0; i < array.size(); ++i) {
                const QString error = validateValue(array.at(i), itemSchema,
                                                    path + "[" + QString::number(i) + "]");
                if (!error.isEmpty()) return error;
            }
        }
    }
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (const QJsonValue& requiredValue : schema.value("required").toArray()) {
            const QString key = requiredValue.toString();
            if (!key.isEmpty() && !object.contains(key))
                return path + "." + key + " is required";
        }
        const QJsonObject properties = schema.value("properties").toObject();
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            if (!object.contains(it.key())) continue;
            const QString error = validateValue(object.value(it.key()), it.value().toObject(),
                                                path + "." + it.key());
            if (!error.isEmpty()) return error;
        }
        if (schema.value("additionalProperties").isBool()
            && !schema.value("additionalProperties").toBool()) {
            for (auto it = object.begin(); it != object.end(); ++it)
                if (!properties.contains(it.key())) return path + "." + it.key() + " is not allowed";
        }
    }
    return {};
}

static ToolResult validateAndCall(const AgentTool& tool, const std::string& argsJson) {
    QJsonParseError parseError;
    const QByteArray raw = QByteArray::fromStdString(argsJson.empty() ? std::string("{}") : argsJson);
    const QJsonDocument argsDoc = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !argsDoc.isObject())
        return invalidArguments("Tool arguments must be a valid JSON object.");

    const QJsonObject args = argsDoc.object();
    const QJsonObject schema = AgentJson::parseObject(tool.inputSchemaJson);
    const QString validationError = validateValue(args, schema, "arguments");
    if (!validationError.isEmpty()) return invalidArguments(validationError);
    return tool.handler(argsDoc.toJson(QJsonDocument::Compact).toStdString());
}
bool ToolRegistry::add(const AgentTool& t) {
    QJsonObject inputSchema;
    QJsonObject outputSchema;
    if (!hasExplicitDangerLevel(t)
        || !parseSchemaObject(t.inputSchemaJson, inputSchema)
        || !parseSchemaObject(t.outputSchemaJson, outputSchema))
        return false;

    AgentTool copy = t;
    if (copy.mcpName.empty()) copy.mcpName = "dpa." + copy.name;
    if (copy.title.empty()) copy.title = copy.description;
    // Store canonical, already-validated JSON so later listing/calling cannot reinterpret a
    // malformed schema as an empty object through AgentJson::parseObject().
    copy.inputSchemaJson = QJsonDocument(inputSchema).toJson(QJsonDocument::Compact).toStdString();
    copy.outputSchemaJson = QJsonDocument(outputSchema).toJson(QJsonDocument::Compact).toStdString();
    _tools[copy.name] = copy;
    return true;
}
bool ToolRegistry::has(const std::string& n) const { return _tools.count(n) > 0; }
bool ToolRegistry::hasMcp(const std::string& mcpName) const {
    for (const auto& kv : _tools)
        if (kv.second.mcpName == mcpName) return true;
    return false;
}
std::string ToolRegistry::approvalCatalogSha256() const {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    auto append = [&hash](const std::string& value) {
        const QByteArray bytes = QByteArray::fromStdString(value);
        hash.addData(QByteArray::number(bytes.size()));
        hash.addData(":", 1);
        hash.addData(bytes);
    };
    for (const auto& kv : _tools) {
        append(kv.first);
        append(kv.second.inputSchemaJson);
        append(kv.second.dangerLevel);
    }
    return hash.result().toHex().toStdString();
}
std::string ToolRegistry::listJson() const {
    QJsonArray arr;
    for (auto& kv : _tools) {
        if (!hasExplicitDangerLevel(kv.second)) continue;
        QJsonObject o;
        o["name"] = QString::fromStdString(kv.second.name);
        o["description"] = QString::fromStdString(kv.second.description);
        o["input_schema"] = AgentJson::parseObject(kv.second.inputSchemaJson);
        arr.append(o);
    }
    return QJsonDocument(arr).toJson(QJsonDocument::Compact).toStdString();
}
std::string ToolRegistry::listMcpJson() const {
    QJsonArray arr;
    for (auto& kv : _tools) {
        const AgentTool& t = kv.second;
        if (t.dangerLevel != "read") continue;
        QJsonObject o;
        o["name"] = QString::fromStdString(t.mcpName);
        o["internal_name"] = QString::fromStdString(t.name);
        o["title"] = QString::fromStdString(t.title);
        o["description"] = QString::fromStdString(t.description);
        o["inputSchema"] = AgentJson::parseObject(t.inputSchemaJson);
        o["outputSchema"] = AgentJson::parseObject(t.outputSchemaJson);
        o["category"] = QString::fromStdString(t.category);
        o["dangerLevel"] = QString::fromStdString(t.dangerLevel);
        arr.append(o);
    }
    return QJsonDocument(arr).toJson(QJsonDocument::Compact).toStdString();
}
ToolResult ToolRegistry::call(const std::string& name, const std::string& argsJson) const {
    auto it = _tools.find(name);
    if (it == _tools.end()) { ToolResult r; r.ok=false; r.error="unknown_tool"; return r; }
    if (!hasExplicitDangerLevel(it->second)) return scopeForbidden();
    return validateAndCall(it->second, argsJson);
}
ToolResult ToolRegistry::callMcp(const std::string& mcpName, const std::string& argsJson) const {
    for (auto& kv : _tools) {
        if (kv.second.mcpName != mcpName) continue;
        if (kv.second.dangerLevel != "read") return scopeForbidden();
        return validateAndCall(kv.second, argsJson);
    }
    ToolResult r;
    r.ok = false;
    r.error = "unknown_tool";
    return r;
}
bool ToolRegistry::isWrite(const std::string& name) const {
    auto it = _tools.find(name);
    return it == _tools.end() || it->second.dangerLevel != "read";
}
bool ToolRegistry::isWriteMcp(const std::string& mcpName) const {
    for (auto& kv : _tools) {
        if (kv.second.mcpName == mcpName) return kv.second.dangerLevel != "read";
    }
    return true;
}
