#include "AgentChatDock.h"
#include "MarkdownLite.h"
#include "AgentUiParse.h"
#include "AgentTheme.h"
#include "CampaignPanel.h"
#include "BaselineCompareCard.h"
#include <QtMath>
#include <QtWidgets/QWidget>
#include <QtWidgets/QApplication>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSizePolicy>
#include <QFileInfo>
#include <QThread>
#include <QTimer>
#include <QKeyEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QHash>
#include <QSet>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMessageBox>
#include <QDialog>
#include <QFormLayout>
#include <QShortcut>
#include <QKeySequence>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QLocale>
#include <QSignalBlocker>
#include <QMainWindow>
#include <QScreen>
#include <QShowEvent>
#include <QPainter>
#include <QClipboard>
#include <QTextDocument>
#include <QRegularExpression>
#include <QFileDialog>
#include <QStandardPaths>
#include <algorithm>
using namespace Model;
using namespace ViewModel;

// ============================ ProgressTimelineBar ============================
// Custom-painted horizontal bar showing workflow step progress.
class ProgressTimelineBar : public QWidget {
public:
    explicit ProgressTimelineBar(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedHeight(6);
        _pulseTimer = new QTimer(this);
        connect(_pulseTimer, &QTimer::timeout, this, [this]() {
            _pulsePhase = (_pulsePhase + 1) % 60;
            update();
        });
        _pulseTimer->start(50);
    }
    ~ProgressTimelineBar() override { _pulseTimer->stop(); }

    void setTotalSteps(int total)     { _totalSteps = total; update(); }
    void setCompletedSteps(int c)     { _completedSteps = c; update(); }
    void setRunningIndex(int idx)     { _runningIndex = idx; update(); }
    void setFailedIndices(const QSet<int>& s) { _failedIndices = s; update(); }
    void setFinished()                { _finished = true; _runningIndex = -1; update(); }

    QSize sizeHint() const override { return QSize(200, 6); }
    QSize minimumSizeHint() const override { return QSize(60, 6); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRect r = rect().adjusted(2, 0, -2, 0);
        if (r.width() < 4 || _totalSteps <= 0) return;

        const int segCount = _totalSteps;
        const double gap = 3.0;
        const double totalGaps = gap * (segCount - 1);
        const double segW = qMax(2.0, (r.width() - totalGaps) / segCount);
        const double barH = 6.0;
        const double radius = 3.0;

        auto colorFor = [&](int idx) -> QColor {
            if (_failedIndices.contains(idx)) return QColor(AgentTheme::currentBadRed());
            if (idx < _completedSteps) return QColor(AgentTheme::currentOkGreen());
            if (idx == _runningIndex) {
                double brightness = 0.55 + 0.45 * qSin(_pulsePhase * M_PI / 30.0);
                QColor c(AgentTheme::currentStepRunning());
                c.setAlphaF(brightness);
                return c;
            }
            return QColor(AgentTheme::currentTextSecondary()).lighter(130);
        };

        for (int i = 0; i < segCount; ++i) {
            double x = r.x() + i * (segW + gap);
            QRectF seg(x, r.y() + (r.height() - barH) / 2.0, segW, barH);
            p.setPen(Qt::NoPen);
            p.setBrush(colorFor(i));
            p.drawRoundedRect(seg, radius, radius);
        }
    }

private:
    int _totalSteps = 0;
    int _completedSteps = 0;
    int _runningIndex = -1;
    QSet<int> _failedIndices;
    bool _finished = false;
    int _pulsePhase = 0;
    QTimer* _pulseTimer = nullptr;
};

// ============================ StepIndicator ============================
// Small circle widget representing a single workflow step state.
class StepIndicator : public QWidget {
public:
    enum State { Pending, Running, Completed, Failed };

    explicit StepIndicator(State state, const QString& tooltip, QWidget* parent = nullptr)
        : QWidget(parent), _state(state) {
        setFixedSize(16, 16);
        setToolTip(tooltip);
    }

    void setState(State s) { _state = s; update(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRect r = rect().adjusted(1, 1, -1, -1);
        const QColor green(AgentTheme::currentOkGreen());
        const QColor amber(AgentTheme::currentStepRunning());
        const QColor red(AgentTheme::currentBadRed());
        const QColor gray(AgentTheme::currentTextSecondary());

        p.setPen(Qt::NoPen);
        switch (_state) {
        case Completed:
            p.setBrush(green);
            p.drawEllipse(r);
            break;
        case Running:
            p.setBrush(QColor(amber.red(), amber.green(), amber.blue(), 50));
            p.setPen(QPen(amber, 2));
            p.drawEllipse(r);
            break;
        case Failed:
            p.setBrush(red);
            p.drawEllipse(r);
            break;
        case Pending:
        default:
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(gray, 1.5));
            p.drawEllipse(r);
            break;
        }
    }

private:
    State _state = Pending;
};

// ============================ WorkflowCard ============================
// A collapsible card that shows the agent's tool calls as a live step timeline
// with a visual progress bar, step indicators, and elapsed time.
class WorkflowCard : public QFrame {
public:
    explicit WorkflowCard(QWidget* parent = nullptr) : QFrame(parent) {
        setObjectName("wfCard");
        setStyleSheet(AgentTheme::cardFrameStyle());
        auto* v = new QVBoxLayout(this); v->setContentsMargins(0,0,0,0); v->setSpacing(0);
        _head = new QToolButton(this);
        _head->setCheckable(true); _head->setChecked(true);
        _head->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        _head->setCursor(Qt::PointingHandCursor);
        _head->setStyleSheet(QString("QToolButton{border:none;background:%2;border-top-left-radius:10px;"
            "border-top-right-radius:10px;padding:8px 11px;text-align:left;font-weight:bold;color:%1;}")
            .arg(AgentTheme::currentTextPrimary(), AgentTheme::currentSurfaceElevated()));
        v->addWidget(_head);

        _progressBar = new ProgressTimelineBar(this);
        _stepIndicators = new QWidget(this);
        _stepIndicators->setFixedHeight(22);
        auto* indicatorsH = new QHBoxLayout(_stepIndicators);
        indicatorsH->setContentsMargins(0, 0, 0, 0); indicatorsH->setSpacing(4);
        indicatorsH->setAlignment(Qt::AlignLeft);
        _elapsedLabel = new QLabel(this);
        _elapsedLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        _elapsedLabel->setStyleSheet(QString("color:%1;font-family:Consolas;font-size:10px;background:transparent;border:none;")
            .arg(AgentTheme::currentTextTertiary()));
        _elapsedLabel->setFixedHeight(16);

        auto* progressRow = new QWidget(this);
        auto* progressRowH = new QHBoxLayout(progressRow);
        progressRowH->setContentsMargins(11, 4, 11, 0); progressRowH->setSpacing(6);
        progressRowH->addWidget(_progressBar, 1);
        progressRowH->addWidget(_elapsedLabel, 0);

        auto* indicatorsRow = new QWidget(this);
        auto* indicatorsRowH = new QHBoxLayout(indicatorsRow);
        indicatorsRowH->setContentsMargins(11, 0, 11, 0); indicatorsRowH->setSpacing(6);
        indicatorsRowH->setAlignment(Qt::AlignLeft);
        indicatorsRowH->addWidget(_stepIndicators);

        v->addWidget(progressRow);
        v->addWidget(indicatorsRow);

        _stepsHost = new QWidget(this);
        _stepsLay = new QVBoxLayout(_stepsHost);
        _stepsLay->setContentsMargins(11,4,11,10); _stepsLay->setSpacing(2);
        v->addWidget(_stepsHost);
        QObject::connect(_head, &QToolButton::toggled, _stepsHost, &QWidget::setVisible);
        QObject::connect(_head, &QToolButton::toggled, _progressBar, &QWidget::setVisible);
        QObject::connect(_head, &QToolButton::toggled, indicatorsRow, &QWidget::setVisible);
        QObject::connect(_head, &QToolButton::toggled, progressRow, &QWidget::setVisible);

        _elapsedTimer = new QTimer(this);
        connect(_elapsedTimer, &QTimer::timeout, this, &WorkflowCard::updateElapsed);
        _workflowStartMs = QDateTime::currentMSecsSinceEpoch();
        _elapsedTimer->start(1000);
        updateElapsed();
        relabel();
    }

    void addStep(const QString& name, const QString& argsJson) {
        auto* row = new QWidget(_stepsHost);
        auto* h = new QHBoxLayout(row); h->setContentsMargins(0,3,0,3); h->setSpacing(8);
        auto* dot = new QLabel(QString::fromUtf8("⟳"), row);          // ⟳ running
        dot->setFixedWidth(16); dot->setAlignment(Qt::AlignTop|Qt::AlignHCenter);
        dot->setStyleSheet(QString("color:%1;font-weight:bold;").arg(AgentTheme::currentStepRunning()));
        auto* col = new QVBoxLayout(); col->setContentsMargins(0,0,0,0); col->setSpacing(1);
        auto* nameBtn = new QToolButton(row);
        nameBtn->setText(name + QString::fromUtf8("  ▸"));
        nameBtn->setCheckable(true); nameBtn->setCursor(Qt::PointingHandCursor);
        nameBtn->setStyleSheet(QString("QToolButton{border:none;background:transparent;color:%1;"
            "font-family:Consolas;font-weight:bold;text-align:left;}").arg(AgentTheme::currentTextPrimary()));
        auto* detail = new QLabel(prettyArgs(argsJson), row);
        detail->setWordWrap(true); detail->setVisible(false);
        detail->setStyleSheet(QString("color:%1;font-family:Consolas;font-size:11px;background:%3;"
            "border:1px solid %2;border-radius:6px;padding:5px 7px;").arg(AgentTheme::currentTextSecondary()).arg(AgentTheme::currentBorderDefault()).arg(AgentTheme::currentSurfaceElevated()));
        QObject::connect(nameBtn, &QToolButton::toggled, detail, &QLabel::setVisible);
        col->addWidget(nameBtn); col->addWidget(detail);
        h->addWidget(dot); h->addLayout(col, 1);
        _stepsLay->addWidget(row);
        Row r; r.dot = dot; r.detail = detail; r.name = name;
        r.startMs = QDateTime::currentMSecsSinceEpoch();
        r.index = _count;
        _rows.insert(name, r);
        ++_count;
        if (_totalFromState == 0) _progressBar->setTotalSteps(_count);
        _progressBar->setRunningIndex(_count - 1);
        _progressBar->setCompletedSteps(_completedCount);
        rebuildStepIndicators();
        relabel();
    }

    void resolveStep(const QString& name, const QString& resultJson) {
        auto it = _rows.find(name);
        if (it == _rows.end()) return;
        const qint64 elapsedMs = QDateTime::currentMSecsSinceEpoch() - it->startMs;
        it->dot->setText(QString::fromUtf8("✓"));                     // ✓ done
        it->dot->setStyleSheet(QString("color:%1;font-weight:bold;").arg(AgentTheme::currentOkGreen()));
        QString cur = it->detail->text();
        it->detail->setText(cur + "<br><b>结果:</b> " + prettySummary(resultJson)
            + QString("<br><span style='color:%1'>耗时 %2 ms</span>")
                  .arg(AgentTheme::currentTextSecondary()).arg(elapsedMs));
        ++_completedCount;
        _progressBar->setCompletedSteps(_completedCount);
        _progressBar->setRunningIndex(nextRunningIndex());
        rebuildStepIndicators();
    }

    void failStep(const QString& name, const QString& reason) {   // ✗ failed
        auto it = _rows.find(name);
        if (it == _rows.end()) return;
        it->dot->setText(QString::fromUtf8("✗"));
        it->dot->setStyleSheet(QString("color:%1;font-weight:bold;").arg(AgentTheme::currentBadRed()));
        const QString text = reason.isEmpty()
            ? QString::fromUtf8("工具返回 ok=false") : reason.toHtmlEscaped();
        it->detail->setText(it->detail->text()
            + QString("<br><b style='color:%1'>失败:</b> ").arg(AgentTheme::currentBadRed()) + text);
        _failedIndices.insert(it->index);
        _progressBar->setFailedIndices(_failedIndices);
        _progressBar->setRunningIndex(nextRunningIndex());
        rebuildStepIndicators();
    }

    void finish(bool allOk = true) {   // settle still-running rows at turn end
        for (auto& r : _rows) if (r.dot->text() != QString::fromUtf8("✓")
                               && r.dot->text() != QString::fromUtf8("✗")) {
            if (!allOk && !_failedIndices.contains(r.index)) {
                _failedIndices.insert(r.index);
                _progressBar->setFailedIndices(_failedIndices);
            }
            r.dot->setText(allOk ? QString::fromUtf8("✓") : QString::fromUtf8("✗"));
            r.dot->setStyleSheet(QString("color:%1;font-weight:bold;")
                .arg(allOk ? AgentTheme::currentOkGreen() : AgentTheme::currentBadRed()));
            ++_completedCount;
        }
        _progressBar->setCompletedSteps(_completedCount);
        _progressBar->setFinished();
        _elapsedTimer->stop();
        updateElapsed();
        rebuildStepIndicators();
    }

    void setState(const QString& stateJson) {
        const QJsonObject state = QJsonDocument::fromJson(stateJson.toUtf8()).object();
        const QString value = state.value("state").toString();
        static const QHash<QString, QString> labels{
            {"context_ready", QString::fromUtf8("上下文就绪")},
            {"analysis_pending", QString::fromUtf8("分析中")},
            {"analysis_complete", QString::fromUtf8("分析完成")},
            {"diagnosis_complete", QString::fromUtf8("诊断完成")},
            {"report_pending", QString::fromUtf8("报告生成中")},
            {"complete", QString::fromUtf8("已完成")},
            {"failed", QString::fromUtf8("失败，可重试")},
            {"cancelled", QString::fromUtf8("已取消")}
        };
        _stateLabel = labels.value(value, value);
        const int totalFromJson = state.value("total_steps").toInt(0);
        if (totalFromJson > 0) {
            _totalFromState = totalFromJson;
            _progressBar->setTotalSteps(totalFromJson);
        }
        relabel();
    }

private:
    struct Row { QLabel* dot; QLabel* detail; QString name; qint64 startMs = 0; int index = -1; };
    QToolButton* _head;
    ProgressTimelineBar* _progressBar;
    QWidget* _stepIndicators;
    QLabel* _elapsedLabel;
    QWidget* _stepsHost; QVBoxLayout* _stepsLay;
    QHash<QString, Row> _rows;
    int _count = 0;
    int _completedCount = 0;
    int _totalFromState = 0;
    QSet<int> _failedIndices;
    QString _stateLabel;
    qint64 _workflowStartMs = 0;
    QTimer* _elapsedTimer = nullptr;

    int nextRunningIndex() const {
        int best = -1;
        for (auto it = _rows.cbegin(); it != _rows.cend(); ++it) {
            if (it->dot->text() == QString::fromUtf8("⟳") && (best < 0 || it->index < best))
                best = it->index;
        }
        return best;
    }

    void rebuildStepIndicators() {
        QLayoutItem* item;
        while ((item = _stepIndicators->layout()->takeAt(0)) != nullptr) {
            if (item->widget()) item->widget()->deleteLater();
            delete item;
        }
        if (_count == 0) return;
        auto* h = qobject_cast<QHBoxLayout*>(_stepIndicators->layout());
        if (!h) { h = new QHBoxLayout(_stepIndicators); h->setContentsMargins(0,0,0,0); h->setSpacing(4); }

        QList<Row> sortedRows;
        for (auto it = _rows.cbegin(); it != _rows.cend(); ++it) sortedRows.append(*it);
        std::sort(sortedRows.begin(), sortedRows.end(),
            [](const Row& a, const Row& b) { return a.index < b.index; });

        for (const Row& r : sortedRows) {
            StepIndicator::State st = StepIndicator::Pending;
            if (_failedIndices.contains(r.index)) {
                st = StepIndicator::Failed;
            } else if (r.dot->text() == QString::fromUtf8("✓")) {
                st = StepIndicator::Completed;
            } else if (r.dot->text() == QString::fromUtf8("⟳")) {
                st = StepIndicator::Running;
            }
            h->addWidget(new StepIndicator(st, r.name, _stepIndicators));
        }
    }

    void updateElapsed() {
        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - _workflowStartMs;
        const int secs = static_cast<int>(elapsed / 1000);
        _elapsedLabel->setText(QString("%1:%2")
            .arg(secs / 60, 2, 10, QChar('0'))
            .arg(secs % 60, 2, 10, QChar('0')));
    }

    void relabel() {
        int total = _totalFromState > 0 ? _totalFromState : _count;
        QString text = QString::fromUtf8("⚙ 诊断工作流  ·  %1 步").arg(total);
        if (!_stateLabel.isEmpty()) text += QString::fromUtf8("  ·  ") + _stateLabel;
        _head->setText(text);
    }
    static QString prettyArgs(const QString& j) {
        QString a = j.trimmed(); if (a.isEmpty() || a == "{}") return QString::fromUtf8("<b>参数:</b> (无)");
        return QString::fromUtf8("<b>参数:</b> ") + a.toHtmlEscaped();
    }
    static QString prettySummary(const QString& resultJson) {
        // Pull "summary" out of the ToolResult JSON for a concise line; fall back to a short slice.
        int i = resultJson.indexOf("\"summary\"");
        if (i >= 0) { int c = resultJson.indexOf(':', i); int q1 = resultJson.indexOf('"', c+1);
            int q2 = resultJson.indexOf('"', q1+1); if (q1>0 && q2>q1) return resultJson.mid(q1+1, q2-q1-1).toHtmlEscaped(); }
        return resultJson.left(80).toHtmlEscaped();
    }
};

// ============================ AgentChatDock ============================
AgentChatDock::AgentChatDock(QWidget* parent) : QDockWidget(parent) {
    initWindow();
    setViewModel(CreateExtension(VM, "AgentChat"));
    setWindowTitle(QString::fromUtf8("AI 助手"));
}
AgentChatDock::~AgentChatDock() {
    QSettings settings("DPA", "DPA-2026");
    settings.beginGroup("AgentChatDock");
    settings.setValue("geometry", saveGeometry());
    settings.setValue("floating", isFloating());
    settings.endGroup();
}

