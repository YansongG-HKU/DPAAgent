#include <QtTest/QtTest>

#include "MeasurementAdapterContract.h"
#include "AgentJson.h"

#include <QJsonArray>

using namespace Model;

class TstMeasurementAdapterContract : public QObject {
    Q_OBJECT
private slots:
    void catalog_marks_external_integrations_with_runtime_boundary() {
        const QJsonObject catalog = AgentJson::parseObject(measurementAdapterCatalogJson());
        QCOMPARE(catalog.value("schema").toString(), QString("dpa.measurement.adapter.catalog"));
        const QJsonArray records = catalog.value("records").toArray();
        QCOMPARE(records.size(), 5);
        int implemented = 0;
        int blocked = 0;
        for (const QJsonValue& record : records) {
            const auto validation = validateMeasurementAdapterDescriptorJson(
                AgentJson::dump(record.toObject()));
            QVERIFY(validation.ok);
            if (record.toObject().value("implementation_status").toString() == "implemented") ++implemented;
            if (record.toObject().value("implementation_status").toString() == "blocked") ++blocked;
        }
        QCOMPARE(implemented, 3);
        QCOMPARE(blocked, 2);
    }

    void planned_descriptor_is_not_silently_available() {
        MeasurementAdapterDescriptor descriptor;
        descriptor.adapterId = "external-test";
        descriptor.kind = "file";
        descriptor.format = "asam.mdf4";
        descriptor.implementationStatus = "planned";
        descriptor.trust = "imported";
        descriptor.capabilities = {"metadata"};
        QVERIFY(validateMeasurementAdapterDescriptorJson(
            measurementAdapterDescriptorJson(descriptor)).ok);
    }

    void blocked_descriptor_is_valid_but_not_available() {
        MeasurementAdapterDescriptor descriptor;
        descriptor.adapterId = "ods-test";
        descriptor.kind = "network";
        descriptor.format = "asam.ods";
        descriptor.implementationStatus = "blocked";
        descriptor.trust = "imported";
        descriptor.capabilities = {"probe"};
        QVERIFY(validateMeasurementAdapterDescriptorJson(
            measurementAdapterDescriptorJson(descriptor)).ok);
    }

    void invalid_status_is_rejected() {
        const QJsonObject catalog = AgentJson::parseObject(measurementAdapterCatalogJson());
        QJsonObject record = catalog.value("records").toArray().at(0).toObject();
        record["implementation_status"] = "available";
        const auto validation = validateMeasurementAdapterDescriptorJson(AgentJson::dump(record));
        QVERIFY(!validation.ok);
        QCOMPARE(QString::fromStdString(validation.error), QString("adapter_value_invalid"));
    }
};

int runTstMeasurementAdapterContract(int argc, char** argv) {
    TstMeasurementAdapterContract test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_measurementadaptercontract.moc"
