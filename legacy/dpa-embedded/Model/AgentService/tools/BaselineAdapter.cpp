#include "BaselineMonitor.h"
#include "Diagnostics.h"     // findTopNPeaks
#include "iTest.h"
#include "iDataProcess.h"
#include <algorithm>
#include <array>
#include <cmath>
using namespace Model;

namespace {
bool supportedBaselineSpectrum(const std::string& processType) {
    static const std::array<const char*, 4> supported = {"FFT", "DFT", "PS", "RMSSPECTRUM"};
    return std::any_of(supported.begin(), supported.end(), [&](const char* value) {
        return processType == value;
    });
}
}

// Adapter: extract the current result's dominant peak frequencies and exact provenance for
// baseline capture/compare. Framework-coupled, plugin-only, build-verified.
// Same result-resolution path as DiagnoseAdapter (PlotSet->CurrentLayout->CurrentCell->Process).
MachineBaseline Model::currentBaselineFeatures(M* dpaTest, int topN) {
    MachineBaseline b;   // valid stays false until we have real data
    if (!dpaTest) { b.error = "no_current_test"; return b; }
    iTest* test = (iTest*)dpaTest;
    auto proc = test->value<iDataProcess*>("PlotSet", "CurrentLayout", "CurrentCell", "Process");
    if (!proc) { b.error = "no_current_result"; return b; }
    b.provenance.processType = proc->value<std::string>("ProcessType");
    if (proc->value<std::string>("ProcessCategory") != "SpectrumDominProcess"
        || !supportedBaselineSpectrum(b.provenance.processType)) {
        b.error = "unsupported_result_domain";
        return b;
    }
    iData* result = proc->value<iData*>("PublishedResultData");
    if (!result) result = proc->value<iData*>("ResultData");
    if (!result) result = proc->value<iData*>("LastResultData");
    if (!result) { b.error = "no_current_result"; return b; }
    b.provenance.resultDataForm = result->value<int>("DataForm");
    if ((b.provenance.resultDataForm & iData::FreqForm) == 0) {
        b.error = "unsupported_result_domain";
        return b;
    }
    const int chCount = result->childrenCount("Channels");
    if (chCount <= 0) { b.error = "no_current_result"; return b; }
    const int channelId = result->value<int>("Channels", 0, "ChannelId");
    b.provenance.resultUnit = result->value<std::string>("Channels", 0, "Unit");
    std::vector<double> x, y;
    if (!result->getThinXY(channelId, 0.0, 0.0, x, y) || y.empty()) {
        b.error = "no_current_result";
        return b;
    }
    if (x.size() != y.size() || x.size() < 3) {
        b.error = "invalid_spectrum_axis";
        return b;
    }

    iData* source = proc->value<iData*>("Units", 0, "SourceSelect", "Data");
    b.provenance.sourceChannel = proc->value<int>("Units", 0, "SourceSelect", "ChannelId");
    if (!source) { b.error = "current_result_provenance_unavailable"; return b; }
    b.provenance.sourceDataId = source->value<std::string>("Id");
    const int sourceChannelCount = source->childrenCount("Channels");
    for (int i = 0; i < sourceChannelCount; ++i) {
        if (source->value<int>("Channels", i, "ChannelId") == b.provenance.sourceChannel) {
            b.provenance.sourceUnit = source->value<std::string>("Channels", i, "Unit");
            b.provenance.sourcePhysicalType = source->value<std::string>("Channels", i, "PhysicType");
            b.provenance.samplingRateHz = source->value<double>("Channels", i, "SamplingRate");
            break;
        }
    }
    if (b.provenance.sourceDataId.empty() || b.provenance.sourceUnit.empty()
        || !(b.provenance.samplingRateHz > 0)) {
        b.error = "current_result_provenance_unavailable";
        return b;
    }
    for (size_t i = 0; i < x.size(); ++i) {
        if (!std::isfinite(x[i]) || !std::isfinite(y[i]) || x[i] < 0
            || (i > 0 && x[i] <= x[i - 1])) {
            b.error = "invalid_spectrum_axis";
            return b;
        }
    }
    b.error = validateFrequencyRange(b.provenance.samplingRateHz, x.back());
    if (!b.error.empty()) return b;

    for (const auto& p : findTopNPeaks(x, y, topN > 0 ? topN : 8)) b.peakFreqsHz.push_back(p.freqHz);
    if (b.peakFreqsHz.empty()) { b.error = "no_peaks"; return b; }
    b.provenance.resultFunctionType = proc->value<std::string>("ResultFunctionType");
    b.provenance.axisTypeX = proc->value<int>("AxisTypeX");
    b.provenance.frameSize = proc->value<int>("FrameSize");
    b.provenance.averageNum = proc->value<int>("AverageNum");
    b.provenance.overlapIndex = proc->value<int>("OverLapIndex");
    b.provenance.overlapValue = proc->value<double>("OverLapValue");
    b.provenance.windowType = proc->value<int>("WinType");
    b.provenance.configFingerprint = baselineConfigFingerprint(b.provenance);
    b.configFingerprint = b.provenance.configFingerprint;
    b.valid = true;
    return b;
}
