# PitchBot

An offline Android overlay that plays eFootball offline matches by reading the radar.
Native C++ engine (xT / xP decisions, tracking, shooting) with a Kotlin app. Nothing leaves your phone.

**Version:** 6.33 "Goal Hunger" (app 2.45) · **Releases:** a new version about every 2 weeks

## What it does
- **Possession:** fast-triangle build-up; a crowded carrier releases the ball instead of dribbling into the press.
- **Passing:** each pass is scored by arrival chance (xP) and threat gained (xT); defenders' reach is modelled.
- **Through and lofted balls:** led to the runner's future position; a lofted ball over a packed midfield.
- **Shooting:** takes chances inside the box, needs an open lane further out; aims at the far corner.
- **Late-game plan:** protect a lead or chase a goal. All settings are in the app under Advanced settings.

## Install
1. Download the latest APK from [Releases](https://github.com/rein-baremetal/PitchBot/releases/latest).
2. Allow "Install unknown apps" for your browser or file manager, then open the APK.
3. Allow overlay, the accessibility service and notifications. Map the controls once in the wizard.
4. Open the game, press Volume Up and tap the dot in your team's colour. Volume Down at full time.

## Build and test
```
gradle :app:assembleDebug          # needs JDK 17, Android SDK, NDK 26.1.10909125, CMake 3.22.1
sh engine_test/run_tests.sh        # engine tests, g++ only, no phone needed
```
Optional OpenCV ball detection: set `opencv.dir` in `gradle.properties`.

## Fork it and make it better
Forks are welcome, and the licence (MIT) lets you change and share it freely.
1. Fork, branch, change. Keep `sh engine_test/run_tests.sh` green; add a test for new behaviour.
2. Open a pull request with what you changed and the numbers that show it helped.
3. Good first targets: spells where the ball is never released, shot selection, better radar tracking.

Send play logs (Export error report in the app) with bug reports; they make fixes much faster.

## Custom work
Need your own version, features or tuning? I take paid gigs.
**Email:** reindev.official@gmail.com

*Built by Reinford, Kenya. Independent project, not affiliated with Konami or eFootball.*
