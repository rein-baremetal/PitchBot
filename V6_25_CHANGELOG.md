# v6.25 changelog — Ball release, outlets and cut-back finishing

Built from the 2026-10-22 backup (3 Legendary matches, 4 radar recordings). Findings from replaying the recorded tracks through the v6.24 engine:

- 44–49 % of our-possession frames had **no pass option at all** (16 m pass cap, lateral ban, long lanes rated unplayable). The carrier then dribbled into pressure: 28–33 turnovers a match, 2–3 shots.
- In 5–14 of 41–48 possession spells the engine never released the ball before it was lost.
- 19 engine checks were failing (every pass candidate had been filtered out in those scenes).

## What changed
- **Pass range 16 → 24 m** (`PB_DEFAULT_PASS_LEN`). Frames with no pass option fall from 44–49 % to 18–27 %; pass decisions per recording rise by about 25–50 %; the 90th-percentile time to first release drops from 3.4–5.5 s to 1.0–2.7 s.
- **Long-pass risk penalty** (`longPassPenalty`): beyond 18 m a pass loses utility in proportion to how likely it is to be cut out, so the longer range is only used when the lane is genuinely clean.
- **Forward lane means a playable lane**: a forward option only blocks sideways passes when its pass probability is at least 0.5 (before, any long, nearly-dead lane counted as open and shut every outlet).
- **Boxed-in outlet** (`crowdSideBand`, 12 m): when the carrier is crowded and there is no playable forward lane, a recycle up to 12 m back is allowed. Backward balls toward our own box and keeper stay banned (`backPassBox`, `gkPassBan`).
- **Cut-back finishing** (`cutBack`): in the final third, a pass to a team-mate in the central box with at least 3 m of room and a clearly better line to goal than the carrier has gets a bonus (keeper excluded from the lane test). It is the pass that makes the bait work: the defender is drawn, the free man scores.
- **Lofted through ball** picks the best through target whose lofted version can actually land (before, the single best target was tried and the rescue was dropped if that one failed).
- The v6.24 near-zero-interception values (`clearSlope`, `defSpeed`, `marginHalf`, `minPassXp`), the fast-touch timings and the finishing-pack thresholds are unchanged.

## Tests
- `engine_test/run_tests.sh` builds and runs every check.
- test_engine 160/160, rescue, edge, fix, meta, upgrade: all pass.
- Legacy checks that assumed level or back passes under the forward-only rule now set `forwardOnly = false` in their own scene, so they still test their own feature.
- Still failing, and failing before this version too: stun_checks (4), track_checks (3), tactics_checks (2), rules_checks (1, "turn tick before the pass", which the zero-hesitation fast-touch design contradicts). These are expectations from older builds, not new breakage.

## Not verified
Android build and live eFootball play (no SDK or phone here). The replay is open-loop: it shows what the engine would have decided on recorded frames, not match results. No scoreline is promised. Keep Record sessions ON and send the next backup.

App 2.36 (versionCode 36).
