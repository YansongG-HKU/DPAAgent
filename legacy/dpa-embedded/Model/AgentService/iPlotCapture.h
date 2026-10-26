#pragma once
#include <string>
namespace Model {
    // Provided by the View layer (it owns the plot widgets) and consumed by the agent to
    // capture the current plot cell (PSD/FFT spectrum, waveform) as an image. A plain
    // interface — ServiceHelper is type-keyed (void*), not M-based — so the Model service
    // needs no View/Qt-widget dependency. Register from the View via
    // ServiceHelper<iPlotCapture>().addService(...); look up via GetDefaultService2.
    class iPlotCapture {
    public:
        virtual ~iPlotCapture() = default;
        // Base64-encoded PNG of the current plot cell, or "" when none is available.
        // Called on the MAIN thread (agent tool handlers marshal there before touching widgets).
        virtual std::string captureCurrentPngBase64() = 0;
    };
}
