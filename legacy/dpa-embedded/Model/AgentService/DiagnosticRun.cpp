#include "DiagnosticRun.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>

namespace Model {
namespace {

bool isWorkflowStep(DiagnosticRunState state) {
    return state >= DiagnosticRunState::ContextReady && state <= DiagnosticRunState::Complete;
}

bool isActiveStep(DiagnosticRunState state) {
    return state >= DiagnosticRunState::ContextReady && state <= DiagnosticRunState::ReportPending;
}

bool parseState(const QString& text, DiagnosticRunState& state) {
    if (text == "context_ready") state = DiagnosticRunState::ContextReady;
    else if (text == "analysis_pending") state = DiagnosticRunState::AnalysisPending;
    else if (text == "analysis_complete") state = DiagnosticRunState::AnalysisComplete;
    else if (text == "diagnosis_complete") state = DiagnosticRunState::DiagnosisComplete;
    else if (text == "report_pending") state = DiagnosticRunState::ReportPending;
    else if (text == "complete") state = DiagnosticRunState::Complete;
    else if (text == "failed") state = DiagnosticRunState::Failed;
    else if (text == "cancelled") state = DiagnosticRunState::Cancelled;
    else return false;
    return true;
}

DiagnosticRunState nextState(DiagnosticRunEvent event) {
    switch (event) {
    case DiagnosticRunEvent::StartAnalysis:     return DiagnosticRunState::AnalysisPending;
    case DiagnosticRunEvent::CompleteAnalysis:  return DiagnosticRunState::AnalysisComplete;
    case DiagnosticRunEvent::CompleteDiagnosis: return DiagnosticRunState::DiagnosisComplete;
    case DiagnosticRunEvent::RequestReport:     return DiagnosticRunState::ReportPending;
    case DiagnosticRunEvent::CompleteReport:    return DiagnosticRunState::Complete;
    default:                                     return DiagnosticRunState::Failed;
    }
}

} // namespace

const char* diagnosticRunStateName(DiagnosticRunState state) {
    switch (state) {
    case DiagnosticRunState::ContextReady:      return "context_ready";
    case DiagnosticRunState::AnalysisPending:   return "analysis_pending";
    case DiagnosticRunState::AnalysisComplete:  return "analysis_complete";
    case DiagnosticRunState::DiagnosisComplete: return "diagnosis_complete";
    case DiagnosticRunState::ReportPending:     return "report_pending";
    case DiagnosticRunState::Complete:          return "complete";
    case DiagnosticRunState::Failed:            return "failed";
    case DiagnosticRunState::Cancelled:         return "cancelled";
    }
    return "";
}

bool DiagnosticRun::isTerminal() const {
    return _state == DiagnosticRunState::Complete || _state == DiagnosticRunState::Cancelled;
}

bool DiagnosticRun::canApply(DiagnosticRunEvent event) const {
    if (_state == DiagnosticRunState::Failed)
        return event == DiagnosticRunEvent::Retry || event == DiagnosticRunEvent::Cancel;
    if (isTerminal()) return false;
    if (event == DiagnosticRunEvent::Fail || event == DiagnosticRunEvent::Cancel) return true;

    switch (_state) {
    case DiagnosticRunState::ContextReady:
        return event == DiagnosticRunEvent::StartAnalysis;
    case DiagnosticRunState::AnalysisPending:
        return event == DiagnosticRunEvent::CompleteAnalysis;
    case DiagnosticRunState::AnalysisComplete:
        return event == DiagnosticRunEvent::CompleteDiagnosis;
    case DiagnosticRunState::DiagnosisComplete:
        return event == DiagnosticRunEvent::RequestReport;
    case DiagnosticRunState::ReportPending:
        return event == DiagnosticRunEvent::CompleteReport;
    default:
        return false;
    }
}

bool DiagnosticRun::apply(DiagnosticRunEvent event, long long nowMs, const std::string& error) {
    if (nowMs < 0 || nowMs < _updatedAt || !canApply(event)) return false;
    if (event == DiagnosticRunEvent::Fail && error.empty()) return false;

    if (event == DiagnosticRunEvent::Fail) {
        _state = DiagnosticRunState::Failed;
        _error = error;
    } else if (event == DiagnosticRunEvent::Retry) {
        _state = _currentStep;
        _error.clear();
    } else if (event == DiagnosticRunEvent::Cancel) {
        _state = DiagnosticRunState::Cancelled;
    } else {
        _state = nextState(event);
        _currentStep = _state;
        _error.clear();
    }
    _updatedAt = nowMs;
    return true;
}

bool DiagnosticRun::interruptPendingAfterRestart(long long nowMs) {
    if (_state != DiagnosticRunState::AnalysisPending
        && _state != DiagnosticRunState::ReportPending) return false;
    if (nowMs < _updatedAt) nowMs = _updatedAt;
    return apply(DiagnosticRunEvent::Fail, nowMs, "interrupted_by_restart");
}

std::string DiagnosticRun::toJson() const {
    QJsonObject root;
    root["state"] = QString::fromLatin1(diagnosticRunStateName(_state));
    root["current_step"] = QString::fromLatin1(diagnosticRunStateName(_currentStep));
    root["error"] = QString::fromStdString(_error);
    root["updated_at"] = static_cast<double>(_updatedAt);
    return QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
}

bool DiagnosticRun::fromJson(const std::string& json, DiagnosticRun& out) {
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(json), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) return false;

    const QJsonObject root = doc.object();
    if (!root.value("state").isString() || !root.value("current_step").isString()
        || !root.value("error").isString() || !root.value("updated_at").isDouble()) return false;

    DiagnosticRunState state;
    DiagnosticRunState step;
    if (!parseState(root.value("state").toString(), state)
        || !parseState(root.value("current_step").toString(), step)
        || !isWorkflowStep(step)) return false;

    const double updatedAt = root.value("updated_at").toDouble();
    constexpr double maxExactJsonInteger = 9007199254740991.0;
    if (!std::isfinite(updatedAt) || updatedAt < 0 || updatedAt > maxExactJsonInteger
        || std::floor(updatedAt) != updatedAt) return false;

    const std::string error = root.value("error").toString().toStdString();
    if (state == DiagnosticRunState::Failed) {
        if (!isActiveStep(step) || error.empty()) return false;
    } else if (state == DiagnosticRunState::Cancelled) {
        if (!isActiveStep(step)) return false;
    } else {
        if (state != step || !error.empty()) return false;
    }

    DiagnosticRun parsed(static_cast<long long>(updatedAt));
    parsed._state = state;
    parsed._currentStep = step;
    parsed._error = error;
    out = parsed;
    return true;
}

} // namespace Model
