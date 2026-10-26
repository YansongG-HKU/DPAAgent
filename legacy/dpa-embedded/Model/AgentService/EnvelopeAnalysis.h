#pragma once
#include <vector>
namespace Model {
    // Envelope (amplitude-demodulation) spectrum, the key to rolling-element bearing
    // diagnosis. Pure-core (no framework/fftw) so it is fully unit-testable:
    //   optional band-pass -> analytic-signal magnitude (Hilbert) -> spectrum of the envelope.
    struct EnvSpectrum { std::vector<double> freqHz; std::vector<double> mag; };

    // Hilbert amplitude envelope of a real signal (same length as input).
    std::vector<double> hilbertEnvelope(const std::vector<double>& x);

    // Envelope spectrum of a time waveform sampled at fs Hz. If bandHiHz>bandLoHz>0 a
    // frequency-domain band-pass is applied first (isolate the resonance band that carries
    // the bearing impacts). Returns the one-sided magnitude spectrum of the envelope.
    EnvSpectrum envelopeSpectrum(const std::vector<double>& waveform, double fs,
                                 double bandLoHz, double bandHiHz);
}
