#include "EnvelopeAnalysis.h"
#include <cmath>
#include <cstddef>
#include <algorithm>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
using namespace Model;

static size_t nextPow2(size_t n) { size_t p = 1; while (p < n) p <<= 1; return p; }

// In-place iterative radix-2 Cooley-Tukey FFT (inverse when `inv`). Size must be a power of 2.
static void fft(std::vector<double>& re, std::vector<double>& im, bool inv) {
    const size_t n = re.size();
    if (n <= 1) return;
    for (size_t i = 1, j = 0; i < n; ++i) {         // bit-reversal permutation
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2.0 * M_PI / (double)len * (inv ? 1.0 : -1.0);
        const double wr = std::cos(ang), wi = std::sin(ang);
        for (size_t i = 0; i < n; i += len) {
            double cwr = 1.0, cwi = 0.0;
            for (size_t k = 0; k < len / 2; ++k) {
                const double xr = re[i + k + len/2], xi = im[i + k + len/2];
                const double vr = xr * cwr - xi * cwi;
                const double vi = xr * cwi + xi * cwr;
                const double ur = re[i + k], ui = im[i + k];
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len/2] = ur - vr; im[i + k + len/2] = ui - vi;
                const double ncwr = cwr * wr - cwi * wi;
                cwi = cwr * wi + cwi * wr; cwr = ncwr;
            }
        }
    }
    if (inv) for (size_t i = 0; i < n; ++i) { re[i] /= (double)n; im[i] /= (double)n; }
}

std::vector<double> Model::hilbertEnvelope(const std::vector<double>& x) {
    const size_t orig = x.size();
    if (orig == 0) return {};
    if (!std::all_of(x.begin(), x.end(), [](double value) { return std::isfinite(value); }))
        return {};
    if (orig == 1) return {std::abs(x.front())};
    const size_t N = nextPow2(orig);
    std::vector<double> re(N, 0.0), im(N, 0.0);
    for (size_t i = 0; i < orig; ++i) re[i] = x[i];
    fft(re, im, false);
    // Analytic-signal filter: keep DC & Nyquist, double positive freqs, zero negatives.
    for (size_t i = 1; i < N/2; ++i) { re[i] *= 2.0; im[i] *= 2.0; }
    for (size_t i = N/2 + 1; i < N; ++i) { re[i] = 0.0; im[i] = 0.0; }
    fft(re, im, true);
    std::vector<double> env(orig);
    for (size_t i = 0; i < orig; ++i) {
        env[i] = std::hypot(re[i], im[i]);
        if (!std::isfinite(env[i])) return {};
    }
    return env;
}

EnvSpectrum Model::envelopeSpectrum(const std::vector<double>& waveform, double fs,
                                    double bandLoHz, double bandHiHz) {
    EnvSpectrum out;
    const size_t orig = waveform.size();
    if (orig < 4 || !std::isfinite(fs) || fs <= 0
        || !std::isfinite(bandLoHz) || !std::isfinite(bandHiHz)
        || !std::all_of(waveform.begin(), waveform.end(),
                        [](double value) { return std::isfinite(value); })) return out;
    const bool noBand = bandLoHz == 0 && bandHiHz == 0;
    if (!noBand && (bandLoHz < 0 || bandHiHz <= bandLoHz || bandHiHz > fs / 2)) return out;

    std::vector<double> sig(waveform);

    // Optional frequency-domain band-pass to isolate the impact-carrying resonance band.
    if (bandHiHz > bandLoHz && bandLoHz >= 0) {
        const size_t N = nextPow2(orig);
        std::vector<double> re(N, 0.0), im(N, 0.0);
        for (size_t i = 0; i < orig; ++i) re[i] = waveform[i];
        fft(re, im, false);
        const double df = fs / (double)N;
        for (size_t i = 0; i <= N/2; ++i) {
            const double f = i * df;
            const bool keep = (f >= bandLoHz && f <= bandHiHz);
            if (!keep) {
                re[i] = im[i] = 0.0;
                if (i != 0 && i != N/2) { re[N-i] = im[N-i] = 0.0; }  // mirror negative freq
            }
        }
        fft(re, im, true);
        sig.assign(orig, 0.0);
        for (size_t i = 0; i < orig; ++i) sig[i] = re[i];
    }

    std::vector<double> env = hilbertEnvelope(sig);
    if (env.empty()) return out;
    // Remove DC so the envelope spectrum is not dominated by the mean level.
    double mean = 0; for (double v : env) mean += v; mean /= (double)env.size();
    for (double& v : env) v -= mean;

    const size_t N2 = nextPow2(env.size());
    std::vector<double> re(N2, 0.0), im(N2, 0.0);
    for (size_t i = 0; i < env.size(); ++i) re[i] = env[i];
    fft(re, im, false);
    const double df = fs / (double)N2;
    const size_t half = N2 / 2;
    out.freqHz.resize(half + 1);
    out.mag.resize(half + 1);
    for (size_t i = 0; i <= half; ++i) {
        out.freqHz[i] = i * df;
        // One-sided amplitude: interior bins fold in their conjugate partner (x2); DC (i==0)
        // and Nyquist (i==half) are self-conjugate and must NOT be doubled.
        const double ss = (i == 0 || i == half) ? 1.0 : 2.0;
        out.mag[i] = std::hypot(re[i], im[i]) * ss / (double)env.size();
        if (!std::isfinite(out.mag[i])) return {};
    }
    return out;
}
