# Preserved framework-free numerical core

EnvelopeAnalysis.h/.cpp retain the original Hilbert/envelope implementation bytes.
They have no Qt or FFTW dependency. Source receipts are in the migration manifest.
The new standalone smoke check uses analytically known carrier/modulation signals,
invalid inputs and sample-rate scaling. It is not a full port of the legacy
AgentService, live DPA analysis, or physical bearing-diagnostic acceptance.
See ../../NOTICE.md before reusing or redistributing imported source.