void AgentChatDock::showEvent(QShowEvent* event) {
    QDockWidget::showEvent(event);
    QTimer::singleShot(0, this, [this]() {
        auto* mainWindow = qobject_cast<QMainWindow*>(window());
        QWidget* ancestor = parentWidget();
        while (!mainWindow && ancestor) {
            mainWindow = qobject_cast<QMainWindow*>(ancestor);
            ancestor = ancestor->parentWidget();
        }
        if (!mainWindow) {
            for (QWidget* topLevel : QApplication::topLevelWidgets()) {
                if ((mainWindow = qobject_cast<QMainWindow*>(topLevel))) break;
            }
        }
        if (!mainWindow) return;
        if (!isFloating()) mainWindow->resizeDocks({this}, {380}, Qt::Horizontal);
        QTimer::singleShot(0, this, [this, mainWindow]() {
            const QScreen* screen = mainWindow->screen();
            const QRect frame = screen ? screen->availableGeometry() : mainWindow->frameGeometry();
            const QRect dockFrame = isFloating()
                ? frameGeometry()
                : QRect(mapToGlobal(QPoint(0, 0)), size());
            if (dockFrame.intersected(frame).width() >= 320
                && dockFrame.width() >= 380 && dockFrame.height() >= 420) return;
            if (!isFloating()) setFloating(true);
            resize(420, std::max(420, std::min(760, frame.height() - 80)));
            move(frame.right() - width() - 24, frame.top() + 56);
            raise();
            activateWindow();
        });
    });
}

void AgentChatDock::initWindow() {
    auto* central = new QWidget(this);
    central->setStyleSheet(QString("QWidget{background:%2;} QLabel{color:%1;}").arg(AgentTheme::currentTextPrimary(), AgentTheme::currentSurfaceBg()));
    auto* root = new QVBoxLayout(central); root->setContentsMargins(0,0,0,0); root->setSpacing(0);

    // ---- header ----
    auto* header = new QWidget(central);
    header->setStyleSheet(QString("background:%2;border-bottom:1px solid %1;").arg(AgentTheme::currentBorderDefault(), AgentTheme::currentSurfaceElevated()));
    auto* hh = new QHBoxLayout(header); hh->setContentsMargins(11,8,10,8); hh->setSpacing(8);
    auto* logo = new QLabel(QString::fromUtf8("◎"), header);
    logo->setStyleSheet(QString("background:%1;color:%2;border-radius:7px;font-size:14px;").arg(AgentTheme::currentAccentPrimary(), AgentTheme::currentTextOnAccent()));
    logo->setFixedSize(26,26); logo->setAlignment(Qt::AlignCenter);
    logo->setAccessibleName(QString::fromUtf8("AI助手图标"));
    auto* title = new QLabel(QString::fromUtf8("AI 助手"), header);
    title->setStyleSheet("font-weight:bold;font-size:14px;");
    title->setAccessibleName(QString::fromUtf8("AI助手标题"));
    _statusDot = new QLabel(QString::fromUtf8("● 就绪"), header);
    _statusDot->setStyleSheet(QString("color:%1;font-family:Consolas;font-size:11px;").arg(AgentTheme::currentOkGreen()));
    _statusDot->setAccessibleName(QString::fromUtf8("连接状态"));
    _providerBadge = new QLabel(header);
    _providerBadge->setVisible(false);
    _providerBadge->setCursor(Qt::PointingHandCursor);
    _providerBadge->setAccessibleName(QString::fromUtf8("模型提供商状态"));
    _newSessionBtn = new QToolButton(header); _newSessionBtn->setText(QString::fromUtf8("✎"));
    _newSessionBtn->setToolTip(QString::fromUtf8("新会话 (Ctrl+N)")); _newSessionBtn->setCursor(Qt::PointingHandCursor);
    _newSessionBtn->setAccessibleName(QString::fromUtf8("新建会话"));
    _newSessionBtn->setStyleSheet(QString("QToolButton{border:1px solid %1;border-radius:6px;padding:2px 6px;color:%2;}").arg(AgentTheme::currentBorderDefault()).arg(AgentTheme::currentTextSecondary()));
    connect(_newSessionBtn, &QToolButton::clicked, this, &AgentChatDock::onNewSession);
    _historyBtn = new QToolButton(header); _historyBtn->setText(QString::fromUtf8("◷"));
    _historyBtn->setToolTip(QString::fromUtf8("恢复本地会话")); _historyBtn->setCursor(Qt::PointingHandCursor);
    _runtimeBtn = new QToolButton(header); _runtimeBtn->setText(QString::fromUtf8("ⓘ"));
    _runtimeBtn->setToolTip(QString::fromUtf8("运行配置（不显示密钥）")); _runtimeBtn->setCursor(Qt::PointingHandCursor);
    _themeBtn = new QToolButton(header); _themeBtn->setText(QString::fromUtf8("◐"));
    _themeBtn->setToolTip(QString::fromUtf8("主题 (深浅色切换)")); _themeBtn->setCursor(Qt::PointingHandCursor);
    for (QToolButton* button : {_historyBtn, _runtimeBtn, _themeBtn})
        button->setStyleSheet(QString("QToolButton{border:1px solid %1;border-radius:6px;padding:2px 6px;color:%2;}").arg(AgentTheme::currentBorderDefault()).arg(AgentTheme::currentTextSecondary()));
    connect(_historyBtn, &QToolButton::clicked, this, &AgentChatDock::showSessionMenu);
    connect(_runtimeBtn, &QToolButton::clicked, this, &AgentChatDock::showRuntimeInfo);
    connect(_themeBtn, &QToolButton::clicked, this, [this]() {
        AgentTheme::toggleDarkMode();
        update();
        QMessageBox::information(this, QString::fromUtf8("主题"),
            QString::fromUtf8("主题已切换。新卡片将使用新主题，已存在的卡片需要重启才能完全生效。"));
    });
    hh->addWidget(logo); hh->addWidget(title); hh->addStretch(); hh->addWidget(_statusDot);
    hh->addWidget(_providerBadge);
    hh->addWidget(_runtimeBtn); hh->addWidget(_themeBtn); hh->addWidget(_historyBtn); hh->addWidget(_newSessionBtn);
    root->addWidget(header);

    // ---- deterministic experiment companion ----
    auto* companion = new QWidget(central);
    companion->setStyleSheet(QString("background:%2;border-bottom:1px solid %1;").arg(AgentTheme::currentBorderDefault(), AgentTheme::currentSurfaceCard()));
    auto* ch = new QHBoxLayout(companion); ch->setContentsMargins(11,6,10,6); ch->setSpacing(7);
    _companionToggle = new QToolButton(companion);
    _companionToggle->setText(QString::fromUtf8("◉ 智能陪试"));
    _companionToggle->setCheckable(true);
    _companionToggle->setCursor(Qt::PointingHandCursor);
    _companionToggle->setStyleSheet(QString(
        "QToolButton{border:1px solid %1;border-radius:9px;padding:4px 9px;color:%2;background:%4;}"
        "QToolButton:checked{border-color:%3;color:%3;background:%5;font-weight:bold;}")
        .arg(AgentTheme::currentBorderDefault()).arg(AgentTheme::currentTextSecondary()).arg(AgentTheme::currentAccentPrimary())
        .arg(AgentTheme::currentSurfaceCard(), AgentTheme::currentAccentSubtle()));
    _companionState = new QLabel(QString::fromUtf8("已关闭"), companion);
    _companionState->setStyleSheet(QString("color:%1;font-size:11px;").arg(AgentTheme::currentTextSecondary()));
    _healthBtn = new QToolButton(companion);
    _healthBtn->setText(QString::fromUtf8("预检"));
    _healthBtn->setToolTip(QString::fromUtf8("查看试验准备度与健康状态"));
    _healthBtn->setCursor(Qt::PointingHandCursor);
    _healthBtn->setStyleSheet(QString(
        "QToolButton{border:1px solid %1;border-radius:7px;padding:3px 7px;color:%2;background:%3;}")
        .arg(AgentTheme::currentBorderDefault()).arg(AgentTheme::currentTextSecondary()).arg(AgentTheme::currentSurfaceCard()));
    _evidenceBtn = new QToolButton(companion);
    _evidenceBtn->setText(QString::fromUtf8("证据包"));
    _evidenceBtn->setToolTip(QString::fromUtf8("生成隐私最小化、可校验的当前试验证据包（执行前会确认）"));
    _evidenceBtn->setCursor(Qt::PointingHandCursor);
    _evidenceBtn->setStyleSheet(QString(
        "QToolButton{border:1px solid %1;border-radius:7px;padding:3px 7px;color:%2;background:%3;}")
        .arg(AgentTheme::currentBorderDefault()).arg(AgentTheme::currentTextSecondary()).arg(AgentTheme::currentSurfaceCard()));
    _companionSettings = new QToolButton(companion);
    _companionSettings->setText(QString::fromUtf8("⚙"));
    _companionSettings->setToolTip(QString::fromUtf8("陪试参数"));
    _companionSettings->setCursor(Qt::PointingHandCursor);
    _companionSettings->setStyleSheet(QString(
        "QToolButton{border:1px solid %1;border-radius:7px;padding:3px 7px;color:%2;background:%3;}")
        .arg(AgentTheme::currentBorderDefault()).arg(AgentTheme::currentTextSecondary()).arg(AgentTheme::currentSurfaceCard()));
    ch->addWidget(_companionToggle); ch->addWidget(_companionState); ch->addStretch();
    ch->addWidget(_healthBtn);
    ch->addWidget(_evidenceBtn);
    ch->addWidget(_companionSettings);
    root->addWidget(companion);
    connect(_companionToggle, &QToolButton::toggled, this, [this](bool on) {
        if (!_vm) return;
        _vm->setValue("CompanionEnabledRequest", on);
        if (!_vm->excute("SetCompanionEnabled"))
            updateCompanionStatus(QString::fromStdString(
                _vm->value<std::string>("CompanionStatusJson")));
    });
    connect(_companionSettings, &QToolButton::clicked,
            this, &AgentChatDock::showCompanionSettings);
    connect(_healthBtn, &QToolButton::clicked,
            this, &AgentChatDock::showExperimentHealth);
    connect(_evidenceBtn, &QToolButton::clicked, this, [this]() {
        if (_running || _analysisPending || !_input) return;
        _input->setPlainText(QString::fromUtf8(
            "请生成当前试验的隐私最小化证据包，完成后告诉我 Artifact ID、SHA-256、事件数量和仿真配方状态。"));
        sendCurrent();
    });

    // ---- experiment campaign panel ----
    _campaignToggle = new QToolButton(central);
    _campaignToggle->setText(QString::fromUtf8("▶ 试验战役"));
    _campaignToggle->setCheckable(true);
    _campaignToggle->setChecked(false);
    _campaignToggle->setCursor(Qt::PointingHandCursor);
    _campaignToggle->setStyleSheet(QString(
        "QToolButton{border:none;border-bottom:1px solid %1;padding:6px 11px;"
        "color:%2;background:%3;font-size:12px;text-align:left;}")
        .arg(AgentTheme::currentBorderDefault()).arg(AgentTheme::currentTextSecondary()).arg(AgentTheme::currentSurfaceElevated()));
    _campaignPanel = new CampaignPanel(central);
    _campaignPanel->setVisible(false);
    root->addWidget(_campaignToggle);
    root->addWidget(_campaignPanel);
    connect(_campaignToggle, &QToolButton::toggled, this, [this](bool on) {
        _campaignToggle->setText(on ? QString::fromUtf8("▾ 试验战役")
                                    : QString::fromUtf8("▶ 试验战役"));
        _campaignPanel->setVisible(on);
        if (on) _campaignPanel->refreshFromService();
    });

    // ---- message stream ----
    _scroll = new QScrollArea(central); _scroll->setWidgetResizable(true);
    _scroll->setFrameShape(QFrame::NoFrame);
    _scroll->setStyleSheet(QString("QScrollArea{background:%1;}").arg(AgentTheme::currentSurfaceBg()));
    _host = new QWidget(); _host->setStyleSheet(QString("background:%1;").arg(AgentTheme::currentSurfaceBg()));
    _stream = new QVBoxLayout(_host); _stream->setContentsMargins(12,12,12,12); _stream->setSpacing(10);
    _stream->addStretch();
    _scroll->setWidget(_host);
    root->addWidget(_scroll, 1);

    // ---- quick actions ----
    _quickActions = new QWidget(central);
    auto* qh = new QHBoxLayout(_quickActions); qh->setContentsMargins(12,8,12,0); qh->setSpacing(6);
    const char* chips[] = {"诊断当前结果", "解读这张图", "存为基线", "生成报告"};
    for (const char* c : chips) {
        auto* chip = new QToolButton(_quickActions); chip->setText(QString::fromUtf8(c)); chip->setCursor(Qt::PointingHandCursor);
        chip->setStyleSheet(AgentTheme::chipButtonStyle());
        QString text = QString::fromUtf8(c);
        connect(chip, &QToolButton::clicked, this, [this, text]() { if (_input) { _input->setPlainText(text); sendCurrent(); } });
        qh->addWidget(chip);
    }
    qh->addStretch();
    root->addWidget(_quickActions);

    // ---- attachment label + input row ----
    _attachmentLabel = new QLabel(central); _attachmentLabel->setVisible(false);
    _attachmentLabel->setStyleSheet(QString("color:%1;font-size:11px;padding:2px 12px;").arg(AgentTheme::currentTextSecondary()));
    root->addWidget(_attachmentLabel);

    auto* inbar = new QWidget(central);
    inbar->setStyleSheet(QString("background:%2;border-top:1px solid %1;").arg(AgentTheme::currentBorderDefault(), AgentTheme::currentSurfaceElevated()));
    auto* ih = new QHBoxLayout(inbar); ih->setContentsMargins(10,9,10,9); ih->setSpacing(7);
    _attachBtn = new QPushButton(QString::fromUtf8("▤"), inbar); _attachBtn->setToolTip(QString::fromUtf8("附件"));
    _attachBtn->setFixedSize(34,34); _attachBtn->setCursor(Qt::PointingHandCursor);
    _attachBtn->setAccessibleName(QString::fromUtf8("添加附件"));
    _attachBtn->setStyleSheet(QString("QPushButton{border:1px solid %1;border-radius:9px;background:%3;color:%2;font-size:15px;}").arg(AgentTheme::currentBorderDefault()).arg(AgentTheme::currentTextSecondary()).arg(AgentTheme::currentSurfaceCard()));
    _input = new QTextEdit(inbar);     _input->setPlaceholderText(QString::fromUtf8("问点什么，或拖入一张谱图… (Ctrl+Enter 发送)"));
    _input->setFixedHeight(40); _input->setStyleSheet(QString("QTextEdit{border:1px solid %1;border-radius:10px;padding:7px 9px;background:%2;font-size:13px;}").arg(AgentTheme::currentBorderDefault(), AgentTheme::currentSurfaceCard()));
    _input->setAccessibleName(QString::fromUtf8("消息输入框"));
    _input->setAcceptDrops(true);
    _input->installEventFilter(this);
    _stopBtn = new QPushButton(QString::fromUtf8("■"), inbar); _stopBtn->setToolTip(QString::fromUtf8("停止 (Esc)"));
    _stopBtn->setFixedSize(34,34); _stopBtn->setVisible(false); _stopBtn->setCursor(Qt::PointingHandCursor);
    _stopBtn->setAccessibleName(QString::fromUtf8("停止生成"));
    _stopBtn->setStyleSheet(QString("QPushButton{border:1px solid %1;border-radius:9px;background:%2;color:%1;}").arg(AgentTheme::currentSevCritical(), AgentTheme::currentBadSubtle()));
    connect(_stopBtn, &QPushButton::clicked, this, [this]() { if (_vm) _vm->excute("Cancel"); });
    _sendBtn = new QPushButton(QString::fromUtf8("➤"), inbar); _sendBtn->setToolTip(QString::fromUtf8("发送 (Ctrl+Enter)"));
    _sendBtn->setFixedSize(34,34); _sendBtn->setCursor(Qt::PointingHandCursor);
    _sendBtn->setAccessibleName(QString::fromUtf8("发送消息"));
    _sendBtn->setStyleSheet(QString("QPushButton{border:1px solid %1;border-radius:9px;background:%1;color:%2;font-size:14px;}").arg(AgentTheme::currentAccentPrimary(), AgentTheme::currentTextOnAccent()));
    connect(_sendBtn, &QPushButton::clicked, this, [this]() { sendCurrent(); });
    ih->addWidget(_attachBtn); ih->addWidget(_input, 1); ih->addWidget(_stopBtn); ih->addWidget(_sendBtn);
    root->addWidget(inbar);

    connect(_attachBtn, &QPushButton::clicked, this, [this]() {
        if (!_vm) return;
        QStringList files = QFileDialog::getOpenFileNames(this, QString::fromUtf8("选择附件"));
        if (files.isEmpty()) return;
        _vm->setValue("Attachments", files.join("|").toStdString());
        updateAttachmentLabel();
    });

    central->setLayout(root);
    setWidget(central);
    setMinimumWidth(340);
    setAccessibleName(QString::fromUtf8("AI助手面板"));

    // ---- keyboard shortcuts ----
    auto* shortcutNew = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_N), this);
    connect(shortcutNew, &QShortcut::activated, this, &AgentChatDock::onNewSession);
    auto* shortcutStop = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(shortcutStop, &QShortcut::activated, this, [this]() {
        if (_running && _vm) _vm->excute("Cancel");
        else if (_searchBar && _searchBar->isVisible()) toggleSearchBar();
    });

    // ---- Ctrl+F search bar ----
    _searchBar = new QWidget(central);
    _searchBar->setStyleSheet(QString("background:%2;border-bottom:1px solid %1;")
        .arg(AgentTheme::currentBorderDefault(), AgentTheme::currentSurfaceElevated()));
    _searchBar->setVisible(false);
    auto* sh = new QHBoxLayout(_searchBar); sh->setContentsMargins(12,6,12,6); sh->setSpacing(6);
    auto* searchIcon = new QLabel(QString::fromUtf8("🔍"), _searchBar);
    searchIcon->setStyleSheet("font-size:13px;");
    sh->addWidget(searchIcon);
    _searchInput = new QLineEdit(_searchBar);
    _searchInput->setPlaceholderText(QString::fromUtf8("搜索对话…"));
    _searchInput->setStyleSheet(QString(
        "QLineEdit{border:1px solid %1;border-radius:8px;padding:5px 8px;"
        "background:%2;color:%3;font-size:12px;}")
        .arg(AgentTheme::currentBorderDefault(), AgentTheme::currentSurfaceCard(), AgentTheme::currentTextPrimary()));
    _searchInput->setClearButtonEnabled(true);
    sh->addWidget(_searchInput, 1);
    _searchCountLabel = new QLabel(QString::fromUtf8("0/0"), _searchBar);
    _searchCountLabel->setStyleSheet(QString("color:%1;font-family:Consolas;font-size:11px;")
        .arg(AgentTheme::currentTextSecondary()));
    _searchCountLabel->setFixedWidth(50);
    _searchCountLabel->setAlignment(Qt::AlignCenter);
    sh->addWidget(_searchCountLabel);
    auto* searchPrevBtn = new QToolButton(_searchBar);
    searchPrevBtn->setText(QString::fromUtf8("▲"));
    searchPrevBtn->setCursor(Qt::PointingHandCursor);
    searchPrevBtn->setStyleSheet(QString("QToolButton{border:1px solid %1;border-radius:6px;padding:2px 6px;color:%2;background:%3;}")
        .arg(AgentTheme::currentBorderDefault(), AgentTheme::currentTextSecondary(), AgentTheme::currentSurfaceCard()));
    sh->addWidget(searchPrevBtn);
    auto* searchNextBtn = new QToolButton(_searchBar);
    searchNextBtn->setText(QString::fromUtf8("▼"));
    searchNextBtn->setCursor(Qt::PointingHandCursor);
    searchNextBtn->setStyleSheet(QString("QToolButton{border:1px solid %1;border-radius:6px;padding:2px 6px;color:%2;background:%3;}")
        .arg(AgentTheme::currentBorderDefault(), AgentTheme::currentTextSecondary(), AgentTheme::currentSurfaceCard()));
    sh->addWidget(searchNextBtn);
    auto* searchCloseBtn = new QToolButton(_searchBar);
    searchCloseBtn->setText(QString::fromUtf8("✕"));
    searchCloseBtn->setCursor(Qt::PointingHandCursor);
    searchCloseBtn->setStyleSheet(QString("QToolButton{border:1px solid %1;border-radius:6px;padding:2px 6px;color:%2;background:%3;}")
        .arg(AgentTheme::currentBorderDefault(), AgentTheme::currentTextSecondary(), AgentTheme::currentSurfaceCard()));
    sh->addWidget(searchCloseBtn);
    // insert search bar at index 2 (after header=0, companion=1)
    root->insertWidget(2, _searchBar);

    // search logic: a helper lambda that reads all text from _scroll's content
    // and highlights/finds matches inside the QTextEdit-based message labels.
    // Since messages are QLabel (not QTextEdit), we use a different approach:
    // we iterate QLabel children in the stream, find ones containing the text,
    // scroll to the match, and count results.
    auto doSearch = [this](bool forward) {
        if (!_searchInput || !_scroll) return;
        const QString query = _searchInput->text().trimmed();
        if (query.isEmpty()) { _searchCountLabel->setText(QString::fromUtf8("0/0")); return; }

        // Collect all QLabel widgets in the stream
        QList<QLabel*> labels;
        for (int i = 0; i < _stream->count(); ++i) {
            QLayoutItem* item = _stream->itemAt(i);
            if (!item || !item->widget()) continue;
            // Check the widget and its children for QLabels
            auto findLabels = [&](QWidget* w) {
                for (QLabel* lbl : w->findChildren<QLabel*>()) {
                    if (lbl->textFormat() == Qt::RichText || lbl->text().contains(query, Qt::CaseInsensitive))
                        labels << lbl;
                }
            };
            findLabels(item->widget());
        }

        _searchTotal = 0;
        int nextIdx = -1;
        QString plainQuery = query.toHtmlEscaped();

        for (QLabel* lbl : labels) {
            QString rawText = lbl->text();
            // strip HTML tags for matching
            QString plainText = rawText;
            plainText.remove(QRegularExpression(QString::fromUtf8("<[^>]*>")));
            if (plainText.isEmpty()) continue;

            int pos = 0;
            while ((pos = plainText.indexOf(query, pos, Qt::CaseInsensitive)) != -1) {
                ++_searchTotal;
                if (forward && nextIdx == -1) nextIdx = _searchTotal;
                ++pos;
            }
        }

        if (_searchTotal == 0) { _searchCountLabel->setText(QString::fromUtf8("0/0")); return; }
        if (_searchCurrent < 1 || _searchCurrent > _searchTotal) _searchCurrent = forward ? 1 : _searchTotal;

        _searchCountLabel->setText(QString::fromUtf8("%1/%2").arg(_searchCurrent).arg(_searchTotal));

        // scroll to the matching label
        int matchIdx = 0;
        for (QLabel* lbl : labels) {
            QString rawText = lbl->text();
            QString plainText = rawText;
            plainText.remove(QRegularExpression(QString::fromUtf8("<[^>]*>")));
            if (plainText.isEmpty()) continue;
            int pos = 0;
            while ((pos = plainText.indexOf(query, pos, Qt::CaseInsensitive)) != -1) {
                ++matchIdx;
                if (matchIdx == _searchCurrent) {
                    // scroll the label into view
                    _scroll->ensureWidgetVisible(lbl);
                    return;
                }
                ++pos;
            }
        }
    };

    connect(_searchInput, &QLineEdit::textChanged, this, [this, doSearch]() {
        _searchCurrent = 1;
        doSearch(true);
    });
    connect(_searchInput, &QLineEdit::returnPressed, this, [this, doSearch]() {
        _searchCurrent = qMin(_searchCurrent + 1, _searchTotal);
        doSearch(true);
    });
    connect(searchNextBtn, &QToolButton::clicked, this, [this, doSearch]() {
        _searchCurrent = qMin(_searchCurrent + 1, _searchTotal);
        doSearch(true);
    });
    connect(searchPrevBtn, &QToolButton::clicked, this, [this, doSearch]() {
        _searchCurrent = qMax(_searchCurrent - 1, 1);
        doSearch(false);
    });
    connect(searchCloseBtn, &QToolButton::clicked, this, &AgentChatDock::toggleSearchBar);

    auto* shortcutSearch = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_F), this);
    connect(shortcutSearch, &QShortcut::activated, this, &AgentChatDock::toggleSearchBar);

    // ---- right-click context menu on scroll area ----
    _scroll->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(_scroll, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QMenu menu(this);
        menu.setStyleSheet(QString("QMenu{background:%3;border:1px solid %1;border-radius:6px;padding:4px;}QMenu::item{padding:6px 20px;border-radius:4px;}QMenu::item:selected{background:%2;}").arg(AgentTheme::currentBorderDefault()).arg(AgentTheme::currentAccentSubtle()).arg(AgentTheme::currentSurfaceCard()));
        auto* copyAll = menu.addAction(QString::fromUtf8("复制全部对话"));
        auto* exportChat = menu.addAction(QString::fromUtf8("导出对话为文本"));
        menu.addSeparator();
        auto* selectAll = menu.addAction(QString::fromUtf8("全选"));
        QAction* chosen = menu.exec(_scroll->mapToGlobal(pos));
        if (!chosen) return;
        if (chosen == copyAll) {
            QString text;
            for (int i = 0; i < _stream->count(); ++i) {
                if (auto* lbl = qobject_cast<QLabel*>(_stream->itemAt(i)->widget())) {
                    QString t = lbl->text().trimmed();
                    if (!t.isEmpty()) text += t + "\n\n";
                }
            }
            QApplication::clipboard()->setText(text);
        } else if (chosen == exportChat) {
            QString fileName = QFileDialog::getSaveFileName(this, QString::fromUtf8("导出对话"),
                QString::fromUtf8("对话记录.txt"), QString::fromUtf8("Text (*.txt)"));
            if (fileName.isEmpty()) return;
            QFile file(fileName);
            if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
                for (int i = 0; i < _stream->count(); ++i) {
                    if (auto* lbl = qobject_cast<QLabel*>(_stream->itemAt(i)->widget())) {
                        QString t = lbl->text().trimmed();
                        if (!t.isEmpty()) file.write(t.toUtf8() + "\n\n");
                    }
                }
                file.close();
            }
        } else if (chosen == selectAll) {
            _input->selectAll();
        }
    });

    // ---- layout persistence ----
    QSettings settings("DPA", "DPA-2026");
    settings.beginGroup("AgentChatDock");
    if (settings.contains("geometry")) restoreGeometry(settings.value("geometry").toByteArray());
    if (settings.contains("floating")) setFloating(settings.value("floating").toBool());
    settings.endGroup();
}

