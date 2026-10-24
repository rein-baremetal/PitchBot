# v6.24 changelog — Fast touches + calculated through + near-zero interception

Builds on the V6.23 finishing pack. Goal: turn high possession into clinical, low-turnover progressive play that can beat Legendary AI.

## Fast-paced touches
- Decision loop tightened: decisionIntervalMs 150→110, actionCooldownMs 350→260, fastCooldownMs 280→180, min/max pass hold reduced.
- One-touch window widened (radius 3.4→4.2 m, TTC 0.60→0.75 s, max length 30 m).
- wOneTouch 0.30→0.48 so one-touch options are preferred when safe.
- Anticipate / pre-align: earlier fire (oneTouchPreMs 50→85 ms), wider slack, lower min ball speed, higher pre-align angle, faster re-arm (280 ms / 90 ms).

## Calculated through balls before the ball is stolen
- throughLead 4.0→5.5 m (lead the runner more so the ball arrives ahead of the covering defender).
- Through-ball probability penalty relaxed (×0.85 → ×0.92) while still requiring a positive intercept margin.
- Extra utility bonus on high-xP throughs when the carrier is under pressure or in a chase state — the pass is preferred precisely when the ball is about to be lost.

## Near-zero interception
- Interception model made more pessimistic: defSpeed 6.0→6.4, defBody 0.85, marginHalf 0.8→1.35 m.
- clearSlope 1.1→1.35 and clearHalf raised so only lanes with real spare metres survive.
- minPassXp 0.62→0.72 (Legendary still adds its own extra). Low-margin passes are demoted hard; only high-confidence passes are selected.

## Version
- Android app 2.35 (versionCode 35).

No guaranteed scoreline. Record sessions ON, export backups after Legendary matches, and feed the new logs back for the next tuning pass.
