# Preview branch consolidation — 2026-09-28

The canonical preview branch is `preview_v1` (underscore).

The two former branches had independent histories:

- Original preview: `abd1e1907c336dadb45b8bc337a8596b227876a3` (`preview_v1`).
- Tested runtime publication: `1ba12ad7988ce395e7a9b66334e8c2e460e95d90` (`preview-v1`, also on `master`).

This consolidation records both parents in a merge commit. It starts from the original preview snapshot and incorporates reviewed runtime changes, rather than replacing its independent database and WebUI work.

Imported from the tested runtime publication:

- SQLite I–V grid lookup, explicit 18s6p model configuration, and corrected curve generation.
- Electrical AC-power precomputation for environmental loads.
- Timestamp-aligned mission loading and invalid temperature/RH filtering.
- Mission/IV tests, two-case fixture, Python dependencies and deployment documentation.
- NSRDB output-path and credential-configuration fixes, retaining the older API-key name as an alias.

Preserved from the original preview:

- Complete/incomplete component-database checks, custom database paths and force rebuilds.
- CUDA compiler discovery and optional bundled-SQLite setup.
- WebUI component configuration, results presentation and backend tests.
- The deployment service example, lifetime-summary utility, and existing tracked inputs.

The component initializer accepts both `--skip-pv` and `--skip-legacy-pv`. New integration tests cover component-only generation, verification of an existing database, and repair of an incomplete database after explicit `--force`.

Unrelated compiled binaries, duplicate experimental UI files and research-report assets from `master` are not copied into this clean preview branch. Its existing files and both commit histories remain available. `master` is unchanged. The duplicate `preview-v1` name is removed only after the consolidated branch has passed checks and been pushed successfully.

Validation before publication: mission/IV tests, report/CLI tests (14 checks), simplified thermal compatibility, two database-initialization integration tests, and two existing WebUI backend tests passed. Every C++/CUDA source/header matches the previously GPU-tested `1ba12ad` snapshot byte for byte; no new GPU run was performed for this consolidation. The original preview backend, frontend, deployment service, database verifier, environment setup and lifetime-summary utility were also checked for preservation.
