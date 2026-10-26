#include "RibbonPageAgent.h"
#include "QtWidgetBase.h"
#include "HeaderDefine.h"
#include "iAgentService.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalBlocker>

RibbonPageAgent::RibbonPageAgent(QWidget* parent /*= nullptr*/) : RibbonPageBase(parent)
{
	agentGroup();

	// Global page: always visible and never switches the current test.
	setProperty("TabType", "AI助手");
	setProperty("TabCategory", "GlobalTab");
}

RibbonPageAgent::~RibbonPageAgent()
{
}

void RibbonPageAgent::agentGroup()
{
	RibbonPage* page = this;
	RibbonGroup* group = page->addGroup("AI助手");

	_toggleDockAct = group->addAction(QIcon(), "对话面板", Qt::ToolButtonTextUnderIcon);
	_toggleDockAct->setCheckable(true);
	_toggleDockAct->setChecked(false);

	connect(_toggleDockAct, &QAction::toggled, this, [this](bool on) {
		emit sigToggleDock(on);
	});

	_companionAct = group->addAction(QIcon(), "智能陪试", Qt::ToolButtonTextUnderIcon);
	_companionAct->setCheckable(true);
	_companionAct->setToolTip("本地监测采集、通道限值和数据连续性；不会自动停机");
	auto service = GetDefaultService(Model::iAgentService);
	if (!service) {
		_companionAct->setEnabled(false);
		return;
	}
	_companionAct->setChecked(service->companionEnabled());
	connect(_companionAct, &QAction::toggled, this, [service](bool on) {
		service->setCompanionEnabled(on);
	});
	connect(service, SIGNAL(companionStatusChanged(QString)),
		this, SLOT(onCompanionStatus(QString)));
}

void RibbonPageAgent::onCompanionStatus(QString statusJson)
{
	if (!_companionAct) return;
	const bool enabled = QJsonDocument::fromJson(statusJson.toUtf8()).object()
		.value("enabled").toBool(false);
	QSignalBlocker blocker(_companionAct);
	_companionAct->setChecked(enabled);
}
