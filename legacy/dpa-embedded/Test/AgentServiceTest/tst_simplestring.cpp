#include <QtTest/QtTest>

#include "Utils/SimpleString.h"

#include <cstdint>
#include <cstdio>

class TstSimpleString : public QObject
{
    Q_OBJECT

private slots:
    void pointerFormattingPreservesNativeWidth()
    {
        const std::uintptr_t value = sizeof(std::uintptr_t) > sizeof(std::uint32_t)
            ? static_cast<std::uintptr_t>(UINT64_C(0x123456789ABCDEF0))
            : static_cast<std::uintptr_t>(UINT32_C(0x89ABCDEF));
        const auto pointer = reinterpret_cast<const void*>(value);

        char expected[2 + (sizeof(std::uintptr_t) * 2) + 1] = {};
        const int written = std::snprintf(
            expected,
            sizeof(expected),
            "0x%llX",
            static_cast<unsigned long long>(value));
        QVERIFY(written > 0);
        QVERIFY(static_cast<std::size_t>(written) < sizeof(expected));

        QCOMPARE(SimpleString(pointer).toString(), std::string(expected));
        QCOMPARE(SimpleString(static_cast<const void*>(nullptr)).toString(), std::string("0x0"));
    }
};

int runTstSimpleString(int argc, char** argv)
{
    TstSimpleString test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_simplestring.moc"
