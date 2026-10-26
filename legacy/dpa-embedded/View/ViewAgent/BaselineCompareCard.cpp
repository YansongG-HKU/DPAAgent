#include "BaselineCompareCard.h"
#include "AgentTheme.h"

#include "qcustomplot.h"
#include <QOpenGLWidget>

#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QSpacerItem>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QFont>
#include <QMargins>
#include <QColor>

#include <algorithm>
#include <limits>

namespace {

// Chart row positions. These rows are NON-PHYSICAL: the baseline's peak
// frequencies and the current capture's peak frequencies are spectrally equal
// markers on one frequency axis. The three y-rows merely group the markers by
// membership (baseline / current / new) so the comparison is readable. Nothing
// here implies amplitude, delta or any other physical measure.
constexpr double kYBaselineRow = 0.5;
constexpr double kYCurrentRow  = 1.0;
constexpr double kYNewRow      = 1.5;

QColor chartTextColor()   { return QColor(QString::fromLatin1(AgentTheme::TEXT_PRIMARY)); }
QColor chartGridColor()   { return QColor(QString::fromLatin1(AgentTheme::BORDER_DEFAULT)); }
QColor chartAxisColor()   { return QColor(QString::fromLatin1(AgentTheme::BORDER_STRONG)); }
QColor baselineColor()    { return QColor(QString::fromLatin1(AgentTheme::BLUE_PRIMARY)); }
QColor currentColor()     { return QColor(QString::fromLatin1(AgentTheme::ACCENT_PRIMARY)); }
QColor newPeakColor()     { return QColor(QString::fromLatin1(AgentTheme::SEV_CRITICAL)); }

QFont chartFont(int pointSize)
{
    return QFont(QString::fromUtf8("Microsoft YaHei"), pointSize);
}

QJsonObject payloadObject(const QJsonDocument& document)
{
    if (!document.isObject())
        return QJsonObject();
    const QJsonObject root = document.object();
    // ToolResult::toJson wraps the deviation payload under "data"; a bare
    // dataJson payload also has to work.
    const QJsonValue data = root.value(QStringLiteral("data"));
    return data.isObject() ? data.toObject() : root;
}

bool hasArray(const QJsonObject& object, const QStringList& keys)
{
    for (const QString& key : keys) {
        if (object.value(key).isArray())
            return true;
    }
    return false;
}

QJsonArray firstArray(const QJsonObject& object, const QStringList& keys)
{
    for (const QString& key : keys) {
        const QJsonValue value = object.value(key);
        if (value.isArray())
            return value.toArray();
    }
    return QJsonArray();
}

QVector<double> toFreqs(const QJsonArray& array)
{
    QVector<double> freqs;
    freqs.reserve(array.size());
    for (const QJsonValue& value : array) {
        if (value.isDouble() && value.toDouble() > 0.0)
            freqs.push_back(value.toDouble());
    }
    std::sort(freqs.begin(), freqs.end());
    return freqs;
}

QString boolText(bool value)
{
    return value ? QString::fromUtf8("是") : QString::fromUtf8("否");
}

} // namespace

BaselineCompareCard::BaselineCompareCard(const QString& toolResultJson, QWidget* parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("baselineCompareCard"));
    setStyleSheet(QStringLiteral(
        "#baselineCompareCard{"
            "background:#ffffff;"
            "border:1px solid #dde4e6;"
            "border-radius:11px;"
        "}"));

    const DeviationPayload payload = parsePayload(toolResultJson);
    buildUi(payload);
}

