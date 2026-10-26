#include "ClaudeProvider.h"
#include "AgentConfig.h"
#include "LlmRouting.h"
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QUrl>
#include <QByteArray>
#include <QString>
#include <QMetaObject>
#include <QMutexLocker>

namespace Model {

void ClaudeProvider::cancel() {
    _cancelled.store(true);
    QMutexLocker lock(&_replyMutex);
    if (_activeReply) QMetaObject::invokeMethod(_activeReply, "abort", Qt::QueuedConnection);
}

LlmReply ClaudeProvider::chat(const std::vector<LlmMessage>& messages, const std::string& toolsJson) {
    if (_cancelled.load()) {
        LlmReply result{LlmReply::Error, "", "", "", "", "cancelled"};
        result.errorCode = "cancelled";
        return result;
    }
    if (const std::string transportError = validateCredentialedEndpoint(_cfg);
        !transportError.empty()) {
        LlmReply result{LlmReply::Error, "", "", "", "", transportError};
        result.errorCode = transportError;
        return result;
    }
    if (_cfg.apiKey.empty()) {
        LlmReply r;
        r.kind = LlmReply::Error;
        r.error = "missing_api_key";
        r.errorCode = "missing_api_key";
        return r;
    }

    std::string body = buildClaudeRequestBody(messages, toolsJson, _cfg);

    QNetworkAccessManager nam;
    QNetworkRequest req(QUrl(QString::fromStdString(_cfg.baseUrl)));
    req.setRawHeader("x-api-key", QByteArray::fromStdString(_cfg.apiKey));
    req.setRawHeader("anthropic-version", "2023-06-01");
    req.setRawHeader("content-type", "application/json");
    if (_cfg.timeoutMs > 0) req.setTransferTimeout(_cfg.timeoutMs);

    // Synchronous POST: drive a local event loop until the reply finishes.
    QEventLoop loop;
    QNetworkReply* reply = nam.post(req, QByteArray::fromStdString(body));
    {
        QMutexLocker lock(&_replyMutex);
        _activeReply = reply;
        if (_cancelled.load()) QMetaObject::invokeMethod(reply, "abort", Qt::QueuedConnection);
    }
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    {
        QMutexLocker lock(&_replyMutex);
        if (_activeReply == reply) _activeReply = nullptr;
    }

    LlmReply result;
    if (_cancelled.load()) {
        result.kind = LlmReply::Error;
        result.error = "cancelled";
        result.errorCode = "cancelled";
        reply->deleteLater();
        return result;
    }
    if (reply->error() != QNetworkReply::NoError) {
        // Read body anyway: Anthropic returns a JSON error object on HTTP 4xx/5xx,
        // which carries a more useful message than the transport errorString.
        QByteArray data = reply->readAll();
        if (!data.isEmpty()) {
            result = parseClaudeReply(data.toStdString());
            if (result.kind == LlmReply::Error) {
                annotateProviderFailure(result, reply);
                reply->deleteLater();
                return result;
            }
        }
        result.kind = LlmReply::Error;
        result.error = reply->errorString().toStdString();
        annotateProviderFailure(result, reply);
        reply->deleteLater();
        return result;
    }

    QByteArray data = reply->readAll();
    reply->deleteLater();
    return parseClaudeReply(data.toStdString());
}

}