bool AgentChatDock::eventFilter(QObject* obj, QEvent* ev) {
    // Drag-and-drop on input: accept image files
    if (obj == _input) {
        if (ev->type() == QEvent::DragEnter) {
            auto* de = static_cast<QDragEnterEvent*>(ev);
            if (de->mimeData()->hasUrls()) {
                for (const QUrl& url : de->mimeData()->urls()) {
                    QString path = url.toLocalFile();
                    QString suffix = QFileInfo(path).suffix().toLower();
                    if (suffix == "png" || suffix == "jpg" || suffix == "jpeg"
                        || suffix == "bmp" || suffix == "tiff" || suffix == "tif") {
                        de->acceptProposedAction();
                        return true;
                    }
                }
            }
            return false;
        }
        if (ev->type() == QEvent::Drop) {
            auto* de = static_cast<QDropEvent*>(ev);
            QStringList imagePaths;
            for (const QUrl& url : de->mimeData()->urls()) {
                QString path = url.toLocalFile();
                QString suffix = QFileInfo(path).suffix().toLower();
                if (suffix == "png" || suffix == "jpg" || suffix == "jpeg"
                    || suffix == "bmp" || suffix == "tiff" || suffix == "tif")
                    imagePaths.append(path);
            }
            if (!imagePaths.isEmpty() && _vm) {
                QString existing = QString::fromStdString(_vm->value<std::string>("Attachments"));
                if (!existing.isEmpty()) imagePaths.prepend(existing);
                _vm->setValue("Attachments", imagePaths.join("|").toStdString());
                updateAttachmentLabel();
            }
            return true;
        }
    }
    // Keyboard events
    if (obj == _input && ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        // Ctrl+Enter or plain Enter (without Shift) sends the message
        if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter)
            && (ke->modifiers() & Qt::ControlModifier
                || !(ke->modifiers() & Qt::ShiftModifier))) {
            sendCurrent();
            return true;
        }
        // Esc stops generation or clears input
        if (ke->key() == Qt::Key_Escape) {
            if (_running && _vm) _vm->excute("Cancel");
            else if (_input) _input->clear();
            return true;
        }
    }
    // Esc on the dock itself stops generation
    if (obj == this && ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == Qt::Key_Escape && _running && _vm) {
            _vm->excute("Cancel");
            return true;
        }
    }
    return QDockWidget::eventFilter(obj, ev);
}

void AgentChatDock::appendWidget(QWidget* w) {
    // insert before the trailing stretch (last item)
    _stream->insertWidget(_stream->count() - 1, w);
    _prunable.push_back(w);
    pruneStream();
    autoScroll();
}
void AgentChatDock::pruneStream() {
    // Bound the widget graph: long sessions render hundreds of cards otherwise.
    // Keep the newest `kStreamWidgetCap` prunable widgets; never touch the live
    // workflow card or the pending confirmation card. deleteLater keeps the
    // removal safe from any in-flight signal emission.
    constexpr size_t kStreamWidgetCap = 90;
    while (_prunable.size() > kStreamWidgetCap) {
        QWidget* oldest = _prunable.front();
        _prunable.erase(_prunable.begin());
        if (!oldest) continue;
        if (oldest == _activeWorkflow || oldest == _activeConfirmCard) continue;
        _stream->removeWidget(oldest);
        oldest->deleteLater();
    }
}
void AgentChatDock::autoScroll() {
    QTimer::singleShot(0, this, [this]() { if (_scroll) _scroll->verticalScrollBar()->setValue(_scroll->verticalScrollBar()->maximum()); });
}

