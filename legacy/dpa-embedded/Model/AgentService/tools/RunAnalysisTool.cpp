#include "tools/RunAnalysisTool.h"
#include "AgentJson.h"
#include <QJsonObject>
#include <algorithm>
#include <array>
using namespace Model;

RunAnalysisArgs Model::parseRunAnalysisArgs(const std::string& argsJson) {
    RunAnalysisArgs a;
    QJsonObject o = Model::AgentJson::parseObject(argsJson);
    if (o.contains("type"))         a.type         = o.value("type").toString().toStdString();
    if (o.contains("frameSize"))    a.frameSize    = o.value("frameSize").toInt(a.frameSize);
    if (o.contains("averageNum"))   a.averageNum   = o.value("averageNum").toInt(a.averageNum);
    if (o.contains("overlapIndex")) a.overlapIndex = o.value("overlapIndex").toInt(a.overlapIndex);
    return a;
}

AnalysisValidation Model::validateAnalysisRequest(const RunAnalysisArgs& a) {
    AnalysisValidation v;
    if (a.type.empty()) {
        v.ok = false;
        v.error = "missing_type";
        return v;
    }
    // Whitelist of supported analysis types. Anything outside is rejected so a
    // model can never drive an arbitrary/unknown process type into the engine.
    // Curated to SINGLE-input, NON-mutating analyses only: excludes dual/reference-
    // channel types (COH/CPSD/CPS/TRANS/FREQTRANS/CROSS*/COLAFREQ/LS), destructive data ops
    // (SLIP/ADZERO/CROP/MOVE/JOINDATA), and signal generators / window modifiers
    // (SIG*/WIN*). These are framework keys from DataProcessService/MyActivator.
    // NOTE: FREQTRANS inherits DataProcessDualFreq (reads a reference channel) like TRANS —
    // the single-channel agent path never sets a ref, so it must be excluded, not whitelisted.
    static const std::array<const char*, 24> kWhitelist = {
        "FFT", "DFT", "ESD", "FSD", "PSD", "PS", "RMSSPECTRUM", "SPL", "SRS", "RF",
        "FIR", "IIR", "DIFF", "INTEGR", "RESAMP", "DELAP", "AUTOC", "AUTO",
        "AVGPRO", "PEAKPRO", "ABSPEAKPRO", "PEAKCOEPRO", "RMSPRO", "SINEPROCESS"
    };
    const bool supported = std::any_of(kWhitelist.begin(), kWhitelist.end(),
        [&](const char* type) { return a.type == type; });
    if (!supported) {
        v.ok = false;
        v.error = "unsupported_analysis";
        return v;
    }

    static const std::array<int, 9> kFrameSizes = {
        128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768
    };
    if (a.frameSize != 0
        && std::find(kFrameSizes.begin(), kFrameSizes.end(), a.frameSize) == kFrameSizes.end()) {
        v.ok = false;
        v.error = "invalid_frame_size";
    } else if (a.averageNum < 0 || a.averageNum > 99) {
        v.ok = false;
        v.error = "invalid_average_count";
    } else if (a.overlapIndex < -1 || a.overlapIndex > 5) {
        v.ok = false;
        v.error = "invalid_overlap_index";
    }
    return v;
}

ToolResult Model::buildRunAnalysisResult(const AnalysisValidation& v,
                                         const AnalysisOutcome& o,
                                         const RunAnalysisArgs& a) {
    ToolResult r;
    if (!v.ok) {
        r.ok = false;
        r.error = v.error;
        if (v.error == "missing_type") r.summary = "缺少类型";
        else if (v.error == "unsupported_analysis") r.summary = "不支持的分析类型: " + a.type;
        else if (v.error == "invalid_frame_size") r.summary = "谱线数必须是 128 到 32768 的受支持二次幂。";
        else if (v.error == "invalid_average_count") r.summary = "平均次数必须在 1 到 99 之间，或省略该参数。";
        else if (v.error == "invalid_overlap_index") r.summary = "Overlap 索引必须在 0 到 5 之间，或省略该参数。";
        else r.summary = "分析参数无效。";
        return r;
    }
    if (!o.started) {
        r.ok = false;
        r.error = o.error.empty() ? std::string("analysis_failed") : o.error;
        r.summary = std::string("分析执行失败: ") + a.type;
        return r;
    }
    r.ok = true;
    r.summary = "已对当前试验执行 " + a.type + " 分析";
    QJsonObject d;
    d["type"]    = QString::fromStdString(a.type);
    d["started"] = true;
    d["result"]  = QString::fromStdString(o.resultName);
    r.dataJson = Model::AgentJson::dump(d);
    return r;
}
