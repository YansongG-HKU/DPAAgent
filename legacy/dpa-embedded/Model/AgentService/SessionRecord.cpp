#include "SessionRecord.h"
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace Model {

static bool isBoundedToken(const QString& value, int maxLength, bool allowDot = false) {
    if (value.isEmpty() || value.size() > maxLength) return false;
    for (const QChar character : value) {
        const ushort code = character.unicode();
        if ((code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z')
            || (code >= '0' && code <= '9') || code == '-' || code == '_'
            || (allowDot && code == '.')) continue;
        return false;
    }
    return true;
}

static bool isSha256(const QJsonValue& value) {
    if (!value.isString() || value.toString().size() != 64) return false;
    for (const QChar character : value.toString()) {
        const ushort code = character.unicode();
        if (!((code >= '0' && code <= '9') || (code >= 'a' && code <= 'f'))) return false;
    }
    return true;
}

static bool isValidPendingWrite(const QJsonObject& pending) {
    static const char* required[] = {
        "correlationId", "confirmationId", "tool", "argsSha256",
        "contextFingerprint", "catalogSha256", "approvedAtMs", "startedMs"
    };
    if (pending.size() != int(sizeof(required) / sizeof(required[0]))) return false;
    for (const char* key : required)
        if (!pending.contains(key)) return false;
    if (!isBoundedToken(pending.value("correlationId").toString(), 128)
        || !isBoundedToken(pending.value("confirmationId").toString(), 128)
        || !isBoundedToken(pending.value("tool").toString(), 128, true)
        || !isSha256(pending.value("argsSha256"))
        || !isSha256(pending.value("contextFingerprint"))
        || !isSha256(pending.value("catalogSha256"))) return false;
    for (const char* key : {"approvedAtMs", "startedMs"}) {
        const QJsonValue value = pending.value(key);
        if (!value.isDouble() || value.toDouble() < 0 || value.toDouble() > 9007199254740991.0
            || std::floor(value.toDouble()) != value.toDouble())
            return false;
    }
    return true;
}

std::string buildSessionJson(const SessionRecord& s) {
    QJsonObject root;
    root["id"]    = QString::fromStdString(s.id);
    root["title"] = QString::fromStdString(s.title);
    if (!s.diagnosticRunJson.empty()) {
        QJsonParseError runError;
        const QJsonDocument run = QJsonDocument::fromJson(
            QByteArray::fromStdString(s.diagnosticRunJson), &runError);
        if (runError.error == QJsonParseError::NoError && run.isObject())
            root["diagnostic_run"] = run.object();
    }
    if (!s.pendingWriteJson.empty()) {
        QJsonParseError pendingError;
        const QJsonDocument pending = QJsonDocument::fromJson(
            QByteArray::fromStdString(s.pendingWriteJson), &pendingError);
        if (pendingError.error != QJsonParseError::NoError || !pending.isObject()
            || !isValidPendingWrite(pending.object())) return {};
        root["pending_write"] = pending.object();
    }
    QJsonArray companionEvents;
    for (const auto& json : s.companionEvents) {
        QJsonParseError eventError;
        const QJsonDocument event = QJsonDocument::fromJson(
            QByteArray::fromStdString(json), &eventError);
        if (eventError.error == QJsonParseError::NoError && event.isObject())
            companionEvents.append(event.object());
    }
    if (!companionEvents.isEmpty()) root["companion_events"] = companionEvents;
    QJsonArray turns;
    for (const auto& t : s.turns) {
        QJsonObject turn;
        turn["role"]    = QString::fromStdString(t.role);
        turn["content"] = QString::fromStdString(t.content);
        turns.append(turn);
    }
    root["turns"] = turns;
    return QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
}

SessionRecord parseSessionJson(const std::string& json) {
    SessionRecord s;
    constexpr size_t maxSerializedBytes = 8 * 1024 * 1024;
    constexpr int maxTurns = 200;
    constexpr int maxCompanionEvents = 200;
    constexpr int maxCompanionEventBytes = 64 * 1024;
    constexpr int maxPendingWriteBytes = 8 * 1024;
    // Runtime turns keep a 512 KiB content prefix and may append a small truncation
    // marker (plus at most a few UTF-8 replacement bytes at the cut boundary).
    constexpr int maxPersistedTurnBytes = 513 * 1024;
    constexpr size_t maxSessionBytes = 4 * 1024 * 1024;
    if (json.size() > maxSerializedBytes) return s;

    QJsonParseError err;
    auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(json), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return s;
    auto root = doc.object();

    if (!root.value("id").isString() || !root.value("title").isString()
        || !root.value("turns").isArray()) return {};
    s.id    = root.value("id").toString().toStdString();
    s.title = root.value("title").toString().toStdString();
    const bool validId = s.id.empty() || (s.id.size() <= 128
        && std::all_of(s.id.begin(), s.id.end(), [](unsigned char c) {
            return std::isalnum(c) != 0 || c == '-' || c == '_';
        }));
    if (!validId || QByteArray::fromStdString(s.title).size() > 512) return {};

    if (root.contains("diagnostic_run") && !root.value("diagnostic_run").isObject()) return {};
    if (root.value("diagnostic_run").isObject())
        s.diagnosticRunJson = QJsonDocument(root.value("diagnostic_run").toObject())
            .toJson(QJsonDocument::Compact).toStdString();
    if (root.contains("pending_write") && !root.value("pending_write").isObject()) return {};
    if (root.value("pending_write").isObject()) {
        const QJsonObject pending = root.value("pending_write").toObject();
        const QByteArray serialized = QJsonDocument(pending).toJson(QJsonDocument::Compact);
        if (serialized.size() > maxPendingWriteBytes || !isValidPendingWrite(pending)) return {};
        s.pendingWriteJson = serialized.toStdString();
    }
    if (root.contains("companion_events") && !root.value("companion_events").isArray()) return {};
    const auto events = root.value("companion_events").toArray();
    if (events.size() > maxCompanionEvents) return {};
    size_t sessionBytes = s.pendingWriteJson.size();
    for (const auto& value : events) {
        if (!value.isObject()) return {};
        const QByteArray event = QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
        if (event.size() > maxCompanionEventBytes) return {};
        sessionBytes += static_cast<size_t>(event.size());
        if (sessionBytes > maxSessionBytes) return {};
        s.companionEvents.push_back(event.toStdString());
    }
    auto arr = root.value("turns").toArray();
    if (arr.size() > maxTurns) return {};
    for (const auto& v : arr) {
        if (!v.isObject()) return {};
        auto o = v.toObject();
        if (!o.value("role").isString() || !o.value("content").isString()) return {};
        SessionTurn t;
        t.role    = o.value("role").toString().toStdString();
        t.content = o.value("content").toString().toStdString();
        if (t.role != "user" && t.role != "assistant" && t.role != "tool"
            && t.role != "operator_feedback") return {};
        if (t.content.size() > static_cast<size_t>(maxPersistedTurnBytes)) return {};
        sessionBytes += t.role.size() + t.content.size();
        if (sessionBytes > maxSessionBytes) return {};
        s.turns.push_back(t);
    }
    return s;
}

} // namespace Model
