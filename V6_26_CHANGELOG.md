# v6.26 changelog — Shot frequency and own-half discipline

Addresses the open items from v6.25 feedback:

- Goals / wins still unproven (open-loop replay shows decisions, not results). Legendary can still win; judge over several matches.
- Shot count stayed at 2–3: the previous release did not change when shots fire.
- Sideways play near our own goal: tighten the back-pass / recycle band.

## What changed

### More shots (less suppression)
- **boxShotXp** 0.22 → 0.18, **boxShotRange** 28 → 30 m: more in-box lanes qualify as shots (fully blocked lines still suppressed).
- **cueLaneXp** 0.32 → 0.28, **cuePrimeXp** 0.55 → 0.48: slightly lower bar so more real chances arm a cue.
- **shotSpacingMs** 1400 → 900, **shotRearmMs** 900 → 700, **cueMinGapMs** / **cueHoldMs** / **cueRearmMs** shortened: second looks come sooner after a miss or block.
- **shotMaxDist** 28 → 30 m, **shotSkipTurnOpp** 3.2 → 2.8 m, **shotPriorityAx** 14 → 12 m.
- **urgency** 0.35 → 0.48 (starts after 1.5 s) and shoot utility multiplier 3.5 → 4.5: finishing beats sterile retention when a lane exists.
- **stunSafe** / **stunLaneMin** left at the rescue-safe values so a defender inside 1.5–1.8 m still cancels the wind-up (test-backed).

### Tighter play near our own goal
- **backPassBox** 24 → 28 m: no backward pass to anyone that close to our goal line.
- **crowdSideBand** 12 → 8 m, **sideBand** 2 → 1 m.
- **filterForwardOnly**: when the carrier is within ~30 m of our goal line the recycle band collapses to 0.5 m — almost no sideways recycle deep in our half; clear or go forward.

Cut-back, outlets, pass range and the rest of the v6.25 release are unchanged.

## Tests
- `engine_test/run_tests.sh` builds and runs every check.
- test_engine, rescue, edge, fix, meta, upgrade: all pass.
- Pre-existing legacy failures unchanged in character: rules_checks (1), tactics_checks (2), stun_checks (2), track_checks (3).

## Not verified
Android build and live eFootball play. Replay remains open-loop: decisions, not scorelines. Keep Record sessions ON and send the next backup so shot count and conversion can be measured for real.

App 2.37 (versionCode 37).
