#include "MyActivator.h"
#include "AgentServiceImpl.h"
REGISTER_BUNDLE_ACTIVATOR_CLASS2(AgentServiceActivator)
using namespace Model;
void AgentServiceActivator::start(iBundleContext* context) {
    ServiceHelper<iAgentService> sh;
    servicePtr = new AgentServiceImpl;
    sh.addService(servicePtr);
}
void AgentServiceActivator::afterStart(iBundleContext* context) {
    servicePtr->initBindings();
}
void AgentServiceActivator::stop(iBundleContext* context) {
    ServiceHelper<iAgentService> sh;
    sh.removeService(servicePtr);
    SAFE_DELETE(servicePtr);
}