BaselineCompareCard::DeviationPayload BaselineCompareCard::parsePayload(const QString& toolResultJson)
{
    DeviationPayload payload;
    const QJsonDocument document = QJsonDocument::fromJson(toolResultJson.toUtf8());
    // Invalid JSON keeps hasPeakArrays=false -> the card shows the honest
    // "no peak detail" state instead of an empty chart.
    if (!document.isObject()) {
        return payload;
    }

    const QJsonObject root = document.object();
    const QJsonObject data = payloadObject(document);
    payload.baselineHz = toFreqs(firstArray(data, { QStringLiteral("baseline_peaks_hz"),
                                                    QStringLiteral("baseline_peaks"),
                                                    QStringLiteral("peak_freqs_hz"),
                                                    QStringLiteral("baselinePeaksHz") }));
    payload.currentHz = toFreqs(firstArray(data, { QStringLiteral("current_peaks_hz"),
                                                   QStringLiteral("current_peaks"),
                                                   QStringLiteral("latest_peaks_hz"),
                                                   QStringLiteral("peaks_hz"),
                                                   QStringLiteral("currentPeaksHz") }));
    payload.newHz = toFreqs(firstArray(data, { QStringLiteral("new_peaks_hz"),
                                               QStringLiteral("new_peaks"),
                                               QStringLiteral("newPeaksHz") }));
    payload.hasPeakArrays = hasArray(data, { QStringLiteral("baseline_peaks_hz"),
                                             QStringLiteral("baseline_peaks"),
                                             QStringLiteral("peak_freqs_hz"),
                                             QStringLiteral("baselinePeaksHz") })
        || hasArray(data, { QStringLiteral("current_peaks_hz"),
                            QStringLiteral("current_peaks"),
                            QStringLiteral("latest_peaks_hz"),
                            QStringLiteral("peaks_hz"),
                            QStringLiteral("currentPeaksHz") })
        || hasArray(data, { QStringLiteral("new_peaks_hz"),
                            QStringLiteral("new_peaks"),
                            QStringLiteral("newPeaksHz") });

    const QJsonValue hasBaseline = data.value(QStringLiteral("has_baseline"));
    payload.hasBaseline = hasBaseline.isBool()
        ? hasBaseline.toBool()
        : data.value(QStringLiteral("hasBaseline")).toBool(true);
    const QJsonValue isoWorsened = data.value(QStringLiteral("iso_worsened"));
    payload.isoWorsened = isoWorsened.isBool()
        ? isoWorsened.toBool()
        : data.value(QStringLiteral("isoWorsened")).toBool(false);
    payload.severity = data.value(QStringLiteral("severity")).toString(QStringLiteral("info"));
    payload.summary = root.value(QStringLiteral("summary")).toString(
        data.value(QStringLiteral("summary")).toString());
    return payload;
}

void BaselineCompareCard::buildUi(const DeviationPayload& payload)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 12);
    layout->setSpacing(8);

    auto* headerRow = new QHBoxLayout();
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(6);
    auto* title = new QLabel(QString::fromUtf8("◈ 基线对比"), this);
    title->setStyleSheet(QString("color:%1;font-weight:bold;font-size:14px;")
        .arg(QString::fromLatin1(AgentTheme::TEXT_PRIMARY)));
    headerRow->addWidget(title);
    headerRow->addItem(new QSpacerItem(1, 1, QSizePolicy::Expanding, QSizePolicy::Fixed));
    layout->addLayout(headerRow);

    if (!payload.hasPeakArrays) {
        // No peak detail in the payload at all: stay honest instead of drawing
        // an empty fake chart.
        _emptyHint = new QLabel(QString::fromUtf8("对比数据不包含峰值细节"), this);
        _emptyHint->setAlignment(Qt::AlignCenter);
        _emptyHint->setStyleSheet(QString(
            "background:#f7f9f9;color:%1;border:1px solid %2;border-radius:8px;"
            "padding:18px 12px;font-size:12px;")
            .arg(QString::fromLatin1(AgentTheme::TEXT_SECONDARY))
            .arg(QString::fromLatin1(AgentTheme::BORDER_DEFAULT)));
        layout->addWidget(_emptyHint);
    } else {
        buildChart(payload);
    }

    auto* summaryRow = new QHBoxLayout();
    summaryRow->setContentsMargins(0, 0, 0, 0);
    summaryRow->setSpacing(10);

    // "新增峰: N 个" — the DeviationReport has no peak-amplitude values, so we
    // report only the frequency count and positions; nothing else is claimed.
    _peaksLine = new QLabel(QString::fromUtf8("新增峰: %1 个").arg(payload.newHz.size()), this);
    _peaksLine->setStyleSheet(QString("color:%1;font-size:12px;")
        .arg(QString::fromLatin1(AgentTheme::TEXT_PRIMARY)));
    summaryRow->addWidget(_peaksLine);

    _isoLine = new QLabel(QString::fromUtf8("ISO 严重度恶化: %1")
        .arg(boolText(payload.isoWorsened)), this);
    _isoLine->setStyleSheet(QString("color:%1;font-size:12px;")
        .arg(QString::fromLatin1(AgentTheme::TEXT_PRIMARY)));
    summaryRow->addWidget(_isoLine);

    _severityBadge = new QLabel(payload.severity.toUpper(), this);
    _severityBadge->setStyleSheet(AgentTheme::severityBadgeStyle(payload.severity));
    summaryRow->addWidget(_severityBadge);
    summaryRow->addItem(new QSpacerItem(1, 1, QSizePolicy::Expanding, QSizePolicy::Fixed));
    layout->addLayout(summaryRow);

    if (!payload.summary.isEmpty()) {
        _summaryLabel = makeSummaryLabel();
        _summaryLabel->setText(payload.summary.toHtmlEscaped());
        layout->addWidget(_summaryLabel);
    }

    if (!payload.hasBaseline && _summaryLabel
        && !_summaryLabel->text().contains(QString::fromUtf8("基线"))) {
        _summaryLabel->setText(_summaryLabel->text()
            + QString("<br><span style='color:%1'>%2</span>")
                  .arg(QString::fromLatin1(AgentTheme::TEXT_TERTIARY))
                  .arg(QString::fromUtf8("未提供可对比的基线：仅记录当前偏移结果。")));
    }
}

