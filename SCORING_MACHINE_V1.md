# PitchBot backend audit — backup 2026-10-22

## Scope
Compared `PitchBot_v6_28_SafePossession_GoalHunt.zip` source with `pitchbot-backup-2026-10-22.zip` runtime data. The backup is a data snapshot, not an older source-code backup. It contains six manually scored match summaries, nine track CSVs, six match logs, a learned `brain617.txt` model and preferences. It cannot establish that the backup contains an older/better implementation of engine logic.

## Strong existing foundations
- Native C++ decision engine plus Kotlin Android orchestration.
- Separate tactical packs and extensive C++ regression suites.
- Radar tracking/replay inputs, persistence/backup, diagnostic logs.
- v6.28 adds safe-possession/box-entry/counter-break utility and `SHOT_FIRED` logging.
- Existing changelog explicitly distinguishes open-loop replay from real match results.

## High-priority gaps observed in the backup data
1. **Tiny labelled sample:** six matches only. This is not enough to reliably tune a learned model or claim superiority over Legendary AI.
2. **Weak outcome attribution:** match rows store possession, pass attempts/completions, shots, losses and goals, but not shot location/type/xG, on-target result, blocked/saved outcome, big chances, box entries, turnover danger, action-to-outcome linkage, score/time state or opponent difficulty.
3. **Shot telemetry mismatch risk:** the backup summary reports 19 shots total, while `SHOT_FIRED` occurrences are not a dependable replacement for a full shot-event ledger. The app needs a unique event ID and one authoritative shot record per attempted shot.
4. **No calibration/holdout evaluation:** no Brier score, log loss, calibration curve, per-tactic ablation, version comparison, or held-out match split in the backup.
5. **Tracking uncertainty not represented in outcomes:** tracks are positions sampled around eight times/second and can contain missing/edge-clipped detections. The evaluator needs to retain confidence/staleness and mark uncertain frames rather than treating every track point as ground truth.
6. **Learning provenance is opaque:** `brain617.txt` is a serialized model, but the backup does not provide a feature schema, model version metadata, training sample IDs, learning-rate/weight bounds, or rollback record sufficient to audit whether the learned changes helped.
7. **No reproducible match-level comparison:** data does not include controlled A/B match groups, same opponent/difficulty/tactics, or a frozen baseline model identifier.

## Additions in this package
- `analysis/scoring_machine.py`: offline, standard-library-only audit and what-if simulator. It reads a backup ZIP and reports scoring/defensive/possession diagnostics, sample-size warnings, and transparent assumptions. It does **not** control the game or claim to predict wins.
- `SCORING_MACHINE_V1.md`: implementation roadmap and validation gates.
- `SCORING_AUDIT_2026-10-22.json`: generated report from the supplied backup.

## Recommended backend/data additions (in order)
### P0 — authoritative event ledger
For every decision and action, persist: match ID, event ID, engine version, model version/hash, tactic/config hash, timestamp/game clock, score state, action type, candidate set, chosen action, utility components, predicted pass success/shot xG, perception confidence/staleness, target location, action fired/confirmed, and result label when observable. Use one event ID to link decision -> execution -> outcome and prevent duplicate shot counts.

### P0 — proper outcome labels
Record shot attempted/on-target/goal/saved/blocked/off-target; shot distance/angle/foot/body type; nearest defender and keeper position; rebound/second-ball result; pass attempted/completed/intercepted; turnover zone; box entry; counterattack; goals for/against. Unknown outcomes must remain unknown, not silently become failures.

### P1 — calibration and safe learning
Keep a frozen baseline and challenger. Train only on completed, deduplicated matches; split by whole match (never random frames from the same match across train/test). Report Brier score/log loss for probability predictions and confidence bins. Bound parameter changes, require minimum sample count, and support one-tap rollback to last known-good model.

### P1 — tactical ablation
Compare v6.28 against the same baseline with one pack enabled/disabled at a time: safe-first, turnover cost, box-entry, counter-break, conversion/shot priority, cut-back and rebound. Use matched opponent, difficulty, formation, controls and match duration; report shots, shots on target, goals, dangerous turnovers, goals conceded, box entries, pass completion and match result.

### P2 — uncertainty-aware action selection
Track data freshness and confidence. When ball/players are occluded, radar mapping drifts, or a receiver is stale, lower confidence and prefer a safe action or re-acquire perception. Avoid treating repeated duplicate coordinates as real movement.

### P2 — scoring chance creation
Use a transparent staged objective: (1) retain safe possession when under pressure, (2) progress into the final third, (3) create central box/cut-back/through-ball chances, (4) choose shot vs pass using expected goal value and block/keeper risk, (5) react to rebounds, (6) immediately re-establish defensive shape after loss. Do not increase shot volume by lowering safety constraints globally.

## Validation gates before claiming “better than Legendary AI”
- Build and run native tests, then build the Android APK with the project Gradle/NDK toolchain.
- Run dry-run calibration and verify actual shot-button firing against `SHOT_FIRED` logs.
- Collect at least 30–50 varied labelled matches for preliminary trends; use more for stable estimates.
- Use a held-out set of later matches and report confidence intervals. Avoid cherry-picking scorelines.
- Declare superiority only after repeated, controlled live matches show improved goal difference/win rate without unacceptable defensive deterioration.

## Safety / integrity
This is a local gameplay automation/analysis project. The added analyzer only reads backup data; it does not connect to game servers, evade anti-cheat systems, or modify network traffic.
