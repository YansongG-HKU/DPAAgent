#include "AgentJson.h"
namespace Model { namespace AgentJson {
    QJsonObject parseObject(const std::string& json) {
        auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(json));
        return doc.isObject() ? doc.object() : QJsonObject();
    }
    std::string dump(const QJsonObject& o) {
        return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();
    }
}}