void AgentChatDock::addUserBubble(const QString& text, const QStringList& attachNames) {
    auto* row = new QWidget(_host); auto* h = new QHBoxLayout(row); h->setContentsMargins(0,0,0,0);
    auto* b = new QLabel(row); b->setWordWrap(true); b->setTextFormat(Qt::RichText);
    QString body = text.toHtmlEscaped();
    if (!attachNames.isEmpty()) body += QString::fromUtf8("<br><span style='font-size:11px'>▤ ") + attachNames.join(", ").toHtmlEscaped() + "</span>";
    b->setText(body);
    b->setStyleSheet(QString("background:%1;color:%2;border-radius:12px;border-bottom-right-radius:4px;padding:8px 11px;font-size:13px;").arg(AgentTheme::currentAccentPrimary(), AgentTheme::currentTextOnAccent()));
    b->setMaximumWidth(300);
    h->addStretch(); h->addWidget(b);
    appendWidget(row);
}
void AgentChatDock::addAiBubble(const QString& markdown) {
    auto* row = new QWidget(_host); auto* h = new QHBoxLayout(row); h->setContentsMargins(0,0,0,0);
    auto* b = new QLabel(row); b->setWordWrap(true); b->setTextFormat(Qt::RichText);
    b->setText(QString::fromStdString(Model::toRichText(markdown.toStdString())));
    b->setStyleSheet(QString("background:%1;color:%2;border:1px solid %3;border-radius:12px;border-bottom-left-radius:4px;padding:8px 11px;font-size:13px;").arg(AgentTheme::currentSurfaceHover()).arg(AgentTheme::currentTextPrimary()).arg(AgentTheme::currentBorderDefault()));
    b->setMaximumWidth(320);
    h->addWidget(b); h->addStretch();
    appendWidget(row);
}
void AgentChatDock::addDiagnosticCard(const QString& toolResultJson) {
    const QJsonObject root = QJsonDocument::fromJson(toolResultJson.toUtf8()).object();
    const QJsonObject data = root.value("data").toObject();
    const QJsonArray findingsArr = data.value("findings").toArray();
    if (findingsArr.isEmpty() && !root.value("ok").toBool(false)) return;

    const int assessedCount = data.value("assessed_count").toInt();
    const int totalFindings = findingsArr.size();
    const QString assessmentStatus = data.value("assessment_status").toString();
    const QString algorithmVer = data.value("algorithm_version").toString();
    const QString ruleVer = data.value("rule_version").toString();
    const QString standardProfile = data.value("standard_profile").toString();
    std::vector<std::string> evidenceRefs;
    for (const QJsonValue& ref : root.value("evidence_refs").toArray())
        evidenceRefs.push_back(ref.toString().toStdString());

    auto* card = new QFrame(_host);
    card->setStyleSheet(QString("QFrame{background:%2;border:1px solid %1;border-radius:11px;}").arg(AgentTheme::currentBorderDefault(), AgentTheme::currentSurfaceCard()));
    auto* v = new QVBoxLayout(card); v->setContentsMargins(0,0,0,0); v->setSpacing(0);

    // ---- header with assessment status ----
    QString statusIcon, statusColor;
    if (assessmentStatus == "assessed") { statusIcon = QString::fromUtf8("✓"); statusColor = AgentTheme::currentOkGreen(); }
    else if (assessmentStatus == "partially_assessed") { statusIcon = QString::fromUtf8("△"); statusColor = AgentTheme::currentWarnAmber(); }
    else if (assessmentStatus == "unassessed") { statusIcon = QString::fromUtf8("○"); statusColor = AgentTheme::currentTextSecondary(); }
    else { statusIcon = QString::fromUtf8("—"); statusColor = AgentTheme::currentTextSecondary(); }

    auto* headRow = new QWidget(card);
    auto* headH = new QHBoxLayout(headRow); headH->setContentsMargins(12,8,12,8); headH->setSpacing(8);
    auto* headIcon = new QLabel(statusIcon, headRow);
    headIcon->setStyleSheet(QString("color:%1;font-weight:bold;font-size:14px;").arg(statusColor));
    headIcon->setFixedWidth(18);
    auto* headTitle = new QLabel(QString::fromUtf8("诊断假设 · 基于当前频谱证据"), headRow);
    headTitle->setStyleSheet(QString("font-weight:bold;color:%1;").arg(AgentTheme::currentTextPrimary()));
    headH->addWidget(headIcon); headH->addWidget(headTitle, 1);

    // provenance chips
    if (!algorithmVer.isEmpty()) {
        auto* algoChip = new QLabel(algorithmVer, headRow);
        algoChip->setStyleSheet(QString("background:%1;color:%2;border-radius:6px;padding:1px 6px;font-family:Consolas;font-size:9px;").arg(AgentTheme::currentStepRunning(), AgentTheme::currentTextOnAccent()));
        headH->addWidget(algoChip);
    }
    if (!ruleVer.isEmpty()) {
        auto* ruleChip = new QLabel(ruleVer, headRow);
        ruleChip->setStyleSheet(QString("background:%1;color:%2;border-radius:6px;padding:1px 6px;font-family:Consolas;font-size:9px;").arg(AgentTheme::currentStepRunning(), AgentTheme::currentTextOnAccent()));
        headH->addWidget(ruleChip);
    }
    if (!standardProfile.isEmpty()) {
        auto* standardChip = new QLabel(standardProfile, headRow);
        standardChip->setStyleSheet(QString("background:%1;color:%2;border-radius:6px;padding:1px 6px;font-family:Consolas;font-size:9px;").arg(AgentTheme::currentNeutralChip(), AgentTheme::currentTextSecondary()));
        headH->addWidget(standardChip);
    }

    auto* headFrame = new QFrame(card);
    headFrame->setStyleSheet(QString("background:%2;border-top-left-radius:11px;border-top-right-radius:11px;border-bottom:1px solid %1;").arg(AgentTheme::currentBorderDefault(), AgentTheme::currentSurfaceElevated()));
    auto* headFrameLay = new QHBoxLayout(headFrame); headFrameLay->setContentsMargins(0,0,0,0); headFrameLay->setSpacing(0);
    headFrameLay->addWidget(headRow);
    v->addWidget(headFrame);

    // Show every non-assessed subdomain. A finding card without its limits is not an
    // expert assessment; it is an overconfident partial result.
    const QJsonObject scope = data.value("assessment_scope").toObject();
    QStringList scopeLimits;
    const struct { const char* key; const char* label; } scopeFields[] = {
        {"profile", "资产/测量档案"}, {"rotor_order", "转频阶次"},
        {"bearing_frequency", "轴承特征频率"}, {"resonance", "共振"},
        {"severity", "严重度标准"}
    };
    for (const auto& field : scopeFields) {
        const QJsonObject item = scope.value(QString::fromLatin1(field.key)).toObject();
        const QString status = item.value("status").toString();
        if (status.isEmpty() || status == QStringLiteral("assessed")) continue;
        const QString statusText = status == QStringLiteral("requires_additional_test")
            ? QString::fromUtf8("需补充试验")
            : status == QStringLiteral("not_requested") ? QString::fromUtf8("未请求")
                                                        : QString::fromUtf8("未评估");
        scopeLimits << QString::fromUtf8("%1：%2 — %3")
            .arg(QString::fromUtf8(field.label), statusText, item.value("reason").toString());
    }
    if (!scopeLimits.isEmpty()) {
        auto* scopeLabel = new QLabel(QString::fromUtf8("评估边界（不构成已评估结论）\n• ")
            + scopeLimits.join(QString::fromUtf8("\n• ")), card);
        scopeLabel->setWordWrap(true);
        scopeLabel->setStyleSheet(QString("background:%1;color:%2;border-bottom:1px solid %3;padding:7px 12px;font-size:10px;")
            .arg(AgentTheme::currentWarnSubtle(), AgentTheme::currentAmberDark(), AgentTheme::currentBorderDefault()));
        v->addWidget(scopeLabel);
    }

    if (findingsArr.isEmpty()) {
        auto* noFinding = new QLabel(
            QString::fromUtf8("当前规则未触发达到阈值的故障假设；这不等于设备健康认证。\n")
                + root.value("summary").toString(), card);
        noFinding->setWordWrap(true);
        noFinding->setStyleSheet(QString("color:%1;padding:10px 12px;font-size:11px;")
            .arg(AgentTheme::currentTextSecondary()));
        v->addWidget(noFinding);
        appendWidget(card);
        return;
    }

    // ---- findings ----
    for (int fi = 0; fi < findingsArr.size(); ++fi) {
        const QJsonObject fObj = findingsArr[fi].toObject();
        const QString fault = fObj.value("fault").toString();
        const QString sev = fObj.value("severity").toString();
        const double ruleScore = fObj.value("rule_match_score").toDouble();
        const bool assessed = fObj.value("assessed").toBool(true);
        const QString evidence = fObj.value("evidence").toString();
        const QJsonArray rootCausesArr = fObj.value("root_causes").toArray();
        const QJsonArray recsArr = fObj.value("recommendations").toArray();
        const QString ruleId = fObj.value("rule_id").toString();

        // severity color
        QString sevColor, sevBg;
        if (sev == "alarm") { sevColor = AgentTheme::currentBadRed(); sevBg = AgentTheme::currentBadSubtle(); }
        else if (sev == "warn") { sevColor = AgentTheme::currentWarnAmber(); sevBg = AgentTheme::currentWarnSubtle(); }
        else if (sev == "info") { sevColor = AgentTheme::currentStepRunning(); sevBg = AgentTheme::currentBlueSubtle(); }
        else { sevColor = AgentTheme::currentTextSecondary(); sevBg = AgentTheme::currentNeutralChip(); }

        auto* fw = new QWidget(card);
        auto* fv = new QVBoxLayout(fw); fv->setContentsMargins(12,9,12,9); fv->setSpacing(5);

        // row 1: fault + severity badge + rule match score
        auto* topRow = new QWidget(fw);
        auto* topH = new QHBoxLayout(topRow); topH->setContentsMargins(0,0,0,0); topH->setSpacing(6);
        auto* faultLabel = new QLabel(fault, topRow);
        faultLabel->setStyleSheet(QString("font-weight:bold;font-size:13px;color:%1;").arg(AgentTheme::currentTextPrimary()));
        faultLabel->setTextFormat(Qt::RichText);
        topH->addWidget(faultLabel);

        // severity heatmap badge
        auto* sevBadge = new QLabel(sev.toUpper(), topRow);
        sevBadge->setStyleSheet(QString(
            "background:%1;color:%2;border-radius:8px;padding:1px 7px;"
            "font-family:Consolas;font-size:10px;font-weight:bold;").arg(sevColor, AgentTheme::currentTextOnAccent()));
        topH->addWidget(sevBadge);

        // pulsing animation for critical
        if (sev == "alarm") {
            auto* pulseDot = new QLabel(QString::fromUtf8("●"), topRow);
            pulseDot->setStyleSheet(QString("color:%1;font-size:8px;").arg(AgentTheme::currentBadRed()));
            pulseDot->setToolTip(QString::fromUtf8("关键告警"));
            topH->addWidget(pulseDot);
        }

        topH->addStretch();

        // rule ID chip
        if (!ruleId.isEmpty()) {
            auto* ruleChip = new QLabel(ruleId, topRow);
            ruleChip->setStyleSheet(QString("background:%2;color:%1;border-radius:5px;padding:1px 5px;"
                "font-family:Consolas;font-size:9px;").arg(AgentTheme::currentTextSecondary(), AgentTheme::currentNeutralChip()));
            topH->addWidget(ruleChip);
        }

        fv->addWidget(topRow);

        // row 2: deterministic rule strength, never presented as probability/confidence
        const int matchPct = qBound(0, qRound(ruleScore * 100.0), 100);
        auto* confRow = new QWidget(fw);
        auto* confH = new QHBoxLayout(confRow); confH->setContentsMargins(0,0,0,0); confH->setSpacing(6);
        auto* confLabel = new QLabel(QString::fromUtf8("规则匹配"), confRow);
        confLabel->setStyleSheet(QString("color:%1;font-size:10px;").arg(AgentTheme::currentTextSecondary()));
        confLabel->setFixedWidth(50);
        confLabel->setToolTip(QString::fromUtf8("确定性规则匹配强度，不是概率或诊断置信度"));
        confH->addWidget(confLabel);

        auto* confTrack = new QFrame(confRow);
        confTrack->setFixedSize(120, 8);
        confTrack->setStyleSheet(QString("background:%2;border:1px solid %1;border-radius:4px;").arg(AgentTheme::currentBorderDefault(), AgentTheme::currentNeutralChip()));
        auto* confFill = new QFrame(confTrack);
        confFill->setStyleSheet(QString("background:%1;border-radius:3px;").arg(sevColor));
        confFill->setGeometry(1, 1, (120 - 2) * matchPct / 100, 6);
        confH->addWidget(confTrack);

        auto* confPct = new QLabel(QString::fromUtf8("%1 / 1.00").arg(ruleScore, 0, 'f', 2), confRow);
        confPct->setStyleSheet(QString("color:%1;font-family:Consolas;font-size:10px;font-weight:bold;").arg(sevColor));
        confH->addWidget(confPct);

        if (!assessed) {
            auto* naBadge = new QLabel(QString::fromUtf8("未评估"), confRow);
            naBadge->setStyleSheet(QString("background:%2;color:%1;border-radius:5px;padding:1px 5px;"
                "font-family:Consolas;font-size:9px;").arg(AgentTheme::currentTextSecondary(), AgentTheme::currentNeutralChip()));
            confH->addWidget(naBadge);
        }
        confH->addStretch();
        fv->addWidget(confRow);

        // row 3: literal rule evidence; no synthetic "evidence depth" score
        if (!evidence.isEmpty()) {
            auto* evText = new QLabel(QString::fromUtf8("规则证据：") + evidence, fw);
            evText->setWordWrap(true);
            evText->setStyleSheet(QString(
                "background:%1;color:%2;border-radius:6px;padding:4px 8px;"
                "font-family:Consolas;font-size:11px;").arg(AgentTheme::currentNeutralChip(), AgentTheme::currentTextPrimary()));
            fv->addWidget(evText);
        }

        // row 4: hypotheses and actions; these are not a proven causal chain
        if (!rootCausesArr.isEmpty() || !recsArr.isEmpty()) {
            auto* chainWidget = new QWidget(fw);
            auto* chainV = new QVBoxLayout(chainWidget); chainV->setContentsMargins(0,0,0,0); chainV->setSpacing(2);

            auto* chainTitle = new QLabel(QString::fromUtf8("候选原因与核查建议"), chainWidget);
            chainTitle->setStyleSheet(QString("color:%1;font-size:10px;font-weight:bold;").arg(AgentTheme::currentTextSecondary()));
            chainV->addWidget(chainTitle);
            if (!rootCausesArr.isEmpty()) {
                QStringList causes;
                for (const QJsonValue& value : rootCausesArr) causes << value.toString();
                auto* rcNode = new QLabel(QString::fromUtf8("可能原因：") + causes.join(QString::fromUtf8("；")), chainWidget);
                rcNode->setWordWrap(true);
                rcNode->setStyleSheet(QString(
                    "background:%2;color:%3;border:1px solid %1;border-radius:6px;"
                    "padding:3px 8px;font-size:10px;").arg(AgentTheme::currentWarnAmber(), AgentTheme::currentWarnSubtle(), AgentTheme::currentAmberDark()));
                chainV->addWidget(rcNode);
            }
            if (!recsArr.isEmpty()) {
                QStringList recommendations;
                for (const QJsonValue& value : recsArr) recommendations << value.toString();
                auto* recNode = new QLabel(QString::fromUtf8("核查/处置建议：") + recommendations.join(QString::fromUtf8("；")), chainWidget);
                recNode->setWordWrap(true);
                recNode->setStyleSheet(QString(
                    "background:%2;color:%3;border:1px solid %1;border-radius:6px;"
                    "padding:3px 8px;font-size:10px;").arg(AgentTheme::currentAccentPrimary(), AgentTheme::currentAccentSubtle(), AgentTheme::currentAccentText()));
                chainV->addWidget(recNode);
            }
            fv->addWidget(chainWidget);
        }

        // row 6: action buttons
        auto* btnRow = new QWidget(fw);
        auto* btnH = new QHBoxLayout(btnRow); btnH->setContentsMargins(0,2,0,0); btnH->setSpacing(6);

        auto* confirmBtn = new QPushButton(QString::fromUtf8("确认诊断"), btnRow);
        confirmBtn->setCursor(Qt::PointingHandCursor);
        confirmBtn->setStyleSheet(QString(
            "QPushButton{background:%1;color:%2;border:none;border-radius:7px;padding:4px 10px;font-size:11px;}"
            "QPushButton:hover{background:%3;}").arg(AgentTheme::currentAccentPrimary(), AgentTheme::currentTextOnAccent(), AgentTheme::currentAccentHover()));
        btnH->addWidget(confirmBtn);

        auto* falsePosBtn = new QPushButton(QString::fromUtf8("标记误报"), btnRow);
        falsePosBtn->setCursor(Qt::PointingHandCursor);
        falsePosBtn->setStyleSheet(QString(
            "QPushButton{background:%3;color:%1;border:1px solid %2;border-radius:7px;padding:4px 10px;font-size:11px;}"
            "QPushButton:hover{background:%4;}").arg(AgentTheme::currentTextSecondary(), AgentTheme::currentBorderDefault(), AgentTheme::currentSurfaceCard(), AgentTheme::currentSurfaceElevated()));
        btnH->addWidget(falsePosBtn);

        auto* moreInfoBtn = new QPushButton(QString::fromUtf8("需要更多信息"), btnRow);
        moreInfoBtn->setCursor(Qt::PointingHandCursor);
        moreInfoBtn->setStyleSheet(QString(
            "QPushButton{background:%3;color:%1;border:1px solid %2;border-radius:7px;padding:4px 10px;font-size:11px;}"
            "QPushButton:hover{background:%4;}").arg(AgentTheme::currentTextSecondary(), AgentTheme::currentBorderDefault(), AgentTheme::currentSurfaceCard(), AgentTheme::currentSurfaceElevated()));
        btnH->addWidget(moreInfoBtn);
        if (!assessed) {
            confirmBtn->setVisible(false);
            falsePosBtn->setVisible(false);
        }
        btnH->addStretch();
        fv->addWidget(btnRow);

        // Submit the service's real contract and only show success after it accepts the write.
        const auto submitFeedback = [this, fault, sev, ruleId, evidenceRefs,
                                     confirmBtn, falsePosBtn, moreInfoBtn]
            (const QString& type, QPushButton* selected, const QString& successText) {
            if (!_vm) {
                selected->setText(QString::fromUtf8("记录失败"));
                selected->setToolTip(QString::fromUtf8("反馈服务不可用"));
                return;
            }
            const std::string payload = buildDiagnosticFeedbackJson(
                type.toStdString(), fault.toStdString(), sev.toStdString(),
                ruleId.toStdString(), evidenceRefs);
            _vm->setValue("OperatorFeedbackJson", payload);
            if (!_vm->excute("RecordOperatorFeedback")) {
                const QJsonObject result = QJsonDocument::fromJson(QByteArray::fromStdString(
                    _vm->value<std::string>("OperatorFeedbackResult"))).object();
                selected->setText(QString::fromUtf8("记录失败·重试"));
                selected->setToolTip(result.value("summary").toString().isEmpty()
                    ? result.value("error").toString() : result.value("summary").toString());
                return;
            }
            selected->setText(successText);
            confirmBtn->setEnabled(false);
            falsePosBtn->setEnabled(false);
            moreInfoBtn->setEnabled(false);
        };
        connect(confirmBtn, &QPushButton::clicked, this, [submitFeedback, confirmBtn]() {
            submitFeedback(QStringLiteral("correct"), confirmBtn, QString::fromUtf8("✓ 已确认"));
        });
        connect(falsePosBtn, &QPushButton::clicked, this, [submitFeedback, falsePosBtn]() {
            submitFeedback(QStringLiteral("false_positive"), falsePosBtn, QString::fromUtf8("✓ 已标记"));
        });
        connect(moreInfoBtn, &QPushButton::clicked, this, [submitFeedback, moreInfoBtn]() {
            submitFeedback(QStringLiteral("needs_more_information"), moreInfoBtn, QString::fromUtf8("✓ 已记录"));
        });

        v->addWidget(fw);

        // separator between findings
        if (fi < findingsArr.size() - 1) {
            auto* sep = new QFrame(card);
            sep->setFrameShape(QFrame::HLine);
            sep->setStyleSheet(QString("background:%1;max-height:1px;").arg(AgentTheme::currentBorderDefault()));
            v->addWidget(sep);
        }
    }

    appendWidget(card);
}
void AgentChatDock::addReportCard(const QString& toolResultJson) {
    const QJsonObject root = QJsonDocument::fromJson(toolResultJson.toUtf8()).object();
    const QJsonObject data = root.value("data").toObject();
    const QString report = data.value("report").toString();
    if (report.isEmpty()) return;
    const QJsonObject provenance = data.value("provenance").toObject();

    auto* card = new QFrame(_host);
    card->setStyleSheet(QString("QFrame{background:%2;border:1px solid %1;border-radius:11px;}").arg(AgentTheme::currentBorderDefault(), AgentTheme::currentSurfaceCard()));
    auto* v = new QVBoxLayout(card); v->setContentsMargins(0,0,0,0); v->setSpacing(0);

    // ---- report metadata header ----
    auto* headFrame = new QFrame(card);
    headFrame->setStyleSheet(QString(
        "background:%2;border-top-left-radius:11px;border-top-right-radius:11px;"
        "border-bottom:1px solid %1;").arg(AgentTheme::currentBorderDefault(), AgentTheme::currentSurfaceElevated()));
    auto* headLay = new QVBoxLayout(headFrame); headLay->setContentsMargins(12,8,12,8); headLay->setSpacing(4);

    auto* headTitle = new QLabel(QString::fromUtf8("◈  分析报告"), headFrame);
    headTitle->setStyleSheet(QString("font-weight:bold;color:%1;").arg(AgentTheme::currentTextPrimary()));
    headLay->addWidget(headTitle);

    // metadata row
    auto* metaRow = new QWidget(headFrame);
    auto* metaH = new QHBoxLayout(metaRow); metaH->setContentsMargins(0,0,0,0); metaH->setSpacing(8);
    const QString dataSource = provenance.value("data_id").toString();
    const QString analysisName = provenance.value("analysis_name").toString();
    const QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    if (!dataSource.isEmpty()) {
        auto* dsChip = new QLabel(QString::fromUtf8("数据源: %1").arg(dataSource), metaRow);
        dsChip->setStyleSheet(QString("background:%2;color:%1;border-radius:5px;padding:1px 6px;font-size:9px;").arg(AgentTheme::currentTextSecondary(), AgentTheme::currentNeutralChip()));
        metaH->addWidget(dsChip);
    }
    if (!analysisName.isEmpty()) {
        auto* atChip = new QLabel(QString::fromUtf8("分析类型: %1").arg(analysisName), metaRow);
        atChip->setStyleSheet(QString("background:%2;color:%1;border-radius:5px;padding:1px 6px;font-size:9px;").arg(AgentTheme::currentTextSecondary(), AgentTheme::currentNeutralChip()));
        metaH->addWidget(atChip);
    }
    auto* tsChip = new QLabel(timestamp, metaRow);
    tsChip->setStyleSheet(QString("color:%1;font-family:Consolas;font-size:9px;").arg(AgentTheme::currentTextSecondary()));
    metaH->addWidget(tsChip);
    metaH->addStretch();
    headLay->addWidget(metaRow);
    v->addWidget(headFrame);

    // ---- table of contents ----
    QStringList sections;
    QRegularExpression re(QString::fromUtf8("^## (.+)$"), QRegularExpression::MultilineOption);
    QRegularExpressionMatchIterator it = re.globalMatch(report);
    while (it.hasNext()) sections << it.next().captured(1);
    if (!sections.isEmpty()) {
        auto* tocFrame = new QWidget(card);
        auto* tocLay = new QHBoxLayout(tocFrame); tocLay->setContentsMargins(12,6,12,2); tocLay->setSpacing(0);
        auto* tocLabel = new QLabel(QString::fromUtf8("目录"), tocFrame);
        tocLabel->setStyleSheet(QString("color:%1;font-size:10px;font-weight:bold;margin-right:6px;").arg(AgentTheme::currentTextSecondary()));
        tocLay->addWidget(tocLabel);
        for (int i = 0; i < sections.size(); ++i) {
            if (i > 0) {
                auto* dot = new QLabel(QString::fromUtf8(" · "), tocFrame);
                dot->setStyleSheet(QString("color:%1;font-size:10px;").arg(AgentTheme::currentBorderDefault()));
                tocLay->addWidget(dot);
            }
            auto* tocBtn = new QToolButton(tocFrame);
            tocBtn->setText(sections[i]);
            tocBtn->setCursor(Qt::PointingHandCursor);
            tocBtn->setStyleSheet(QString(
                "QToolButton{border:none;background:transparent;color:%1;font-size:10px;"
                "text-decoration:underline;padding:0;}"
                "QToolButton:hover{color:%2;}").arg(AgentTheme::currentStepRunning(), AgentTheme::currentTextPrimary()));
            tocLay->addWidget(tocBtn);
        }
        tocLay->addStretch();
        v->addWidget(tocFrame);
    }

    // ---- baseline deviation highlight (if baseline section exists) ----
    const bool hasBaseline = report.contains(QString::fromUtf8("基线")) || report.contains(QString::fromUtf8("baseline"), Qt::CaseInsensitive);
    if (hasBaseline) {
        auto* baselineBar = new QWidget(card);
        auto* blH = new QHBoxLayout(baselineBar); blH->setContentsMargins(12,4,12,4); blH->setSpacing(6);
        auto* blIcon = new QLabel(QString::fromUtf8("📊"), baselineBar);
        blIcon->setStyleSheet(QString("font-size:12px;"));
        blH->addWidget(blIcon);
        auto* blLabel = new QLabel(QString::fromUtf8("包含基线偏差对比"), baselineBar);
        blLabel->setStyleSheet(QString("color:%1;font-size:10px;font-weight:bold;").arg(AgentTheme::currentStepRunning()));
        blH->addWidget(blLabel);
        // visual comparison bar
        auto* blBar = new QFrame(baselineBar);
        blBar->setFixedSize(80, 6);
        blBar->setStyleSheet(QString("background:qlineargradient(x1:0,y1:0,x2:1,y2:0,"
            "stop:0 %1, stop:0.5 %2, stop:1 %3);border-radius:3px;").arg(AgentTheme::currentOkGreen()).arg(AgentTheme::currentWarnAmber()).arg(AgentTheme::currentBadRed()));
        blH->addWidget(blBar);
        blH->addStretch();
        v->addWidget(baselineBar);
    }

    // ---- report body ----
    auto* body = new QTextEdit(card);
    body->setReadOnly(true);
    body->setMarkdown(report);
    body->setStyleSheet(QString(
        "QTextEdit{border:none;background:transparent;padding:8px 12px;font-size:12px;"
        "font-family:'Segoe UI',sans-serif;line-height:1.5;}"));
    body->setMinimumHeight(80);
    body->setMaximumHeight(360);
    v->addWidget(body);

    // ---- export buttons ----
    auto* exportRow = new QWidget(card);
    auto* expH = new QHBoxLayout(exportRow); expH->setContentsMargins(12,6,12,8); expH->setSpacing(6);

    auto* exportPdfBtn = new QPushButton(QString::fromUtf8("导出 PDF"), exportRow);
    exportPdfBtn->setCursor(Qt::PointingHandCursor);
    exportPdfBtn->setStyleSheet(QString(
        "QPushButton{background:%1;color:%2;border:none;border-radius:7px;padding:4px 12px;font-size:11px;}"
        "QPushButton:hover{background:%3;}").arg(AgentTheme::currentAccentPrimary(), AgentTheme::currentTextOnAccent(), AgentTheme::currentAccentHover()));
    expH->addWidget(exportPdfBtn);

    auto* copyMdBtn = new QPushButton(QString::fromUtf8("复制 Markdown"), exportRow);
    copyMdBtn->setCursor(Qt::PointingHandCursor);
    copyMdBtn->setStyleSheet(QString(
        "QPushButton{background:%3;color:%1;border:1px solid %2;border-radius:7px;padding:4px 12px;font-size:11px;}"
        "QPushButton:hover{background:%4;}").arg(AgentTheme::currentTextSecondary(), AgentTheme::currentBorderDefault(), AgentTheme::currentSurfaceCard(), AgentTheme::currentSurfaceElevated()));
    expH->addWidget(copyMdBtn);
    expH->addStretch();

    auto* statusLabel = new QLabel(exportRow);
    statusLabel->setStyleSheet(QString("color:%1;font-size:10px;").arg(AgentTheme::currentTextSecondary()));
    expH->addWidget(statusLabel);
    v->addWidget(exportRow);

    // connect export actions
    connect(exportPdfBtn, &QPushButton::clicked, this, [this, body, report, exportPdfBtn, statusLabel]() {
        QString fileName = QFileDialog::getSaveFileName(this, QString::fromUtf8("导出报告"),
            QString::fromUtf8("分析报告.md"), QString::fromUtf8("Markdown (*.md);;Text (*.txt)"));
        if (fileName.isEmpty()) return;
        QFile file(fileName);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            file.write(report.toUtf8());
            file.close();
            exportPdfBtn->setText(QString::fromUtf8("✓ 已导出"));
            exportPdfBtn->setEnabled(false);
            statusLabel->setText(QString::fromUtf8("已保存: %1").arg(QFileInfo(fileName).fileName()));
        }
    });
    connect(copyMdBtn, &QPushButton::clicked, this, [this, report, copyMdBtn, statusLabel]() {
        QApplication::clipboard()->setText(report);
        copyMdBtn->setText(QString::fromUtf8("✓ 已复制"));
        copyMdBtn->setEnabled(false);
        statusLabel->setText(QString::fromUtf8("Markdown 已复制到剪贴板"));
    });

    appendWidget(card);
}

