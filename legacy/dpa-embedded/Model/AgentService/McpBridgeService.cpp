#include "McpBridgeService.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <algorithm>

using namespace Model;

McpBridgeService::McpBridgeService(iAgentService* agent, int maxRequests,
                                   long long rateWindowMs)
    : _agent(agent), _limiter(maxRequests, rateWindowMs) {}

McpBridgeService::~McpBridgeService() {
    if (_server) {
        _server->close();
        delete _server;
        _server = nullptr;
        QFile::remove(manifestPath());
    }
}

bool McpBridgeService::start() {
    if (_server) return true;
    _readToken = makeToken();
    _server = new QTcpServer();
    if (!_server->listen(QHostAddress::LocalHost, 0)) {
        delete _server;
        _server = nullptr;
        // Don't leave a stale manifest from a previous run pointing at a dead bridge —
        // the Node server would keep trying the old port/token forever.
        QFile::remove(manifestPath());
        return false;
    }
    QObject::connect(_server, &QTcpServer::newConnection, _server, [this]() {
        while (_server->hasPendingConnections()) {
            QTcpSocket* socket = _server->nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() { onReadyRead(socket); });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
        }
    });
    QFile::remove(manifestPath());
    if (!writeManifest()) {
        _server->close();
        delete _server;
        _server = nullptr;
        return false;
    }
    return true;
}

QString McpBridgeService::manifestDir() const {
    QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (!base.isEmpty()) return base + "/DPA-2026/mcp";
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/mcp";
}

QString McpBridgeService::manifestPath() const {
    return manifestDir() + "/bridge.json";
}

QString McpBridgeService::makeToken() const {
    QString token;
    token.reserve(64);
    QRandomGenerator* rng = QRandomGenerator::system();
    for (int i = 0; i < 8; ++i)
        token += QString::number(rng->generate(), 16).rightJustified(8, '0');
    return token;
}

bool McpBridgeService::writeManifest() const {
    if (!QDir().mkpath(manifestDir())) return false;
    QJsonObject endpoints;
    endpoints["health"] = "/health";
    endpoints["tools"] = "/tools";
    endpoints["call"] = "/call";
    QJsonObject obj;
    obj["schema_version"] = "0.1";
    obj["host"] = "127.0.0.1";
    obj["port"] = int(_server->serverPort());
    obj["read_token"] = _readToken;
    obj["pid"] = QString::number(QCoreApplication::applicationPid());
    obj["created_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    obj["endpoints"] = endpoints;
    QSaveFile f(manifestPath());
    if (!f.open(QIODevice::WriteOnly)) return false;
    const QByteArray manifest = QJsonDocument(obj).toJson(QJsonDocument::Indented);
    if (f.write(manifest) != manifest.size()) return false;
    return f.commit();
}

struct ParsedContentLength {
    bool valid = true;
    bool tooLarge = false;
    int value = 0;
};

static ParsedContentLength contentLength(const QByteArray& header, int maximum) {
    ParsedContentLength parsed;
    bool seen = false;
    for (const QByteArray& line : header.split('\n')) {
        const QByteArray trimmed = line.trimmed();
        if (!trimmed.toLower().startsWith("content-length:")) continue;
        if (seen) {
            parsed.valid = false;
            return parsed;
        }
        seen = true;
        const QByteArray raw = trimmed.mid(15).trimmed();
        if (raw.isEmpty()
            || std::any_of(raw.cbegin(), raw.cend(), [](char c) { return c < '0' || c > '9'; })) {
            parsed.valid = false;
            return parsed;
        }
        bool ok = false;
        const qlonglong value = raw.toLongLong(&ok, 10);
        if (!ok) {
            parsed.valid = false;
            return parsed;
        }
        if (value > maximum) {
            parsed.valid = false;
            parsed.tooLarge = true;
            return parsed;
        }
        parsed.value = static_cast<int>(value);
    }
    return parsed;
}

static QByteArray headerValue(const QByteArray& header, const QByteArray& name) {
    QByteArray prefix = name.toLower() + ":";
    for (const QByteArray& line : header.split('\n')) {
        QByteArray trimmed = line.trimmed();
        if (trimmed.toLower().startsWith(prefix)) return trimmed.mid(prefix.size()).trimmed();
    }
    return {};
}

static bool constantTimeEquals(const QByteArray& supplied, const QByteArray& expected) {
    const int count = std::max(supplied.size(), expected.size());
    unsigned int difference = static_cast<unsigned int>(supplied.size() ^ expected.size());
    for (int i = 0; i < count; ++i) {
        const unsigned char left = i < supplied.size()
            ? static_cast<unsigned char>(supplied.at(i)) : 0;
        const unsigned char right = i < expected.size()
            ? static_cast<unsigned char>(expected.at(i)) : 0;
        difference |= static_cast<unsigned int>(left ^ right);
    }
    return difference == 0;
}

static bool readOnlyToolCatalog(iAgentService* agent, QJsonArray& out) {
    if (!agent) return false;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromStdString(agent->listMcpTools()), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) return false;

    for (const QJsonValue& value : document.array()) {
        if (!value.isObject()) continue;
        const QJsonObject tool = value.toObject();
        const QString name = tool.value("name").toString();
        if (!name.isEmpty() && tool.value("dangerLevel").toString() == "read"
            && !agent->isMcpToolWrite(name.toStdString()))
            out.append(tool);
    }
    return true;
}