void BaselineCompareCard::buildChart(const DeviationPayload& payload)
{
    _chart = new QCustomPlot(this);
    _chart->setFixedHeight(262);
    _chart->setOpenGl(true);
    _chart->setBackground(QBrush(QColor(Qt::white)));

    // Margin: keep the axes off the card edge and leave room for the right-side
    // legend without touching the frequency labels.
    _chart->axisRect()->setAutoMargins(QCP::msLeft | QCP::msRight
                                       | QCP::msTop | QCP::msBottom);
    _chart->axisRect()->setMargins(QMargins(8, 6, 8, 6));
    _chart->plotLayout()->setRowSpacing(0);
    _chart->plotLayout()->setColumnSpacing(0);

    const double kNewBandHalfTolRatio = 0.025;    // matches the 5% baseline match tolerance
    // Red translucent shading band behind each new-peak marker (grouped by
    // ±2.5% of the peak frequency, the same tolerance compareToBaseline uses).
    for (double freq : payload.newHz) {
        const double halfWidth = std::max(freq * kNewBandHalfTolRatio, 0.5);
        auto* band = new QCPItemRect(_chart);
        band->setPen(QPen(Qt::NoPen));
        band->setBrush(QBrush(QColor(207, 74, 63, 40)));
        band->topLeft->setCoords(freq - halfWidth, kYNewRow + 0.20);
        band->bottomRight->setCoords(freq + halfWidth, kYNewRow - 0.20);
    }

    struct RowSeries
    {
        double row;
        QColor color;
        QString name;
        const QVector<double>* freqs;
    };
    const RowSeries series[] = {
        { kYBaselineRow, baselineColor(), QString::fromUtf8("基线峰值"), &payload.baselineHz },
        { kYCurrentRow,  currentColor(),  QString::fromUtf8("当前峰值"), &payload.currentHz },
        { kYNewRow,      newPeakColor(),  QString::fromUtf8("新增峰值"), &payload.newHz },
    };

    for (const RowSeries& item : series) {
        auto* graph = _chart->addGraph();
        graph->setName(item.name);
        graph->setLineStyle(QCPGraph::lsNone);
        graph->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssDisc,
                                               item.color, item.color, 7.0));
        graph->addToLegend();
        for (double freq : *item.freqs) {
            graph->addData(freq, item.row);
            // Short stem under the marker, same colour: stem markers.
            auto* stem = new QCPItemLine(_chart);
            stem->setPen(QPen(item.color, 1.4));
            stem->start->setCoords(freq, item.row - 0.10);
            stem->end->setCoords(freq, item.row);
        }
    }

    // x-axis domain from the data (or a neutral domain when the payload is
    // array-with-no-peaks); y-axis is the documented non-physical membership rows.
    double lo = 0.0;
    double hi = 1.0;
    bool haveData = !payload.baselineHz.isEmpty() || !payload.currentHz.isEmpty()
        || !payload.newHz.isEmpty();
    if (haveData) {
        double minFreq = std::numeric_limits<double>::max();
        double maxFreq = std::numeric_limits<double>::lowest();
        auto scan = [&](const QVector<double>& freqs) {
            for (double f : freqs) {
                if (f < minFreq) minFreq = f;
                if (f > maxFreq) maxFreq = f;
            }
        };
        scan(payload.baselineHz);
        scan(payload.currentHz);
        scan(payload.newHz);
        if (maxFreq <= minFreq) {
            lo = (minFreq > 0.0) ? minFreq * 0.9 : 0.0;
            hi = (minFreq > 0.0) ? minFreq * 1.1 : 1.0;
        } else {
            const double span = maxFreq - minFreq;
            lo = minFreq - span * 0.06;
            hi = maxFreq + span * 0.06;
        }
    }
    _chart->xAxis->setRange(lo, hi);

    const QFont font9 = chartFont(9);
    const QFont font10 = chartFont(10);
    const QColor text = chartTextColor();
    _chart->xAxis->setLabel(QString::fromUtf8("频率 (Hz)"));
    _chart->yAxis->setLabel(QString::fromUtf8("峰值位置"));
    _chart->xAxis->setLabelFont(font10);
    _chart->yAxis->setLabelFont(font10);
    _chart->xAxis->setLabelColor(text);
    _chart->yAxis->setLabelColor(text);
    _chart->xAxis->setTickLabelFont(font9);
    _chart->yAxis->setTickLabelFont(font9);
    _chart->xAxis->setTickLabelColor(text);
    _chart->yAxis->setTickLabelColor(text);
    _chart->xAxis->setBasePen(QPen(chartAxisColor()));
    _chart->yAxis->setBasePen(QPen(chartAxisColor()));

    const QPen gridPen(chartGridColor(), 1, Qt::DotLine);
    _chart->xAxis->grid()->setPen(gridPen);
    _chart->yAxis->grid()->setPen(gridPen);
    _chart->xAxis->grid()->setSubGridVisible(false);
    _chart->yAxis->grid()->setSubGridVisible(false);
    _chart->yAxis->grid()->setZeroLinePen(Qt::NoPen);

    // Y ticks: three membership rows only (see the doc comment at the top).
    QSharedPointer<QCPAxisTickerText> yTicker(new QCPAxisTickerText);
    yTicker->addTick(kYBaselineRow, QString::fromUtf8("基线"));
    yTicker->addTick(kYCurrentRow, QString::fromUtf8("当前"));
    yTicker->addTick(kYNewRow, QString::fromUtf8("新增"));
    _chart->yAxis->setTicker(yTicker);
    _chart->yAxis->setRange(0.0, 2.0);

    auto* xTicker = new QCPAxisTickerFixed;
    xTicker->setTickStep((hi - lo) / 6.0);
    _chart->xAxis->setTicker(QSharedPointer<QCPAxisTicker>(xTicker));

    _chart->legend->setVisible(true);
    _chart->legend->setBrush(QBrush(QColor(255, 255, 255, 235)));
    _chart->legend->setBorderPen(QPen(chartGridColor()));
    _chart->legend->setFont(font9);
    _chart->axisRect()->insetLayout()->setInsetAlignment(0, Qt::AlignRight | Qt::AlignTop);

    _chart->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom | QCP::iSelectPlottables
                            | QCP::iSelectLegend);
    _chart->replot();

    auto* layout = qobject_cast<QVBoxLayout*>(this->layout());
    if (layout) {
        layout->addWidget(_chart);

        auto* hint = new QLabel(
            QString::fromUtf8("注：纵轴三行仅为基线/当前/新增的示意位置，非物理量。"), this);
        hint->setWordWrap(true);
        hint->setStyleSheet(QString("color:%1;font-size:10px;")
            .arg(QString::fromLatin1(AgentTheme::TEXT_TERTIARY)));
        layout->addWidget(hint);
    }
}

QLabel* BaselineCompareCard::makeSummaryLabel()
{
    auto* label = new QLabel(this);
    label->setWordWrap(true);
    label->setTextFormat(Qt::RichText);
    label->setStyleSheet(QString("color:%1;font-size:11px;")
        .arg(QString::fromLatin1(AgentTheme::TEXT_SECONDARY)));
    return label;
}
