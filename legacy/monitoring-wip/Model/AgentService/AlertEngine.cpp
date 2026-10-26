#include "AlertEngine.h"
#include <cmath>
#include <algorithm>
#include <chrono>

AlertEngine::AlertEngine(const Config& cfg) : _cfg(cfg) {}

void AlertEngine::setConfig(const Config& cfg) { _cfg = cfg; }

std::vector<AlertEngine::AlarmDecision> AlertEngine::evaluate(
    const std::string& channelId,
    double value,
    double minValue,
    double maxValue,
    double upperLimit,
    double lowerLimit,
    double kurtosis,
    double slopePerMin,
    int64_t timestampMs,
    bool isStale,
    bool isInvalid) {

    std::vector<AlarmDecision> decisions;
    const int64_t nowMs = timestampMs;

    auto addDecision = [&](ConditionType type, Severity severity,
                           bool conditionMet, const std::string& desc,
                           double limitVal = 0.0) {
        decisions.push_back(
            evaluateSingle(channelId, type, severity, value, limitVal,
                           nowMs, conditionMet, desc));
    };

    if (isInvalid) {
        addDecision(ConditionType::Invalid, Severity::Warning, true,
                    "data_invalid: non-finite value in window");
    } else {
        auto& invState = state(channelId, ConditionType::Invalid);
        if (invState.active) {
            addDecision(ConditionType::Invalid, Severity::Warning, false,
                        "data_invalid recovered");
        }
    }

    if (isStale) {
        addDecision(ConditionType::Stale, Severity::Warning, true,
                    "data_stale: no new data window");
    } else {
        auto& staleState = state(channelId, ConditionType::Stale);
        if (staleState.active) {
            addDecision(ConditionType::Stale, Severity::Warning, false,
                        "data_stale recovered");
        }
    }

    if (std::isfinite(upperLimit)) {
        const double band = _cfg.deadBand.apply(upperLimit);
        const double hardThreshold = upperLimit;
        const double recoveryThreshold = upperLimit - band;
        const bool met = std::isfinite(maxValue) && maxValue > hardThreshold;
        auto& s = state(channelId, ConditionType::High);
        bool recovered = s.active && std::isfinite(value)
                         && value < recoveryThreshold;
        if (met || recovered) {
            addDecision(ConditionType::High, Severity::Critical, met,
                        met ? "limit_high: value exceeded upper limit"
                            : "limit_high recovered (dead-band)",
                        upperLimit);
        }
    }

    if (std::isfinite(lowerLimit)) {
        const double band = _cfg.deadBand.apply(lowerLimit);
        const double hardThreshold = lowerLimit;
        const double recoveryThreshold = lowerLimit + band;
        const bool met = std::isfinite(minValue) && minValue < hardThreshold;
        auto& s = state(channelId, ConditionType::Low);
        bool recovered = s.active && std::isfinite(value)
                         && value > recoveryThreshold;
        if (met || recovered) {
            addDecision(ConditionType::Low, Severity::Critical, met,
                        met ? "limit_low: value below lower limit"
                            : "limit_low recovered (dead-band)",
                        lowerLimit);
        }
    }

    if (_cfg.trendAlarmEnabled && std::isfinite(slopePerMin)
        && std::isfinite(_cfg.rateOfChangeWarningPerMin)) {
        const double absSlope = std::abs(slopePerMin);
        if (absSlope >= _cfg.rateOfChangeWarningPerMin) {
            const bool crit = std::isfinite(_cfg.rateOfChangeCriticalPerMin)
                              && absSlope >= _cfg.rateOfChangeCriticalPerMin;
            addDecision(ConditionType::RateOfChange,
                        crit ? Severity::Critical : Severity::Warning, true,
                        "rate_of_change: " + std::to_string(slopePerMin)
                        + " units/min");
        }
    }

    if (_cfg.kurtosisAlarmEnabled && std::isfinite(kurtosis)) {
        if (std::isfinite(_cfg.kurtosisCriticalThreshold)
            && kurtosis >= _cfg.kurtosisCriticalThreshold) {
            addDecision(ConditionType::KurtosisHigh, Severity::Critical, true,
                        "kurtosis_high: " + std::to_string(kurtosis)
                        + " >= critical threshold");
        } else if (std::isfinite(_cfg.kurtosisWarningThreshold)
                   && kurtosis >= _cfg.kurtosisWarningThreshold) {
            addDecision(ConditionType::KurtosisHigh, Severity::Warning, true,
                        "kurtosis_high: " + std::to_string(kurtosis)
                        + " >= warning threshold");
        }
    }

    std::vector<AlarmDecision> result;
    for (auto& d : decisions) {
        if (!d.hasDecision) continue;
        AlarmState& s = state(d.channelId, d.conditionType);
        if (d.isActive) {
            if (s.consecutiveCount >= _cfg.sustainedSamples
                && !isSuppressed(s, nowMs)) {
                result.push_back(d);
                s.lastFiredMs = nowMs;
            }
        } else if (s.active) {
            s.active = false;
            s.consecutiveCount = 0;
            d.isRecovered = true;
            result.push_back(d);
        }
    }
    return result;
}

