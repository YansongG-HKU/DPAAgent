#include <QtTest/QtTest>
#include "DiagnosticRun.h"

using namespace Model;

class TstDiagnosticRun : public QObject {
    Q_OBJECT
private slots:
    void enforces_recovery_and_terminal_states() {
        DiagnosticRun run(100);
        QCOMPARE((int)run.state(), (int)DiagnosticRunState::ContextReady);
        QVERIFY(!run.apply(DiagnosticRunEvent::CompleteAnalysis, 101));
        QVERIFY(!run.apply(DiagnosticRunEvent::StartAnalysis, 99));
        QCOMPARE(run.updatedAt(), 100LL);

        QVERIFY(run.apply(DiagnosticRunEvent::StartAnalysis, 110));
        QVERIFY(!run.apply(DiagnosticRunEvent::Fail, 111));
        QVERIFY(run.apply(DiagnosticRunEvent::Fail, 120, "analysis timeout"));
        QCOMPARE((int)run.state(), (int)DiagnosticRunState::Failed);
        QCOMPARE((int)run.currentStep(), (int)DiagnosticRunState::AnalysisPending);
        QCOMPARE(QString::fromStdString(run.error()), QString("analysis timeout"));

        DiagnosticRun restored;
        QVERIFY(DiagnosticRun::fromJson(run.toJson(), restored));
        QCOMPARE((int)restored.state(), (int)DiagnosticRunState::Failed);
        QCOMPARE((int)restored.currentStep(), (int)DiagnosticRunState::AnalysisPending);
        QCOMPARE(restored.updatedAt(), 120LL);
        QVERIFY(!restored.apply(DiagnosticRunEvent::CompleteAnalysis, 121));
        QVERIFY(restored.apply(DiagnosticRunEvent::Retry, 130));
        QCOMPARE((int)restored.state(), (int)DiagnosticRunState::AnalysisPending);
        QVERIFY(restored.error().empty());

        QVERIFY(restored.apply(DiagnosticRunEvent::CompleteAnalysis, 140));
        QVERIFY(restored.apply(DiagnosticRunEvent::CompleteDiagnosis, 150));
        QVERIFY(restored.apply(DiagnosticRunEvent::RequestReport, 160));
        QVERIFY(restored.apply(DiagnosticRunEvent::CompleteReport, 170));
        QCOMPARE((int)restored.state(), (int)DiagnosticRunState::Complete);
        QVERIFY(restored.isTerminal());
        QVERIFY(!restored.apply(DiagnosticRunEvent::Cancel, 180));

        DiagnosticRun cancelled(200);
        QVERIFY(cancelled.apply(DiagnosticRunEvent::StartAnalysis, 210));
        QVERIFY(cancelled.apply(DiagnosticRunEvent::Fail, 220, "device offline"));
        QVERIFY(cancelled.apply(DiagnosticRunEvent::Cancel, 230));
        QCOMPARE((int)cancelled.state(), (int)DiagnosticRunState::Cancelled);
        QCOMPARE((int)cancelled.currentStep(), (int)DiagnosticRunState::AnalysisPending);
        QCOMPARE(QString::fromStdString(cancelled.error()), QString("device offline"));
        QVERIFY(cancelled.isTerminal());
        QVERIFY(!cancelled.apply(DiagnosticRunEvent::Retry, 240));

        DiagnosticRun interruptedAnalysis(300);
        QVERIFY(interruptedAnalysis.apply(DiagnosticRunEvent::StartAnalysis, 310));
        QVERIFY(interruptedAnalysis.interruptPendingAfterRestart(305));
        QCOMPARE((int)interruptedAnalysis.state(), (int)DiagnosticRunState::Failed);
        QCOMPARE((int)interruptedAnalysis.currentStep(), (int)DiagnosticRunState::AnalysisPending);
        QCOMPARE(interruptedAnalysis.error(), std::string("interrupted_by_restart"));
        QCOMPARE(interruptedAnalysis.updatedAt(), 310LL);
        QVERIFY(interruptedAnalysis.apply(DiagnosticRunEvent::Retry, 320));

        DiagnosticRun interruptedReport(400);
        QVERIFY(interruptedReport.apply(DiagnosticRunEvent::StartAnalysis, 410));
        QVERIFY(interruptedReport.apply(DiagnosticRunEvent::CompleteAnalysis, 420));
        QVERIFY(interruptedReport.apply(DiagnosticRunEvent::CompleteDiagnosis, 430));
        QVERIFY(interruptedReport.apply(DiagnosticRunEvent::RequestReport, 440));
        QVERIFY(interruptedReport.interruptPendingAfterRestart(450));
        QCOMPARE((int)interruptedReport.state(), (int)DiagnosticRunState::Failed);
        QCOMPARE((int)interruptedReport.currentStep(), (int)DiagnosticRunState::ReportPending);
        QCOMPARE(interruptedReport.error(), std::string("interrupted_by_restart"));

        DiagnosticRun unchanged(300);
        QVERIFY(!DiagnosticRun::fromJson(
            R"({"state":"unknown","current_step":"context_ready","error":"","updated_at":1})", unchanged));
        QCOMPARE((int)unchanged.state(), (int)DiagnosticRunState::ContextReady);
        QCOMPARE(unchanged.updatedAt(), 300LL);
    }
};

int runTstDiagnosticRun(int argc, char** argv) {
    TstDiagnosticRun test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_diagnosticrun.moc"
