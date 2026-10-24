# PitchBot scoring audit add-on

This package preserves the original v6.28 Android/C++ source and adds offline scoring-analysis tooling. It does not claim that the Android bot itself has been upgraded to a validated stronger gameplay policy.

## Run the audit
From this directory:
```bash
python3 analysis/scoring_machine.py validation/pitchbot-backup-2026-10-22.zip --simulate --out SCORING_AUDIT_2026-10-22.json
python3 -m unittest -v analysis/test_scoring_machine.py
```

## What the report says
The supplied backup contains six manually labelled matches: 3 goals for, 15 against, 19 reported shots, 3.17 shots per match, 0.5 goals per match, and 2.5 goals conceded per match. The sample is too small to validate performance claims. The readiness index is a transparent heuristic diagnostic, not a probability of winning.

## Important boundary
The simulator is offline and read-only. Actual tactical-policy changes should be added to the native engine only after controlled A/B evaluation against a frozen v6.28 baseline, and must pass the existing native regression suite and an Android/NDK build.