AlertEngine::AlarmDecision AlertEngine::evaluateSingle(
    const std::string& channelId,
    ConditionType type,
    Severity severity,
    double value,
    double limit,
    int64_t timestampMs,
    bool conditionMet,
    const std::string& desc) {

    AlarmState& s = state(channelId, type);
    AlarmDecision d;
    d.hasDecision = true;
    d.channelId = channelId;
    d.conditionType = type;
    d.severity = severity;
    d.alarmId = s.alarmId;
    d.isAcknowledged = s.acknowledged;

    if (conditionMet) {
        s.consecutiveCount++;
        if (!s.active) {
            s.active = true;
            s.activatedAtMs = timestampMs;
            s.escalationCount = 0;
            if (s.alarmId.empty()) {
                s.alarmId = channelId + "_" + std::to_string(static_cast<int>(type))
                            + "_" + std::to_string(timestampMs);
            }
        }
        if (!s.acknowledged && _cfg.escalationMs > 0
            && (timestampMs - s.activatedAtMs) >= _cfg.escalationMs) {
            severity = Severity::Critical;
            s.escalationCount++;
        }
        d.isActive = true;
        d.severity = severity;
        d.consecutiveCount = s.consecutiveCount;
        d.activeDurationMs = timestampMs - s.activatedAtMs;
        d.description = desc;
    } else {
        d.isActive = false;
        d.isRecovered = s.active;
        d.consecutiveCount = 0;
        d.activeDurationMs = 0;
        d.description = desc;
    }
    return d;
}

AlertEngine::AlarmState& AlertEngine::state(
    const std::string& channelId, ConditionType type) {
    return _states[channelId][static_cast<int>(type)];
}

bool AlertEngine::isSuppressed(const AlarmState& s, int64_t nowMs) const {
    if (s.lastFiredMs == 0) return false;
    return (nowMs - s.lastFiredMs) < _cfg.cooldownMs;
}

AlertEngine::AlarmState AlertEngine::acknowledge(
    const std::string& channelId,
    ConditionType conditionType,
    const std::string& note) {
    AlarmState& s = state(channelId, conditionType);
    s.acknowledged = true;
    s.acknowledgedAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    s.note = note;
    return s;
}

void AlertEngine::resetChannel(const std::string& channelId) {
    _states.erase(channelId);
}

void AlertEngine::resetAll() { _states.clear(); }

AlertEngine::Stats AlertEngine::stats() const {
    Stats st;
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    for (const auto& [ch, conds] : _states) {
        for (const auto& [type, s] : conds) {
            if (s.active || s.consecutiveCount > 0) {
                st.totalAlarms++;
                if (s.active) st.activeAlarms++;
                if (s.acknowledged) st.acknowledgedAlarms++;
                if (!s.active && s.acknowledged) st.recoveredAlarms++;
                if (s.active && st.oldestActiveMs == 0) {
                    st.oldestActiveMs = s.activatedAtMs;
                }
            }
        }
    }
    return st;
}
