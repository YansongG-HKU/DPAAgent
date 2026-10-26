#ifndef AGENTCHATDOCK_H
#define AGENTCHATDOCK_H

#include <QtWidgets/QDockWidget>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QFrame>
#include <QtWidgets/QLineEdit>
#include <QPointer>
#include <QJsonObject>
#include <string>
#include "HeaderDefine.h"

class WorkflowCard;   // defined in the .cpp
class CampaignPanel;
class QToolButton;

// Upgraded AI assistant panel: message bubbles + Markdown, a live workflow timeline of the
// agent's tool calls, grounded diagnostic cards, inline human-in-the-loop confirmation, and
// full chat interactions. Renders via self-drawn QWidgets (no QWebEngine).
class AgentChatDock : public QDockWidget
{
    Q_OBJECT
public:
    explicit AgentChatDock(QWidget* parent = nullptr);
    ~AgentChatDock();

    Model::M* _vm = nullptr;
    void setViewModel(Model::M*);
    void onPropertyChanged(const std::string& Index, utils::Property* prop);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void showEvent(QShowEvent* event) override;

private:
    void initWindow();
    void appendWidget(QWidget* w);
    void autoScroll();
    void addUserBubble(const QString& text, const QStringList& attachNames);
    void addAiBubble(const QString& markdown);
    void addDiagnosticCard(const QString& toolResultJson);
    void addReportCard(const QString& toolResultJson);
    void addFeedbackCard(const QString& toolResultJson);
    void addMonitoringTrendCard(const QString& trendJson);
    void addCompanionEventCard(const QString& eventJson);
    void addConfirmCard(const QString& confirmationId, const QString& toolName,
                        const QString& argsSummary);
    void addRefinementCard(const QString& detailJson);
    void addCheckpointCard(const QString& detailJson);
    void addCompanionDashboardCard(const QString& statusJson);
    void invalidateConfirmCard(const QString& reason);
    void setRunning(bool running);
    void updateControls();
    void updateAttachmentLabel();
    void sendCurrent();
    void clearStream();
    void showSessionMenu();
    void showRuntimeInfo();
    void refreshProviderBadge(const QString& runtimeInfoJson);
    QString runtimeSummaryText(const QJsonObject& info) const;
    void showCompanionSettings();
    void showExperimentHealth();
    void updateCompanionStatus(const QString& statusJson);
    void restoreSessionUi(const QString& sessionJson);

    // header
    QLabel* _statusDot = nullptr;
    QLabel* _providerBadge = nullptr;          // provider health pill (green/amber/red)
    QToolButton* _newSessionBtn = nullptr;
    QToolButton* _historyBtn = nullptr;
    QToolButton* _runtimeBtn = nullptr;
    QToolButton* _themeBtn = nullptr;
    QToolButton* _companionToggle = nullptr;
    QLabel* _companionState = nullptr;
    QToolButton* _healthBtn = nullptr;
    QToolButton* _evidenceBtn = nullptr;
    QToolButton* _companionSettings = nullptr;
    // stream
    QScrollArea* _scroll = nullptr;
    QWidget* _host = nullptr;
    QVBoxLayout* _stream = nullptr;
    QWidget* _quickActions = nullptr;
    // campaign status section (between companion bar and message stream)
    CampaignPanel* _campaignPanel = nullptr;
    QToolButton* _campaignToggle = nullptr;
    // input
    QLabel* _attachmentLabel = nullptr;
    QTextEdit* _input = nullptr;
    QPushButton* _attachBtn = nullptr;
    QPushButton* _sendBtn = nullptr;
    QPushButton* _stopBtn = nullptr;
    // turn state
    WorkflowCard* _activeWorkflow = nullptr;   // current turn's workflow card, null between turns
    QPointer<QFrame> _activeConfirmCard;
    std::vector<QWidget*> _prunable;           // message widgets eligible for trimming (FIFO)
    bool _running = false;
    bool _analysisPending = false;
    bool _turnAbnormal = false;                // cancelled/failed turn -> workflow ends with ✗
    void pruneStream();
    // chat search (Ctrl+F)
    QWidget* _searchBar = nullptr;
    QLineEdit* _searchInput = nullptr;
    QLabel* _searchCountLabel = nullptr;
    int _searchCurrent = 0;
    int _searchTotal = 0;
    void toggleSearchBar();

private slots:
    void onStep(QString kind, QString name, QString detail);
    void onConfirm(QString confirmationId, QString toolName, QString argsSummary);
    void onNewSession();
};

#endif // AGENTCHATDOCK_H
