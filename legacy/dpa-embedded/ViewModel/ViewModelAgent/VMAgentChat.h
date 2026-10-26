#pragma once
#include "HeaderDefine.h"
#include <QString>
namespace ViewModel {
    class VMAgentChat : public VM {
        Q_OBJECT
    public:
        VMAgentChat();
        virtual ~VMAgentChat();
        void init();
        void setUpCmd();
        virtual void onPropertyChanged(const std::string& Index, utils::Property* prop);
    public slots:
        void onReplyReady(QString reply);
        void onConfirmationRequired(QString confirmationId, QString toolName, QString argsSummary);
        void onAgentStep(QString kind, QString name, QString detail);
        void onCompanionStatusChanged(QString statusJson);
        void onCompanionEvent(QString eventJson);
    signals:
        void sigStep(QString kind, QString name, QString detail);
        void sigConfirm(QString confirmationId, QString toolName, QString argsSummary);
        void sigCompanionStatus(QString statusJson);
        void sigCompanionEvent(QString eventJson);
    public:
        std::string _history        = "";
        std::string _input          = "";
        std::string _attachments    = "";
        std::string _status         = "idle";
        std::string _output         = "";
        std::string _pendingConfirm = "";
        std::string _pendingConfirmationId;
        std::string _confirmationDecisionId;
        std::string _sessionsJson   = R"({"ok":true,"sessions":[]})";
        std::string _resumeSessionId;
        std::string _resumedSessionJson;
        std::string _runtimeInfoJson;
        std::string _companionStatusJson = R"({"enabled":false,"state":"off"})";
        std::string _companionConfigJson = R"({"enabled":false})";
        std::string _companionEventJson;
        std::string _companionConfigRequest;
        std::string _companionConfigResult;
        std::string _companionExplainEventJson;
        std::string _companionAcknowledgeRequest;
        std::string _companionAcknowledgeResult;
        std::string _operatorFeedbackJson;
        std::string _operatorFeedbackResult;
        bool _companionEnabledRequest = false;
        bool _analysisPending = false;
    private:
        bool explainCompanionEvent(const QString& eventJson, bool automatic);
    };
}
