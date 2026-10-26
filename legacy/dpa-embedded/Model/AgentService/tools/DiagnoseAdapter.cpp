#include "Diagnostics.h"
#include "iTest.h"
#include "iDataProcess.h"
#include <algorithm>
#include <array>
#include <cmath>
using namespace Model;

namespace {
bool supportedDiagnosticSpectrum(const std::string& processType) {
    // Only conventional single-channel frequency spectra whose X axis is Hz and
    // whose relative line magnitudes can support the current harmonic rules.
    // SPL/SRS/RF/cross spectra and time/statistical results are intentionally excluded.
    static const std::array<const char*, 4> supported = {
        "FFT", "DFT", "PS", "RMSSPECTRUM"
    };
    return std::any_of(supported.begin(), supported.end(), [&](const char* value) {
        return processType == value;
    });
}
}

std::string Model::validateCurrentMeasurementBinding(M* dpaTest,
                                                     const MeasurementProfile& measurement) {
    if (!dpaTest) return "no_current_test";
    iTest* test = static_cast<iTest*>(dpaTest);
    iDataProcess* process = test->value<iDataProcess*>("PlotSet", "CurrentLayout", "CurrentCell", "Process");
    if (!process || process->childrenCount("Units") <= 0)
        return "current_result_provenance_unavailable";
    iData* source = process->value<iData*>("Units", 0, "SourceSelect", "Data");
    if (!source) return "current_result_provenance_unavailable";

    MeasurementBinding actual;
    actual.dataId = source->value<std::string>("Id");
    actual.channel = process->value<int>("Units", 0, "SourceSelect", "ChannelId");

    int channelIndex = -1;
    const int channelCount = source->childrenCount("Channels");
    for (int i = 0; i < channelCount; ++i) {
        if (source->value<int>("Channels", i, "ChannelId") == actual.channel) {
            channelIndex = i;
            break;
        }
    }
    if (channelIndex < 0) return "current_result_provenance_unavailable";
    actual.unit = source->value<std::string>("Channels", channelIndex, "Unit");
    actual.samplingRateHz = source->value<double>("Channels", channelIndex, "SamplingRate");
    return validateMeasurementBinding(measurement, actual);
}

// Adapter: extract the current supported spectrum. The pure diagnostic engine is
// invoked by the tool handler only after domain/provenance checks succeed.
CurrentDiagnosticSpectrum Model::currentDiagnosticSpectrum(M* dpaTest) {
    CurrentDiagnosticSpectrum out;
    if (!dpaTest) { out.error = "no_current_test"; return out; }
    iTest* test = (iTest*)dpaTest;
    auto proc = test->value<iDataProcess*>("PlotSet", "CurrentLayout", "CurrentCell", "Process");
    if (!proc) { out.error = "no_current_result"; return out; }
    out.analysisType = proc->value<std::string>("ProcessType");
    if (proc->value<std::string>("ProcessCategory") != "SpectrumDominProcess"
        || !supportedDiagnosticSpectrum(out.analysisType)) {
        out.error = "unsupported_result_domain";
        return out;
    }
    iData* result = proc->value<iData*>("PublishedResultData");
    if (!result) result = proc->value<iData*>("ResultData");
    if (!result) result = proc->value<iData*>("LastResultData");
    if (!result) { out.error = "no_current_result"; return out; }
    if ((result->value<int>("DataForm") & iData::FreqForm) == 0) {
        out.error = "unsupported_result_domain";
        return out;
    }
    const int chCount = result->childrenCount("Channels");
    if (chCount <= 0) { out.error = "no_current_result"; return out; }
    const int channelId = result->value<int>("Channels", 0, "ChannelId");
    std::vector<double> x, y;
    if (!result->getThinXY(channelId, 0.0, 0.0, x, y) || y.empty()) {
        out.error = "no_current_result";
        return out;
    }
    if (x.size() != y.size() || x.size() < 3) {
        out.error = "invalid_spectrum_axis";
        return out;
    }
    iData* source = proc->value<iData*>("Units", 0, "SourceSelect", "Data");
    const int sourceChannel = proc->value<int>("Units", 0, "SourceSelect", "ChannelId");
    double samplingRate = 0;
    if (source) {
        const int sourceChannelCount = source->childrenCount("Channels");
        for (int i = 0; i < sourceChannelCount; ++i) {
            if (source->value<int>("Channels", i, "ChannelId") == sourceChannel) {
                samplingRate = source->value<double>("Channels", i, "SamplingRate");
                break;
            }
        }
    }
    if (!(samplingRate > 0)) {
        out.error = "current_result_provenance_unavailable";
        return out;
    }
    for (size_t i = 0; i < x.size(); ++i) {
        if (!std::isfinite(x[i]) || !std::isfinite(y[i]) || x[i] < 0
            || (i > 0 && x[i] <= x[i - 1])) {
            out.error = "invalid_spectrum_axis";
            return out;
        }
    }
    const std::string rangeError = validateFrequencyRange(samplingRate, x.back());
    if (!rangeError.empty()) {
        out.error = rangeError;
        return out;
    }
    out.peaks = findTopNPeaks(x, y, 12);
    out.valid = true;
    return out;
}
