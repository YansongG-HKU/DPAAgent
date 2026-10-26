# DPAAgent governance

Owner-approved intelligence destination as of 2026-10-26 (Asia/Hong_Kong).
The default branch remains master. New intelligence development belongs here:
desktop agent UX, providers, memory, planning, experiment orchestration, expert
reasoning, scientific helpers and their evaluations.

DPA-2026 owns native device/acquisition/storage/DSP/UI correctness and operation
authorization. Keep the client and bridge protocol versioned together. Models
cannot replace native interlocks, quality checks, consent or a live alarm authority.

Work through scoped PRs with CODEOWNERS review. CODEOWNERS is a review routing file,
not proof of enabled server-side branch protection. Do not bypass existing checks.
Keep master and upstream shell history; do not merge the private product's history.
Preserve original upstream README files and notices. New DPA features live in their
own modules to keep future pi-app updates reviewable.

Before importing source: record exact source commit/path/blob/SHA-256, check the
publication scope and dependency/license provenance, exclude private evidence and
secrets, and bind target bytes/modifications in the migration manifest. No databases,
model traces, machine paths, standard copies or commercial binaries in public commits.
Imported source is not implicitly relicensed under the desktop package metadata.

Each acceptance record separates source receipt, build, automated tests, live DPA,
real-model evaluation, authorized domain evaluation and physical device acceptance.
Missing or failing evidence stays missing or failing. A migration PR is not a release.

Full historical implementation/evidence remains in the private preservation registry.
Public migration work items describe capabilities without republishing private logs.
Do not close original work as completed until its new disposition and validation are
linked; historical branches/tags are retained for recovery.

Issues was disabled at handoff. Enabling it returned HTTP 403 (integration permission).
Use the versioned migration checklist and migration PR for public coordination; no
server-side protection or settings change is claimed.
