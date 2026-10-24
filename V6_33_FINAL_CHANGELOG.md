# PitchBot v6.33 Goal Hunger — final (app 2.45)

Fixes
- Crowded carrier at midfield or beyond no longer dribbles into the press: a short sideways/back release (up to `crowdSideBandMid`, 8 m) is allowed. Own-third behaviour is unchanged, and passes toward our own box stay blocked at generation.
- Forced lofted ball over a dense midfield now also considers a stationary runner 6 m or more ahead (previously it needed a moving runner, so it never fired).
- Shot cues and the central-box forced finish require a lane of at least `convMinLane`; a man standing on the shot line no longer triggers a cue.
- Outside-box xG floor is now `longShotMinXg` (default 0.04, unchanged behaviour).

Tests
- Whole suite green (11 of 11). Stale expectations updated to the fast cooldown, the 0.95 net cap and the forward-runner through-ball rule; long-range shot checks run with an explicit long-shot profile.
- Clean under AddressSanitizer/UBSan on the suites run (test_engine, scoring, rescue, rules).
- Real-frame replay: engine released before the steal in 57% of spells (was 52%); median first release 0.43 s (was 0.58 s); p90 1.88 s (was 2.73 s).

Housekeeping
- Version-history wording removed from code comments.

Defaults and UI (app 2.45)
- New-install defaults now match the tuned settings: aggression 0.72, shooting range 0.32, pass speed 0.88, keep-ball 0.75, shot hold 160 ms, take-over 204 ms, counter temperature 0.13, holding temperature 0.46, session recording on, dry-run off.
- Main screen restyled in an iOS look: grouped cards on a light background, large title, green switches, segmented tactic control, tinted secondary buttons and press feedback.
