#include <QtTest/QtTest>
#include "AdvisoryTrigger.h"
#include "../../Model/DataService/RealTimeData.h"
#include <limits>
using namespace Model;
class TstAdvisory : public QObject { Q_OBJECT
    static ResultSummary res(double peak, double rms) {
        ResultSummary s; s.found = true; s.name = "PSD";
        s.peaks["peak_value"] = peak; s.stats["rms"] = rms; return s;
    }
private slots:
    void disabled_never_fires() {
        AdvisoryConfig cfg; cfg.enabled = false; cfg.peakValueMax = 1.0;
        AdvisoryState st; auto d = evaluateAdvisory(res(9, 9), cfg, 1000, st);
        QVERIFY(!d.fire);
    }
    void not_found_never_fires() {
        AdvisoryConfig cfg; cfg.enabled = true; cfg.peakValueMax = 1.0;
        AdvisoryState st; ResultSummary s; s.found = false;
        QVERIFY(!evaluateAdvisory(s, cfg, 1000, st).fire);
    }
    void within_limits_no_fire() {
        AdvisoryConfig cfg; cfg.enabled = true; cfg.peakValueMax = 100.0; cfg.maxByMetric["rms"] = 100.0;
        AdvisoryState st; QVERIFY(!evaluateAdvisory(res(1, 1), cfg, 1000, st).fire);
    }
    void peak_over_limit_fires() {
        AdvisoryConfig cfg; cfg.enabled = true; cfg.peakValueMax = 5.0;
        AdvisoryState st; auto d = evaluateAdvisory(res(9, 1), cfg, 1000, st);
        QVERIFY(d.fire);
        QCOMPARE(QString::fromStdString(d.metric), QString("peak_value"));
        QCOMPARE(d.value, 9.0); QCOMPARE(d.limit, 5.0);
    }
    void stat_over_limit_fires() {
        AdvisoryConfig cfg; cfg.enabled = true; cfg.maxByMetric["rms"] = 2.0;
        AdvisoryState st; auto d = evaluateAdvisory(res(0, 7), cfg, 1000, st);
        QVERIFY(d.fire);
        QCOMPARE(QString::fromStdString(d.metric), QString("rms"));
    }
    void throttles_repeat_then_allows_after_interval() {
        AdvisoryConfig cfg; cfg.enabled = true; cfg.peakValueMax = 5.0; cfg.minIntervalMs = 1000;
        AdvisoryState st;
        QVERIFY(evaluateAdvisory(res(9, 1), cfg, 1000, st).fire);   // first fires
        QVERIFY(!evaluateAdvisory(res(9, 1), cfg, 1500, st).fire);  // within 1000ms -> suppressed
        QVERIFY(evaluateAdvisory(res(9, 1), cfg, 2600, st).fire);   // after interval -> fires
    }
    void streaming_statistics_use_industrial_definitions() {
        QCOMPARE(oneSecondWindowSampleCount(1000.4), 1000LL);
        QCOMPARE(oneSecondWindowSampleCount(0.5), 0LL);
        QCOMPARE(oneSecondWindowSampleCount(std::numeric_limits<double>::infinity()), 0LL);
        QCOMPARE(sampleTimestampUs(1000000, 0.5), 1500000LL);
        QCOMPARE(sampleTimestampUs(1000000, std::numeric_limits<double>::infinity()), 0LL);
        StreamingStatisticWindow window;
        QVERIFY(!window.append(3.0, 10.0, 2));
        QVERIFY(window.append(4.0, 11.0, 2));
        QVERIFY(window.valid);
        QCOMPARE(window.count, 2LL);
        QCOMPARE(window.minimum, 3.0);
        QCOMPARE(window.maximum, 4.0);
        QVERIFY(std::abs(double(window.mean) - 3.5) < 1e-12);
        QVERIFY(std::abs(window.rms() - std::sqrt(12.5)) < 1e-12);
        QVERIFY(std::abs(window.stddev() - 0.5) < 1e-12);
        QCOMPARE(window.startX, 10.0);
        QCOMPARE(window.endX, 11.0);
        QVERIFY(window.completeX());

        window.reset();
        QVERIFY(window.append(std::numeric_limits<double>::quiet_NaN(), 12.0, 1));
        QVERIFY(!window.valid);

        window.reset();
        QVERIFY(!window.append(1.0, 2.0, 2));
        QVERIFY(window.append(2.0, 1.0, 2));
        QVERIFY(!window.completeX());
    }
    void realtime_data_publishes_each_completed_window_once() {
        RealTimeData data;
        iDataChannel* channel = data.addChannel();
        QVERIFY(channel);
        channel->setValue("SamplingRate", 2.0);
        const int channelId = channel->value<int>("ChannelId");
        std::vector<int> ids{channelId};
        Property selected(&ids);
        QCOMPARE(data.excute("StartStatisticCalc", &selected), 1);

        std::vector<double> x{0.0, 0.5};
        std::vector<double> y{3.0, 4.0};
        QVERIFY(data.append(channelId, x, y));
        QCOMPARE(channel->value<__int64>("StatisticsSequence"), 1LL);
        QCOMPARE(channel->value<__int64>("StatisticsCount"), 2LL);
        QVERIFY(channel->value<bool>("StatisticsValid"));
        QVERIFY(channel->value<bool>("StatisticsHasX"));
        QVERIFY(std::abs(channel->value<double>("Rms") - std::sqrt(12.5)) < 1e-12);
        QVERIFY(std::abs(channel->value<double>("StdDev") - 0.5) < 1e-12);
        QCOMPARE(channel->value<double>("Min"), 3.0);
        QCOMPARE(channel->value<double>("Max"), 4.0);

        x = {1.0}; y = {5.0};
        QVERIFY(data.append(channelId, x, y));
        QCOMPARE(channel->value<__int64>("StatisticsSequence"), 1LL);
        x = {1.5}; y = {6.0};
        QVERIFY(data.append(channelId, x, y));
        QCOMPARE(channel->value<__int64>("StatisticsSequence"), 2LL);

        StatisticWindowReadRequest read;
        read.channelId = channelId;
        QCOMPARE(data.excute("ReadStatisticWindows", &read), 1);
        QCOMPARE(read.windows.size(), size_t(2));
        QCOMPARE(read.windows[0].sequence, 1LL);
        QCOMPARE(read.windows[1].sequence, 2LL);
        QVERIFY(read.windows[0].completedAtMs > 0);
        QVERIFY(read.windows[1].completedAtMs >= read.windows[0].completedAtMs);
        QVERIFY(std::abs(read.windows[0].rms - std::sqrt(12.5)) < 1e-12);
        QCOMPARE(read.windows[1].minimum, 5.0);
        QCOMPARE(read.windows[1].maximum, 6.0);

        read.afterSequence = 1;
        QCOMPARE(data.excute("ReadStatisticWindows", &read), 1);
        QCOMPARE(read.windows.size(), size_t(1));
        QCOMPARE(read.windows[0].sequence, 2LL);

        for (int window = 0; window < 65; ++window) {
            x = {double(window + 2), double(window + 2) + 0.5};
            y = {double(window), double(window + 1)};
            QVERIFY(data.append(channelId, x, y));
        }
        read.afterSequence = 0;
        QCOMPARE(data.excute("ReadStatisticWindows", &read), 1);
        QCOMPARE(read.latestSequence, 67LL);
        QCOMPARE(read.oldestSequence, 4LL);
        QCOMPARE(read.windows.size(), RealTimeDataChannel::kStatisticWindowHistoryCap);
        QCOMPARE(read.windows.front().sequence, 4LL);
        QCOMPARE(read.windows.back().sequence, 67LL);

        QCOMPARE(data.excute("StopStatisticCalc", &selected), 1);
        QVERIFY(!channel->value<bool>("StatisticsValid"));
        read.afterSequence = 0;
        QCOMPARE(data.excute("ReadStatisticWindows", &read), 1);
        QVERIFY(read.windows.empty());
    }
    void companion_1_normal_four_hour_three_run_soak() {
        CompanionConfig cfg; cfg.enabled = true; cfg.sustainedSamples = 3;
        cfg.cooldownMs = 60000; cfg.hysteresisRatio = 0.02;
        for (int run = 0; run < 3; ++run) {
            CompanionState state;
            size_t eventCount = 0;
            CompanionChannelSample s;
            s.channelId = 1; s.name = "SIM-NORMAL"; s.unit = "g";
            s.hasValue = true; s.limitEnabled = true;
            s.lowLimit = -5; s.highLimit = 5;
            for (long long second = 0; second < 4 * 60 * 60; ++second) {
                const double offset = double((second % 21) - 10) * 0.05;
                s.value = offset; s.minimum = -1 + offset; s.maximum = 1 + offset;
                eventCount += evaluateCompanionChannels({s}, cfg, second * 1000, state).size();
            }
            QCOMPARE(eventCount, size_t(0));
        }

        CompanionState state;
        CompanionChannelSample invalid;
        invalid.channelId = 2; invalid.hasValue = true; invalid.limitEnabled = true;
        invalid.lowLimit = 5; invalid.highLimit = 5;
        invalid.minimum = std::numeric_limits<double>::quiet_NaN();
        invalid.maximum = 99;
        QVERIFY(evaluateCompanionChannels({invalid}, cfg, 0, state).empty());
        cfg.enabled = false; invalid.stale = true;
        QVERIFY(evaluateCompanionChannels({invalid}, cfg, 1000, state).empty());
    }
    void companion_2_sustained_high_cooldown_and_recovery() {
        CompanionConfig cfg; cfg.enabled = true; cfg.sustainedSamples = 3;
        cfg.cooldownMs = 1000; cfg.hysteresisRatio = 0.1;
        CompanionState state;
        CompanionChannelSample s;
        s.channelId = 7; s.name = "CH7"; s.unit = "g"; s.hasValue = true;
        s.limitEnabled = true; s.lowLimit = -5; s.highLimit = 5;
        s.minimum = 0; s.maximum = 9;
        QVERIFY(evaluateCompanionChannels({s}, cfg, 0, state).empty());
        QVERIFY(evaluateCompanionChannels({s}, cfg, 100, state).empty());
        auto first = evaluateCompanionChannels({s}, cfg, 200, state);
        QCOMPARE(first.size(), size_t(1));
        QCOMPARE(QString::fromStdString(first[0].kind), QString("limit_high"));
        QVERIFY(evaluateCompanionChannels({s}, cfg, 800, state).empty());
        QCOMPARE(evaluateCompanionChannels({s}, cfg, 1300, state).size(), size_t(1));
        s.maximum = 4.8;
        QVERIFY(evaluateCompanionChannels({s}, cfg, 1400, state).empty());
        s.maximum = 4.5;
        const auto recovered = evaluateCompanionChannels({s}, cfg, 1500, state);
        QCOMPARE(recovered.size(), size_t(1));
        QCOMPARE(QString::fromStdString(recovered[0].kind), QString("limit_high_recovered"));
    }
    void companion_3_sustained_low_and_hysteretic_recovery() {
        CompanionConfig cfg; cfg.enabled = true; cfg.sustainedSamples = 2;
        cfg.hysteresisRatio = 0.1;
        CompanionState state;
        CompanionChannelSample s;
        s.channelId = 3; s.name = "SIM-LOW"; s.hasValue = true; s.limitEnabled = true;
        s.lowLimit = -2; s.highLimit = 2; s.minimum = -3; s.maximum = 0;
        QVERIFY(evaluateCompanionChannels({s}, cfg, 0, state).empty());
        auto alarm = evaluateCompanionChannels({s}, cfg, 100, state);
        QCOMPARE(alarm.size(), size_t(1));
        QCOMPARE(QString::fromStdString(alarm[0].kind), QString("limit_low"));
        s.minimum = -1.9;
        QVERIFY(evaluateCompanionChannels({s}, cfg, 200, state).empty());
        s.minimum = -1.8;
        auto recovered = evaluateCompanionChannels({s}, cfg, 300, state);
        QCOMPARE(recovered.size(), size_t(1));
        QVERIFY(recovered[0].recovery);
    }
    void companion_4_transient_spikes_are_suppressed() {
        CompanionConfig cfg; cfg.enabled = true; cfg.sustainedSamples = 3;
        CompanionState state;
        CompanionChannelSample s;
        s.channelId = 4; s.name = "SIM-SPIKE"; s.hasValue = true; s.limitEnabled = true;
        s.lowLimit = -10; s.highLimit = 10; s.minimum = 0;
        size_t eventCount = 0;
        for (int cycle = 0; cycle < 100; ++cycle) {
            s.maximum = 11;
            eventCount += evaluateCompanionChannels({s}, cfg, cycle * 3000, state).size();
            eventCount += evaluateCompanionChannels({s}, cfg, cycle * 3000 + 1000, state).size();
            s.maximum = 9;
            eventCount += evaluateCompanionChannels({s}, cfg, cycle * 3000 + 2000, state).size();
        }
        QCOMPARE(eventCount, size_t(0));
        QVERIFY(!state.channels[4].high.active);
    }
    void companion_5_stale_cooldown_and_recovery() {
        CompanionConfig cfg; cfg.enabled = true; cfg.sustainedSamples = 2;
        cfg.cooldownMs = 1000;
        CompanionState state;
        CompanionChannelSample s;
        s.channelId = 5; s.name = "SIM-DROPOUT"; s.stale = true;
        QVERIFY(evaluateCompanionChannels({s}, cfg, 0, state).empty());
        auto alarm = evaluateCompanionChannels({s}, cfg, 100, state);
        QCOMPARE(alarm.size(), size_t(1));
        QCOMPARE(QString::fromStdString(alarm[0].kind), QString("data_stale"));
        QVERIFY(evaluateCompanionChannels({s}, cfg, 500, state).empty());
        QCOMPARE(evaluateCompanionChannels({s}, cfg, 1200, state).size(), size_t(1));
        s.stale = false;
        const auto recovered = evaluateCompanionChannels({s}, cfg, 1300, state);
        QCOMPARE(recovered.size(), size_t(1));
        QCOMPARE(QString::fromStdString(recovered[0].kind), QString("data_stale_recovered"));
    }
    void companion_6_device_and_storage_event_matrix() {
        const auto error = describeCompanionDeviceEvent("Current_Error", "SIM-DAQ", "I/O failure", true);
        QVERIFY(error.valid); QCOMPARE(QString::fromStdString(error.severity), QString("alarm"));
        QVERIFY(error.explain); QCOMPARE(QString::fromStdString(error.message), QString("I/O failure"));

        const auto info = describeCompanionDeviceEvent("Current_Info", "SIM-DAQ", "存储已停止", true);
        QVERIFY(info.valid); QCOMPARE(QString::fromStdString(info.severity), QString("info"));
        QVERIFY(!info.explain);

        const auto disconnected = describeCompanionDeviceEvent("NetworkStateChange", "SIM-DAQ", "", false);
        QVERIFY(disconnected.valid); QCOMPARE(QString::fromStdString(disconnected.severity), QString("alarm"));
        QVERIFY(disconnected.explain);
        const auto connected = describeCompanionDeviceEvent("NetworkStateChange", "SIM-DAQ", "", true);
        QCOMPARE(QString::fromStdString(connected.severity), QString("info")); QVERIFY(!connected.explain);

        const auto disk = describeCompanionDeviceEvent("DISKFULL", "SIM-DAQ", "", true);
        QVERIFY(disk.valid); QVERIFY(QString::fromStdString(disk.title).contains(QString::fromUtf8("存储空间不足")));
        QVERIFY(describeCompanionDeviceEvent("DEVICEDAQSTOPED", "SIM-DAQ", "", true).valid);
        QVERIFY(describeCompanionDeviceEvent("CLOSEHOST", "SIM-DAQ", "", true).valid);
        const auto limit = describeCompanionDeviceEvent("LimitStateChange", "SIM-DAQ", "", true);
        QCOMPARE(QString::fromStdString(limit.severity), QString("warning"));
        QVERIFY(!describeCompanionDeviceEvent("UNKNOWN", "SIM-DAQ", "", true).valid);
    }
    void companion_alarm_ids_acknowledgement_and_escalation_are_stable() {
        CompanionConfig cfg; cfg.enabled = true; cfg.sustainedSamples = 1;
        cfg.cooldownMs = 100; cfg.escalationMs = 500;
        CompanionState state;
        CompanionChannelSample stale;
        stale.channelId = 9; stale.name = "CH9"; stale.stale = true;

        const auto first = evaluateCompanionChannels({stale}, cfg, 0, state);
        QCOMPARE(first.size(), size_t(1));
        QCOMPARE(QString::fromStdString(first[0].lifecycle), QString("active"));
        QVERIFY(!first[0].alarmId.empty());
        const std::string alarmId = first[0].alarmId;

        const auto repeated = evaluateCompanionChannels({stale}, cfg, 100, state);
        QCOMPARE(repeated.size(), size_t(1));
        QVERIFY(repeated[0].alarmId == alarmId);
        QCOMPARE(QString::fromStdString(repeated[0].lifecycle), QString("repeat"));
        QVERIFY(acknowledgeCompanionAlarm(state, alarmId, 150));
        QVERIFY(!acknowledgeCompanionAlarm(state, "missing", 151));
        QVERIFY(evaluateCompanionChannels({stale}, cfg, 700, state).empty());

        stale.stale = false; stale.hasValue = true;
        const auto recovered = evaluateCompanionChannels({stale}, cfg, 800, state);
        QCOMPARE(recovered.size(), size_t(1));
        QVERIFY(recovered[0].alarmId == alarmId);
        QVERIFY(recovered[0].acknowledged);

        CompanionState unacknowledged;
        stale.stale = true; stale.hasValue = false;
        evaluateCompanionChannels({stale}, cfg, 1000, unacknowledged);
        const auto escalated = evaluateCompanionChannels({stale}, cfg, 1500, unacknowledged);
        QCOMPARE(escalated.size(), size_t(1));
        QCOMPARE(QString::fromStdString(escalated[0].lifecycle), QString("escalated"));
        QCOMPARE(QString::fromStdString(escalated[0].severity), QString("critical"));
        const auto escalatedRepeat = evaluateCompanionChannels({stale}, cfg, 1600, unacknowledged);
        QCOMPARE(escalatedRepeat.size(), size_t(1));
        QCOMPARE(QString::fromStdString(escalatedRepeat[0].lifecycle), QString("repeat"));
        QCOMPARE(QString::fromStdString(escalatedRepeat[0].severity), QString("critical"));

        CompanionState qualityState;
        CompanionChannelSample invalid;
        invalid.channelId = 10; invalid.name = "CH10"; invalid.qualityInvalid = true;
        const auto qualityAlarm = evaluateCompanionChannels({invalid}, cfg, 2000, qualityState);
        QCOMPARE(qualityAlarm.size(), size_t(1));
        QCOMPARE(QString::fromStdString(qualityAlarm[0].kind), QString("data_invalid"));
        invalid.qualityInvalid = false; invalid.hasValue = true;
        const auto qualityRecovery = evaluateCompanionChannels({invalid}, cfg, 2100, qualityState);
        QCOMPARE(qualityRecovery.size(), size_t(1));
        QCOMPARE(QString::fromStdString(qualityRecovery[0].kind), QString("data_invalid_recovered"));
    }
    void preflight_reports_stable_hardware_blockers() {
        const auto result = evaluateExperimentPreflight({});
        QVERIFY(!result.readyToAcquire);
        QVERIFY(!result.readyToMonitor);
        QCOMPARE(QString::fromStdString(result.overall), QString("blocked"));
        QCOMPARE(result.blockers.size(), size_t(4));
        QCOMPARE(QString::fromStdString(result.blockers[0]), QString("no_project"));
        QCOMPARE(QString::fromStdString(result.blockers[3]), QString("no_acquisition_channels"));
        QCOMPARE(result.nodes.size(), size_t(7));
        QCOMPARE(QString::fromStdString(result.nodes[0].state), QString("blocked"));
    }
    void preflight_keeps_optional_monitoring_out_of_acquisition_gate() {
        ExperimentPreflightInput input;
        input.hasProject = true;
        input.hasDaqTest = true;
        input.connected = true;
        input.selectedChannels = 4;
        const auto result = evaluateExperimentPreflight(input);
        QVERIFY(result.readyToAcquire);
        QVERIFY(!result.readyToMonitor);
        QCOMPARE(QString::fromStdString(result.overall), QString("ready"));
        QVERIFY(result.blockers.empty());
        QCOMPARE(QString::fromStdString(result.warnings[0]), QString("companion_disabled"));
        QCOMPARE(QString::fromStdString(result.nodes[3].state), QString("waiting"));
    }
    void preflight_distinguishes_degraded_from_active_monitoring() {
        ExperimentPreflightInput input;
        input.hasProject = true;
        input.hasDaqTest = true;
        input.connected = true;
        input.acquiring = true;
        input.selectedChannels = 4;
        input.companionEnabled = true;
        input.monitoredChannels = 2;
        auto degraded = evaluateExperimentPreflight(input);
        QVERIFY(degraded.readyToAcquire);
        QVERIFY(!degraded.readyToMonitor);
        QCOMPARE(QString::fromStdString(degraded.overall), QString("degraded"));

        input.saving = true;
        input.validLimitChannels = 2;
        input.activeMonitoredChannels = 2;
        input.dataSources = 1;
        const auto monitoring = evaluateExperimentPreflight(input);
        QVERIFY(monitoring.readyToMonitor);
        QVERIFY(monitoring.warnings.empty());
        QCOMPARE(QString::fromStdString(monitoring.overall), QString("monitoring"));
        QCOMPARE(QString::fromStdString(monitoring.nodes[6].state), QString("active"));
    }
    void preflight_surfaces_non_blocking_observability_warnings() {
        ExperimentPreflightInput input;
        input.hasProject = true;
        input.hasDaqTest = true;
        input.connected = true;
        input.acquiring = true;
        input.selectedChannels = 3;
        input.companionEnabled = true;
        input.monitoredChannels = 2;
        input.activeMonitoredChannels = 2;
        input.dataSources = 1;
        const auto result = evaluateExperimentPreflight(input);
        QVERIFY(result.readyToAcquire);
        QVERIFY(result.readyToMonitor);
        QCOMPARE(QString::fromStdString(result.overall), QString("monitoring_with_warnings"));
        QCOMPARE(result.warnings.size(), size_t(2));
        QCOMPARE(QString::fromStdString(result.warnings[0]), QString("no_valid_limits"));
        QCOMPARE(QString::fromStdString(result.warnings[1]), QString("storage_not_running"));

        input.saving = true;
        input.validLimitChannels = 1;
        const auto partial = evaluateExperimentPreflight(input);
        QCOMPARE(partial.warnings.size(), size_t(1));
        QCOMPARE(QString::fromStdString(partial.warnings[0]), QString("partial_limit_coverage"));
        QCOMPARE(QString::fromStdString(partial.nodes[2].state), QString("warning"));
    }
    void monitoring_health_is_deterministic_and_hysteretic() {
        MonitoringHealthInput input;
        auto off = evaluateMonitoringHealth(input);
        QCOMPARE(QString::fromStdString(off.state), QString("off"));
        QCOMPARE(QString::fromStdString(off.reasons[0]), QString("monitoring_disabled"));

        input.enabled = true;
        auto waiting = evaluateMonitoringHealth(input);
        QCOMPARE(QString::fromStdString(waiting.state), QString("unavailable"));
        QCOMPARE(QString::fromStdString(waiting.reasons[0]), QString("no_daq_test"));

        input.hasDaqTest = true;
        input.acquiring = true;
        input.connected = true;
        input.monitoredChannels = 2;
        input.activeMonitoredChannels = 2;
        input.dataFresh = true;
        input.dataAgeMs = 100;
        input.staleAfterMs = 1000;
        auto degraded = evaluateMonitoringHealth(input);
        QCOMPARE(QString::fromStdString(degraded.state), QString("degraded"));
        QVERIFY(degraded.ready);
        QCOMPARE(QString::fromStdString(degraded.reasons[0]), QString("storage_not_running"));

        input.saving = true;
        auto healthy = evaluateMonitoringHealth(input);
        QCOMPARE(QString::fromStdString(healthy.state), QString("healthy"));
        QVERIFY(healthy.live);
        QVERIFY(healthy.ready);

        input.invalidChannels = 1;
        auto invalid = evaluateMonitoringHealth(input);
        QCOMPARE(QString::fromStdString(invalid.state), QString("degraded"));
        QCOMPARE(QString::fromStdString(invalid.reasons[0]), QString("data_quality_invalid"));
        input.invalidChannels = 0;

        input.dataAgeMs = 1000;
        auto stale = evaluateMonitoringHealth(input);
        QCOMPARE(QString::fromStdString(stale.state), QString("stale"));
        QCOMPARE(QString::fromStdString(stale.reasons[0]), QString("data_stale"));

        input.dataAgeMs = 100;
        input.activeAlerts = 1;
        auto alarm = evaluateMonitoringHealth(input);
        QCOMPARE(QString::fromStdString(alarm.state), QString("alarm"));
        QCOMPARE(QString::fromStdString(alarm.severity), QString("critical"));
    }
};
int runTstAdvisory(int argc,char**argv){ TstAdvisory t; return QTest::qExec(&t,argc,argv); }
#include "tst_advisory.moc"
