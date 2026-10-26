#pragma once
#include <QJsonObject>
#include <QJsonDocument>
#include <string>
namespace Model { namespace AgentJson {
    QJsonObject parseObject(const std::string& json);  // {} on parse failure
    std::string dump(const QJsonObject& o);            // compact
}}
