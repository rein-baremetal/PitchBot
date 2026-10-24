# v6.28 changelog — Safe possession, box entry, counter break

Measured on the 2026-10-22 backup tracks (open-loop replay of the engine's decisions, not match results).

## Finding
In 50–65 % of the passes the engine chose, the pass probability was below 0.5, and in almost every one of those frames no safer pass existed. The shortage is safe receivers under Legendary's press, not the choice among options. Zero interception cannot be guaranteed from the ball carrier's side alone; this version removes the avoidable risk and rewards the moves that create safe options.

## Added (all switchable in TacticalConfig)
- **Turnover cost** (`turnoverCost`): a pass with probability under 0.60 loses utility, most deep in our own half, none beyond +30 m. Low-probability passes chosen fell 23–39 % on the three tracks; the mean probability of the chosen pass rose from 0.45–0.48 to 0.51–0.53.
- **Safe first** (`safeFirst`): when a clean pass exists (probability ≥ 0.80, lane clearance ≥ 2.8 m), doubtful passes and dribbles into a tackle lose utility.
- **Receiver contest** (`recvContestR`): a defender within 2.6 m of the receiver costs the pass utility.
- **Box entry** (`boxEntry`): a pass from outside the box to a free man inside it (and a free carry into it) gets a bonus.
- **Counter break** (`counterBreak`): in the first 4.2 s after winning the ball, a clean forward pass of 9 m or more into space gets a bonus.
- **Shot-fired log**: each shot writes `SHOT_FIRED | DIST | XG | NEAREST_OPP` into the shot log, so the next backup shows how many decided shots actually left the foot.

## Trade-off to watch
About 14–20 % fewer pass decisions, replaced by carries. First-release timing is about the same (median 0.2–0.5 s; never released in 7–12 of 41–48 spells versus 6–11 before). If the bot holds the ball too long in play, lower `wTurnover` (0.40) first.

## Not changed
Interception model values, fast-touch timings and the v6.27 finishing pack.

## Tests
test_engine, rescue, edge, fix, meta, upgrade: 0 failures. Not run on a phone or in eFootball.

App 2.39 (versionCode 39).
