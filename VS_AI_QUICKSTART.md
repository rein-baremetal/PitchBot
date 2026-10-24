# Using PitchBot against the offline AI

## Safeguards built into the engine
1. **Radar map follows the capture size.** The map from radar pixels to pitch metres is rebuilt on every capture-size change (portrait to
   landscape), and a self-check rebuilds it if a quarter or more of the tracked dots are off the pitch for 3 checks in a row (a note goes
   into the error report).
2. **Pass learner needs lost passes.** Failed passes are matched loosely so they are logged; the learner refuses to update when lost passes are
   missing from the log; pressure, crowding and length weights can never turn positive; a model saved in a damaged state is discarded when loaded.
3. **No tackle close to your goal.** No tackle while the ball carrier is within 26 m of your goal line (inside and around the box): the
   defender contains and jockeys instead.
4. **No back pass to the keeper or box.** The keeper is never a target, nor is anybody behind the ball within 24 m of your goal line, even
   with the rescue rules off. The "virtual keeper" back-pass is off by default.

## What is verified (on a desktop, not in the game)
- Every C++ test program passes (`engine_test/run_tests.sh`).
- 12 real eFootball radar crops: balls found, 45/46 and 53/53 dots matched.
NOT verified: the Android build (no Android SDK here) and live play. No win rate or goals per match is promised: play with
Record radar sessions ON and keep the backup for review.

## First match checklist
1. Press Reset learning once, then enter the real score after every match.
2. Dry-run ON first, dots on players and ball. Then OFF, Legendary ON for the hardest AI level.
3. After the first minute, export the backup: in the recording the kick-off ball should read about (500, 500) and 11+11 dots should stay
   between 0 and 1000. If not, the radar box needs another auto-fit.
