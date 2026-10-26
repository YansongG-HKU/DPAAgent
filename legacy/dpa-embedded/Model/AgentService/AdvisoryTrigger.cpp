#include "AdvisoryTrigger.h"
#include <algorithm>
#include <cmath>
using namespace Model;

AdvisoryDecision Model::evaluateAdvisory(const ResultSummary& s, const AdvisoryConfig& cfg,
                                         long long nowMs, AdvisoryState& st) {
    AdvisoryDecision d;
    if (!cfg.enabled || !s.found) return d;

    // Find the first over-limit metric: peak first, then configured stats.
    std::string metric; double value = 0, limit = 0;
    if (cfg.peakValueMax > 0) {
        auto it = s.peaks.find("peak_value");
        if (it != s.peaks.end() && it->second > cfg.peakValueMax) {
            metric = "peak_value"; value = it->second; limit = cfg.peakValueMax;
        }
    }
    if (metric.empty()) {
        for (const auto& kv : cfg.maxByMetric) {
            auto it = s.stats.find(kv.first);
            if (it != s.stats.end() && it->second > kv.second) {
                metric = kv.first; value = it->second; limit = kv.second; break;
            }
        }
    }
    if (metric.empty()) return d;   // nothing over limit

    // Throttle: suppress a repeat on the same metric within the min interval.
    if (st.lastKey == metric && (nowMs - st.lastFiredMs) < cfg.minIntervalMs) return d;

    st.lastFiredMs = nowMs; st.lastKey = metric;
    d.fire = true; d.metric = metric; d.value = value; d.limit = limit;
    d.message = "分析结果「" + s.name + "」的 " + metric + " = " + std::to_string(value)
              + " 超过阈值 " + std::to_string(limit) + "，需要我解读吗？";
    return d;
}

namespace {
void evaluateCondition(bool violated, bool recovered, const char* kind,
                       const char* severity, double value, double limit,
                       const CompanionChannelSample& sample,
                       const CompanionConfig& cfg, long long nowMs,
                       CompanionConditionState& state,
                       std::vector<CompanionDecision>& out) {
    if (violated) {
        state.consecutive++;
        const bool first = !state.active && state.consecutive >= cfg.sustainedSamples;
        const bool escalate = state.active && !state.acknowledged && !state.escalated
            && std::string(severity) == "warning" && cfg.escalationMs > 0
            && nowMs - state.activatedMs >= cfg.escalationMs;
        const bool repeat = state.active && !state.acknowledged && !escalate
            && nowMs - state.lastFiredMs >= cfg.cooldownMs;
        if (first) {
            state.active = true;
            state.activatedMs = nowMs;
            state.lastFiredMs = nowMs;
            state.occurrenceCount = 1;
            state.alarmId = std::string(kind) + ":" + std::to_string(sample.channelId)
                + ":" + std::to_string(nowMs);
        } else if (escalate) {
            state.escalated = true;
            state.lastFiredMs = nowMs;
            state.occurrenceCount++;
        } else if (repeat) {
            state.lastFiredMs = nowMs;
            state.occurrenceCount++;
        }
        if (first || escalate || repeat) {
            CompanionDecision decision;
            decision.kind = kind;
            decision.severity = state.escalated ? "critical" : severity;
            decision.channelId = sample.channelId;
            decision.channelName = sample.name;
            decision.unit = sample.unit;
            decision.value = value;
            decision.limit = limit;
            decision.alarmId = state.alarmId;
            decision.lifecycle = first ? "active" : (escalate ? "escalated" : "repeat");
            decision.occurrenceCount = state.occurrenceCount;
            decision.acknowledged = state.acknowledged;
            out.push_back(decision);
        }
        return;
    }

    state.consecutive = 0;
    if (state.active && recovered) {
        CompanionDecision decision;
        decision.kind = std::string(kind) + "_recovered";
        decision.severity = "info";
        decision.channelId = sample.channelId;
        decision.channelName = sample.name;
        decision.unit = sample.unit;
        decision.value = value;
        decision.limit = limit;
        decision.recovery = true;
        decision.alarmId = state.alarmId;
        decision.lifecycle = "recovered";
        decision.occurrenceCount = state.occurrenceCount;
        decision.acknowledged = state.acknowledged;
        out.push_back(decision);
        state = CompanionConditionState{};
    }
}
}

