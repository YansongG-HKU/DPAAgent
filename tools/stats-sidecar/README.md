# Preserved statistics sidecar

Original optimization, sequential-boundary and offline-monitoring implementation
plus tests, received from the pinned DPA baseline. Exact source identities and
local modifications are recorded in docs/migration/source-manifest-20261026.json
at repository root. See NOTICE.md for source-license status.

Run `python stats_sidecar.py request.json` for the one-shot bounded JSON interface.
This is an offline scientific helper; it never operates devices or becomes a live
alarm authority. CUSUM requires an independent baseline and explicit decision bound.
Trend compression verifies reconstruction error at original timestamps.

Run `python -m unittest discover -s tools/stats-sidecar -p test_monitor_analysis.py`
from repository root for dependency-free monitoring/protocol checks. Other suites
require the dependencies listed in requirements.txt; BayBE and viblearn additionally
require their own explicit installation. The requirements are original compatibility
ranges, not a reproducible lock or license approval. Historical verification claims
are not transferred into a claim that this checkout passed those suites.
