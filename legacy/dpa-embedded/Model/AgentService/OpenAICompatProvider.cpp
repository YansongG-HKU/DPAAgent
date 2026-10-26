#include "OpenAICompatProvider.h"
#include "AgentConfig.h"
#include "LlmRouting.h"
#include "OpenAIWire.h"
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QUrl>
#include <QByteArray>
#include <QCryptographicHash>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QString>
#include <QMetaObject>
#include <QMutexLocker>

namespace Model {

void OpenAICompatProvider::cancel() {
    _cancelled.store(true);
    QMutexLocker lock(&_replyMutex);
    if (_activeReply) QMetaObject::invokeMethod(_activeReply, "abort", Qt::QueuedConnection);
}

LlmReply OpenAICompatProvider::chat(const std::vector<LlmMessage>& messages, const std::string& toolsJson) {
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
    const std::string resolvedEndpoint = resolveOpenAICompatEndpoint(_cfg.baseUrl);
    const QString resolvedEndpointQ = QString::fromStdString(resolvedEndpoint);
    const bool useResponsesEndpoint =
        resolvedEndpointQ.endsWith(QLatin1String("/responses"), Qt::CaseInsensitive) ||
        resolvedEndpointQ.endsWith(QLatin1String("/responses/"), Qt::CaseInsensitive) ||
        resolvedEndpointQ.endsWith(QLatin1String("/v1/responses"), Qt::CaseInsensitive) ||
        resolvedEndpointQ.endsWith(QLatin1String("/v1/responses/"), Qt::CaseInsensitive);
    const std::string body = useResponsesEndpoint
                                  ? buildOpenAIResponsesRequestBody(messages, toolsJson, _cfg)
                                  : buildOpenAIRequestBody(messages, toolsJson, _cfg);

    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(resolvedEndpointQ)};
    req.setRawHeader("content-type", "application/json");
    // Many local OpenAI-compatible servers (Ollama, vLLM) accept no key; only
    // send Authorization when one is configured.
    if (!_cfg.apiKey.empty())
        req.setRawHeader("Authorization", QByteArray("Bearer ") + QByteArray::fromStdString(_cfg.apiKey));
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
    const QByteArray contentType = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError) {
        // Read body anyway: OpenAI-compatible servers return a JSON error object on
        // HTTP 4xx/5xx, which carries a more useful message than the transport errorString.
        QByteArray data = reply->readAll();
        if (!data.isEmpty()) {
            result = parseOpenAIReply(data.toStdString());
            if (result.kind == LlmReply::Error && result.error != "parse_error") {
                annotateProviderFailure(result, reply);
                reply->deleteLater();
                return result;
            }
        }
        qWarning().noquote()
            << "OpenAI-compatible transport error"
            << "network_error=" << reply->errorString()
            << "http_status=" << httpStatus
            << "content_type=" << QString::fromLatin1(contentType)
            << "bytes=" << data.size()
            << "sha256=" << QString::fromLatin1(QCryptographicHash::hash(
                   data, QCryptographicHash::Sha256).toHex());
        result.kind = LlmReply::Error;
        result.error = reply->errorString().toStdString();
        annotateProviderFailure(result, reply);
        reply->deleteLater();
        return result;
    }

    QByteArray data = reply->readAll();
    reply->deleteLater();
    result = parseOpenAIReply(data.toStdString());
    if (result.kind == LlmReply::Error && result.error == "parse_error") {
        result.errorCode = "parse_error";
        QJsonParseError jsonError;
        const QJsonDocument document = QJsonDocument::fromJson(data, &jsonError);
        QStringList topLevelKeys;
        if (document.isObject()) topLevelKeys = document.object().keys();
        qWarning().noquote()
            << "OpenAI-compatible response parse_error"
            << "http_status=" << httpStatus
            << "content_type=" << QString::fromLatin1(contentType)
            << "bytes=" << data.size()
            << "sha256=" << QString::fromLatin1(QCryptographicHash::hash(
                   data, QCryptographicHash::Sha256).toHex())
            << "json_error=" << jsonError.errorString()
            << "top_level_keys=" << topLevelKeys.join(',');
    }
    return result;
}

}