// ============================ TrendSparkline ============================
class TrendSparkline : public QWidget {
public:
    explicit TrendSparkline(const QVector<double>& values, const QString& color, QWidget* parent = nullptr)
        : QWidget(parent), _values(values), _color(color) {
        setFixedHeight(30);
        setMinimumWidth(60);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing, true);
        const QRect r = rect().adjusted(2, 4, -2, -4);
        if (_values.size() < 2 || r.width() < 2 || r.height() < 2) return;
        double lo = _values[0], hi = _values[0];
        for (const double v : _values) { lo = qMin(lo, v); hi = qMax(hi, v); }
        if (qFuzzyCompare(lo, hi)) { lo -= 1; hi += 1; }
        const int n = _values.size();
        QVector<QPointF> pts; pts.reserve(n);
        for (int i = 0; i < n; ++i) {
            const double x = r.x() + (r.width() * i) / (n - 1);
            const double y = r.y() + r.height() * (1.0 - (_values[i] - lo) / (hi - lo));
            pts.append(QPointF(x, y));
        }
        QPen pen(_color); pen.setWidthF(1.5); p.setPen(pen);
        p.drawPolyline(pts);
        p.setBrush(QBrush(QColor(_color))); p.setPen(Qt::NoPen);
        p.drawEllipse(pts.last(), 3, 3);
    }
private:
    QVector<double> _values;
    QString _color;
};

void AgentChatDock::addMonitoringTrendCard(const QString& trendJson) {
    const QJsonObject root = QJsonDocument::fromJson(trendJson.toUtf8()).object();
    const QJsonArray channelsArr = root.value("channels").toArray();
    if (channelsArr.isEmpty()) return;

    auto* card = new QFrame(_host);
    card->setStyleSheet(QString("QFrame{background:%1;border:1px solid %2;border-radius:11px;}")
        .arg(AgentTheme::currentSurfaceCard()).arg(AgentTheme::currentBorderDefault()));
    auto* v = new QVBoxLayout(card); v->setContentsMargins(0,0,0,0); v->setSpacing(0);

    auto* headFrame = new QFrame(card);
    headFrame->setStyleSheet(QString(
        "background:%1;border-top-left-radius:11px;border-top-right-radius:11px;"
        "border-bottom:1px solid %2;")
        .arg(AgentTheme::currentSurfaceElevated()).arg(AgentTheme::currentBorderDefault()));
    auto* headH = new QHBoxLayout(headFrame); headH->setContentsMargins(12,8,12,8); headH->setSpacing(6);
    auto* headIcon = new QLabel(QString::fromUtf8("◉"), headFrame);
    headIcon->setStyleSheet(QString("color:%1;font-size:14px;").arg(AgentTheme::currentAccentPrimary()));
    headH->addWidget(headIcon);
    auto* headTitle = new QLabel(QString::fromUtf8("在线陪试监测（1 秒窗口）"), headFrame);
    headTitle->setStyleSheet(QString("font-weight:bold;color:%1;").arg(AgentTheme::currentTextPrimary()));
    headH->addWidget(headTitle, 1);
    v->addWidget(headFrame);

    for (const QJsonValue& chVal : channelsArr) {
        const QJsonObject ch = chVal.toObject();
        const QString name = ch.value("name").toString(ch.value("channel_id").toString());
        const double current = ch.value("current").toDouble();
        const QString unit = ch.value("unit").toString();
        const double limitLo = ch.value("limit_lo").toDouble();
        const double limitHi = ch.value("limit_hi").toDouble();
        const QString status = ch.value("status").toString("normal");
        const QString updatedAt = ch.value("updated_at").toString();
        const QJsonArray trendArr = ch.value("trend").toArray();
        const double slopePerMinute = ch.value("slope_per_minute").toDouble();

        QString statusColor;
        if (status == "alarm" || status == "critical") statusColor = AgentTheme::currentBadRed();
        else if (status == "warn" || status == "warning") statusColor = AgentTheme::currentWarnAmber();
        else statusColor = AgentTheme::currentOkGreen();

        QString trendArrow;
        if (trendArr.size() >= 2) {
            const double slopeNoiseFloor = qMax(1.0, qAbs(current)) * 1e-6;
            if (slopePerMinute > slopeNoiseFloor) trendArrow = QString::fromUtf8("↑");
            else if (slopePerMinute < -slopeNoiseFloor) trendArrow = QString::fromUtf8("↓");
            else trendArrow = QString::fromUtf8("→");
        } else {
            trendArrow = QString::fromUtf8("→");
        }
        const QString arrowColor = trendArrow == QString::fromUtf8("↑") ? AgentTheme::currentBadRed()
                                 : trendArrow == QString::fromUtf8("↓") ? AgentTheme::currentOkGreen()
                                 : AgentTheme::currentTextSecondary();

        auto* chWidget = new QWidget(card);
        auto* chLay = new QHBoxLayout(chWidget); chLay->setContentsMargins(12,6,12,6); chLay->setSpacing(8);

        auto* statusDot = new QLabel(QString::fromUtf8("●"), chWidget);
        statusDot->setStyleSheet(QString("color:%1;font-size:10px;").arg(statusColor));
        chLay->addWidget(statusDot);

        auto* nameLabel = new QLabel(name, chWidget);
        nameLabel->setStyleSheet(QString("font-weight:bold;font-size:12px;color:%1;")
            .arg(AgentTheme::currentTextPrimary()));
        nameLabel->setFixedWidth(100);
        chLay->addWidget(nameLabel);

        auto* valueLabel = new QLabel(QString::fromUtf8("%1 %2").arg(current, 0, 'g', 8).arg(unit), chWidget);
        valueLabel->setStyleSheet(QString("font-family:Consolas;font-size:13px;font-weight:bold;color:%1;")
            .arg(AgentTheme::currentTextPrimary()));
        chLay->addWidget(valueLabel);

        auto* arrowLabel = new QLabel(trendArrow, chWidget);
        arrowLabel->setStyleSheet(QString("font-size:16px;font-weight:bold;color:%1;").arg(arrowColor));
        arrowLabel->setToolTip(QString::fromUtf8("显示用线性斜率：%1 %2/分钟；未作为告警或预测基线。")
            .arg(slopePerMinute, 0, 'g', 6).arg(unit));
        chLay->addWidget(arrowLabel);

        QVector<double> trendValues;
        for (const QJsonValue& tv : trendArr) trendValues << tv.toDouble();
        if (!trendValues.isEmpty()) {
            const QString sparkColor = (status == "alarm" || status == "critical") ? AgentTheme::currentBadRed()
                                     : status == "warn" ? AgentTheme::currentWarnAmber()
                                     : AgentTheme::currentAccentPrimary();
            auto* sparkline = new TrendSparkline(trendValues, sparkColor, chWidget);
            chLay->addWidget(sparkline, 1);
        } else {
            chLay->addStretch(1);
        }

        if (limitLo != 0 || limitHi != 0) {
            auto* rangeLabel = new QLabel(
                QString::fromUtf8("[%1 ~ %2 %3]").arg(limitLo, 0, 'g', 6).arg(limitHi, 0, 'g', 6).arg(unit), chWidget);
            rangeLabel->setStyleSheet(QString("color:%1;font-family:Consolas;font-size:9px;")
                .arg(AgentTheme::currentTextSecondary()));
            chLay->addWidget(rangeLabel);
        }

        auto* statusBadge = new QLabel(status.toUpper(), chWidget);
        statusBadge->setStyleSheet(QString(
            "background:%1;color:%2;border-radius:6px;padding:1px 6px;"
            "font-family:Consolas;font-size:9px;font-weight:bold;")
            .arg(statusColor).arg(AgentTheme::currentTextOnAccent()));
        chLay->addWidget(statusBadge);

        if (!updatedAt.isEmpty()) {
            auto* tsLabel = new QLabel(updatedAt, chWidget);
            tsLabel->setStyleSheet(QString("color:%1;font-family:Consolas;font-size:9px;")
                .arg(AgentTheme::currentTextSecondary()));
            chLay->addWidget(tsLabel);
        }

        v->addWidget(chWidget);

        if (chVal != channelsArr.last()) {
            auto* sep = new QFrame(card);
            sep->setFrameShape(QFrame::HLine);
            sep->setStyleSheet(QString("background:%1;max-height:1px;")
                .arg(AgentTheme::currentBorderDefault()));
            v->addWidget(sep);
        }
    }

    appendWidget(card);
}

void AgentChatDock::addFeedbackCard(const QString& toolResultJson) {
    const QJsonObject root = QJsonDocument::fromJson(toolResultJson.toUtf8()).object();
    const QString feedbackType = root.value("feedbackType").toString();
    const QString severity = root.value("severity").toString();
    const QString feedbackId = root.value("feedbackId").toString();
    if (feedbackType.isEmpty()) return;
    auto* card = new QFrame(_host);
    QString borderColor = severity == "critical" ? AgentTheme::currentSevCritical() : (severity == "concern" ? AgentTheme::currentSevConcern() : AgentTheme::currentSevInfo());
    card->setStyleSheet(QString("QFrame{background:%2;border:1px solid %1;border-radius:11px;}").arg(borderColor, AgentTheme::currentSurfaceCard()));
    auto* v = new QVBoxLayout(card); v->setContentsMargins(12,9,12,9); v->setSpacing(4);
    QString typeLabel;
    if (feedbackType == "false_positive") typeLabel = "误报";
    else if (feedbackType == "false_negative") typeLabel = "漏报";
    else if (feedbackType == "correct") typeLabel = "正确";
    else if (feedbackType == "needs_more_information") typeLabel = "需补充信息";
    else if (feedbackType == "disposition") typeLabel = "处置";
    else if (feedbackType == "correction") typeLabel = "纠正";
    else typeLabel = "其他";
    auto* t = new QLabel(QString("<span style='font-weight:bold;font-size:13px'>反馈已记录</span> "
        "<span style='background:%2;color:%3;border-radius:8px;padding:1px 7px;font-size:10px'>%4</span> "
        "<span style='color:%6;font-family:Consolas;font-size:10px'>%5</span>")
        .arg(typeLabel).arg(borderColor).arg(severity == "critical" ? AgentTheme::currentSevCritical() : severity == "concern" ? AgentTheme::currentSevConcern() : AgentTheme::currentSevInfo())
        .arg(severity.toUpper()).arg(feedbackId.left(8)).arg(AgentTheme::currentTextTertiary()), card);
    t->setTextFormat(Qt::RichText);
    v->addWidget(t);
    appendWidget(card);
}
void AgentChatDock::invalidateConfirmCard(const QString& reason) {
    if (!_activeConfirmCard) return;
    if (auto* timer = _activeConfirmCard->findChild<QTimer*>())
        timer->stop();
    if (auto* state = _activeConfirmCard->findChild<QLabel*>(QStringLiteral("confirmationState")))
        state->setText(reason.toHtmlEscaped());
    _activeConfirmCard->setEnabled(false);
    _activeConfirmCard.clear();
}

void AgentChatDock::addConfirmCard(const QString& confirmationId, const QString& toolName,
                                   const QString& argsSummary) {
    invalidateConfirmCard(QString::fromUtf8("该确认已失效。"));
    auto* card = new QFrame(_host);
    card->setStyleSheet(QString("QFrame{background:%1;border:1px solid %2;border-radius:11px;}").arg(AgentTheme::currentWarnSubtle(), AgentTheme::currentSevConcern()));
    auto* v = new QVBoxLayout(card); v->setContentsMargins(12,10,12,10); v->setSpacing(7);
    auto* t = new QLabel(QString::fromUtf8("⚠ 需要你确认：写操作 ") + toolName, card);
    t->setWordWrap(true); t->setStyleSheet(QString("color:%1;font-weight:bold;").arg(AgentTheme::currentAmberDark()));
    v->addWidget(t);
    auto* details = new QLabel(card);
    details->setTextFormat(Qt::RichText);
    details->setWordWrap(true);
    details->setText(QStringLiteral("<b>关键参数摘要</b><br>")
        + argsSummary.toHtmlEscaped().replace(QStringLiteral("\n"), QStringLiteral("<br>")));
    details->setStyleSheet(QString("color:%1;background:%3;border:1px solid %2;"
                                   "border-radius:6px;padding:6px;font-size:11px;")
                               .arg(AgentTheme::currentTextSecondary(), AgentTheme::currentBorderDefault(), AgentTheme::currentSurfaceCard()));
    v->addWidget(details);
    auto* countdown = new QLabel(card);
    countdown->setObjectName(QStringLiteral("confirmationCountdown"));
    constexpr qint64 kConfirmTimeoutMs = 5 * 60 * 1000;
    const qint64 deadlineMs = QDateTime::currentMSecsSinceEpoch() + kConfirmTimeoutMs;
    countdown->setText(QString::fromUtf8("确认剩余: 05:00"));
    countdown->setStyleSheet(QString("color:%1;font-size:11px;font-weight:bold;").arg(AgentTheme::currentAmberDark()));
    v->addWidget(countdown);
    auto* timer = new QTimer(card);
    timer->setInterval(1000);
    connect(timer, &QTimer::timeout, this, [countdown, deadlineMs, timer, card]() {
        const qint64 remaining = deadlineMs - QDateTime::currentMSecsSinceEpoch();
        if (remaining <= 0) {
            timer->stop();
            countdown->setText(QString::fromUtf8("已超时"));
            card->setEnabled(false);
            return;
        }
        const int secs = static_cast<int>(remaining / 1000);
        countdown->setText(QString::fromUtf8("确认剩余: %1:%2")
            .arg(secs / 60, 2, 10, QChar('0'))
            .arg(secs % 60, 2, 10, QChar('0')));
    });
    timer->start();
    auto* state = new QLabel(card);
    state->setObjectName(QStringLiteral("confirmationState"));
    state->setStyleSheet(QString("color:%1;font-size:11px;").arg(AgentTheme::currentTextSecondary()));
    v->addWidget(state);
    auto* bh = new QHBoxLayout(); bh->setSpacing(8);
    auto* ok = new QPushButton(QString::fromUtf8("确认执行"), card);
    ok->setStyleSheet(QString("QPushButton{background:%1;color:%2;border:none;border-radius:8px;padding:6px 14px;font-weight:bold;}").arg(AgentTheme::currentOkGreen(), AgentTheme::currentTextOnAccent()));
    ok->setCursor(Qt::PointingHandCursor);
    auto* no = new QPushButton(QString::fromUtf8("拒绝"), card);
    no->setStyleSheet(QString("QPushButton{background:%3;color:%1;border:1px solid %2;border-radius:8px;padding:6px 14px;}").arg(AgentTheme::currentTextSecondary(), AgentTheme::currentBorderDefault(), AgentTheme::currentSurfaceCard()));
    no->setCursor(Qt::PointingHandCursor);
    bh->addWidget(ok); bh->addWidget(no); bh->addStretch();
    v->addLayout(bh);
    connect(ok, &QPushButton::clicked, this, [this, card, state, timer, confirmationId]() {
        timer->stop();
        int accepted = 0;
        if (_vm) {
            _vm->setValue("ConfirmationDecisionId", confirmationId.toStdString());
            accepted = _vm->excute("Confirm");
        }
        state->setText(accepted ? QString::fromUtf8("✓ 已确认")
                                : QString::fromUtf8("该确认已失效。"));
        card->setEnabled(false);
        if (_activeConfirmCard == card) _activeConfirmCard.clear();
    });
    connect(no, &QPushButton::clicked, this, [this, card, state, timer, confirmationId]() {
        timer->stop();
        int accepted = 0;
        if (_vm) {
            _vm->setValue("ConfirmationDecisionId", confirmationId.toStdString());
            accepted = _vm->excute("Reject");
        }
        state->setText(accepted ? QString::fromUtf8("✕ 已拒绝")
                                : QString::fromUtf8("该确认已失效。"));
        card->setEnabled(false);
        if (_activeConfirmCard == card) _activeConfirmCard.clear();
    });
    _activeConfirmCard = card;
    appendWidget(card);
}

