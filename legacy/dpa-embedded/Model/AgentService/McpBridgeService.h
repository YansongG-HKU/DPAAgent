#pragma once
#include "iAgentService.h"
#include "RateLimiter.h"
#include <QByteArray>
#include <QJsonObject>
#include <QString>

class QTcpServer;
class QTcpSocket;

namespace Model {
    class McpBridgeService {
    public:
        explicit McpBridgeService(iAgentService* agent, int maxRequests = 20,
                                  long long rateWindowMs = 1000);
        ~McpBridgeService();
        bool start();
        QString manifestPath() const;
        static QJsonArray readAuditLog(int count = 20);
    private:
        QString manifestDir() const;
        QString makeToken() const;
        bool writeManifest() const;
        void onReadyRead(QTcpSocket* socket);
        void handleRequest(QTcpSocket* socket, const QByteArray& header, const QByteArray& body);
        void writeJson(QTcpSocket* socket, int status, const QJsonObject& obj) const;
        void logAudit(const QString& tool, const QByteArray& argsJson, int statusCode, qint64 elapsedMs, QTcpSocket* socket);
        void rotateAuditIfNeeded();
        QString auditPath() const;

        iAgentService* _agent = nullptr;
        QTcpServer* _server = nullptr;
        QString _readToken;   // Competition bridge is intentionally read-only.
        RateLimiter _limiter;   // Production defaults: <=20 authenticated requests/sec.
    };
}
