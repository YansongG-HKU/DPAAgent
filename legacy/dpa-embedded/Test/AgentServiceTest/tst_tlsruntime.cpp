#include <QtTest>
#include <QSslSocket>

class TstTlsRuntime : public QObject
{
    Q_OBJECT
private slots:
    void supportsSslWithoutCredentials()
    {
        QVERIFY2(QSslSocket::supportsSsl(), "Qt could not initialize its TLS runtime.");

        bool valid = false;
        const int requestedHoldMs = qgetenv("DPA_TLS_PROBE_HOLD_MS").toInt(&valid);
        if (valid && requestedHoldMs > 0)
            QTest::qWait(qMin(requestedHoldMs, 10000));
    }
};

int runTstTlsRuntime(int argc, char** argv)
{
    TstTlsRuntime test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_tlsruntime.moc"
