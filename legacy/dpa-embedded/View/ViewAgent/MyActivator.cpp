#include "MyActivator.h"
#include "RibbonPageAgent.h"
#include "AgentChatDock.h"

REGISTER_BUNDLE_ACTIVATOR_CLASS2(ViewAgentActivator)
REGISTER_REMOTE_BUNDLE_CLASS("AI助手", RibbonPage, RibbonPageAgent)
REGISTER_REMOTE_BUNDLE_CLASS("AgentChat", QDockWidget, AgentChatDock)

void ViewAgentActivator::start(iBundleContext* context)
{
}

void ViewAgentActivator::afterStart(iBundleContext* context)
{
}

void ViewAgentActivator::stop(iBundleContext* context)
{
}