std::vector<CompanionDecision> Model::evaluateCompanionChannels(
    const std::vector<CompanionChannelSample>& samples,
    const CompanionConfig& cfg, long long nowMs, CompanionState& state) {
    std::vector<CompanionDecision> out;
    if (!cfg.enabled || cfg.sustainedSamples < 1 || cfg.cooldownMs < 0
        || cfg.escalationMs < 0
        || cfg.hysteresisRatio < 0) return out;

    for (const auto& sample : samples) {
        if (sample.channelId < 0) continue;
        auto& channel = state.channels[sample.channelId];
        const bool limitsValid = sample.limitEnabled && sample.hasValue
            && std::isfinite(sample.minimum) && std::isfinite(sample.maximum)
            && std::isfinite(sample.lowLimit) && std::isfinite(sample.highLimit)
            && sample.lowLimit < sample.highLimit;
        const double highBand = std::max(1.0, std::abs(sample.highLimit)) * cfg.hysteresisRatio;
        const double lowBand = std::max(1.0, std::abs(sample.lowLimit)) * cfg.hysteresisRatio;
        evaluateCondition(limitsValid && sample.maximum > sample.highLimit,
                          limitsValid && sample.maximum <= sample.highLimit - highBand,
                          "limit_high", "alarm", sample.maximum, sample.highLimit,
                          sample, cfg, nowMs, channel.high, out);
        evaluateCondition(limitsValid && sample.minimum < sample.lowLimit,
                          limitsValid && sample.minimum >= sample.lowLimit + lowBand,
                          "limit_low", "alarm", sample.minimum, sample.lowLimit,
                          sample, cfg, nowMs, channel.low, out);
        evaluateCondition(sample.stale, !sample.stale,
                           "data_stale", "warning", sample.value, 0,
                           sample, cfg, nowMs, channel.stale, out);
        evaluateCondition(sample.qualityInvalid && !sample.stale,
                          !sample.qualityInvalid && sample.hasValue,
                          "data_invalid", "warning", sample.value, 0,
                          sample, cfg, nowMs, channel.invalid, out);
    }
    return out;
}

bool Model::acknowledgeCompanionAlarm(CompanionState& state,
                                      const std::string& alarmId, long long nowMs) {
    if (alarmId.empty()) return false;
    for (auto& channel : state.channels) {
        CompanionConditionState* conditions[] = {
            &channel.second.high, &channel.second.low,
            &channel.second.stale, &channel.second.invalid
        };
        for (CompanionConditionState* condition : conditions) {
            if (!condition->active || condition->alarmId != alarmId) continue;
            condition->acknowledged = true;
            condition->acknowledgedMs = nowMs;
            return true;
        }
    }
    return false;
}

MonitoringHealth Model::evaluateMonitoringHealth(const MonitoringHealthInput& input) {
    MonitoringHealth result;
    if (!input.enabled) {
        result.state = "off";
        result.severity = "info";
        result.reasons.push_back("monitoring_disabled");
        return result;
    }
    if (!input.hasDaqTest) {
        result.state = "unavailable";
        result.severity = "warning";
        result.reasons.push_back("no_daq_test");
        return result;
    }
    if (!input.acquiring) {
        result.state = "waiting";
        result.severity = "info";
        result.reasons.push_back("acquisition_not_running");
        return result;
    }
    result.live = true;
    if (!input.connected) {
        result.state = "unavailable";
        result.severity = "critical";
        result.reasons.push_back("device_disconnected");
        return result;
    }
    if (input.monitoredChannels <= 0) {
        result.state = "degraded";
        result.severity = "warning";
        result.reasons.push_back("no_monitored_channels");
        return result;
    }
    if (input.activeMonitoredChannels <= 0) {
        result.state = "degraded";
        result.severity = "warning";
        result.reasons.push_back("no_live_monitored_channels");
        return result;
    }
    if (input.activeAlerts > 0) {
        result.state = "alarm";
        result.severity = "critical";
        result.reasons.push_back("active_alerts");
        return result;
    }
    if (input.invalidChannels > 0) {
        result.state = "degraded";
        result.severity = "warning";
        result.reasons.push_back("data_quality_invalid");
        return result;
    }
    const long long age = std::max(0LL, input.dataAgeMs);
    const long long staleAfter = std::max(1LL, input.staleAfterMs);
    if (!input.dataFresh || age >= staleAfter) {
        result.state = "stale";
        result.severity = "warning";
        result.reasons.push_back("data_stale");
        return result;
    }
    if (!input.saving) {
        result.state = "degraded";
        result.severity = "warning";
        result.ready = true;
        result.reasons.push_back("storage_not_running");
        return result;
    }
    result.state = "healthy";
    result.severity = "info";
    result.ready = true;
    return result;
}