void AgentChatDock::addRefinementCard(const QString& detailJson) {
    UiRefinementProposal proposal;
    if (!parseRefinementProposal(detailJson.toStdString(), proposal)) return;
    auto* card = new QFrame(_host);
    card->setStyleSheet(
        QString("QFrame{background:%1;border:1px solid %2;border-radius:11px;}")
        .arg(AgentTheme::currentBlueSubtle(), AgentTheme::currentBluePrimary()));
    auto* v = new QVBoxLayout(card); v->setContentsMargins(12,10,12,10); v->setSpacing(7);

    auto* t = new QLabel(QString::fromUtf8("🧪 下一轮试验设计提案（待授权）"), card);
    t->setWordWrap(true);
    t->setStyleSheet(QString("color:%1;font-weight:bold;").arg(AgentTheme::currentBlueDark()));
    v->addWidget(t);

    auto joinNames = [](const std::vector<std::string>& in) {
        QString out;
        for (const auto& s : in)
            out += (out.isEmpty() ? QString() : QStringLiteral(",")) + QString::fromStdString(s);
        return out;
    };
    QString refined = proposal.refinedFactors.empty()
        ? QString::fromUtf8("无") : joinNames(proposal.refinedFactors);
    QString fixed = proposal.fixedFactors.empty()
        ? QString::fromUtf8("无") : joinNames(proposal.fixedFactors);
    const bool hasEstimate = proposal.hasPower && proposal.power >= 0;
    QString powerText = QString::fromUtf8("未估计（无噪声基线）");
    if (proposal.hasPower)
        powerText = proposal.power >= 0
            ? QStringLiteral("%1%").arg(proposal.power * 100, 0, 'f', 0)
            : QString::fromUtf8("—");
    const QString detailsText = QStringLiteral(
        "<b>意图</b>：基于战役 %1 的结论精化设计<br>"
        "<b>对照</b>：精化 [%2] ｜ 固定于最优 [%3]<br>"
        "<b>影响</b>：%4 次运行 × %5 次重复<br>"
        "<b>功效</b>：%6<br>"
        "<b>中止</b>：执行后可随时取消；如配置 interim_after 将自动暂停待审<br>"
        "<b>溯源</b>：提案 %7 ｜ 契约 %8…")
        .arg(QString::fromStdString(proposal.sourceCampaignId),
             refined, fixed,
             QString::number(proposal.plannedRuns),
             QString::number(proposal.replicates),
             powerText,
              QString::fromStdString(proposal.refinementArtifactId),
              QString::fromStdString(proposal.proposedSha256).left(16));
    auto* details = new QLabel(card);
    details->setTextFormat(Qt::RichText);
    details->setWordWrap(true);
    details->setText(detailsText);
    details->setStyleSheet(QString("color:%1;background:%3;border:1px solid %2;"
                                   "border-radius:6px;padding:6px;font-size:11px;")
                               .arg(AgentTheme::currentTextSecondary(), AgentTheme::currentBorderDefault(), AgentTheme::currentSurfaceCard()));
    v->addWidget(details);

    // efficacy gauge: fill by estimated power; calibration state as a coloured badge
    if (hasEstimate) {
        const int percent = qBound(0, qRound(proposal.power * 100.0), 100);
        const char* fillColor = percent >= 80 ? AgentTheme::currentOkGreen()
                              : percent >= 50 ? AgentTheme::currentWarnAmber()
                                              : AgentTheme::currentBadRed();
        auto* powerRow = new QWidget(card);
        auto* ph = new QHBoxLayout(powerRow); ph->setContentsMargins(0,0,0,0); ph->setSpacing(7);
        auto* track = new QFrame(powerRow);
        track->setFixedSize(140, 10);
        track->setStyleSheet(QString("background:%2;border:1px solid %1;border-radius:5px;")
                                 .arg(AgentTheme::currentBorderDefault(), AgentTheme::currentNeutralChip()));
        auto* fill = new QFrame(track);
        fill->setStyleSheet(QString("background:%1;border-radius:4px;").arg(fillColor));
        fill->setGeometry(1, 1, std::max(6, (140 - 2) * percent / 100), 8);
        ph->addWidget(track);
        auto* badge = new QLabel(proposal.powerCalibrated
            ? QString::fromUtf8("已校准") : QString::fromUtf8("未过校准"), powerRow);
        badge->setStyleSheet(QString(
            "background:%1;color:%2;border-radius:6px;padding:1px 7px;font-size:10px;")
            .arg(proposal.powerCalibrated ? AgentTheme::currentOkGreen() : AgentTheme::currentBadRed(), AgentTheme::currentTextOnAccent()));
        ph->addWidget(badge);
        ph->addStretch();
        v->addWidget(powerRow);
    }

    auto* state = new QLabel(QString::fromUtf8(
        "执行仍会经过应用内写操作确认门。"), card);
    state->setStyleSheet(QString("color:%1;font-size:11px;").arg(AgentTheme::currentTextSecondary()));
    v->addWidget(state);

    auto* bh = new QHBoxLayout(); bh->setSpacing(8);
    auto* ok = new QPushButton(QString::fromUtf8("按提案授权执行"), card);
    ok->setStyleSheet(QString("QPushButton{background:%1;color:%2;border:none;"
                      "border-radius:8px;padding:6px 14px;font-weight:bold;}").arg(AgentTheme::currentBluePrimary(), AgentTheme::currentTextOnAccent()));
    ok->setCursor(Qt::PointingHandCursor);
    auto* no = new QPushButton(QString::fromUtf8("暂不执行"), card);
    no->setStyleSheet(QString("QPushButton{background:%3;color:%1;border:1px solid %2;"
                              "border-radius:8px;padding:6px 14px;}").arg(AgentTheme::currentTextSecondary(), AgentTheme::currentBorderDefault(), AgentTheme::currentSurfaceCard()));
    no->setCursor(Qt::PointingHandCursor);
    bh->addWidget(ok); bh->addWidget(no); bh->addStretch();
    v->addLayout(bh);
    connect(ok, &QPushButton::clicked, this, [this, card, state, proposal]() {
        state->setText(QString::fromUtf8("✓ 已发起：请在模型发起工具调用后确认执行。"));
        card->setEnabled(false);
        if (_input) {
            _input->setPlainText(QStringLiteral(
                "请读取会话工件 %1 并核对 proposed_contract_sha256=%2 后，"
                "原样使用其 proposed_contract 调用 execute_autonomous_experiment_campaign "
                "开始新一轮试验战役；不要修改契约的任何字段。")
                .arg(QString::fromStdString(proposal.refinementArtifactId),
                     QString::fromStdString(proposal.proposedSha256)));
            sendCurrent();
        }
    });
    connect(no, &QPushButton::clicked, this, [this, card, state]() {
        state->setText(QString::fromUtf8("提案已存档为会话工件，未发起执行。"));
        card->setEnabled(false);
    });
    appendWidget(card);
}

void AgentChatDock::addCheckpointCard(const QString& detailJson) {
    UiCheckpointHint hint;
    if (!parseCheckpointHint(detailJson.toStdString(), hint) || !hint.awaiting) return;
    auto* card = new QFrame(_host);
    card->setStyleSheet(
        QString("QFrame{background:%1;border:1px solid %2;border-radius:11px;}")
        .arg(AgentTheme::currentWarnSubtle(), AgentTheme::currentSevConcern()));
    auto* v = new QVBoxLayout(card); v->setContentsMargins(12,10,12,10); v->setSpacing(7);
    auto* t = new QLabel(QString::fromUtf8("⏸ 试验战役已到中期检查点"), card);
    t->setWordWrap(true);
    t->setStyleSheet(QString("color:%1;font-weight:bold;").arg(AgentTheme::currentAmberDark()));
    v->addWidget(t);
    auto* details = new QLabel(card);
    details->setTextFormat(Qt::RichText);
    details->setWordWrap(true);
    details->setText(QStringLiteral(
        "<b>进度</b>：%1 / %2 次运行已完成，证据均已绑定<br>"
        "<b>性质</b>：运营/安全监督门——请核查设备状态与数据合理性；"
        "该暂停不是经校正的期中统计检验<br>"
        "<b>检查点</b>：%3")
        .arg(hint.completedRuns).arg(hint.plannedRuns)
        .arg(QString::fromStdString(hint.checkpointArtifactId)));
    details->setStyleSheet(QString("color:%1;background:%3;border:1px solid %2;"
                                   "border-radius:6px;padding:6px;font-size:11px;")
                               .arg(AgentTheme::currentTextSecondary(), AgentTheme::currentBorderDefault(), AgentTheme::currentSurfaceCard()));
    v->addWidget(details);

    // segmented progress bar: done=green, failed=red, remaining=grey (border).
    // The payload's runs[] only holds executed records (each with an explicit ok flag),
    // so failed = count(ok=false) and the rest of planned_runs is still to come.
    int failedRuns = 0;
    QJsonObject payload = QJsonDocument::fromJson(detailJson.toUtf8()).object();
    if (!payload.value("data").toObject().isEmpty()) payload = payload.value("data").toObject();
    for (const QJsonValue& value : payload.value("runs").toArray())
        if (!value.toObject().value("ok").toBool(true)) ++failedRuns;
    const int totalRuns = qMax(hint.plannedRuns,
                               hint.completedRuns + failedRuns);
    const int remainingRuns = qMax(0, totalRuns - hint.completedRuns - failedRuns);
    if (totalRuns > 0) {
        auto* barHost = new QWidget(card);
        auto* bh2 = new QHBoxLayout(barHost); bh2->setContentsMargins(0,0,0,0); bh2->setSpacing(2);
        auto addSegment = [&](int count, const char* color) {
            if (count <= 0) return;
            auto* segment = new QFrame(barHost);
            segment->setFixedHeight(8);
            segment->setStyleSheet(QString("background:%1;border-radius:3px;").arg(color));
            bh2->addWidget(segment, count);
        };
        addSegment(hint.completedRuns, AgentTheme::currentOkGreen());
        addSegment(failedRuns, AgentTheme::currentBadRed());
        addSegment(remainingRuns, AgentTheme::currentBorderDefault());
        v->addWidget(barHost);
    }
    auto* bh = new QHBoxLayout(); bh->setSpacing(8);
    auto* go = new QPushButton(QString::fromUtf8("继续剩余运行"), card);
    go->setStyleSheet(QString("QPushButton{background:%1;color:%2;border:none;"
                      "border-radius:8px;padding:6px 14px;font-weight:bold;}").arg(AgentTheme::currentOkGreen(), AgentTheme::currentTextOnAccent()));
    go->setCursor(Qt::PointingHandCursor);
    auto* stop = new QPushButton(QString::fromUtf8("终止战役"), card);
    stop->setStyleSheet(QString("QPushButton{background:%2;color:%3;"
                                "border:1px solid %1;border-radius:8px;padding:6px 14px;}")
                            .arg(AgentTheme::currentBorderDefault(), AgentTheme::currentSurfaceCard(), AgentTheme::currentRedStop()));
    stop->setCursor(Qt::PointingHandCursor);
    bh->addWidget(go); bh->addWidget(stop); bh->addStretch();
    v->addLayout(bh);
    const QString artifactId = QString::fromStdString(hint.checkpointArtifactId);
    const QString sha = QString::fromStdString(hint.campaignSha256);
    connect(go, &QPushButton::clicked, this, [this, card, artifactId, sha]() {
        if (_input) {
            _input->setPlainText(QStringLiteral(
                "请使用与本次战役完全相同的契约（campaign_sha256=%1）调用 "
                "resume_autonomous_experiment_campaign：checkpoint_artifact_id=%2，"
                "decision=continue。契约字段必须逐字保持不变。").arg(sha, artifactId));
            sendCurrent();
        }
        card->setEnabled(false);
    });
    connect(stop, &QPushButton::clicked, this, [this, card, artifactId, sha]() {
        if (_input) {
            _input->setPlainText(QStringLiteral(
                "请使用相同契约（campaign_sha256=%1）调用 "
                "resume_autonomous_experiment_campaign：checkpoint_artifact_id=%2，"
                "decision=abort，并汇总已完成运行的结论。").arg(sha, artifactId));
            sendCurrent();
        }
        card->setEnabled(false);
    });
    appendWidget(card);
}

void AgentChatDock::addCompanionEventCard(const QString& eventJson) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(eventJson.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return;
    const QJsonObject event = document.object();
    if (event.value("source").toString() != QStringLiteral("local_monitor")) return;
    const QString severity = event.value("severity").toString();
    const bool alarm = severity == QStringLiteral("alarm") || severity == QStringLiteral("critical");
    const bool warning = severity == QStringLiteral("warning") || severity == QStringLiteral("warn");
    const QString alarmId = event.value("alarm_id").toString();
    const QString lifecycle = event.value("lifecycle").toString();
    const bool acknowledged = event.value("acknowledged").toBool(false);
    const QString border = alarm ? AgentTheme::currentSevCritical() : (warning ? AgentTheme::currentSevConcern() : AgentTheme::currentAccentPrimary());
    const QString background = alarm ? AgentTheme::currentBadSubtle() : (warning ? AgentTheme::currentWarnSubtle() : AgentTheme::currentAccentSubtle());
    const QString icon = alarm ? QString::fromUtf8("⚠")
        : (warning ? QString::fromUtf8("◆") : QString::fromUtf8("●"));
    auto* card = new QFrame(_host);
    card->setStyleSheet(QString("QFrame{background:%1;border:1px solid %2;border-radius:11px;}")
                            .arg(background, border));
    auto* layout = new QVBoxLayout(card); layout->setContentsMargins(12,9,12,9); layout->setSpacing(5);
    const qint64 timestamp = qint64(event.value("timestamp_ms").toDouble());
    const QString time = timestamp > 0
        ? QLocale::c().toString(QDateTime::fromMSecsSinceEpoch(timestamp), "HH:mm:ss") : QString();
    auto* title = new QLabel(icon + "  " + event.value("title").toString().toHtmlEscaped()
        + QString(" <span style='color:%2;font-size:10px'>%1 · 本地规则</span>").arg(time, AgentTheme::currentTextTertiary()), card);
    title->setTextFormat(Qt::RichText); title->setWordWrap(true);
    title->setStyleSheet(QString("color:%1;font-weight:bold;").arg(border));
    layout->addWidget(title);
    auto* message = new QLabel(event.value("message").toString().toHtmlEscaped()
        .replace("\n", "<br>"), card);
    message->setTextFormat(Qt::RichText); message->setWordWrap(true);
    message->setStyleSheet(QString("color:%1;font-size:12px;").arg(AgentTheme::currentTextPrimary()));
    layout->addWidget(message);
    if (event.contains("channel_id")) {
        auto* meta = new QLabel(QString::fromUtf8("通道 %1 · 读数 %2 %3 · 限值 %4 %3")
            .arg(event.value("channel_id").toInt())
            .arg(event.value("value").toDouble(), 0, 'g', 8)
            .arg(event.value("unit").toString().toHtmlEscaped())
            .arg(event.value("limit").toDouble(), 0, 'g', 8), card);
        meta->setStyleSheet(QString("color:%1;font-family:Consolas;font-size:10px;").arg(AgentTheme::currentTextSecondary()));
        layout->addWidget(meta);
    }
    auto* actions = new QHBoxLayout(); actions->setContentsMargins(0,2,0,0);
    if (!alarmId.isEmpty() && !acknowledged && lifecycle != QStringLiteral("recovered")
        && lifecycle != QStringLiteral("acknowledged")) {
        auto* acknowledge = new QPushButton(QString::fromUtf8("确认收到"), card);
        acknowledge->setCursor(Qt::PointingHandCursor);
        acknowledge->setToolTip(QString::fromUtf8("仅记录操作员已收到告警；不会清除条件或控制设备。"));
        acknowledge->setStyleSheet(QString(
            "QPushButton{background:%1;color:%2;border:1px solid %2;border-radius:7px;padding:4px 10px;}")
            .arg(AgentTheme::currentSurfaceCard(), border));
        actions->addWidget(acknowledge);
        connect(acknowledge, &QPushButton::clicked, this, [this, acknowledge, alarmId]() {
            if (!_vm) return;
            const QJsonObject request{{"alarm_id", alarmId},
                                      {"note", QString::fromUtf8("界面确认")}};
            _vm->setValue("CompanionAcknowledgeRequest",
                QJsonDocument(request).toJson(QJsonDocument::Compact).toStdString());
            if (_vm->excute("AcknowledgeCompanionAlarm")) {
                acknowledge->setText(QString::fromUtf8("已确认"));
                acknowledge->setEnabled(false);
            } else {
                const QJsonObject result = QJsonDocument::fromJson(QByteArray::fromStdString(
                    _vm->value<std::string>("CompanionAcknowledgeResult"))).object();
                QMessageBox::warning(this, QString::fromUtf8("告警未确认"),
                    QString::fromUtf8("该告警可能已恢复或不再活动（%1）。")
                        .arg(result.value("error").toString()));
            }
        });
    }
    auto* explain = new QPushButton(QString::fromUtf8("AI 解读"), card);
    explain->setCursor(Qt::PointingHandCursor);
    explain->setStyleSheet(QString(
        "QPushButton{background:%2;color:%1;border:1px solid %1;border-radius:7px;padding:4px 10px;}")
        .arg(border, AgentTheme::currentSurfaceCard()));
    actions->addWidget(explain); actions->addStretch(); layout->addLayout(actions);
    connect(explain, &QPushButton::clicked, this, [this, explain, eventJson]() {
        if (!_vm) return;
        if (_running || _analysisPending) {
            QMessageBox::information(this,
                _analysisPending ? QString::fromUtf8("分析仍在运行") : QString::fromUtf8("AI 正在工作"),
                _analysisPending ? QString::fromUtf8("请先停止当前分析，或等待结果发布后再解读该事件。")
                                 : QString::fromUtf8("请等待当前回复结束后再解读该事件。"));
            return;
        }
        _vm->setValue("CompanionExplainEventJson", eventJson.toStdString());
        if (_vm->excute("ExplainCompanionEvent")) {
            explain->setText(QString::fromUtf8("解读中…"));
            explain->setEnabled(false);
        }
    });
    appendWidget(card);
}

