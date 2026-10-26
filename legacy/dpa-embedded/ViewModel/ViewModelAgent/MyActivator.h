#pragma once
#include "HeaderDefine.h"
class ViewModelAgentActivator : public iBundleActivator {
public:
    void start(iBundleContext* context) override;
    void afterStart(iBundleContext* context) override;
    void stop(iBundleContext* context) override;
};