void McpBridgeService::onReadyRead(QTcpSocket* socket) {
    QByteArray buffer = socket->property("mcp_buffer").toByteArray();
    buffer += socket->readAll();
    // Cap buffered bytes BEFORE parsing/auth: bridge requests are tiny JSON, so a large
    // (or slowly-streamed huge Content-Length) request from any local process must not be
    // allowed to grow app memory unboundedly.
    static const int kMaxRequest = 1 * 1024 * 1024;   // 1 MB
    if (buffer.size() > kMaxRequest) {
        writeJson(socket, 413, QJsonObject{{"ok", false}, {"error", "request_too_large"}});
        socket->setProperty("mcp_buffer", QByteArray());
        return;
    }
    int headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        socket->setProperty("mcp_buffer", buffer);
        return;
    }
    QByteArray header = buffer.left(headerEnd);
    const ParsedContentLength parsedLength = contentLength(header, kMaxRequest);
    if (!parsedLength.valid) {
        writeJson(socket, parsedLength.tooLarge ? 413 : 400,
                  QJsonObject{{"ok", false},
                              {"error", parsedLength.tooLarge
                                  ? QStringLiteral("request_too_large")
                                  : QStringLiteral("invalid_content_length")}});
        socket->setProperty("mcp_buffer", QByteArray());
        return;
    }
    const int bodyLen = parsedLength.value;
    if (buffer.size() < headerEnd + 4 + bodyLen) {
        socket->setProperty("mcp_buffer", buffer);
        return;
    }
    QByteArray body = buffer.mid(headerEnd + 4, bodyLen);
    socket->setProperty("mcp_buffer", QByteArray());
    handleRequest(socket, header, body);
}

void McpBridgeService::handleRequest(QTcpSocket* socket, const QByteArray& header, const QByteArray& body) {
    QList<QByteArray> requestLine = header.split('\n').value(0).trimmed().split(' ');
    QString method = QString::fromUtf8(requestLine.value(0));
    QString path = QString::fromUtf8(requestLine.value(1)).split('?').first();
    const QByteArray auth = headerValue(header, "authorization");
    const QByteArray expectedAuth = QByteArrayLiteral("Bearer ") + _readToken.toLatin1();
    const bool readAuth = constantTimeEquals(auth, expectedAuth);
    if (!readAuth) {
        writeJson(socket, 401, QJsonObject{{"ok", false}, {"error", "unauthorized"}});
        return;
    }
    // M9: rate-limit authenticated requests so a runaway/abusive client can't flood the app.
    if (!_limiter.allow(QDateTime::currentMSecsSinceEpoch())) {
        writeJson(socket, 429, QJsonObject{{"ok", false}, {"error", "rate_limited"}});
        return;
    }
    if (method == "GET" && path == "/health") {
        writeJson(socket, 200, QJsonObject{{"ok", true}, {"service", "dpa-mcp-bridge"}});
        return;
    }
    if (method == "GET" && path == "/tools") {
        QJsonArray readTools;
        if (!readOnlyToolCatalog(_agent, readTools)) {
            writeJson(socket, 500, QJsonObject{{"ok", false}, {"error", "tool_catalog_unavailable"}});
            return;
        }
        writeJson(socket, 200, QJsonObject{{"ok", true}, {"tools", readTools}});
        return;
    }
    if (method != "POST" || path != "/call") {
        writeJson(socket, 404, QJsonObject{{"ok", false}, {"error", "not_found"}});
        return;
    }

    QElapsedTimer auditTimer;
    auditTimer.start();
    QString auditTool;
    QByteArray auditArgs;

    QJsonDocument reqDoc = QJsonDocument::fromJson(body);
    if (!reqDoc.isObject()) {
        logAudit(auditTool, auditArgs, 400, auditTimer.elapsed(), socket);
        writeJson(socket, 400, QJsonObject{{"ok", false}, {"error", "bad_json"}});
        return;
    }
    QJsonObject req = reqDoc.object();
    if (!req.value("name").isString() || req.value("name").toString().isEmpty()) {
        auditTool = req.value("name").toString();
        logAudit(auditTool, auditArgs, 400, auditTimer.elapsed(), socket);
        writeJson(socket, 400, QJsonObject{{"ok", false}, {"error", "invalid_tool_name"}});
        return;
    }
    QString name = req.value("name").toString();
    auditTool = name;
    if (req.contains("arguments") && !req.value("arguments").isObject()) {
        logAudit(auditTool, auditArgs, 400, auditTimer.elapsed(), socket);
        writeJson(socket, 400, QJsonObject{{"ok", false}, {"error", "invalid_arguments"}});
        return;
    }
    auditArgs = QJsonDocument(req.value("arguments").toObject()).toJson(QJsonDocument::Compact);
    // M9: a read-only token may not invoke write-level tools — reject before execution.
    QJsonArray readTools;
    if (!readOnlyToolCatalog(_agent, readTools)) {
        logAudit(auditTool, auditArgs, 500, auditTimer.elapsed(), socket);
        writeJson(socket, 500, QJsonObject{{"ok", false}, {"error", "tool_catalog_unavailable"}});
        return;
    }
    bool explicitlyReadOnly = false;
    for (const QJsonValue& value : readTools) {
        if (value.toObject().value("name").toString() == name) {
            explicitlyReadOnly = true;
            break;
        }
    }
    if (!explicitlyReadOnly) {
        logAudit(auditTool, auditArgs, 403, auditTimer.elapsed(), socket);
        writeJson(socket, 403, QJsonObject{{"ok", false}, {"error", "scope_forbidden"},
                                           {"summary", "Read-only token may call only explicitly classified read tools."}});
        return;
    }
    QJsonObject args = req.value("arguments").toObject();
    QByteArray argsJson = QJsonDocument(args).toJson(QJsonDocument::Compact);
    QElapsedTimer timer;
    timer.start();
    QJsonDocument resultDoc = QJsonDocument::fromJson(QByteArray::fromStdString(_agent->callMcpTool(name.toStdString(), argsJson.toStdString())));
    QJsonObject result = resultDoc.isObject() ? resultDoc.object() : QJsonObject{{"ok", false}, {"error", "bad_result_json"}};
    qint64 elapsedMs = timer.elapsed();
    result["elapsed_ms"] = double(elapsedMs);
    logAudit(auditTool, auditArgs, 200, auditTimer.elapsed(), socket);
    writeJson(socket, 200, result);
}

