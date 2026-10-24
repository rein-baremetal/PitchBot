# PitchBot v6.20 — Coordinated Decision Intelligence

## What changed

This source update adds a small, bounded candidate-utility adjustment to the existing native engine. It is enabled by the existing **Upgrade Pack** switch (`TacticalConfig::upgrades`). It does not add new input gestures or bypass existing offside, interception, clearance, or pass-safety checks.

### v6.20 decision-risk adjustment

- **Receiver confidence:** applies a small penalty when a pass target is occluded or stale, using the existing receiver-reliability estimate.
- **Transition cover:** when progressing out of our own half, checks for fresh teammates behind the carrier. It slightly discourages a moderately risky forward pass when no cover is detected and gives only a small reward when cover and pass safety are both good.
- **Press-aware outlets:** under strong pressure, modestly favours a safe, short outlet and demotes low-probability passes.
- **Long-ball risk:** adds a bounded penalty to very long, low-probability passes.
- **Bounded influence:** the adjustment is clamped to `[-0.12, +0.035]` utility so it cannot overwhelm the primary action score.

## What this does NOT claim

This is a focused incremental layer, not a complete implementation of every item in the broader Elite-Dominance blueprint. It does not guarantee a 9–1–0 record, 10-goal matches, perfect perception, or a new full-depth search planner. Existing tracking, opponent profiles, tactical state machine, action timing, and evaluation infrastructure remain the foundation and should be validated together.

## Test plan

1. Build the Android app with the project's Gradle/NDK toolchain.
2. Run engine regression checks and real-radar-frame checks.
3. Use Dry-run first to check tracking and direction calibration.
4. Compare v6.19 and v6.20 in matched test conditions, separately against Legendary AI and human opponents.
5. Record wins/draws/losses, goals for/against, possession, pass completion, dangerous turnovers, box entries, and quality chances.
6. Keep the change only if held-out matches show improvement without a material increase in defensive exposure.

## Current verification

- C++ `-fsyntax-only` check of `pitch_engine.cpp`: passed.
- `engine_test/v618_checks.cpp`: passed all checks.
- A full Android APK build and live eFootball matches have not been verified in this environment.
