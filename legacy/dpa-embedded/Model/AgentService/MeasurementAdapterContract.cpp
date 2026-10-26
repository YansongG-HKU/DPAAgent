#include "MeasurementAdapterContract.h"

#include "AgentJson.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace Model {
namespace {

bool exactKeys(const QJsonObject& object, std::initializer_list<const char*> keys) {
    if (object.size() != static_cast<int>(keys.size())) return false;
    for (const char* key : keys)
        if (!object.contains(QString::fromUtf8(key))) return false;
    return true;
}

bool token(const QJsonValue& value, int maximum) {
    if (!value.isString() || value.toString().isEmpty() || value.toString().size() > maximum)
        return false;
    for (const QChar c : value.toString()) {
        const ushort code = c.unicode();
        if ((code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z')
            || (code >= '0' && code <= '9') || code == '-' || code == '_' || code == '.') continue;
        return false;
    }
    return true;
}

bool enumValue(const QJsonValue& value, std::initializer_list<const char*> allowed) {
    if (!value.isString()) return false;
    for (const char* item : allowed)
        if (value.toString() == QString::fromUtf8(item)) return true;
    return false;
}

QJsonObject descriptorObject(const MeasurementAdapterDescriptor& descriptor) {
    QJsonArray capabilities;
    for (const auto& capability : descriptor.capabilities)
        capabilities.append(QString::fromStdString(capability));
    return QJsonObject{
        {"schema", "dpa.measurement.adapter"}, {"version", descriptor.version},
        {"adapter_id", QString::fromStdString(descriptor.adapterId)},
        {"kind", QString::fromStdString(descriptor.kind)},
        {"format", QString::fromStdString(descriptor.format)},
        {"implementation_status", QString::fromStdString(descriptor.implementationStatus)},
        {"trust", QString::fromStdString(descriptor.trust)},
        {"read_only", descriptor.readOnly}, {"local_only", descriptor.localOnly},
        {"capabilities", capabilities}
    };
}

MeasurementAdapterDescriptor descriptor(const char* id, const char* kind, const char* format,
                                        const char* status, const char* trust,
                                        std::initializer_list<const char*> capabilities) {
    MeasurementAdapterDescriptor value;
    value.adapterId = id;
    value.kind = kind;
    value.format = format;
    value.implementationStatus = status;
    value.trust = trust;
    for (const char* capability : capabilities) value.capabilities.emplace_back(capability);
    return value;
}

} // namespace

std::string measurementAdapterDescriptorJson(const MeasurementAdapterDescriptor& descriptor) {
    return AgentJson::dump(descriptorObject(descriptor));
}

std::string measurementAdapterCatalogJson() {
    const std::vector<MeasurementAdapterDescriptor> descriptors = {
        descriptor("dpa.native.qt-adt", "native", "dpa.native", "implemented", "device",
                   {"metadata", "stream", "analysis_input"}),
        descriptor("dpa.simulation.scenario", "simulation", "dpa.simulation.recipe", "implemented", "simulated",
                   {"metadata", "stream", "replay"}),
        descriptor("asam.mdf4.external", "file", "asam.mdf4", "implemented", "imported",
                   {"metadata", "import", "quality", "evidence"}),
        descriptor("asam.ods.external", "network", "asam.ods", "blocked", "imported",
                   {"metadata", "query", "evidence"}),
        descriptor("opendaq.external", "network", "openDAQ", "blocked", "device",
                   {"capabilities", "stream", "processing"})
    };
    QJsonArray records;
    for (const auto& value : descriptors)
        records.append(descriptorObject(value));
    return AgentJson::dump(QJsonObject{
        {"schema", "dpa.measurement.adapter.catalog"}, {"version", 1},
        {"records", records}, {"policy", "planned_adapters_are_not_available"}
    });
}

ContractValidation validateMeasurementAdapterDescriptorJson(const std::string& json) {
    if (json.empty() || json.size() > 64 * 1024) return {false, "adapter_json_invalid"};
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(json), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return {false, "adapter_json_invalid"};
    const QJsonObject object = document.object();
    if (!exactKeys(object, {"schema", "version", "adapter_id", "kind", "format",
                             "implementation_status", "trust", "read_only", "local_only", "capabilities"}))
        return {false, "adapter_schema_invalid"};
    if (object.value("schema").toString() != "dpa.measurement.adapter"
        || object.value("version").toInt() != 1 || !token(object.value("adapter_id"), 128)
        || !enumValue(object.value("kind"), {"native", "simulation", "file", "network"})
        || !token(object.value("format"), 64)
        || !enumValue(object.value("implementation_status"), {"implemented", "planned", "blocked"})
        || !enumValue(object.value("trust"), {"device", "simulated", "imported", "unknown"})
        || !object.value("read_only").isBool() || !object.value("local_only").isBool()
        || !object.value("capabilities").isArray()) return {false, "adapter_value_invalid"};
    const QJsonArray capabilities = object.value("capabilities").toArray();
    if (capabilities.isEmpty() || capabilities.size() > 32)
        return {false, "adapter_capabilities_invalid"};
    for (const QJsonValue& capability : capabilities)
        if (!token(capability, 64)) return {false, "adapter_capability_invalid"};
    if (object.value("implementation_status").toString() == "implemented"
        && object.value("trust").toString() == "unknown") return {false, "adapter_trust_invalid"};
    return {true, {}};
}

} // namespace Model
