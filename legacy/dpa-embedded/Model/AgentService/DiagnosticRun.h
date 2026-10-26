#pragma once
#include <string>

namespace Model {

enum class DiagnosticRunState {
    ContextReady,
    AnalysisPending,
    AnalysisComplete,
    DiagnosisComplete,
    ReportPending,
    Complete,
    Failed,
    Cancelled
};

enum class DiagnosticRunEvent {
    StartAnalysis,
    CompleteAnalysis,
    CompleteDiagnosis,
    RequestReport,
    CompleteReport,
    Fail,
    Retry,
    Cancel
};

const char* diagnosticRunStateName(DiagnosticRunState state);

class DiagnosticRun {
public:
    explicit DiagnosticRun(long long updatedAt = 0) : _updatedAt(updatedAt) {}

    DiagnosticRunState state() const { return _state; }
    DiagnosticRunState currentStep() const { return _currentStep; }
    const std::string& error() const { return _error; }
    long long updatedAt() const { return _updatedAt; }
    bool isTerminal() const;
    bool canApply(DiagnosticRunEvent event) const;

    // Invalid events, empty failure errors, and time going backwards are rejected
    // without mutating the run.
    bool apply(DiagnosticRunEvent event, long long nowMs, const std::string& error = {});

    // A process/report cannot survive application restart. Convert persisted
    // pending work into a retryable failure while preserving currentStep().
    bool interruptPendingAfterRestart(long long nowMs);

    std::string toJson() const;
    static bool fromJson(const std::string& json, DiagnosticRun& out);

private:
    DiagnosticRunState _state = DiagnosticRunState::ContextReady;
    DiagnosticRunState _currentStep = DiagnosticRunState::ContextReady;
    std::string _error;
    long long _updatedAt = 0;
};

} // namespace Model
