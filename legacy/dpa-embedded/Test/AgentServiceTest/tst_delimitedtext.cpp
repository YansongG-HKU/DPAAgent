#include <QtTest/QtTest>

#include "DelimitedText.h"

#include <QBuffer>

class TstDelimitedText : public QObject
{
    Q_OBJECT

private slots:
    void csvPreservesEmptyFields()
    {
        const QStringList fields = DelimitedText::splitCsvRecord(
            QStringLiteral("ID,,sensor-a,sensor-b\r\n"));

        QCOMPARE(fields, QStringList({
            QStringLiteral("ID"),
            QString(),
            QStringLiteral("sensor-a"),
            QStringLiteral("sensor-b") }));
    }

    void csvDecodesQuotedFields()
    {
        const QStringList fields = DelimitedText::splitCsvRecord(
            QStringLiteral("channel,,\"bearing, north\",\"say \"\"hi\"\"\"\r\n"));

        QCOMPARE(fields, QStringList({
            QStringLiteral("channel"),
            QString(),
            QStringLiteral("bearing, north"),
            QStringLiteral("say \"hi\"") }));
    }

    void csvReadsMultilineQuotedRecord()
    {
        QBuffer buffer;
        buffer.setData("name,,\"line 1\r\nline 2\",plain\r\nnext,row\r\n");
        QVERIFY(buffer.open(QIODevice::ReadOnly));

        const QString firstRecord = DelimitedText::readCsvRecord(&buffer);
        const QStringList firstFields = DelimitedText::splitCsvRecord(firstRecord);
        QCOMPARE(firstFields.size(), 4);
        QCOMPARE(firstFields.at(2), QStringLiteral("line 1\r\nline 2"));
        QCOMPARE(firstFields.at(3), QStringLiteral("plain"));

        const QStringList secondFields = DelimitedText::splitCsvRecord(
            DelimitedText::readCsvRecord(&buffer));
        QCOMPARE(secondFields, QStringList({ QStringLiteral("next"), QStringLiteral("row") }));
    }

    void csvHandlesBomBeforeQuotedMultilineFirstField()
    {
        QBuffer buffer;
        buffer.setData("\xEF\xBB\xBF\"line 1\r\nline 2\",value\r\n");
        QVERIFY(buffer.open(QIODevice::ReadOnly));

        const QString record = DelimitedText::readCsvRecord(&buffer);
        QVERIFY(buffer.atEnd());
        const QStringList fields = DelimitedText::splitCsvRecord(record);

        QCOMPARE(fields, QStringList({
            QStringLiteral("line 1\r\nline 2"),
            QStringLiteral("value") }));
    }

    void csvMetadataRowsKeepHeaderWidth()
    {
        const QStringList header = DelimitedText::splitCsvRecord(
            QStringLiteral("channel,,sensor-a,sensor-b\r\n"));
        const QStringList id = DelimitedText::splitCsvRecord(
            QStringLiteral("ID,,id-a,id-b\r\n"));
        const QStringList unit = DelimitedText::splitCsvRecord(
            QStringLiteral("unit,,m/s,m/s\r\n"));
        const QStringList zero = DelimitedText::splitCsvRecord(
            QStringLiteral("zero,,0.0,0.0\r\n"));

        QCOMPARE(header.size(), 4);
        QCOMPARE(id.size(), header.size());
        QCOMPARE(unit.size(), header.size());
        QCOMPARE(zero.size(), header.size());
        QVERIFY(id.at(1).isEmpty());
    }

    void nonEmptySplitPreservesFixedWidthBehavior()
    {
        const QStringList fields = DelimitedText::splitNonEmptyFields(
            QStringLiteral("    1      2"), QLatin1Char(' '));

        QCOMPARE(fields, QStringList({ QStringLiteral("1"), QStringLiteral("2") }));
    }
};

int runTstDelimitedText(int argc, char** argv)
{
    TstDelimitedText test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_delimitedtext.moc"
