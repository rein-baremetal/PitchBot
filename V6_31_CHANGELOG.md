# v6.31 changelog — bug-fix release on top of v6.30 (App 2.42, versionCode 42)

## Engine fixes
- **Shooting into a wall**: with `boxShotXp` at 0.08 and the conversion-first rule, a shot was forced even when the best aim line passed < 1 m from a defender (3 defenders on the line still gave a level-2 cue). `boxShotXp` is back to 0.12 (the documented BEAST value) and the conversion-first override now also needs `convMinLane` (1.2 m, new) of clearance.
- **smNetCap** restored to 0.80 (was 1.15, which let the stacked scoring pack outweigh the safety terms; v6.30 intended 0.60, BEAST 0.90).

## Test suite (all 11 suites now 0 failures)
- Added the missing fixture `engine_test/real_frames/track_excerpt.csv` (real radar track from the backup) and made `run_tests.sh` pass its absolute path, so `track_checks` runs from any directory.
- Updated expectations that no longer matched the intended design: blocked passes are pruned (not merely low-xP); autoShoot never "holds"; zero-hesitation passes/shots (no mandatory turn tick); sampling is checked at the sampler level because bad lanes are hard-vetoed; one-touch dispatch must be before the receiver, not before the trap point.
- Engine suites also run clean under ASan + UBSan; `jni_bridge.cpp` compiles with -Wall, and every Kotlin `external fun` has a matching JNI export.

## Not verified
No Android SDK in the build environment: the APK was not built and nothing was run on a phone or in eFootball. CI (`.github/workflows/build.yml`) builds the APK.