void AgentChatDock::addCompanionDashboardCard(const QString& statusJson) {
    const QJsonObject status = QJsonDocument::fromJson(statusJson.toUtf8()).object();
    if (status.isEmpty()) return;
    const QJsonObject health = status.value("health").toObject();
    if (health.isEmpty()) return;

    const bool connected = status.value("connected").toBool();
    const QString healthState = health.value("state").toString();
    const QString healthSeverity = health.value("severity").toString();
    const double uptimeMs = status.value("uptime_ms").toDouble();
    const qlonglong sampleCount = static_cast<qlonglong>(status.value("sample_count").toDouble());
    const double windowRate = status.value("window_rate_hz").toDouble();
    const QString lastSampleAt = status.value("last_sample_at").toString();

    // derive status from connected + health severity
    QString statusLabel, statusColor;
    if (!connected || healthState == QStringLiteral("disconnected")) {
        statusLabel = QString::fromUtf8("已断开");
        statusColor = AgentTheme::currentBadRed();
    } else if (healthState == QStringLiteral("stale") || healthSeverity == QStringLiteral("warning")) {
        statusLabel = QString::fromUtf8("数据陈旧");
        statusColor = AgentTheme::currentWarnAmber();
    } else {
        statusLabel = QString::fromUtf8("健康");
        statusColor = AgentTheme::currentOkGreen();
    }

    // format uptime as HH:MM:SS
    const qint64 totalSecs = qBound(0LL, qRound64(uptimeMs / 1000.0), 86400LL);
    const int hours = static_cast<int>(totalSecs / 3600);
    const int minutes = static_cast<int>((totalSecs % 3600) / 60);
    const int seconds = static_cast<int>(totalSecs % 60);
    const QString uptimeStr = QString::fromUtf8("%1:%2:%3")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'));

    auto* card = new QFrame(_host);
    card->setStyleSheet(QString("QFrame{background:%2;border:1px solid %1;border-radius:11px;}")
        .arg(AgentTheme::currentBorderDefault(), AgentTheme::currentSurfaceCard()));
    auto* v = new QVBoxLayout(card); v->setContentsMargins(0,0,0,0); v->setSpacing(0);

    // ---- header row ----
    auto* headFrame = new QFrame(card);
    headFrame->setStyleSheet(QString(
        "background:%1;border-top-left-radius:11px;border-top-right-radius:11px;"
        "border-bottom:1px solid %2;")
        .arg(AgentTheme::currentSurfaceElevated(), AgentTheme::currentBorderDefault()));
    auto* headH = new QHBoxLayout(headFrame); headH->setContentsMargins(12,8,12,8); headH->setSpacing(8);
    auto* headIcon = new QLabel(QString::fromUtf8("◉"), headFrame);
    headIcon->setStyleSheet(QString("color:%1;font-size:14px;").arg(AgentTheme::currentAccentPrimary()));
    headH->addWidget(headIcon);
    auto* headTitle = new QLabel(QString::fromUtf8("陪试状态面板"), headFrame);
    headTitle->setStyleSheet(QString("font-weight:bold;color:%1;").arg(AgentTheme::currentTextPrimary()));
    headH->addWidget(headTitle, 1);
    // connection status pill
    auto* statusPill = new QLabel(QString::fromUtf8("● %1").arg(statusLabel), headFrame);
    statusPill->setStyleSheet(QString(
        "background:%1;color:%2;border-radius:8px;padding:2px 8px;"
        "font-family:Consolas;font-size:11px;font-weight:bold;")
        .arg(statusColor, AgentTheme::currentTextOnAccent()));
    headH->addWidget(statusPill);
    v->addWidget(headFrame);

    // ---- metrics grid (2 rows × 2 cols) ----
    auto* metricsHost = new QWidget(card);
    auto* mg = new QGridLayout(metricsHost); mg->setContentsMargins(12,8,12,8); mg->setSpacing(8);

    auto addMetric = [&](int row, int col, const QString& label, const QString& value, const QString& valueColor) {
        auto* cell = new QWidget(metricsHost);
        auto* cl = new QVBoxLayout(cell); cl->setContentsMargins(8,6,8,6); cl->setSpacing(2);
        auto* lbl = new QLabel(label, cell);
        lbl->setStyleSheet(QString("color:%1;font-size:10px;").arg(AgentTheme::currentTextSecondary()));
        cl->addWidget(lbl);
        auto* val = new QLabel(value, cell);
        val->setStyleSheet(QString("color:%1;font-family:Consolas;font-size:14px;font-weight:bold;").arg(valueColor.isEmpty() ? AgentTheme::currentTextPrimary() : valueColor));
        cl->addWidget(val);
        cell->setStyleSheet(QString("background:%1;border-radius:7px;").arg(AgentTheme::currentSurfaceElevated()));
        mg->addWidget(cell, row, col);
    };

    addMetric(0, 0, QString::fromUtf8("运行时长"), uptimeStr, QString());
    addMetric(0, 1, QString::fromUtf8("有效统计窗口"), QString::fromUtf8("%1").arg(sampleCount), QString());
    addMetric(1, 0, QString::fromUtf8("窗口吞吐"),
        QString::fromUtf8("%1 窗口/秒").arg(windowRate, 0, 'f', 2),
        windowRate > 0 ? AgentTheme::currentOkGreen() : AgentTheme::currentTextSecondary());
    addMetric(1, 1, QString::fromUtf8("最后有效窗口"),
        lastSampleAt.isEmpty() ? QString::fromUtf8("—") : lastSampleAt,
        QString());
    v->addWidget(metricsHost);

    // ---- health reasons ----
    const QJsonArray reasons = health.value("reasons").toArray();
    if (!reasons.isEmpty()) {
        auto* reasonsLabel = new QLabel(card);
        QStringList texts;
        for (const QJsonValue& rv : reasons) texts << rv.toString();
        reasonsLabel->setText(QString::fromUtf8("原因: %1").arg(texts.join(QString::fromUtf8("、"))));
        reasonsLabel->setWordWrap(true);
        reasonsLabel->setStyleSheet(QString("color:%1;font-size:10px;padding:2px 12px 6px 12px;")
            .arg(AgentTheme::currentTextSecondary()));
        v->addWidget(reasonsLabel);
    }

    appendWidget(card);
}

void AgentChatDock::sendCurrent() {
    if (!_vm || !_input || _running || _analysisPending) return;
    QString text = _input->toPlainText().trimmed();
    QString attach = QString::fromStdString(_vm->value<std::string>("Attachments"));
    if (text.isEmpty() && attach.isEmpty()) return;
    QStringList names;
    for (const QString& f : attach.split("|", Qt::SkipEmptyParts)) names << QFileInfo(f).fileName();
    addUserBubble(text, names);
    _activeWorkflow = nullptr;                 // a fresh turn starts a fresh workflow card
    _turnAbnormal = false;
    _vm->setValue("Input", text.toStdString());
    _vm->excute("Send");
    _input->clear();
    updateAttachmentLabel();
    setRunning(true);
}

void AgentChatDock::setRunning(bool running) {
    _running = running;
    updateControls();
}

void AgentChatDock::updateControls() {
    const bool canStop = _running || _analysisPending;
    if (_sendBtn) _sendBtn->setVisible(!canStop);
    if (_stopBtn) _stopBtn->setVisible(canStop);
    if (_stopBtn) _stopBtn->setEnabled(canStop);
    if (_newSessionBtn) _newSessionBtn->setEnabled(!canStop);
    if (_historyBtn) _historyBtn->setEnabled(!canStop);
    if (_evidenceBtn) _evidenceBtn->setEnabled(!canStop);
    if (_quickActions) _quickActions->setEnabled(!canStop);
    if (_statusDot) {
        const QString text = _running ? QString::fromUtf8("● 思考中…")
            : _analysisPending ? QString::fromUtf8("● 分析运行中") : QString::fromUtf8("● 就绪");
        _statusDot->setText(text);
        _statusDot->setStyleSheet(canStop ? QString("color:%1;font-family:Consolas;font-size:11px;").arg(AgentTheme::currentStepRunning())
                                          : QString("color:%1;font-family:Consolas;font-size:11px;").arg(AgentTheme::currentOkGreen()));
    }
}

void AgentChatDock::updateAttachmentLabel() {
    if (!_vm || !_attachmentLabel) return;
    QString raw = QString::fromStdString(_vm->value<std::string>("Attachments"));
    QStringList files = raw.split("|", Qt::SkipEmptyParts);
    if (files.isEmpty()) { _attachmentLabel->clear(); _attachmentLabel->setVisible(false); return; }
    QStringList names; for (const QString& f : files) names << QFileInfo(f).fileName();
    _attachmentLabel->setText(QString::fromUtf8("附件: ") + names.join(", "));
    _attachmentLabel->setVisible(true);
}

void AgentChatDock::clearStream() {
    if (!_stream) return;
    _activeConfirmCard.clear();
    while (_stream->count() > 1) {
        QLayoutItem* item = _stream->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
    _prunable.clear();
    _activeWorkflow = nullptr;
}

void AgentChatDock::showSessionMenu() {
    if (!_vm || _running || !_historyBtn) return;
    _vm->excute("RefreshSessions");
    const QJsonObject root = QJsonDocument::fromJson(
        QByteArray::fromStdString(_vm->value<std::string>("SessionsJson"))).object();
    QMenu menu(this);
    const QJsonArray sessions = root.value("sessions").toArray();
    if (!root.value("ok").toBool() || sessions.isEmpty()) {
        QAction* empty = menu.addAction(QString::fromUtf8("暂无可恢复会话"));
        empty->setEnabled(false);
    } else {
        for (const QJsonValue& value : sessions) {
            const QJsonObject session = value.toObject();
            const QString id = session.value("id").toString();
            QString title = session.value("title").toString();
            if (title.isEmpty()) title = QString::fromUtf8("未命名会话");
            QAction* action = menu.addAction(title + "  ·  " + id.right(8));
            connect(action, &QAction::triggered, this, [this, id]() {
                if (!_vm) return;
                _vm->setValue("ResumeSessionId", id.toStdString());
                _vm->excute("ResumeSession");
            });
        }
    }
    menu.exec(_historyBtn->mapToGlobal(QPoint(0, _historyBtn->height())));
}

void AgentChatDock::showRuntimeInfo() {
    if (!_vm) return;
    const QString runtimeInfoJson =
        QString::fromStdString(_vm->value<std::string>("RuntimeInfoJson"));
    refreshProviderBadge(runtimeInfoJson);
    QMessageBox::information(this, QString::fromUtf8("AI 运行配置"),
        runtimeSummaryText(QJsonDocument::fromJson(runtimeInfoJson.toUtf8()).object()));
}

void AgentChatDock::refreshProviderBadge(const QString& runtimeInfoJson) {
    if (!_providerBadge) return;
    const QJsonObject info = QJsonDocument::fromJson(runtimeInfoJson.toUtf8()).object();
    if (info.isEmpty()) { _providerBadge->setVisible(false); return; }
    const QString model = info.value("model").toString();
    QStringList configErrors;
    for (const QJsonValue& error : info.value("config_errors").toArray())
        configErrors << error.toString();
    // "priority_fallback" with a single enabled route behaves like normal service;
    // only a multi-route fallback chain (or config warnings) counts as degraded.
    const bool fallbackRouting =
        info.value("routing_mode").toString(QStringLiteral("single"))
            .contains(QStringLiteral("fallback"))
        && info.value("enabled_provider_route_count").toInt(1) > 1;
    QString mark = QString::fromUtf8("●");
    QString text = model;
    const char* color = AgentTheme::currentOkGreen();
    if (!info.value("api_key_error").toString().isEmpty()) {
        mark = QString::fromUtf8("✗");
        text = QString::fromUtf8("密钥异常");
        color = AgentTheme::currentBadRed();
    } else if (!configErrors.isEmpty() || fallbackRouting) {
        mark = QString::fromUtf8("▲");
        text = model + QString::fromUtf8(" · 降级");
        color = AgentTheme::currentWarnAmber();
    }
    _providerBadge->setText(mark + " " + text.toHtmlEscaped());
    _providerBadge->setStyleSheet(QString(
        "background:%1;color:%2;border-radius:8px;padding:2px 8px;"
        "font-family:Consolas;font-size:11px;font-weight:bold;").arg(color, AgentTheme::currentTextOnAccent()));
    _providerBadge->setToolTip(runtimeSummaryText(info));
    _providerBadge->setVisible(true);
}

QString AgentChatDock::runtimeSummaryText(const QJsonObject& info) const {
    const QString keyStatus = info.value("api_key_configured").toBool()
        ? QString::fromUtf8("已配置（%1）").arg(info.value("api_key_source").toString())
        : QString::fromUtf8("未配置");
    const QJsonObject budget = info.value("turn_budget").toObject();
    const QString routingStatus = QString::fromUtf8("%1，启用 %2 条")
        .arg(info.value("routing_mode").toString(QStringLiteral("single")))
        .arg(info.value("enabled_provider_route_count").toInt(1));
    const auto limitText = [](double value) {
        return value > 0 ? QLocale().toString(static_cast<qlonglong>(value))
                         : QString::fromUtf8("不限");
    };
    const QString budgetStatus = QString::fromUtf8("模型调用 %1，输入 %2，输出 %3，费用 %4 μUSD")
        .arg(limitText(budget.value("max_model_calls").toDouble()))
        .arg(limitText(budget.value("max_input_tokens").toDouble()))
        .arg(limitText(budget.value("max_output_tokens").toDouble()))
        .arg(limitText(budget.value("max_estimated_cost_micros").toDouble()));
    QString text = QString::fromUtf8(
        "提供商：%1\n模型：%2\n接口：%3\n密钥：%4\n路由：%5\n回合预算：%6\n策略版本：%7\n工具数：%8\nMCP：%9\n配置：%10")
        .arg(info.value("provider").toString())
        .arg(info.value("model").toString())
        .arg(info.value("base_url").toString())
        .arg(keyStatus)
        .arg(routingStatus)
        .arg(budgetStatus)
        .arg(info.value("agent_policy_version").toInt())
        .arg(info.value("tool_count").toInt())
        .arg(info.value("mcp_started").toBool()
                 ? QString::fromUtf8("只读，已启动") : QString::fromUtf8("未启动"))
        .arg(info.value("config_path").toString());
    const QString catalogSha256 = info.value("tool_catalog_sha256").toString();
    if (!catalogSha256.isEmpty())
        text += QStringLiteral("\nTool catalog SHA-256: ") + catalogSha256;
    if (info.value("legacy_plaintext_api_key").toBool())
        text += QString::fromUtf8("\n\n警告：当前仍在读取旧版明文密钥，请迁移到 DPAPI。");
    if (!info.value("api_key_error").toString().isEmpty())
        text += QString::fromUtf8("\n\n密钥错误：") + info.value("api_key_error").toString();
    const QJsonArray configErrors = info.value("config_errors").toArray();
    if (!configErrors.isEmpty()) {
        QStringList errors;
        for (const auto& error : configErrors) errors.push_back(error.toString());
        text += QString::fromUtf8("\n\n路由配置警告：") + errors.join(QStringLiteral(", "));
    }
    return text;
}

void AgentChatDock::updateCompanionStatus(const QString& statusJson) {
    const QJsonObject status = QJsonDocument::fromJson(statusJson.toUtf8()).object();
    if (status.isEmpty()) return;
    const bool enabled = status.value("enabled").toBool(false);
    const QString state = status.value("state").toString();
    if (_companionToggle) {
        const QSignalBlocker blocker(_companionToggle);
        _companionToggle->setChecked(enabled);
    }
    if (!_companionState) return;
    const QJsonObject health = status.value("health").toObject();
    const QString healthState = health.value("state").toString(state);
    const QString healthSeverity = health.value("severity").toString();
    QString text;
    QString color = AgentTheme::currentTextSecondary();
    if (!enabled || state == QStringLiteral("off")) {
        text = QString::fromUtf8("已关闭");
    } else if (state == QStringLiteral("waiting")) {
        text = QString::fromUtf8("已开启 · 等待采集"); color = AgentTheme::currentStepRunning();
    } else if (state == QStringLiteral("monitoring")) {
        text = QString::fromUtf8("监测 %1 通道 · %2 · 活动告警 %3")
            .arg(status.value("monitored_channels").toInt())
            .arg(healthState)
            .arg(status.value("active_alerts").toInt());
        color = healthSeverity == QStringLiteral("critical") ? AgentTheme::currentSevCritical()
            : healthSeverity == QStringLiteral("warning") ? AgentTheme::currentSevConcern() : AgentTheme::currentOkGreen();
    } else {
        text = QString::fromUtf8("已开启 · 暂无可监测通道"); color = AgentTheme::currentSevConcern();
    }
    _companionState->setText(text);
    _companionState->setStyleSheet(QString("color:%1;font-size:11px;").arg(color));
    QStringList healthReasons;
    for (const QJsonValue& value : health.value("reasons").toArray())
        healthReasons << value.toString();
    _companionState->setToolTip(QStringLiteral(
        "Health: %1 (%2)\nConnected: %3\nStorage: %4\nPoll age: %5 ms\nData age: %6 ms\nStale channels: %7\nEvents: %8\nReasons: %9")
        .arg(health.value("state").toString(state))
        .arg(health.value("severity").toString())
        .arg(status.value("connected").toBool() ? QString::fromUtf8("是") : QString::fromUtf8("否"))
        .arg(status.value("saving").toBool() ? QString::fromUtf8("进行中") : QString::fromUtf8("未开始"))
        .arg(static_cast<qlonglong>(status.value("poll_age_ms").toDouble()))
        .arg(static_cast<qlonglong>(status.value("data_age_ms").toDouble()))
        .arg(status.value("stale_channels").toInt())
        .arg(status.value("event_count").toInt())
        .arg(healthReasons.isEmpty() ? QStringLiteral("none") : healthReasons.join(QStringLiteral(", "))));
    const QJsonObject preflight = status.value("preflight").toObject();
    if (_healthBtn && !preflight.isEmpty()) {
        const QString overall = preflight.value("overall").toString();
        QString mark = QString::fromUtf8(" ✓");
        QString healthColor = AgentTheme::currentOkGreen();
        if (overall == QStringLiteral("blocked")) {
            mark = QString::fromUtf8(" ×");
            healthColor = AgentTheme::currentSevCritical();
        } else if (overall == QStringLiteral("degraded")
                   || overall == QStringLiteral("monitoring_with_warnings")) {
            mark = QString::fromUtf8(" !");
            healthColor = AgentTheme::currentSevConcern();
        }
        if (health.value("severity").toString() == QStringLiteral("critical")) {
            mark = QString::fromUtf8(" ×");
            healthColor = AgentTheme::currentSevCritical();
        } else if (health.value("severity").toString() == QStringLiteral("warning")
                   && healthColor == AgentTheme::currentOkGreen()) {
            mark = QString::fromUtf8(" !");
            healthColor = AgentTheme::currentSevConcern();
        }
        _healthBtn->setText(QString::fromUtf8("预检") + mark);
        _healthBtn->setStyleSheet(QString(
            "QToolButton{border:1px solid %1;border-radius:7px;padding:3px 7px;color:%1;background:%2;font-weight:bold;}")
            .arg(healthColor, AgentTheme::currentSurfaceCard()));
    }
}

void AgentChatDock::showExperimentHealth() {
    if (!_vm) return;
    const QJsonObject status = QJsonDocument::fromJson(QByteArray::fromStdString(
        _vm->value<std::string>("CompanionStatusJson"))).object();
    const QJsonObject preflight = status.value("preflight").toObject();
    if (preflight.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8("试验预检"),
                             QString::fromUtf8("当前尚无可用的预检快照。"));
        return;
    }

    const QHash<QString, QString> labels{
        {"context", QString::fromUtf8("项目与采集试验")},
        {"device", QString::fromUtf8("采集设备")},
        {"channels", QString::fromUtf8("通道配置")},
        {"acquisition", QString::fromUtf8("数据采集")},
        {"storage", QString::fromUtf8("数据存储")},
        {"companion", QString::fromUtf8("智能陪试")},
        {"data_flow", QString::fromUtf8("实时数据流")}
    };
    const QHash<QString, QString> issues{
        {"no_project", QString::fromUtf8("未打开项目")},
        {"no_daq_test", QString::fromUtf8("未打开采集试验")},
        {"device_disconnected", QString::fromUtf8("采集设备未连接")},
        {"no_acquisition_channels", QString::fromUtf8("未选择采集通道")},
        {"companion_disabled", QString::fromUtf8("智能陪试未启用")},
        {"no_monitored_channels", QString::fromUtf8("未配置陪试通道")},
        {"no_valid_limits", QString::fromUtf8("陪试通道没有有效上下限")},
        {"partial_limit_coverage", QString::fromUtf8("部分陪试通道没有有效上下限")},
        {"storage_not_running", QString::fromUtf8("采集已运行但存储未启动")},
        {"no_live_monitored_channels", QString::fromUtf8("实时数据尚未接入陪试监测")}
    };
    const QJsonObject health = status.value("health").toObject();
    QString text = QString::fromUtf8("监控健康：%1（%2）\n心跳年龄：%3 ms    数据年龄：%4 ms\n\n采集就绪：%5    陪试就绪：%6\n\n")
        .arg(health.value("state").toString(QString::fromUtf8("未知")))
        .arg(health.value("severity").toString(QString::fromUtf8("未知")))
        .arg(static_cast<qlonglong>(status.value("poll_age_ms").toDouble()))
        .arg(static_cast<qlonglong>(status.value("data_age_ms").toDouble()))
        .arg(preflight.value("ready_to_acquire").toBool() ? QString::fromUtf8("是") : QString::fromUtf8("否"))
        .arg(preflight.value("ready_to_monitor").toBool() ? QString::fromUtf8("是") : QString::fromUtf8("否"));
    for (const QJsonValue& value : preflight.value("nodes").toArray()) {
        const QJsonObject node = value.toObject();
        const QString state = node.value("state").toString();
        const QString mark = state == QStringLiteral("blocked") ? QString::fromUtf8("×")
            : state == QStringLiteral("warning") ? QString::fromUtf8("!")
            : (state == QStringLiteral("ok") || state == QStringLiteral("active"))
                ? QString::fromUtf8("●") : QString::fromUtf8("○");
        text += QString("%1  %2  [%3]\n").arg(mark, labels.value(node.value("id").toString()), state);
    }
    auto appendIssues = [&](const char* key, const QString& title) {
        const QJsonArray values = preflight.value(key).toArray();
        if (values.isEmpty()) return;
        QStringList translated;
        for (const QJsonValue& value : values)
            translated << issues.value(value.toString(), value.toString());
        text += "\n" + title + translated.join(QString::fromUtf8("；"));
    };
    appendIssues("blockers", QString::fromUtf8("阻断："));
    appendIssues("warnings", QString::fromUtf8("提醒："));
    QMessageBox::information(this, QString::fromUtf8("试验预检与健康"), text);
}

