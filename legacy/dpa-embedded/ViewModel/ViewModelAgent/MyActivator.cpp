#include "MyActivator.h"
#include "VMAgentChat.h"
using namespace ViewModel;
REGISTER_BUNDLE_ACTIVATOR_CLASS2(ViewModelAgentActivator)
REGISTER_REMOTE_BUNDLE_CLASS("AgentChat", VM, VMAgentChat)   // key "AgentChat", base VM
void ViewModelAgentActivator::start(iBundleContext* context) {}
void ViewModelAgentActivator::afterStart(iBundleContext* context) {}
void ViewModelAgentActivator::stop(iBundleContext* context) {}
