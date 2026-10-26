#include "EnvelopeAnalysis.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

int main() {
    constexpr double pi = 3.14159265358979323846;
    constexpr int n = 1024;
    std::vector<double> carrier(n), modulated(n);
    for (int i = 0; i < n; ++i) {
        carrier[i] = std::cos(2*pi*128*i/n);
        modulated[i] = (1 + 0.5*std::cos(2*pi*16*i/n))*carrier[i];
    }
    auto envelope = Model::hilbertEnvelope(carrier);
    if (envelope.size() != n) return 1;
    for (double value : envelope) if (std::abs(value-1) > 1e-10) return 2;
    const auto spectrum = Model::envelopeSpectrum(modulated, 1024, 0, 0);
    if (spectrum.mag.size() < 17 || spectrum.freqHz[16] != 16) return 3;
    if (std::abs(spectrum.mag[16]-0.5) > 1e-10) return 4;
    const auto scaled = Model::envelopeSpectrum(modulated, 2048, 0, 0);
    if (scaled.freqHz[16] != 32 || std::abs(scaled.mag[16]-spectrum.mag[16]) > 1e-12) return 5;
    if (!Model::hilbertEnvelope({}).empty()) return 6;
    if (!Model::hilbertEnvelope({std::numeric_limits<double>::quiet_NaN()}).empty()) return 7;
    if (!Model::envelopeSpectrum(modulated, 0, 0, 0).mag.empty()) return 8;
    std::cout << "Analytical Hilbert, envelope amplitude, sample-rate and invalid-input checks passed\n";
}
