#ifndef CAMPAIGNPANEL_H
#define CAMPAIGNPANEL_H

#include <QtWidgets/QWidget>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>

// Experiment-campaign status panel: live run-by-run progress with evidence
// bindings, plus the cross-session knowledge ledger. Data comes exclusively
// from governed tool payloads and read-only snapshots.
class CampaignPanel : public QWidget
{
    Q_OBJECT
public:
    explicit CampaignPanel(QWidget* parent = nullptr);

    void applyCampaignResult(const QString& resultJson);
    void applyStatusSnapshot(const QString& statusJson);
    void refreshFromService();

protected:
    void showEvent(QShowEvent* event) override;

private:
    void buildUi();
    void setStateText(const QString& state, const QString& stopReason);

    QLabel* _stateDot = nullptr;
    QLabel* _stateLabel = nullptr;
    QPushButton* _refreshBtn = nullptr;
    QTableWidget* _runsTable = nullptr;
    QTableWidget* _knowledgeTable = nullptr;
};

#endif // CAMPAIGNPANEL_H
