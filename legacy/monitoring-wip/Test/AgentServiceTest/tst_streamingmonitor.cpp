#include <QtTest/QtTest>
#include <cmath>
#include <limits>
#include <vector>

#include "Utils/RingBuffer.h"
#include "Utils/StreamingStatistics.h"
#include "Utils/TimeWindowAggregator.h"
#include "AlertEngine.h"

class TstStreamingMonitor : public QObject
{
    Q_OBJECT
private slots:
    void ringBufferPushPop()
    {
        utils::RingBuffer<int, 4> rb;
        QVERIFY(rb.empty());
        QVERIFY(rb.capacity() == 3);
        QVERIFY(rb.push(10));
        QVERIFY(rb.push(20));
        QVERIFY(rb.push(30));
        QVERIFY(rb.full());
        QVERIFY(!rb.push(40));
        int val = 0;
        QVERIFY(rb.pop(val));
        QVERIFY(val == 10);
        QVERIFY(rb.pop(val));
        QVERIFY(val == 20);
        QVERIFY(rb.pop(val));
        QVERIFY(val == 30);
        QVERIFY(rb.empty());
        QVERIFY(!rb.pop(val));
    }

    void ringBufferPeek()
    {
        utils::RingBuffer<double, 8> rb;
        rb.push(1.5);
        rb.push(2.5);
        rb.push(3.5);
        QVERIFY(rb.peek() == 1.5);
        QVERIFY(rb.peekAt(2) == 3.5);
        rb.popDiscard();
        QVERIFY(rb.peek() == 2.5);
    }

    void ringBufferIterator()
    {
        utils::RingBuffer<int, 4> rb;
        rb.push(1);
        rb.push(2);
        rb.push(3);
        std::vector<int> collected;
        for (const auto& v : rb) collected.push_back(v);
        QVERIFY(collected.size() == 3);
        QVERIFY(collected[0] == 1);
        QVERIFY(collected[1] == 2);
        QVERIFY(collected[2] == 3);
    }

    void ringBufferOverwrite()
    {
        utils::RingBuffer<int, 4> rb;
        for (int i = 0; i < 3; ++i) QVERIFY(rb.push(i));
        QVERIFY(rb.full());
        QVERIFY(!rb.push(99));
        int val = 0;
        QVERIFY(rb.pop(val));
        QVERIFY(val == 0);
        QVERIFY(rb.push(100));
        QVERIFY(rb.size() == 3);
        QVERIFY(rb.peekAt(0) == 1);
        QVERIFY(rb.peekAt(2) == 100);
    }

    void streamingStatsBasic()
    {
        utils::StreamingStatistics ss;
        QVERIFY(ss.empty());
        ss.push(10.0);
        ss.push(20.0);
        ss.push(30.0);
        QVERIFY(ss.count() == 3);
        QVERIFY(std::abs(ss.mean() - 20.0) < 1e-9);
        QVERIFY(std::abs(ss.rms() - std::sqrt((100.0 + 400.0 + 900.0) / 3.0)) < 1e-9);
        QVERIFY(ss.min() == 10.0);
        QVERIFY(ss.max() == 30.0);
        QVERIFY(ss.last() == 30.0);
    }

    void streamingStatsVariance()
    {
        utils::StreamingStatistics ss;
        for (int i = 1; i <= 5; ++i) ss.push(static_cast<double>(i));
        const double expectedVar = 2.5;
        QVERIFY(std::abs(ss.variance() - expectedVar) < 1e-9);
        QVERIFY(std::abs(ss.stddev() - std::sqrt(expectedVar)) < 1e-9);
    }

