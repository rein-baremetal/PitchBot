# v6.29 changelog — Scoring Machine (clinical conversion + pressure-smart possession)

Built from the 2026-10-22 backup: 6 matches, ~60 % possession, 28–33 losses per match, 0–5 pass attempts, 1–6 shots, mixed scorelines vs Legendary. The shortage was not “more passes” or “more shots” alone — it was **safe progressive options under press**, **shot quality over volume**, and **shape after the inevitable turnover**.

## Diagnosis (from matches.json + tracks)

| Metric | Observed | Implication |
|--------|----------|-------------|
| Losses | 28–33 / match | Carries into pressure; low-prob passes already cut by v6.28 |
| Pass attempts | 0–5 | Engine preferred carry; Legendary wins the duel |
| Shots | 1–6 | Volume ok-ish; conversion and chance quality still soft |
| GA | often > GF | Transition defence / recovery after loss still leaky |

v6.28 raised mean pass probability and cut reckless passes. The next failure mode is **holding under a 2–3 m press until the tackle lands**, and **low-xG shots from wide / contested spots** that waste the rare box entry.

## Added (all under `TacticalConfig::scoringMachine`, default on)

### Clinical finishing
- **`clinicalShot`**: demote shots whose xG is below `clinicalMinXg` (0.12) outside the box, or below `clinicalBoxXg` (0.06) inside, especially when contested. Open-goal / 1v1 looks are never demoted. Forces cut-back, through, or progress instead of a low-percentage blast.

### Pressure-smart carry
- **`pressureCarry` + `supportSeekCarryBonus`**: when an opponent is inside `pressCarryR` (3.8 m), a carry that moves toward free support *and* away from the nearest defender gains utility. Blind carries into the press lose relatively.
- **`forceReleasePress`**: if the carrier is inside `forcePressDist` (2.2 m) for `forcePressMs` (420 ms), treat as hold-force → existing safe-pass fallback fires the best short / triangle option instead of another contested carry.

### Final-third overload
- **`finalOverload` + `countZoneNet`**: when attackers outnumber defenders inside `overloadDepth` × `overloadHalfY`, progressive passes, carries into the zone, and shots from an overloaded box gain `wOverload`. Turns numerical advantage into a forced chance.

### Post-loss compact
- **`postLossCompact`**: on every turnover (`prev owner us → them`) arm a `postLossMs` (2.8 s) window. Short safe passes are boosted; long speculative balls and lonely forward carries are demoted. Complements counter-press: win the second ball *or* recover shape, do not both explode and get countered.

### High-tempo when trailing / late
- **`highTempoTrail`**: when behind or past `lateGameFrac`, raise urgency (via `scoreClockUrgency` + direct utility) and slightly relax the progressive pass safety floor (`trailXpRelax`). Keeps the scoring machine aggressive when it must score.

## Integration points
- Candidate utility: clinical demote, overload bonus, support-seek carry, post-loss adjust, trail urgency.
- Owner change: `armPostLoss()` on turnover.
- Decision loop: tight-press force folded into existing `holdForce` / safe-pass selection.
- `reset()`: clears `postLossUntil_` and `pressCloseSince_`.

## Trade-offs to watch
- Clinical demote can reduce shot volume further if chances stay low quality — that is intentional; the goal is better looks, not more speculative ones.
- Force-release under 2.2 m press will increase short-pass count; if Legendary still intercepts the short ones, raise `safeFirstXp` or `wTurnover` slightly rather than disabling force-release.
- Overload bonus only fires when the zone count is real; radar noise that mis-counts players can create false overloads — monitor box entries vs actual shots.

## Not changed
Interception model, stun pack, v6.28 turnover/safe-first/box-entry/counter-break, meta/plan/plus layers.

## Tests
Syntax-checked in editor. Run `engine_test/run_tests.sh` (test_engine, rescue, edge, meta, upgrade) before phone build. Live Legendary verification still requires Record ON + next backup.

App version: bump to **2.40** (versionCode 40) when packaging.
