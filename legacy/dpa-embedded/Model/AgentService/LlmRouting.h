#pragma once
#include "iLLMProvider.h"
#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace Model {

inline void annotateProviderFailure(LlmReply& result, QNetworkReply* reply) {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.httpStatus = status > 0 ? status : -1;
    result.retryable = status == 408 || status == 425 || status == 429 || status >= 500;

    if (status > 0) {
        result.errorCode = "http_" + std::to_string(status);
    } else {
        switch (reply->error()) {
        case QNetworkReply::TimeoutError:
        case QNetworkReply::OperationCanceledError:
            result.errorCode = "network_timeout";
            result.retryable = true;
            break;
        case QNetworkReply::ConnectionRefusedError:
        case QNetworkReply::RemoteHostClosedError:
        case QNetworkReply::HostNotFoundError:
        case QNetworkReply::TemporaryNetworkFailureError:
        case QNetworkReply::NetworkSessionFailedError:
            result.errorCode = "network_unavailable";
            result.retryable = true;
            break;
        default:
            result.errorCode = "network_error";
            break;
        }
    }

    bool secondsOk = false;
    const long long seconds = reply->rawHeader("Retry-After").trimmed().toLongLong(&secondsOk);
    if (secondsOk && seconds >= 0 && seconds <= 86400)
        result.retryAfterMs = seconds * 1000;
}

struct RoutedLlmRoute {
    std::string id;
    std::string providerName;
    std::string model;
    bool pricingConfigured = false;
    long long inputCostMicrosPerMillionTokens = 0;
    long long outputCostMicrosPerMillionTokens = 0;
    std::unique_ptr<iLLMProvider> provider;
};

class RoutedLlmProvider final : public iLLMProvider {
public:
    using Clock = std::function<long long()>;

    RoutedLlmProvider(std::vector<RoutedLlmRoute> routes, int failureThreshold,
                      long long cooldownMs, Clock clock = {})
        : _failureThreshold(failureThreshold > 0 ? failureThreshold : 1),
          _cooldownMs(cooldownMs > 0 ? cooldownMs : 1),
          _clock(clock ? std::move(clock) : Clock([] {
              return std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch()).count();
          })) {
        for (auto& route : routes) _routes.push_back({std::move(route), 0, 0});
    }

    LlmReply chat(const std::vector<LlmMessage>& messages,
                  const std::string& toolsJson) override {
        QJsonArray attempts;
        LlmReply last;
        last.kind = LlmReply::Error;
        last.error = "all_routes_unavailable";
        last.errorCode = "all_routes_unavailable";
        last.retryable = true;

        int called = 0;
        long long totalInput = 0;
        long long totalOutput = 0;
        long long totalCost = 0;
        bool inputKnown = true;
        bool outputKnown = true;
        bool costKnown = true;

        const int attemptLimit = _callAttemptLimit.exchange(INT_MAX);
        for (size_t index = 0; index < _routes.size(); ++index) {
            RouteState& state = _routes[index];
            QJsonObject attempt{{"route_id", QString::fromStdString(state.route.id)}};
            if (!state.route.provider) {
                attempt["status"] = "disabled";
                attempts.append(attempt);
                continue;
            }

            const long long now = _clock();
            if (state.openUntilMs > now) {
                attempt["status"] = "cooldown";
                attempt["retry_after_ms"] = double(state.openUntilMs - now);
                attempts.append(attempt);
                continue;
            }
            if (_cancelled.load()) {
                last.error = "cancelled";
                last.errorCode = "cancelled";
                last.retryable = false;
                break;
            }
            if (called >= attemptLimit) break;

            const bool halfOpenProbe = state.openUntilMs > 0;
            _active.store(static_cast<int>(index));
            LlmReply reply = state.route.provider->chat(messages, toolsJson);
            _active.store(-1);
            ++called;

            reply.routeId = state.route.id;
            reply.providerName = state.route.providerName;
            reply.model = state.route.model;
            estimateCost(reply, state.route);

            if (reply.inputTokens < 0) inputKnown = false;
            else if (totalInput > INT_MAX - reply.inputTokens) inputKnown = false;
            else totalInput += reply.inputTokens;
            if (reply.outputTokens < 0) outputKnown = false;
            else if (totalOutput > INT_MAX - reply.outputTokens) outputKnown = false;
            else totalOutput += reply.outputTokens;
            if (!reply.estimatedCostKnown
                || totalCost > LLONG_MAX - reply.estimatedCostMicros)
                costKnown = false;
            else
                totalCost += reply.estimatedCostMicros;

            attempt["status"] = reply.kind == LlmReply::Error ? "failed" : "selected";
            if (halfOpenProbe) attempt["half_open_probe"] = true;
            if (!reply.errorCode.empty())
                attempt["error_code"] = QString::fromStdString(reply.errorCode);
            if (reply.httpStatus >= 0) attempt["http_status"] = reply.httpStatus;
            if (reply.kind == LlmReply::Error) attempt["retryable"] = reply.retryable;
            if (reply.retryAfterMs >= 0) attempt["retry_after_ms"] = double(reply.retryAfterMs);
            if (reply.inputTokens >= 0) attempt["input_tokens"] = reply.inputTokens;
            if (reply.outputTokens >= 0) attempt["output_tokens"] = reply.outputTokens;
            if (reply.estimatedCostKnown)
                attempt["estimated_cost_micros"] = double(reply.estimatedCostMicros);
            attempts.append(attempt);

            last = reply;
            if (reply.kind != LlmReply::Error) {
                state.consecutiveFailures = 0;
                state.openUntilMs = 0;
                return finish(std::move(reply), attempts, called, inputKnown, totalInput,
                              outputKnown, totalOutput, costKnown, totalCost);
            }
            if (reply.errorCode == "cancelled" || !reply.retryable)
                return finish(std::move(reply), attempts, called, inputKnown, totalInput,
                              outputKnown, totalOutput, costKnown, totalCost);

            ++state.consecutiveFailures;
            if (state.consecutiveFailures >= _failureThreshold) {
                const long long providerDelay = reply.retryAfterMs > 0 ? reply.retryAfterMs : 0;
                state.openUntilMs = _clock() + std::max(_cooldownMs, providerDelay);
            }
        }

        _active.store(-1);
        return finish(std::move(last), attempts, called, inputKnown, totalInput,
                      outputKnown, totalOutput, costKnown, totalCost);
    }

    void cancel() override {
        _cancelled.store(true);
        const int active = _active.load();
        if (active >= 0 && active < static_cast<int>(_routes.size())
            && _routes[active].route.provider)
            _routes[active].route.provider->cancel();
    }

    void resetCancellation() override {
        _cancelled.store(false);
        _active.store(-1);
        for (auto& state : _routes)
            if (state.route.provider) state.route.provider->resetCancellation();
    }

    void setCallAttemptLimit(int limit) override {
        _callAttemptLimit.store(limit > 0 ? limit : 1);
    }

