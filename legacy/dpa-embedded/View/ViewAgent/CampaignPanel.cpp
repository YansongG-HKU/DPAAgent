#include "CampaignPanel.h"
#include "AgentUiParse.h"
#include "AgentTheme.h"

#include "HeaderDefine.h"
#include "iAgentService.h"

#include <QDateTime>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVBoxLayout>
#include <QHBoxLayout>

using namespace Model;

namespace {
// Palette aliases always follow the active theme so this panel cannot drift from the chat dock.
const char* kLine() { return AgentTheme::currentBorderDefault(); }
const char* kMute() { return AgentTheme::currentTextSecondary(); }

QString stateColor(const QString& state) {
    if (state == QStringLiteral("completed")) return QString::fromLatin1(AgentTheme::currentOkGreen());
    if (state == QStringLiteral("failed") || state == QStringLiteral("aborted"))
        return QString::fromLatin1(AgentTheme::currentRedStop());
    if (state == QStringLiteral("arming") || state == QStringLiteral("monitoring")
        || state == QStringLiteral("analyzing") || state == QStringLiteral("running"))
        return QString::fromLatin1(AgentTheme::currentBluePrimary());
    return QString::fromLatin1(AgentTheme::currentNeutralDefault());
}
} // namespace

CampaignPanel::CampaignPanel(QWidget* parent) : QWidget(parent)
{
    buildUi();
}

void CampaignPanel::buildUi()
{
    setStyleSheet(QString("QWidget{background:%1;} QLabel{color:%2;}").arg(AgentTheme::currentSurfaceCard(), AgentTheme::currentTextDark()));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(11, 7, 10, 7);
    layout->setSpacing(5);

    auto* header = new QHBoxLayout();
    header->setSpacing(7);
    _stateDot = new QLabel(QString::fromUtf8("●"), this);
    _stateDot->setStyleSheet(QStringLiteral("color:%1;font-size:11px;").arg(kMute()));
    _stateLabel = new QLabel(QString::fromUtf8("试验战役：空闲"), this);
    _stateLabel->setStyleSheet(QStringLiteral("font-size:12px;color:%1;").arg(kMute()));
    _refreshBtn = new QPushButton(QString::fromUtf8("刷新"), this);
    _refreshBtn->setCursor(Qt::PointingHandCursor);
    _refreshBtn->setFlat(true);
    _refreshBtn->setStyleSheet(QString(
        "QPushButton{border:1px solid %1;border-radius:7px;padding:2px 9px;"
        "color:%2;background:%3;font-size:12px;}").arg(kLine(), kMute(), AgentTheme::currentSurfaceCard()));
    connect(_refreshBtn, &QPushButton::clicked, this,
            &CampaignPanel::refreshFromService);
    header->addWidget(_stateDot);
    header->addWidget(_stateLabel);
    header->addStretch();
    header->addWidget(_refreshBtn);
    layout->addLayout(header);

    auto* runsCaption = new QLabel(QString::fromUtf8("逐运行记录（证据绑定）"), this);
    runsCaption->setStyleSheet(
        QString("font-size:11px;color:%1;font-weight:bold;").arg(kMute()));
    layout->addWidget(runsCaption);

    _runsTable = new QTableWidget(0, 6, this);
    _runsTable->setHorizontalHeaderLabels({QString::fromUtf8("#"),
        QString::fromUtf8("设计"), QString::fromUtf8("因子水平"),
        QString::fromUtf8("结果"), QString::fromUtf8("证据"), QString::fromUtf8("错误")});
    _runsTable->verticalHeader()->setVisible(false);
    _runsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    _runsTable->horizontalHeader()->setStretchLastSection(true);
    _runsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _runsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    _runsTable->setAlternatingRowColors(true);
    _runsTable->setMaximumHeight(120);
    _runsTable->setStyleSheet(QString("QTableWidget{font-size:11px;border:1px solid %1;}").arg(AgentTheme::currentBorderLight()));
    layout->addWidget(_runsTable);

    auto* knowledgeCaption = new QLabel(QString::fromUtf8("试验知识账本（历次结论）"), this);
    knowledgeCaption->setStyleSheet(
        QString("font-size:11px;color:%1;font-weight:bold;").arg(kMute()));
    layout->addWidget(knowledgeCaption);

    _knowledgeTable = new QTableWidget(0, 5, this);
    _knowledgeTable->setHorizontalHeaderLabels({QString::fromUtf8("时间"),
        QString::fromUtf8("战役"), QString::fromUtf8("最强因子"),
        QString::fromUtf8("最优工况"), QString::fromUtf8("噪声σ")});
    _knowledgeTable->verticalHeader()->setVisible(false);
    _knowledgeTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    _knowledgeTable->horizontalHeader()->setStretchLastSection(true);
    _knowledgeTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _knowledgeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    _knowledgeTable->setAlternatingRowColors(true);
    _knowledgeTable->setMaximumHeight(150);
    _knowledgeTable->setStyleSheet(QString("QTableWidget{font-size:11px;border:1px solid %1;}").arg(AgentTheme::currentBorderLight()));
    layout->addWidget(_knowledgeTable);
}

