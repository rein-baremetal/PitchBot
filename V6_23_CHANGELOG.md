# v6.23 changelog — Finishing pack (Legendary scoring)

Based on backup match data (high possession 56–69 %, only 2–3 shots/match, low conversion, 28–33 turnovers):

- **Aggressive finishing thresholds**: boxShotXp 0.50 → 0.22, cueLaneXp 0.60 → 0.32, cueMinXg / cuePrimeXg lowered, cueConfirmFrames 2 → 1.
- **Wider shooting zone**: boxShotRange 23 → 28 m, shootRange 30 → 35 m, shotMaxDist 18.5 → 28 m, longShotRange extended.
- **Faster re-arm & less spacing**: shotSpacingMs 3500 → 1400, shotRearmMs 1500 → 900, cue gaps reduced.
- **Higher urgency**: urgency 0.10 → 0.35, urgencyAfterSec 6 → 2 s so the bot attacks the goal earlier in possession.
- **Stronger shoot preference**: utility multiplier on ACT_SHOOT increased; chance-creation pass bonus (wChance) raised and triggered earlier.
- **Stunning / long shots enabled** by default (stunEnabled = true, wider stun band, slightly lower safety distances).
- **Earlier shot priority**: shotPriorityAx 22.5 → 14 m so shots outrank passes deeper in the final third.
- **More lenient facing**: shotFaceTolDeg raised so more windows actually fire.
- Bumped Android app version to 2.34 (versionCode 34).

Goal: convert the existing possession advantage into a high volume of shots and a decisive scoreline against Legendary AI.
No claim of a guaranteed 10–0; live verification still required. Keep Record sessions ON and export backups for review.
