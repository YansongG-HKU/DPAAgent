#pragma once
#include "HeaderDefine.h"

namespace Model { class AgentServiceImpl; }

class AgentServiceActivator : public iBundleActivator {
public:
    void start(iBundleContext* context) override;
    void afterStart(iBundleContext* context) override;
    void stop(iBundleContext* context) override;
public:
    Model::AgentServiceImpl* servicePtr = nullptr;
};
