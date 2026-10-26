#include <QtTest/QtTest>
#include "tools/RunAnalysisTool.h"
#include "AgentJson.h"
using namespace Model;
class TstRunAnalysis : public QObject { Q_OBJECT
private slots:
    // Whitelist accepts a supported analysis type.
    void whitelist_accepts_PSD() {
        RunAnalysisArgs a; a.type = "PSD";
        AnalysisValidation v = validateAnalysisRequest(a);
        QVERIFY(v.ok);
    }
    // Whitelist rejects an unknown analysis type.
    void rejects_unknown() {
        RunAnalysisArgs a; a.type = "WUBBA";
        AnalysisValidation v = validateAnalysisRequest(a);
        QVERIFY(!v.ok);
        QCOMPARE(QString::fromStdString(v.error), QString("unsupported_analysis"));
    }
    // Widened whitelist accepts more single-input spectral analyses...
    void whitelist_accepts_widened_spectra() {
        for (const char* t : {"PS", "ESD", "DFT", "RMSSPECTRUM", "INTEGR", "SINEPROCESS"}) {
            RunAnalysisArgs a; a.type = t;
            QVERIFY2(validateAnalysisRequest(a).ok, t);
        }
    }
    // ...but still rejects dual/reference-channel and destructive data-mutation types.
    void whitelist_rejects_dual_and_mutating() {
        for (const char* t : {"COH", "CPSD", "TRANS", "FREQTRANS", "MOVEDATA", "JOINDATA", "CROPDATA", "SIGSIN"}) {
            RunAnalysisArgs a; a.type = t;
            QVERIFY2(!validateAnalysisRequest(a).ok, t);
        }
    }
    // Empty type is reported distinctly as missing.
    void rejects_empty() {
        RunAnalysisArgs a; a.type = "";
        AnalysisValidation v = validateAnalysisRequest(a);
        QVERIFY(!v.ok);
        QCOMPARE(QString::fromStdString(v.error), QString("missing_type"));
    }
    void rejects_engine_parameter_values_outside_ui_contract() {
        RunAnalysisArgs badFrame; badFrame.type = "FFT"; badFrame.frameSize = 1000;
        QCOMPARE(QString::fromStdString(validateAnalysisRequest(badFrame).error),
                 QString("invalid_frame_size"));
        RunAnalysisArgs badAverage; badAverage.type = "FFT"; badAverage.averageNum = 100;
        QCOMPARE(QString::fromStdString(validateAnalysisRequest(badAverage).error),
                 QString("invalid_average_count"));
        RunAnalysisArgs badOverlap; badOverlap.type = "FFT"; badOverlap.overlapIndex = 6;
        QCOMPARE(QString::fromStdString(validateAnalysisRequest(badOverlap).error),
                 QString("invalid_overlap_index"));

        RunAnalysisArgs valid; valid.type = "FFT"; valid.frameSize = 32768;
        valid.averageNum = 99; valid.overlapIndex = 5;
        QVERIFY(validateAnalysisRequest(valid).ok);
    }
    // On a started run the result carries the type + started flag.
    void result_on_started() {
        AnalysisValidation v; v.ok = true;
        AnalysisOutcome o; o.started = true; o.resultName = "PSD";
        RunAnalysisArgs a; a.type = "PSD";
        ToolResult r = buildRunAnalysisResult(v, o, a);
        QVERIFY(r.ok);
        QJsonObject d = AgentJson::parseObject(r.dataJson);
        QCOMPARE(d.value("type").toString(), QString("PSD"));
        QVERIFY(d.value("started").toBool());
    }
    // An invalid request surfaces the validation error verbatim and is not ok.
    void result_on_invalid() {
        AnalysisValidation v; v.ok = false; v.error = "unsupported_analysis";
        AnalysisOutcome o;
        RunAnalysisArgs a; a.type = "X";
        ToolResult r = buildRunAnalysisResult(v, o, a);
        QVERIFY(!r.ok);
        QCOMPARE(QString::fromStdString(r.error), QString("unsupported_analysis"));
    }
    // Arg parsing reads the type and an explicit frameSize.
    void parse_reads_params() {
        RunAnalysisArgs a = parseRunAnalysisArgs(R"({"type":"FFT","frameSize":1024})");
        QCOMPARE(QString::fromStdString(a.type), QString("FFT"));
        QCOMPARE(a.frameSize, 1024);
    }
};
int runTstRunAnalysis(int argc,char**argv){ TstRunAnalysis t; return QTest::qExec(&t,argc,argv); }
#include "tst_runanalysis.moc"