ExperimentPreflight Model::evaluateExperimentPreflight(
    const ExperimentPreflightInput& input) {
    ExperimentPreflight result;
    result.snapshot = input;

    if (!input.hasProject) result.blockers.push_back("no_project");
    if (!input.hasDaqTest) result.blockers.push_back("no_daq_test");
    if (!input.connected) result.blockers.push_back("device_disconnected");
    if (input.selectedChannels <= 0) result.blockers.push_back("no_acquisition_channels");

    if (!input.companionEnabled) {
        result.warnings.push_back("companion_disabled");
    } else if (input.monitoredChannels <= 0) {
        result.warnings.push_back("no_monitored_channels");
    } else if (input.validLimitChannels <= 0) {
        result.warnings.push_back("no_valid_limits");
    } else if (input.validLimitChannels < input.monitoredChannels) {
        result.warnings.push_back("partial_limit_coverage");
    }
    if (input.acquiring && !input.saving)
        result.warnings.push_back("storage_not_running");
    if (input.acquiring && input.companionEnabled
        && input.activeMonitoredChannels <= 0)
        result.warnings.push_back("no_live_monitored_channels");

    result.readyToAcquire = result.blockers.empty();
    result.readyToMonitor = result.readyToAcquire && input.acquiring
        && input.companionEnabled && input.monitoredChannels > 0
        && input.activeMonitoredChannels > 0;
    if (!result.readyToAcquire) result.overall = "blocked";
    else if (!input.acquiring) result.overall = "ready";
    else if (!result.readyToMonitor) result.overall = "degraded";
    else if (!result.warnings.empty()) result.overall = "monitoring_with_warnings";
    else result.overall = "monitoring";

    const bool contextReady = input.hasProject && input.hasDaqTest;
    std::string channelState = input.selectedChannels > 0 ? "ok" : "blocked";
    if (channelState == "ok" && input.companionEnabled
        && (input.monitoredChannels <= 0
            || input.validLimitChannels < input.monitoredChannels))
        channelState = "warning";
    std::string companionState = "warning";
    if (result.readyToMonitor) companionState = "active";
    else if (input.companionEnabled && !input.acquiring) companionState = "waiting";
    std::string dataState = "waiting";
    if (input.activeMonitoredChannels > 0) dataState = "active";
    else if (input.acquiring && input.companionEnabled) dataState = "warning";

    result.nodes = {
        {"context", contextReady ? "ok" : "blocked"},
        {"device", input.connected ? "ok" : "blocked"},
        {"channels", channelState},
        {"acquisition", input.acquiring ? "active" : "waiting"},
        {"storage", input.saving ? "active" : (input.acquiring ? "warning" : "waiting")},
        {"companion", companionState},
        {"data_flow", dataState}
    };
    return result;
}

CompanionDeviceDecision Model::describeCompanionDeviceEvent(
    const std::string& index, const std::string& deviceName,
    const std::string& detail, bool connected) {
    CompanionDeviceDecision decision;
    const std::string device = deviceName.empty() ? "采集设备" : deviceName;
    decision.kind = "device_" + index;
    decision.severity = "alarm";
    decision.explain = true;
    if (index == "Current_Error") {
        decision.title = device + " 设备异常";
        decision.message = detail;
    } else if (index == "Current_Info") {
        decision.severity = "info";
        decision.title = device + " 状态消息";
        decision.message = detail;
        decision.explain = false;
    } else if (index == "NetworkStateChange") {
        decision.severity = connected ? "info" : "alarm";
        decision.title = device + (connected ? " 连接已恢复" : " 连接已断开");
        decision.message = connected ? "采集设备网络连接已恢复。" : "采集中检测到设备网络连接断开。";
        decision.explain = !connected;
    } else if (index == "DISKFULL") {
        decision.title = device + " 存储空间不足";
        decision.message = "设备报告磁盘已满，存储可能已停止。";
    } else if (index == "DEVICEDAQSTOPED") {
        decision.title = device + " 意外停止采集";
        decision.message = "设备报告采集意外停止。";
    } else if (index == "CLOSEHOST") {
        decision.title = device + " 连接被强制关闭";
        decision.message = "设备连接被强制关闭。";
    } else if (index == "LimitStateChange") {
        decision.severity = "warning";
        decision.title = device + " 超限状态变化";
        decision.message = "设备报告超限状态发生变化，请核对通道限值。";
    } else {
        return {};
    }
    if (decision.message.empty()) decision.message = "设备报告未分类异常，请查看操作日志。";
    decision.valid = true;
    return decision;
}
