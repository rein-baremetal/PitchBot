# v6.21 Change Log

- Extended the build-up sequence dynamic program to evaluate a root pass plus up to three follow-up passes (four passes total), weighting each path by estimated completion probability.
- Replaced raw shot-conversion threshold tuning with confidence-weighted Beta-binomial shrinkage and a Wilson 95% uncertainty interval.
- Added a saved-match after-action review with prior-five-match comparisons and cautious recommendations.
- Preserved existing match-state controller, opponent-intent model, shot geometry, safety checks, and existing learning exports.
- No guaranteed win rate is claimed; live-game and full Android build testing remain outstanding.
