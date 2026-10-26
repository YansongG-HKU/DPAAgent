#include <QtTest/QtTest>
#include "McpBridgeService.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTcpSocket>

using namespace Model;

namespace {
class ScopedLocalAppData {
public:
    explicit ScopedLocalAppData(const QByteArray& value)
        : _wasSet(qEnvironmentVariableIsSet("LOCALAPPDATA")), _previous(qgetenv("LOCALAPPDATA")) {
        qputenv("LOCALAPPDATA", value);
    }
    ~ScopedLocalAppData() {
        if (_wasSet) qputenv("LOCALAPPDATA", _previous);
        else qunsetenv("LOCALAPPDATA");
    }
private:
    bool _wasSet;
    QByteArray _previous;
};

class FakeAgentService final : public iAgentService {
public:
    std::string listTools() override { return "[]"; }
    std::string callTool(const std::string&, const std::string&) override { return "{}"; }
    std::string listMcpTools() override {
        return R"([{"name":"dpa.read","dangerLevel":"read"},{"name":"dpa.write","dangerLevel":"write"}])";
    }
    std::string callMcpTool(const std::string& name, const std::string&) override {
        ++calls;
        lastTool = name;
        return R"({"ok":true,"summary":"fake read","data":{}})";
    }
    std::string sendMessage(const std::string&) override { return {}; }
    void sendMessageAsync(const std::string&) override {}
    void sendMessageWithAttachmentsAsync(const std::string&, const std::vector<std::string>&) override {}
    void cancelCurrentTurn() override {}
    bool startNewSession() override { return true; }
    std::string listSessions() override { return R"({"ok":true,"sessions":[]})"; }
    std::string resumeSession(const std::string&) override { return "{}"; }
    std::string runtimeInfo() override { return "{}"; }
    void confirmPendingAction(const std::string&, bool) override {}
    bool isMcpToolWrite(const std::string& name) override { return name != "dpa.read"; }

    int calls = 0;
    std::string lastTool;
};

struct BridgeEndpoint {
    quint16 port = 0;
    QByteArray token;
};

BridgeEndpoint readEndpoint(const QString& path) {
    BridgeEndpoint endpoint;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return endpoint;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) return endpoint;
    endpoint.port = static_cast<quint16>(document.object().value("port").toInt());
    endpoint.token = document.object().value("read_token").toString().toLatin1();
    return endpoint;
}

QByteArray exchange(quint16 port, const QByteArray& request) {
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    if (!socket.waitForConnected(1000)) return {};
    if (socket.write(request) != request.size()) return {};
    socket.flush();

    QByteArray response;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        response += socket.readAll();
        if (socket.state() == QAbstractSocket::UnconnectedState) break;
        QTest::qWait(1);
    }
    response += socket.readAll();
    return response;
}

QByteArray authorizedRequest(const QByteArray& method, const QByteArray& path,
                             const QByteArray& token, const QByteArray& body = {}) {
    return method + " " + path + " HTTP/1.1\r\n"
        + "Host: 127.0.0.1\r\n"
        + "Authorization: Bearer " + token + "\r\n"
        + "Content-Type: application/json\r\n"
        + "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
}

bool hasStatus(const QByteArray& response, int status) {
    return response.startsWith("HTTP/1.1 " + QByteArray::number(status) + " ");
}
} // namespace

class TstMcpBridge : public QObject {
    Q_OBJECT
private slots:
    void authentication_and_request_bounds() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        ScopedLocalAppData localAppData(temp.path().toUtf8());
        FakeAgentService agent;
        McpBridgeService bridge(&agent, 100, 60000);
        QVERIFY(bridge.start());
        const BridgeEndpoint endpoint = readEndpoint(bridge.manifestPath());
        QVERIFY(endpoint.port != 0);
        QCOMPARE(endpoint.token.size(), 64);

        const QByteArray unauthorized = exchange(endpoint.port,
            "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n");
        QVERIFY(hasStatus(unauthorized, 401));

        const QByteArray wrongToken(64, '0');
        QVERIFY(hasStatus(exchange(endpoint.port,
            authorizedRequest("GET", "/health", wrongToken)), 401));
        QVERIFY(hasStatus(exchange(endpoint.port,
            authorizedRequest("GET", "/health", endpoint.token)), 200));

        const QByteArray tooLarge = exchange(endpoint.port,
            "POST /call HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 1048577\r\n\r\n");
        QVERIFY(hasStatus(tooLarge, 413));
        const QByteArray invalidLength = exchange(endpoint.port,
            "POST /call HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: -1\r\n\r\n");
        QVERIFY(hasStatus(invalidLength, 400));
        const QByteArray duplicateLength = exchange(endpoint.port,
            "POST /call HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n");
        QVERIFY(hasStatus(duplicateLength, 400));
    }

    void read_only_catalog_and_execution() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        ScopedLocalAppData localAppData(temp.path().toUtf8());
        FakeAgentService agent;
        McpBridgeService bridge(&agent, 100, 60000);
        QVERIFY(bridge.start());
        const BridgeEndpoint endpoint = readEndpoint(bridge.manifestPath());
        QVERIFY(endpoint.port != 0);

        const QByteArray catalog = exchange(endpoint.port,
            authorizedRequest("GET", "/tools", endpoint.token));
        QVERIFY(hasStatus(catalog, 200));
        QVERIFY(catalog.contains("dpa.read"));
        QVERIFY(!catalog.contains("dpa.write"));

        const QByteArray readBody = R"({"name":"dpa.read","arguments":{}})";
        const QByteArray readResponse = exchange(endpoint.port,
            authorizedRequest("POST", "/call", endpoint.token, readBody));
        QVERIFY(hasStatus(readResponse, 200));
        QCOMPARE(agent.calls, 1);
        QCOMPARE(agent.lastTool, std::string("dpa.read"));

        const QByteArray writeBody = R"({"name":"dpa.write","arguments":{}})";
        const QByteArray writeResponse = exchange(endpoint.port,
            authorizedRequest("POST", "/call", endpoint.token, writeBody));
        QVERIFY(hasStatus(writeResponse, 403));
        QCOMPARE(agent.calls, 1);

        const QByteArray badArgs = R"({"name":"dpa.read","arguments":[]})";
        QVERIFY(hasStatus(exchange(endpoint.port,
            authorizedRequest("POST", "/call", endpoint.token, badArgs)), 400));
        QCOMPARE(agent.calls, 1);
    }

    void authenticated_requests_are_rate_limited() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        ScopedLocalAppData localAppData(temp.path().toUtf8());
        FakeAgentService agent;
        McpBridgeService bridge(&agent, 2, 60000);
        QVERIFY(bridge.start());
        const BridgeEndpoint endpoint = readEndpoint(bridge.manifestPath());
        QVERIFY(endpoint.port != 0);
        const QByteArray request = authorizedRequest("GET", "/health", endpoint.token);

        QVERIFY(hasStatus(exchange(endpoint.port, request), 200));
        QVERIFY(hasStatus(exchange(endpoint.port, request), 200));
        QVERIFY(hasStatus(exchange(endpoint.port, request), 429));
    }
};

int runTstMcpBridge(int argc, char** argv) {
    TstMcpBridge test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_mcpbridge.moc"
