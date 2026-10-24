# v6.32 changelog — Fast Triangle Scoring Machine

Base: v6.31. Goal: short, fast-paced possessions that actually score; fix the three failure modes from live matches.

## Root causes (from backup + user report)
1. **Back passes to own half / GK stolen → goals conceded**  
   `crowdSideBand=8` and `backPassBox=34` still allowed recycle toward our goal under press.
2. **Through passes never reached the target**  
   `throughLead=8.5` + `throughLeadPerMs=0.42` over-led runners; balls arrived behind or were cut out.
3. **Shots on target ≈ 0**  
   Stun pack fired many 20–30 m efforts with **XG: 0.00** (see match logs). Volume without quality.

## Fixes
### Own-half safety (no more GK gifts)
- `backPassBox` 34 → **42**, `noPassZone` 14 → **18**, `gkBanDepth` 18 → **22**
- `crowdSideBand` 8 → **2.5**, `sideBand` 1 → **0.5**, `forwardMin` 1 → **1.5**
- `filterForwardOnly`: when within **38 m of our goal line**, **strictly progressive** ground passes only (no zero-gain / backward)

### Through balls that arrive
- `throughLead` 8.5 → **5.0**, `throughLeadPerMs` 0.42 → **0.25**
- Generate through only when receiver is **≥ 6 m ahead** and has **forward velocity > 0.5**
- `wThrough` 0.90 → **0.65**, `rayMin` 0.60 → **0.85**

### Clinical shooting (on-target scoring machine)
- `boxShotRange` 38 → **32**, `boxShotXp` 0.12 → **0.16**, `convMinLane` 1.2 → **1.6**
- `clinicalMinXg` 0.10 → **0.14**, stronger distance demote
- Stun: `stunSafe` ↑, `stunLaneMin` 1.2 → **1.8**, `longShotXp` 0.62 → **0.78**, `longShotFree` 3.5 → **5.0**
- Outside-box stun only arms when `xp >= longShotXp` **and** `xg >= 0.04`

### Short fast-paced tempo
- `wTriangle` 0.08 → **0.22** (wide 40–80°), bonus for short progressive ≤14 m passes
- `decisionIntervalMs` 110 → **95**, `fastCooldownMs` 180 → **130**
- `minPassXp` 0.78 → **0.82**

## Locked prefs (unchanged intent)
tactic Fast triangle, keepBias 0.25, aggression 0.95, shootReach 0.40, passSpeed 0.88, virtualKeeper OFF

## How to judge
Play 10+ matches Record ON, take a backup. Expect: fewer GA from turnovers in our half, throughs that stick, fewer 0-xG stun spam, more box looks and shots that actually hit the frame.