void AgentChatDock::showCompanionSettings() {
    if (!_vm) return;
    QJsonObject config = QJsonDocument::fromJson(QByteArray::fromStdString(
        _vm->value<std::string>("CompanionConfigJson"))).object();
    QDialog dialog(this); dialog.setWindowTitle(QString::fromUtf8("智能陪试参数"));
    dialog.setMinimumWidth(430);
    auto* root = new QVBoxLayout(&dialog); root->setContentsMargins(16,14,16,14); root->setSpacing(10);
    auto* note = new QLabel(QString::fromUtf8(
        "阈值始终来自设备通道配置。这里仅调整确认次数、去重、掉数判定和 AI 解读；"
        "陪试不会自动停机或修改设备参数。"), &dialog);
    note->setWordWrap(true); note->setStyleSheet(QString("color:%1;font-size:11px;").arg(AgentTheme::currentTextSecondary()));
    root->addWidget(note);
    auto* form = new QFormLayout(); form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto* sample = new QSpinBox(&dialog); sample->setRange(250, 10000); sample->setSingleStep(250); sample->setSuffix(" ms");
    sample->setValue(config.value("sample_interval_ms").toInt(1000));
    auto* sustained = new QSpinBox(&dialog); sustained->setRange(1, 10);
    sustained->setValue(config.value("sustained_samples").toInt(3));
    auto* cooldown = new QSpinBox(&dialog); cooldown->setRange(5, 3600); cooldown->setSuffix(QString::fromUtf8(" 秒"));
    cooldown->setValue(config.value("cooldown_ms").toInt(60000) / 1000);
    auto* escalation = new QSpinBox(&dialog); escalation->setRange(60, 86400); escalation->setSuffix(QString::fromUtf8(" 秒"));
    escalation->setValue(config.value("escalation_ms").toInt(1800000) / 1000);
    auto* stale = new QSpinBox(&dialog); stale->setRange(1000, 60000); stale->setSingleStep(500); stale->setSuffix(" ms");
    stale->setValue(config.value("stale_after_ms").toInt(5000));
    auto* summary = new QSpinBox(&dialog); summary->setRange(60, 3600); summary->setSuffix(QString::fromUtf8(" 秒"));
    summary->setValue(config.value("summary_interval_ms").toInt(300000) / 1000);
    auto* hysteresis = new QDoubleSpinBox(&dialog); hysteresis->setRange(0, 25); hysteresis->setDecimals(1); hysteresis->setSuffix(" %");
    hysteresis->setValue(config.value("hysteresis_ratio").toDouble(0.02) * 100.0);
    auto* autoExplain = new QCheckBox(QString::fromUtf8("告警时自动请求 AI 解读（AI 空闲时）"), &dialog);
    autoExplain->setChecked(config.value("auto_explain").toBool(true));
    auto* autoSummary = new QCheckBox(QString::fromUtf8("定期与结束时生成陪试摘要"), &dialog);
    autoSummary->setChecked(config.value("auto_summary").toBool(true));
    form->addRow(QString::fromUtf8("监督刷新间隔"), sample);
    form->addRow(QString::fromUtf8("连续确认次数"), sustained);
    form->addRow(QString::fromUtf8("重复告警冷却"), cooldown);
    form->addRow(QString::fromUtf8("未确认升级时间"), escalation);
    form->addRow(QString::fromUtf8("无新数据判定"), stale);
    form->addRow(QString::fromUtf8("阶段摘要间隔"), summary);
    form->addRow(QString::fromUtf8("恢复滞回比例"), hysteresis);
    form->addRow(QString(), autoExplain); form->addRow(QString(), autoSummary);
    root->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel,
                                          Qt::Horizontal, &dialog);
    buttons->button(QDialogButtonBox::Save)->setText(QString::fromUtf8("保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QString::fromUtf8("取消"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    root->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return;
    const QJsonObject request{
        {"sample_interval_ms", sample->value()},
        {"sustained_samples", sustained->value()},
        {"cooldown_ms", cooldown->value() * 1000},
        {"escalation_ms", escalation->value() * 1000},
        {"stale_after_ms", stale->value()},
        {"summary_interval_ms", summary->value() * 1000},
        {"hysteresis_ratio", hysteresis->value() / 100.0},
        {"auto_explain", autoExplain->isChecked()},
        {"auto_summary", autoSummary->isChecked()}
    };
    _vm->setValue("CompanionConfigRequest",
        QJsonDocument(request).toJson(QJsonDocument::Compact).toStdString());
    if (!_vm->excute("ConfigureCompanion")) {
        const QJsonObject result = QJsonDocument::fromJson(QByteArray::fromStdString(
            _vm->value<std::string>("CompanionConfigResult"))).object();
        QMessageBox::warning(this, QString::fromUtf8("参数未保存"),
            QString::fromUtf8("无效字段：%1（%2）")
                .arg(result.value("field").toString(), result.value("error").toString()));
    }
}

void AgentChatDock::restoreSessionUi(const QString& sessionJson) {
    const QJsonObject root = QJsonDocument::fromJson(sessionJson.toUtf8()).object();
    if (!root.value("ok").toBool()) {
        QMessageBox::warning(this, QString::fromUtf8("恢复会话"),
                             root.value("summary").toString(QString::fromUtf8("会话恢复失败。")));
        return;
    }
    const QJsonObject recovery = root.value("recovery_warning").toObject();
    if (recovery.value("code").toString() == QStringLiteral("tool_outcome_unknown")) {
        QString message = QString::fromUtf8(
            "先前已批准的写操作（%1）可能在软件重启前已经完成。DPA 没有自动重放该操作。"
            "请先核对设备状态和已保存结果，再决定是否重试。\n\n关联 ID：%2")
            .arg(recovery.value("tool").toString(),
                 recovery.value("correlationId").toString());
        if (!recovery.value("persisted").toBool(true))
            message += QString::fromUtf8("\n\n恢复审计未能写入本地数据库。请保留当前界面并人工记录。 ");
        QMessageBox::warning(this, QString::fromUtf8("写操作结果待核对"), message);
    }
    clearStream();
    for (const QJsonValue& value : root.value("turns").toArray()) {
        const QJsonObject turn = value.toObject();
        const QString role = turn.value("role").toString();
        const QString content = turn.value("content").toString();
        if (role == "user") addUserBubble(content, {});
        else if (role == "assistant") addAiBubble(content);
    }
    for (const QJsonValue& value : root.value("companion_events").toArray())
        if (value.isObject()) addCompanionEventCard(QString::fromUtf8(
            QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)));
    if (root.value("diagnostic_run").isObject()) {
        _activeWorkflow = new WorkflowCard(_host);
        _activeWorkflow->setState(QString::fromUtf8(
            QJsonDocument(root.value("diagnostic_run").toObject()).toJson(QJsonDocument::Compact)));
        appendWidget(_activeWorkflow);
    }
    autoScroll();
}

void AgentChatDock::onNewSession() {
    if (_running) return;
    if (_analysisPending) {
        QMessageBox::information(this, QString::fromUtf8("分析仍在运行"),
            QString::fromUtf8("请先停止当前分析，或等待匹配的分析结果发布后再新建会话。"));
        return;
    }
    if (!_vm || !_vm->excute("NewSession")) return;
    invalidateConfirmCard(QString::fromUtf8("会话已切换，该确认已失效。"));
    clearStream();
}

void AgentChatDock::toggleSearchBar() {
    if (!_searchBar) return;
    const bool showing = !_searchBar->isVisible();
    _searchBar->setVisible(showing);
    if (showing && _searchInput) {
        _searchInput->setFocus();
        _searchInput->selectAll();
        _searchCurrent = 0;
    }
}

void AgentChatDock::onStep(QString kind, QString name, QString detail) {
    if (kind == "tool_start") {
        if (!_activeWorkflow) { _activeWorkflow = new WorkflowCard(_host); appendWidget(_activeWorkflow); }
        _activeWorkflow->addStep(name, detail);
    } else if (kind == "tool_result") {
        const QJsonObject result = QJsonDocument::fromJson(detail.toUtf8()).object();
        if (!result.isEmpty() && !result.value("ok").toBool(true)) {
            // ToolResult JSON carries ok=false at the root; surface it as a failed step.
            const QString reason = result.value("error").toString(
                result.value("summary").toString());
            if (_activeWorkflow) _activeWorkflow->failStep(name, reason);
        } else if (_activeWorkflow) {
            _activeWorkflow->resolveStep(name, detail);
        }
        if (name == "diagnose") addDiagnosticCard(detail);
        if (name == "generate_report" || name == "dpa.generate_report") addReportCard(detail);
        if (name == "record_operator_feedback" || name == "dpa.record_operator_feedback") addFeedbackCard(detail);
        if (name == "compare_baseline" || name == "dpa.compare_baseline") {
            auto* card = new BaselineCompareCard(detail, _host);
            appendWidget(card);
        }
        if (name == "get_companion_trend" || name == "dpa.companion.trend") addMonitoringTrendCard(detail);
        if ((name == "companion_status" || name == "dpa.companion.status")
            && QJsonDocument::fromJson(detail.toUtf8()).object().contains("health"))
            addCompanionDashboardCard(detail);
        if (name == "execute_autonomous_experiment_campaign" && _campaignPanel) {
            _campaignToggle->setChecked(true);
            _campaignPanel->applyCampaignResult(detail);
            _campaignPanel->refreshFromService();
        }
        if (name == "refine_autonomous_experiment_campaign") {
            _campaignToggle->setChecked(true);
            addRefinementCard(detail);
            if (_campaignPanel) _campaignPanel->refreshFromService();
        }
        if (name == "execute_autonomous_experiment_campaign"
            || name == "resume_autonomous_experiment_campaign") {
            UiCheckpointHint hint;
            if (parseCheckpointHint(detail.toStdString(), hint) && hint.awaiting)
                addCheckpointCard(detail);
        }
    } else if (kind == "run_state") {
        const QString stateValue = QJsonDocument::fromJson(detail.toUtf8()).object()
                                       .value("state").toString();
        if (stateValue == QStringLiteral("failed") || stateValue == QStringLiteral("cancelled"))
            _turnAbnormal = true;
        if (!_activeWorkflow) { _activeWorkflow = new WorkflowCard(_host); appendWidget(_activeWorkflow); }
        _activeWorkflow->setState(detail);
    }
    autoScroll();
}

void AgentChatDock::onConfirm(QString confirmationId, QString toolName, QString argsSummary) {
    addConfirmCard(confirmationId, toolName, argsSummary);
}

void AgentChatDock::setViewModel(Model::M* vm) {
    if (_vm == vm) return;
    if (_vm) { DeSubscribeM(_vm); disconnect(_vm, nullptr, this, nullptr); }
    _vm = vm;
    if (_vm) {
        SubscribeM(_vm, AgentChatDock);
        connect(_vm, SIGNAL(sigStep(QString,QString,QString)), this, SLOT(onStep(QString,QString,QString)));
        connect(_vm, SIGNAL(sigConfirm(QString,QString,QString)),
                this, SLOT(onConfirm(QString,QString,QString)));
        updateCompanionStatus(QString::fromStdString(
            _vm->value<std::string>("CompanionStatusJson")));
        refreshProviderBadge(QString::fromStdString(
            _vm->value<std::string>("RuntimeInfoJson")));
    }
}

void AgentChatDock::onPropertyChanged(const std::string& Index, utils::Property* prop) {
    if (this->thread() != QThread::currentThread()) {
        QTimer::singleShot(0, this, [=]() { this->onPropertyChanged(Index, prop); });
        return;
    }
    if (Index == "Output") {
        invalidateConfirmCard(QString::fromUtf8("本回合已结束，该确认已过期。"));
        if (_vm) addAiBubble(QString::fromStdString(_vm->value<std::string>("Output")));
        if (_activeWorkflow) { _activeWorkflow->finish(!_turnAbnormal); _activeWorkflow = nullptr; }
    } else if (Index == "Status") {
        if (_vm) {
            std::string st = _vm->value<std::string>("Status");
            setRunning(st != "idle");
            if (st == "waiting_confirm" && _statusDot) _statusDot->setText(QString::fromUtf8("● 等待确认"));
            if (st == "cancelling") {
                _turnAbnormal = true;
                invalidateConfirmCard(QString::fromUtf8("操作已取消，该确认已失效。"));
                if (_statusDot) _statusDot->setText(QString::fromUtf8("● 停止中…"));
                if (_stopBtn) _stopBtn->setEnabled(false);
            }
        }
    } else if (Index == "Attachments") {
        updateAttachmentLabel();
    } else if (Index == "AnalysisPending") {
        if (_vm) {
            _analysisPending = _vm->value<bool>("AnalysisPending");
            updateControls();
        }
    } else if (Index == "ResumedSessionJson") {
        if (_vm) restoreSessionUi(QString::fromStdString(_vm->value<std::string>("ResumedSessionJson")));
    } else if (Index == "CompanionStatusJson") {
        if (_vm) updateCompanionStatus(QString::fromStdString(
            _vm->value<std::string>("CompanionStatusJson")));
    } else if (Index == "CompanionEventJson") {
        if (_vm) addCompanionEventCard(QString::fromStdString(
            _vm->value<std::string>("CompanionEventJson")));
    }
}
