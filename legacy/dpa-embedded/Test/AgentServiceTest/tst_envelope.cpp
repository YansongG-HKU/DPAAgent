#include <QtTest/QtTest>
#include "EnvelopeAnalysis.h"
#include "Diagnostics.h"
#include <cmath>
#include <vector>
#include <limits>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
using namespace Model;
class TstEnvelope : public QObject { Q_OBJECT
private slots:
    void scipy_reference_data() {
        QTest::addColumn<QJsonObject>("record");
        QFile file(QFINDTESTDATA("../../TestData/NumericalReference/envelope-v1.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto root = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(root.value("schema").toString(), QString("dpa.numerical.reference/v1"));
        const auto cases = root.value("cases").toArray();
        QCOMPARE(cases.size(), 10);
        for (const auto entry : cases) {
            const auto record = entry.toObject();
            QTest::newRow(record.value("id").toString().toUtf8().constData()) << record;
        }
    }
    void scipy_reference() {
        QFETCH(QJsonObject, record);
        std::vector<double> waveform;
        for (const auto value : record.value("values").toArray()) waveform.push_back(value.toDouble());
        const auto envelope = hilbertEnvelope(waveform);
        const auto spectrum = envelopeSpectrum(waveform, record.value("sampling_rate_hz").toDouble(),
            record.value("band_low_hz").toDouble(), record.value("band_high_hz").toDouble());
        const auto compare = [](const std::vector<double>& actual, const QJsonArray& expected) {
            QCOMPARE(static_cast<int>(actual.size()), expected.size());
            for (int i = 0; i < expected.size(); ++i) {
                const double reference = expected.at(i).toDouble();
                QVERIFY2(std::isfinite(actual[static_cast<size_t>(i)])
                    && std::abs(actual[static_cast<size_t>(i)] - reference) <= 1e-10 + 1e-9 * std::abs(reference),
                    qPrintable(QString("sample %1 differs from SciPy: %2 vs %3")
                        .arg(i).arg(actual[static_cast<size_t>(i)], 0, 'g', 16).arg(reference, 0, 'g', 16)));
            }
        };
        compare(envelope, record.value("envelope").toArray());
        compare(spectrum.freqHz, record.value("spectrum_frequency_hz").toArray());
        compare(spectrum.mag, record.value("spectrum_amplitude").toArray());
    }
    void invalid_numerical_input_is_rejected() {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        for (double invalid : {nan, inf, -inf}) {
            QVERIFY(hilbertEnvelope({1, invalid, 2, 3}).empty());
            QVERIFY(envelopeSpectrum({1, invalid, 2, 3}, 100, 0, 0).mag.empty());
            QVERIFY(envelopeSpectrum({1, 2, 3, 4}, invalid, 0, 0).mag.empty());
            QVERIFY(envelopeSpectrum({1, 2, 3, 4}, 100, 0, invalid).mag.empty());
        }
        QVERIFY(envelopeSpectrum({1,2,3,4},100,20,10).mag.empty());
        QVERIFY(envelopeSpectrum({1,2,3,4},100,20,60).mag.empty());
        QVERIFY(envelopeSpectrum({1,2,3,4},100,-1,20).mag.empty());
    }
    void hilbert_envelope_of_sine_is_amplitude() {
        const double fs = 10000, f = 100, A = 3.0;
        const int N = 1024;
        std::vector<double> x(N);
        for (int i = 0; i < N; ++i) x[i] = A * std::sin(2*M_PI*f*(i/fs));
        auto env = hilbertEnvelope(x);
        QCOMPARE((int)env.size(), N);
        // Interior samples (away from edge transients) should track the amplitude A.
        QVERIFY(std::fabs(env[N/2] - A) < 0.2);
    }
    void envelope_spectrum_recovers_modulation_frequency() {
        // AM: carrier fc=1024, modulation fm=48 (bin-aligned at fs=8192,N=2048) with depth 0.5.
        // Envelope spectrum must peak at 48 Hz AND report amplitude ~= the modulation depth 0.5,
        // which validates the one-sided 2/N interior scaling (not just argmax frequency).
        const double fs = 8192, fc = 1024, fm = 48, depth = 0.5;
        const int N = 2048;
        std::vector<double> s(N);
        for (int i = 0; i < N; ++i) {
            const double t = i / fs;
            s[i] = (1.0 + depth*std::cos(2*M_PI*fm*t)) * std::cos(2*M_PI*fc*t);
        }
        auto spec = envelopeSpectrum(s, fs, 512, 1536);   // band-pass around the carrier
        QVERIFY(spec.freqHz.size() > 10);
        double bestF = 0, bestM = -1;
        for (size_t i = 0; i < spec.freqHz.size(); ++i)
            if (spec.freqHz[i] > 10 && spec.mag[i] > bestM) { bestM = spec.mag[i]; bestF = spec.freqHz[i]; }
        QVERIFY2(std::fabs(bestF - fm) < 3.0,
                 qPrintable(QString("envelope peak at %1 Hz, expected ~48 Hz").arg(bestF)));
        QVERIFY2(std::fabs(bestM - depth) < 0.12,
                 qPrintable(QString("envelope peak amplitude %1, expected ~0.5").arg(bestM)));
    }
    void empty_or_tiny_input_is_safe() {
        QVERIFY(envelopeSpectrum({}, 1000, 0, 0).mag.empty());
        QVERIFY(envelopeSpectrum({1,2}, 1000, 0, 0).mag.empty());
    }
    void peaks_result_formatting_is_grounded() {
        // buildPeaksResult: non-empty -> ok, summary names the top freq, dataJson carries peaks.
        std::vector<SpectralPeak> peaks = {{50.0, 1.0, 0.9}, {100.0, 0.4, 0.3}};
        ToolResult r = buildPeaksResult(peaks, "包络谱");
        QVERIFY(r.ok);
        QVERIFY(r.summary.find("50") != std::string::npos);
        QVERIFY(r.dataJson.find("peaks") != std::string::npos);
        // empty -> not ok, error flagged (so the model doesn't hallucinate a bearing fault).
        ToolResult e = buildPeaksResult({}, "包络谱");
        QVERIFY(!e.ok);
        QCOMPARE(e.error, std::string("no_peaks"));
    }
};
int runTstEnvelope(int argc,char**argv){ TstEnvelope t; return QTest::qExec(&t,argc,argv); }
#include "tst_envelope.moc"
