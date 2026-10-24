# v6.33 — Goal hunger (on top of v6.32 Fast Triangle)

Problem: high possession, almost no goals → losses. v6.32 fixed own-half gifts and junk long shots;
v6.33 pushes **conversion** so chances are taken.

## Goal-oriented changes
- **Box finishes fire**: `boxShotXp` 0.10, `clinicalBoxXg` 0.02, central-box auto level-2, `openGoalOpp` 2.4
- **Conversion bonus ↑**: stronger reward for central / 1v1 / open looks; shoot utility ×14 urgency
- **Final-third shot bias**: playable shot in the box +0.55 utility over sideways retention
- **Chance creation ↑**: `wChance` 3.20, `wCutBack` 1.70, `wBoxPass` / `wBoxOccupy` up — put teammates in the box
- **Always hunt**: level score still gets trail urgency (not only when behind); late-game finish push
- **No camping**: `boxMustProgressSec` 0.45 — release or shoot in the box
- **shotPriorityAx** 8 — prefer shot earlier in the final third
- Own-half progressive-only and through-lead limits from v6.32 **kept** (those stop GA)

## Still not magic
Live eFootball AI, formation, and button calibration still matter. This profile is built to
**turn possession into shots that can score**, not to park the bus at 70% possession 0–1.
