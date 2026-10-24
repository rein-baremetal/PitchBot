# v6.30 changelog — G2: v6.29 Scoring Machine, hardened + auditable

Base: G1 (v6.29). App 2.41 (versionCode 41).

## Fixes to v6.29
- **Post-loss window clock**: armed from `frameTimeMs_` (was `state_.timeMs`, which lags one frame at the turnover).
- **Press-force reset**: `pressCloseSince_` now clears when the carrier changes, so a new carrier never inherits the previous carrier's press timer.

## Added
- **`smNetCap` (0.60)**: the v6.29 pack (clinical demote, overload, support-seek, post-loss, trail urgency) can move any one candidate by at most ±0.60 utility, so stacked bonuses cannot override the v6.28 safety terms.
- **Shot ledger fields**: `SHOT_FIRED` now ends with `EVT` (unique id per shot), `SCORE`, `PHASE`, `SM` (pack on/off), enabling dedupe and on/off comparison from the next backup.
- **`engine_test/scoring_checks.cpp`**: clinical demote lowers a contested low-xG shot, keeps an open central one, cap sane, master switch, turnover cycle finite. Added to `run_tests.sh`.
- **From C1**: `analysis/scoring_machine.py` (now dedupes by `EVT`), `validation/` backup, audit JSON, roadmap docs.

## How to judge it
Play 10+ matches with `scoringMachine` on and 10+ off (same difficulty), Record ON, take a backup, run `python analysis/scoring_machine.py <backup.zip>`. Six matches cannot prove anything.

## Not changed
All v6.28/v6.29 tuning values, interception model, stun/meta/plan/plus layers.

## Tests
Engine suite run on desktop (see below); not run on a phone or in eFootball.