    void streamingStatsKurtosis()
    {
        utils::StreamingStatistics ss;
        std::vector<double> vals = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0};
        for (double v : vals) ss.push(v);
        QVERIFY(std::isfinite(ss.kurtosis()));
        QVERIFY(std::abs(ss.kurtosis()) < 2.0);
    }

    void streamingStatsPeak()
    {
        utils::StreamingStatistics ss;
        ss.push(-5.0);
        ss.push(3.0);
        ss.push(-1.0);
        QVERIFY(ss.peak() == 5.0);
        QVERIFY(ss.peakToPeak() == 8.0);
    }

    void streamingStatsIgnoresNaN()
    {
        utils::StreamingStatistics ss;
        ss.push(std::numeric_limits<double>::quiet_NaN());
        ss.push(10.0);
        ss.push(std::numeric_limits<double>::infinity());
        ss.push(20.0);
        QVERIFY(ss.count() == 2);
        QVERIFY(std::abs(ss.mean() - 15.0) < 1e-9);
    }

    void streamingStatsCrestFactor()
    {
        utils::StreamingStatistics ss;
        ss.push(-3.0);
        ss.push(3.0);
        ss.push(0.5);
        const double r = ss.rms();
        QVERIFY(std::isfinite(r) && r > 0);
        const double cf = ss.crestFactor();
        QVERIFY(std::isfinite(cf));
        QVERIFY(cf > 0);
    }

    void streamingStatsReset()
    {
        utils::StreamingStatistics ss;
        ss.push(1.0);
        ss.push(2.0);
        ss.reset();
        QVERIFY(ss.empty());
        QVERIFY(ss.count() == 0);
    }

    void timeWindowAggregator()
    {
        auto agg = utils::makeDefaultAggregator();
        QVERIFY(agg.numWindows() == 4);
        for (int i = 0; i < 10; ++i) agg.push(static_cast<double>(i) * 1.0, i * 1000);
        const auto snap = agg.lastSnapshot(0);
        QVERIFY(snap.count > 0);
        QVERIFY(std::isfinite(snap.mean));
    }

    void alertEngineHighLimit()
    {
        AlertEngine::Config cfg;
        cfg.sustainedSamples = 2;
        cfg.cooldownMs = 0;
        cfg.deadBand.ratio = 0.02;
        AlertEngine engine(cfg);

        auto d1 = engine.evaluate("ch1", 5.0, 5.0, 5.0, 10.0, 0.0,
                                  std::numeric_limits<double>::quiet_NaN(),
                                  std::numeric_limits<double>::quiet_NaN(),
                                  1000, false, false);
        QVERIFY(d1.empty());

        auto d2 = engine.evaluate("ch1", 11.0, 10.5, 11.0, 10.0, 0.0,
                                  std::numeric_limits<double>::quiet_NaN(),
                                  std::numeric_limits<double>::quiet_NaN(),
                                  2000, false, false);
        QVERIFY(d2.empty());

        auto d3 = engine.evaluate("ch1", 12.0, 11.5, 12.0, 10.0, 0.0,
                                  std::numeric_limits<double>::quiet_NaN(),
                                  std::numeric_limits<double>::quiet_NaN(),
                                  3000, false, false);
        bool found = false;
        for (const auto& d : d3) {
            if (d.conditionType == AlertEngine::ConditionType::High && d.isActive) {
                found = true;
                QVERIFY(!d.alarmId.empty());
            }
        }
        QVERIFY(found);
    }

    void alertEngineDeadBand()
    {
        AlertEngine::Config cfg;
        cfg.sustainedSamples = 1;
        cfg.cooldownMs = 0;
        cfg.deadBand.ratio = 0.1;
        AlertEngine engine(cfg);

        engine.evaluate("ch1", 11.0, 11.0, 11.0, 10.0, 0.0,
                        std::numeric_limits<double>::quiet_NaN(),
                        std::numeric_limits<double>::quiet_NaN(),
                        1000, false, false);

        auto d = engine.evaluate("ch1", 8.5, 8.5, 8.5, 10.0, 0.0,
                                 std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::quiet_NaN(),
                                 2000, false, false);
        bool recovered = false;
        for (const auto& dec : d) {
            if (dec.conditionType == AlertEngine::ConditionType::High && dec.isRecovered)
                recovered = true;
        }
        QVERIFY(recovered);
    }

    void alertEngineSuppression()
    {
        AlertEngine::Config cfg;
        cfg.sustainedSamples = 1;
        cfg.cooldownMs = 60000;
        AlertEngine engine(cfg);

        engine.evaluate("ch1", 11.0, 11.0, 11.0, 10.0, 0.0,
                        std::numeric_limits<double>::quiet_NaN(),
                        std::numeric_limits<double>::quiet_NaN(),
                        1000, false, false);
        auto d = engine.evaluate("ch1", 12.0, 12.0, 12.0, 10.0, 0.0,
                                 std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::quiet_NaN(),
                                 2000, false, false);
        bool suppressed = true;
        for (const auto& dec : d) {
            if (dec.conditionType == AlertEngine::ConditionType::High && dec.isActive)
                suppressed = false;
        }
        QVERIFY(suppressed);
    }

    void alertEngineAcknowledge()
    {
        AlertEngine::Config cfg;
        cfg.sustainedSamples = 1;
        cfg.cooldownMs = 0;
        AlertEngine engine(cfg);

        engine.evaluate("ch1", 11.0, 11.0, 11.0, 10.0, 0.0,
                        std::numeric_limits<double>::quiet_NaN(),
                        std::numeric_limits<double>::quiet_NaN(),
                        1000, false, false);
        auto s = engine.acknowledge("ch1", AlertEngine::ConditionType::High, "operator note");
        QVERIFY(s.acknowledged);
        QVERIFY(s.note == "operator note");
    }

    void alertEngineStale()
    {
        AlertEngine::Config cfg;
        cfg.sustainedSamples = 1;
        AlertEngine engine(cfg);

        auto d = engine.evaluate("ch1", 5.0, 5.0, 5.0, 10.0, 0.0,
                                 std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::quiet_NaN(),
                                 1000, true, false);
        bool found = false;
        for (const auto& dec : d) {
            if (dec.conditionType == AlertEngine::ConditionType::Stale && dec.isActive)
                found = true;
        }
        QVERIFY(found);
    }

    void alertEngineKurtosis()
    {
        AlertEngine::Config cfg;
        cfg.sustainedSamples = 1;
        cfg.kurtosisAlarmEnabled = true;
        cfg.kurtosisWarningThreshold = 3.0;
        cfg.kurtosisCriticalThreshold = 5.0;
        AlertEngine engine(cfg);

        auto d = engine.evaluate("ch1", 5.0, 5.0, 5.0, 100.0, 0.0,
                                 6.0,
                                 std::numeric_limits<double>::quiet_NaN(),
                                 1000, false, false);
        bool foundCritical = false;
        for (const auto& dec : d) {
            if (dec.conditionType == AlertEngine::ConditionType::KurtosisHigh
                && dec.severity == AlertEngine::Severity::Critical)
                foundCritical = true;
        }
        QVERIFY(foundCritical);
    }

    void alertEngineRateOfChange()
    {
        AlertEngine::Config cfg;
        cfg.sustainedSamples = 1;
        cfg.trendAlarmEnabled = true;
        cfg.rateOfChangeWarningPerMin = 5.0;
        AlertEngine engine(cfg);

        auto d = engine.evaluate("ch1", 5.0, 5.0, 5.0, 100.0, 0.0,
                                 std::numeric_limits<double>::quiet_NaN(),
                                 10.0, 1000, false, false);
        bool found = false;
        for (const auto& dec : d) {
            if (dec.conditionType == AlertEngine::ConditionType::RateOfChange && dec.isActive)
                found = true;
        }
        QVERIFY(found);
    }

    void alertEngineReset()
    {
        AlertEngine::Config cfg;
        cfg.sustainedSamples = 1;
        AlertEngine engine(cfg);
        engine.evaluate("ch1", 11.0, 11.0, 11.0, 10.0, 0.0,
                        std::numeric_limits<double>::quiet_NaN(),
                        std::numeric_limits<double>::quiet_NaN(),
                        1000, false, false);
        engine.resetAll();
        auto st = engine.stats();
        QVERIFY(st.activeAlarms == 0);
    }
};

int runTstStreamingMonitor(int argc, char** argv)
{
    int failures = 0;
    {
        TstStreamingMonitor tc;
        failures += QTest::qExec(&tc, argc, argv);
    }
    return failures;
}

#include "tst_streamingmonitor.moc"