void CampaignPanel::setStateText(const QString& state, const QString& stopReason)
{
    QString text = QString::fromUtf8("试验战役：");
    if (state == QStringLiteral("completed")) text += QString::fromUtf8("已完成");
    else if (state == QStringLiteral("failed")) text += QString::fromUtf8("失败");
    else if (state == QStringLiteral("aborted")) text += QString::fromUtf8("已中止");
    else if (state == QStringLiteral("arming") || state == QStringLiteral("monitoring")
             || state == QStringLiteral("analyzing")) text += QString::fromUtf8("执行中");
    else text += QString::fromUtf8("空闲");
    if (!stopReason.isEmpty())
        text += QStringLiteral("（%1）").arg(stopReason);
    _stateLabel->setText(text);
    _stateDot->setStyleSheet(
        QStringLiteral("color:%1;font-size:11px;").arg(stateColor(state)));
}

void CampaignPanel::applyCampaignResult(const QString& resultJson)
{
    UiCampaignSnapshot snapshot;
    if (!parseCampaignRunRows(resultJson.toStdString(), snapshot)) return;
    setStateText(QString::fromStdString(snapshot.status),
                 QString::fromStdString(snapshot.stopReason));
    _runsTable->setRowCount(static_cast<int>(snapshot.runs.size()));
    for (int row = 0; row < static_cast<int>(snapshot.runs.size()); ++row) {
        const UiCampaignRunRow& run = snapshot.runs[static_cast<size_t>(row)];
        auto put = [this, row](int column, const QString& text, const QString& color) {
            auto* item = new QTableWidgetItem(text);
            item->setForeground(QBrush(QColor(color)));
            _runsTable->setItem(row, column, item);
        };
        put(0, QString::number(run.runIndex), AgentTheme::currentTextDark());
        put(1, QString::number(run.designIndex), AgentTheme::currentTextDark());
        put(2, QString::fromStdString(run.levelsText), AgentTheme::currentTextDark());
        put(3, run.ok ? QString::fromUtf8("✓ 成功") :
            QString::fromUtf8("✗ %1").arg(QString::fromStdString(run.errorCode)),
            run.ok ? AgentTheme::currentOkGreen() : AgentTheme::currentRedStop());
        put(4, QString::fromStdString(run.evidenceId),
            run.evidenceId.empty() ? AgentTheme::currentRedStop() : AgentTheme::currentOkGreen());
        put(5, QString::fromStdString(run.errorCode), AgentTheme::currentNeutralDefault());
    }
}

void CampaignPanel::applyStatusSnapshot(const QString& statusJson)
{
    const QJsonDocument document = QJsonDocument::fromJson(statusJson.toUtf8());
    if (!document.isObject()) return;
    QJsonObject root = document.object();
    QJsonObject payload = root.value("data").toObject();
    if (payload.isEmpty()) payload = root;
    setStateText(payload.value("state").toString(),
                 payload.value("stop_reason").toString());
}

void CampaignPanel::refreshFromService()
{
    auto* service = GetDefaultService(Model::iAgentService);
    if (!service) return;
    applyStatusSnapshot(QString::fromStdString(
        service->callTool("autonomous_experiment_status", "{}")));
    std::string knowledgeJson =
        service->callTool("list_experiment_knowledge", R"({"max_items":30})");
    std::vector<UiKnowledgeRow> rows;
    if (!parseKnowledgeRows(knowledgeJson, rows)) return;
    _knowledgeTable->setRowCount(static_cast<int>(rows.size()));
    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        const UiKnowledgeRow& entry = rows[static_cast<size_t>(row)];
        _knowledgeTable->setItem(row, 0, new QTableWidgetItem(
            QDateTime::fromMSecsSinceEpoch(entry.recordedAtMs)
                .toString(QStringLiteral("MM-dd hh:mm"))));
        _knowledgeTable->setItem(row, 1, new QTableWidgetItem(
            QString::fromStdString(entry.campaignId)));
        QString dominant = entry.testable
            ? QStringLiteral("%1 (Δ=%2, p=%3)")
                  .arg(QString::fromStdString(entry.dominantFactor))
                  .arg(entry.effectRange, 0, 'g', 4)
                  .arg(entry.permP, 0, 'g', 3)
            : QString::fromUtf8("无显著因子");
        _knowledgeTable->setItem(row, 2, new QTableWidgetItem(dominant));
        _knowledgeTable->setItem(row, 3, new QTableWidgetItem(
            QString::fromStdString(entry.bestLevels)));
        _knowledgeTable->setItem(row, 4, new QTableWidgetItem(
            entry.residualEstimable
                ? QString::number(entry.residualStd, 'g', 4)
                : QString::fromUtf8("—")));
    }
}

void CampaignPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refreshFromService();
}
