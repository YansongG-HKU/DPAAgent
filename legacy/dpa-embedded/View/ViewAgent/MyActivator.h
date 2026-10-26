#pragma once
#include "HeaderDefine.h"
class ViewAgentActivator : public iBundleActivator
{
public:
	void start(iBundleContext* context) override;
	void afterStart(iBundleContext* context) override;
	void stop(iBundleContext* context) override;
};
