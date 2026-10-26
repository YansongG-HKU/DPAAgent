#include "Diagnostics.h"
#include "EnvelopeAnalysis.h"
#include "iTest.h"   // Model::M / Model::iTest / Model::iData
using namespace Model;

// Adapter: envelope-demodulation spectrum of a raw time-domain channel waveform.
// Framework-coupled (iTest/iData), plugin-only, NOT unit-tested (build-verified); the DSP it
// calls (envelopeSpectrum) IS unit-tested in tst_envelope. Reads the RAW waveform (getXY, not the
// decimated getThinXY) because envelope demodulation must see the un-aliased impact modulation.
std::vector<SpectralPeak> Model::envelopeSpectrumPeaks(M* dpaTest, const std::string& dataId,
                                                       int channel, double bandLoHz, double bandHiHz,
                                                       int topN) {
    if (!dpaTest) return {};
    iTest* test = (iTest*)dpaTest;
    iData* data = (iData*)test->getData(dataId);
    if (!data) return {};

    // Sampling rate is a per-channel property; channel *id* != channel *index*, so find the
    // index whose ChannelId matches, then read its SamplingRate (fall back to channel 0).
    const int chCount = data->childrenCount("Channels");
    double fs = 0.0;
    for (int i = 0; i < chCount; ++i) {
        if (data->value<int>("Channels", i, "ChannelId") == channel) {
            fs = data->value<double>("Channels", i, "SamplingRate");
            break;
        }
    }
    if (fs <= 0.0 && chCount > 0) fs = data->value<double>("Channels", 0, "SamplingRate");
    if (fs <= 0.0) return {};

    // Raw, un-decimated waveform (getXY); fall back to getThinXY only if getXY is unavailable.
    std::vector<double> x, y;
    if (!data->getXY(channel, x, y) || y.size() < 8) {
        if (!data->getThinXY(channel, 0.0, 0.0, x, y) || y.size() < 8) return {};
    }
    // Cap the segment length so the FFT stays bounded while preserving the raw sample rate.
    const size_t CAP = 1u << 16;   // 65536 samples
    if (y.size() > CAP) y.resize(CAP);

    EnvSpectrum spec = envelopeSpectrum(y, fs, bandLoHz, bandHiHz);
    return findTopNPeaks(spec.freqHz, spec.mag, topN > 0 ? topN : 10);
}