void McpBridgeService::writeJson(QTcpSocket* socket, int status, const QJsonObject& obj) const {
    QByteArray body = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    QByteArray reason = status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 401 ? "Unauthorized" : status == 403 ? "Forbidden" : status == 413 ? "Payload Too Large" : status == 429 ? "Too Many Requests" : status == 500 ? "Internal Server Error" : "Not Found";
    QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + " " + reason + "\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n";
    socket->write(head);
    socket->write(body);
    socket->disconnectFromHost();
}

QString McpBridgeService::auditPath() const {
    return manifestDir() + "/audit.jsonl";
}

void McpBridgeService::logAudit(const QString& tool, const QByteArray& argsJson, int statusCode, qint64 elapsedMs, QTcpSocket* socket) {
    QJsonObject entry;
    entry["ts"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    entry["tool"] = tool;
    entry["args"] = QString::fromUtf8(argsJson.left(200));
    entry["status"] = statusCode;
    entry["elapsed_ms"] = double(elapsedMs);
    QString clientIp = "unknown";
    if (socket) {
        clientIp = socket->peerAddress().toString();
        if (clientIp.startsWith("::ffff:")) clientIp = clientIp.mid(7);
    }
    entry["client_ip"] = clientIp;
    QDir().mkpath(manifestDir());
    QFile file(auditPath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        file.write(QJsonDocument(entry).toJson(QJsonDocument::Compact));
        file.write("\n");
        file.close();
    }
    rotateAuditIfNeeded();
}

void McpBridgeService::rotateAuditIfNeeded() {
    QFile file(auditPath());
    if (!file.exists() || file.size() <= 10LL * 1024 * 1024) return;
    file.close();
    const QString ts = QDateTime::currentDateTimeUtc().toString("yyyyMMdd'T'HHmmss'Z'");
    QFile::rename(auditPath(), manifestDir() + "/audit." + ts + ".jsonl");
}

QJsonArray McpBridgeService::readAuditLog(int count) {
    count = std::max(1, std::min(100, count));
    QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (!base.isEmpty()) base += "/DPA-2026/mcp";
    else base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/mcp";
    QFile file(base + "/audit.jsonl");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QList<QByteArray> lines;
    while (!file.atEnd()) {
        QByteArray line = file.readLine().trimmed();
        if (!line.isEmpty()) lines.append(line);
    }
    file.close();
    QJsonArray entries;
    const int start = std::max(0, lines.size() - count);
    for (int i = start; i < lines.size(); ++i) {
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(lines[i], &err);
        if (err.error == QJsonParseError::NoError && doc.isObject())
            entries.append(doc.object());
    }
    return entries;
}
