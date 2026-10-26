#pragma once

#include <QtCore/QObject>
#include "RibbonPageBase.h"
#include <QtWidgets/QAction>
#include "HeaderDefine.h"

class RibbonPageAgent : public RibbonPageBase
{
	Q_OBJECT
public:
	RibbonPageAgent(QWidget* parent = nullptr);
	~RibbonPageAgent();

private:
	void agentGroup();

private slots:
	void onCompanionStatus(QString statusJson);

protected:
	QAction* _toggleDockAct = nullptr;
	QAction* _companionAct = nullptr;

signals:
	void sigToggleDock(bool show);
};
