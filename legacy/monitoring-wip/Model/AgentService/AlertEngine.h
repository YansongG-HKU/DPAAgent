#pragma once

#include <string>
#include <map>
#include <vector>
#include <cstdint>
#include <functional>
#include <limits>

class AlertEngine {
public:
    enum class Severity { Info, Notice, Warning, Critical, Recovery };
    enum class ConditionType { High, Low, Stale, Invalid, RateOfChange, TrendBreach, KurtosisHigh };

    struct Threshold {
        double upper = std::numeric_limits<double>::quiet_NaN();
        double lower = std::numeric_limits<double>::quiet_NaN();
        double rateOfChangePerMin = std::numeric_limits<double>::quiet_NaN();
        double trendSlopePerMin = std::numeric_limits<double>::quiet_NaN();
        double kurtosisThreshold = std::numeric_limits<double>::quiet_NaN();
    };

    struct DeadBand {
        double ratio = 0.02;
        double absoluteMin = 1.0;
        double apply(double limit) const {
            return std::max(absoluteMin, std::abs(limit) * ratio);
        }
    };

    struct AlarmState {
        bool active = false;
        bool acknowledged = false;
        int64_t activatedAtMs = 0;
        int64_t lastFiredMs = 0;
        int64_t acknowledgedAtMs = 0;
        int consecutiveCount = 0;
        std::string alarmId;
        Severity severity = Severity::Warning;
        ConditionType conditionType = ConditionType::High;
        std::string note;
        int escalationCount = 0;
    };

    struct AlarmDecision {
        bool hasDecision = false;
        std::string channelId;
        ConditionType conditionType;
        Severity severity;
        std::string alarmId;
        bool isActive;
        bool isAcknowledged;
        bool isRecovered;
        int consecutiveCount;
        int64_t activeDurationMs;
        std::string description;
    };

    struct Config {
        DeadBand deadBand;
        int sustainedSamples = 3;
        int64_t cooldownMs = 60000;
        int64_t escalationMs = 1800000;
        bool trendAlarmEnabled = false;
        bool kurtosisAlarmEnabled = false;
        double kurtosisWarningThreshold = 3.0;
        double kurtosisCriticalThreshold = 5.0;
        double rateOfChangeWarningPerMin = std::numeric_limits<double>::quiet_NaN();
        double rateOfChangeCriticalPerMin = std::numeric_limits<double>::quiet_NaN();
    };

    explicit AlertEngine(const Config& cfg = {});

    void setConfig(const Config& cfg);
    const Config& config() const { return _cfg; }

    std::vector<AlarmDecision> evaluate(
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
        bool isInvalid);

    AlarmState acknowledge(const std::string& channelId,
                           ConditionType conditionType,
                           const std::string& note = {});

    void resetChannel(const std::string& channelId);
    void resetAll();

    const std::map<std::string, std::map<int, AlarmState>>& alarmStates() const {
        return _states;
    }

    struct Stats {
        int totalAlarms = 0;
        int activeAlarms = 0;
        int acknowledgedAlarms = 0;
        int recoveredAlarms = 0;
        int64_t oldestActiveMs = 0;
    };
    Stats stats() const;

private:
    using StateKey = std::pair<std::string, int>;

    AlarmDecision evaluateSingle(
        const std::string& channelId,
        ConditionType type,
        Severity severity,
        double value,
        double limit,
        int64_t timestampMs,
        bool conditionMet,
        const std::string& desc);

    AlarmState& state(const std::string& channelId, ConditionType type);
    bool isSuppressed(const AlarmState& s, int64_t nowMs) const;

    Config _cfg;
    std::map<std::string, std::map<int, AlarmState>> _states;
};
