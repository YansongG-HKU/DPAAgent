# Quarantined monitoring WIP overlay

This parallel source overlay is preserved byte-for-byte from the monitoring WIP
snapshot. It is not part of the integrated baseline, Electron runtime or supported
scientific helper. It is not a complete standalone tree: apply only for inspection
against its pinned private source baseline.

Known unresolved problems include duplicate live alarm authority, CUSUM without
a decision bound in the old path, invalid old compression logic and a historical
C++ StreamingMonitor crash. Keep these failures open. The integrated scientific
helper contains subsequent alternatives; preservation is not adoption.
Every file has a separate source_commit/snapshot in the migration manifest.
Private original CRLF/mixed-newline bytes remain in the existing encrypted archive;
these are canonical Git source bytes, not an assertion of original workstation EOLs.
