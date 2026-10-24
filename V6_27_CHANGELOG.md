# v6.27 changelog — Goals pack (conversion, chances, second ball)

Builds on v6.26 (shot volume + own-half discipline). Focus: **turn attempts into goals and wins**.

## Pack contents (all requested items)

### Conversion-first finishing
- Prefer high-xG looks; central box rewarded; low-angle deep shots demoted (`conversionShotBonus`).
- Open-goal / 1v1 in the box forces a finish even with a modest lane score.
- First-time finish: short clean hold (~110 ms) when in the box at speed or free.

### Stronger cut-back
- `wCutBack` 0.30 → 0.55, wider range/box, lower free-room bar.
- Cut-back utility no longer scaled down by 0.4 — free man in the box beats a low-angle carrier shot.

### Final-third must-progress
- After ~1.8 s in the final third without a progressive action, force the best progressive option (`pickBestProgressive`).
- Urgency climbs faster once past the final-third line.

### Second-ball / rebound
- After every shot, a 1.5 s window arms (`armSecondBall`).
- On loose ball, switch to the nearest team-mate to the expected rebound within 18 m.

### Transition defense
- Counter-press window 4.0 → 5.2 s; switch gap 1.0 → 0.7 m (more aggressive swarm).

### Five goal-focused functions
1. **`conversionShotBonus`** — xG / open-goal / central-box scoring for ACT_SHOOT.
2. **`boxOccupationBonus`** — reward deliveries into the central shooting zone.
3. **`flankCrossBonus`** — early flank delivery into a free box target.
4. **`chipGkBonus`** — loft / chip when the opponent keeper is advanced.
5. **`armSecondBall`** — rebound hunt window after a shot.

Supporting helpers: `scoreClockUrgency`, `finalThirdMustProgress`, `pickBestProgressive`, `oppGkDepth`, dynamic through lead (capped).

### Score / clock urgency
- Late-game phase boost when host supplies match phase ≥ 0.72.
- Final-third urgency bump on top of possession urgency.

## Tests
- test_engine, rescue, edge, fix, meta, upgrade: **all pass**.
- Pre-existing legacy failures unchanged: rules (1), tactics (2), stun (2), track (3).

## Not verified
Live eFootball / Legendary scorelines. Replay is open-loop. Keep Record ON and send the next backup.

App **2.38** (versionCode 38).