private:
    struct RouteState {
        RoutedLlmRoute route;
        int consecutiveFailures;
        long long openUntilMs;
    };

    static void estimateCost(LlmReply& reply, const RoutedLlmRoute& route) {
        if (!route.pricingConfigured) return;
        if (route.inputCostMicrosPerMillionTokens == 0
            && route.outputCostMicrosPerMillionTokens == 0) {
            reply.estimatedCostKnown = true;
            return;
        }
        if ((route.inputCostMicrosPerMillionTokens > 0 && reply.inputTokens < 0)
            || (route.outputCostMicrosPerMillionTokens > 0 && reply.outputTokens < 0))
            return;

        long long total = 0;
        auto add = [&](int tokens, long long rate) {
            if (rate == 0) return true;
            if (tokens < 0 || tokens > (LLONG_MAX - 999999) / rate) return false;
            const long long micros = (static_cast<long long>(tokens) * rate + 999999) / 1000000;
            if (total > LLONG_MAX - micros) return false;
            total += micros;
            return true;
        };
        if (!add(reply.inputTokens, route.inputCostMicrosPerMillionTokens)
            || !add(reply.outputTokens, route.outputCostMicrosPerMillionTokens))
            return;
        reply.estimatedCostKnown = true;
        reply.estimatedCostMicros = total;
    }

    static LlmReply finish(LlmReply reply, const QJsonArray& attempts, int called,
                           bool inputKnown, long long totalInput,
                           bool outputKnown, long long totalOutput,
                           bool costKnown, long long totalCost) {
        reply.attemptCount = called;
        reply.inputTokens = inputKnown ? static_cast<int>(totalInput) : -1;
        reply.outputTokens = outputKnown ? static_cast<int>(totalOutput) : -1;
        reply.estimatedCostKnown = costKnown;
        reply.estimatedCostMicros = costKnown ? totalCost : 0;
        reply.routingJson = QJsonDocument(QJsonObject{
            {"mode", "priority_fallback"}, {"attempts", attempts}
        }).toJson(QJsonDocument::Compact).toStdString();
        return reply;
    }

    std::vector<RouteState> _routes;
    int _failureThreshold;
    long long _cooldownMs;
    Clock _clock;
    std::atomic_bool _cancelled{false};
    std::atomic_int _active{-1};
    std::atomic_int _callAttemptLimit{INT_MAX};
};

} // namespace Model
