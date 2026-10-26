#pragma once

// BaselineCompareCard — "baseline vs current" spectral comparison card.
// Renders a QCustomPlot marker chart driven by the compare_baseline tool result
// (fields emitted by Model::buildDeviationResult, wrapped by ToolResult::toJson).
// The card is purely presentational: it reads JSON fields and draws honest
// content. No model/service access; no fabricated numeric values.

#include <QFrame>
#include <QString>
#include <QVector>

class QCustomPlot;
class QLabel;

class BaselineCompareCard : public QFrame
{
public:
    explicit BaselineCompareCard(const QString& toolResultJson, QWidget* parent = nullptr);

private:
    struct DeviationPayload
    {
        QVector<double> baselineHz;      // peaks present in the baseline
        QVector<double> currentHz;       // peaks present in the current capture
        QVector<double> newHz;           // peaks present now but absent in the baseline
        bool hasPeakArrays = false;      // any of the three peak arrays existed in the JSON
        bool hasBaseline = true;
        bool isoWorsened = false;
        QString severity;                // info | warn | alarm
        QString summary;
    };

    static DeviationPayload parsePayload(const QString& toolResultJson);
    void buildUi(const DeviationPayload& payload);
    void buildChart(const DeviationPayload& payload);
    QLabel* makeSummaryLabel();

    QCustomPlot* _chart = nullptr;
    QLabel* _emptyHint = nullptr;
    QLabel* _peaksLine = nullptr;
    QLabel* _isoLine = nullptr;
    QLabel* _severityBadge = nullptr;
    QLabel* _summaryLabel = nullptr;
};
