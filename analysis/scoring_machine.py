#!/usr/bin/env python3
"""PitchBot offline scoring audit. Standard-library only; not a match-outcome predictor."""
import argparse, json, math, statistics, zipfile
from pathlib import Path

def load_backup(path):
    with zipfile.ZipFile(path) as z:
        names = set(z.namelist())
        match_name = "matches.json" if "matches.json" in names else next((n for n in names if n.endswith("/matches.json")), None)
        if not match_name:
            raise ValueError("Backup ZIP does not contain matches.json")
        matches = json.loads(z.read(match_name).decode("utf-8"))
        logs = [n for n in names if n.startswith("logs/") and n.endswith(".txt")]
        log_text = "\n".join(z.read(n).decode("utf-8", "replace") for n in logs)
        tracks = [n for n in names if n.startswith("tracks/") and n.endswith(".csv")]
        manifest = json.loads(z.read("manifest.json")) if "manifest.json" in names else {}
    return matches, log_text, tracks, manifest

def safe_div(a, b):
    return a / b if b else None

def analyse(matches, log_text, tracks, manifest):
    n = len(matches)
    gf = sum(int(m.get("gf", 0)) for m in matches)
    ga = sum(int(m.get("ga", 0)) for m in matches)
    shots = sum(int(m.get("shots", 0)) for m in matches)
    poss = [float(m["poss"]) for m in matches if "poss" in m]
    losses = sum(int(m.get("losses", 0)) for m in matches)
    pass_att = sum(int(m.get("passAtt", 0)) for m in matches)
    pass_ok = sum(int(m.get("passOk", 0)) for m in matches)
    _shot_lines = [line for line in log_text.splitlines() if "SHOT_FIRED" in line]
    # v6.30 lines carry "EVT: <n>"; dedupe by id so repeated log dumps are not double counted.
    _evt = {l.split("EVT:")[1].split("|")[0].strip() for l in _shot_lines if "EVT:" in l}
    shot_fired = len(_evt) + len([l for l in _shot_lines if "EVT:" not in l])
    # This is a diagnostic score, not a predicted probability or betting confidence.
    # Each subscore is bounded 0..100 and the weighting is disclosed.
    shot_sub = min(100.0, shots / max(n, 1) / 6.0 * 100.0)
    conversion_sub = min(100.0, gf / max(shots, 1) / 0.25 * 100.0)
    defensive_sub = max(0.0, min(100.0, 100.0 - (ga / max(n, 1)) * 20.0))
    possession_sub = (sum(poss) / len(poss)) if poss else 0.0
    pass_sub = (pass_ok / pass_att * 100.0) if pass_att else 50.0
    index = round(0.30*shot_sub + 0.25*conversion_sub + 0.20*defensive_sub + 0.15*possession_sub + 0.10*pass_sub, 1)
    return {
      "sample_size": n,
      "matches_recorded_in_manifest": manifest.get("matches"),
      "matches_learned_in_manifest": manifest.get("learned"),
      "goals_for": gf, "goals_against": ga, "goal_difference": gf-ga,
      "goals_for_per_match": round(safe_div(gf,n) or 0, 2),
      "goals_against_per_match": round(safe_div(ga,n) or 0, 2),
      "shots_total": shots, "shots_per_match": round(safe_div(shots,n) or 0, 2),
      "goals_per_shot": round(safe_div(gf,shots) or 0, 3),
      "average_possession_pct": round(sum(poss)/len(poss), 1) if poss else None,
      "passes_attempted": pass_att, "passes_completed": pass_ok,
      "pass_completion_pct": round(safe_div(pass_ok,pass_att)*100,1) if pass_att else None,
      "recorded_losses": losses,
      "shot_fired_log_events": shot_fired,
      "track_files": len(tracks),
      "scoring_readiness_index": index,
      "index_label": "heuristic diagnostic only — not win probability",
      "confidence": "very low" if n < 20 else ("low" if n < 50 else "moderate"),
      "warnings": [
        *([f"Only {n} labelled matches; use at least 30–50 varied matches before trusting trend estimates."] if n < 30 else []),
        *(["Shot logs do not expose a reliable total of attempted shots; shots are taken from manually recorded match summaries."] if shot_fired != shots else []),
        *(["Backup contains no verified action-by-action ground-truth labels for pass success, shot quality, interception, or goal causality."] if n < 100 else []),
        "Track CSVs are radar trajectories, not ground-truth event annotations; replay is open-loop and cannot prove real match outcomes.",
        "Do not tune weights against these same six matches and then call the result validated; hold out later matches."
      ]
    }

def scenario(base, shots_per_match=None, conversion=None, ga_per_match=None, possession=None, pass_completion=None):
    """Transparent what-if calculation. Inputs are assumptions, not predictions."""
    n=max(1, int(base["sample_size"]))
    shots = float(shots_per_match if shots_per_match is not None else base["shots_per_match"])
    conv = float(conversion if conversion is not None else (base["goals_per_shot"] or 0))
    ga = float(ga_per_match if ga_per_match is not None else base["goals_against_per_match"])
    poss = float(possession if possession is not None else (base["average_possession_pct"] or 0))
    pc = float(pass_completion if pass_completion is not None else (base["pass_completion_pct"] or 50))
    gf = shots * conv
    shot_sub=min(100, shots/6*100)
    conv_sub=min(100, conv/0.25*100)
    defsub=max(0,min(100,100-ga*20))
    idx=round(.30*shot_sub+.25*conv_sub+.20*defsub+.15*poss+.10*pc,1)
    return {"assumed_shots_per_match":round(shots,2),"assumed_goals_per_shot":round(conv,3),
            "illustrative_goals_per_match":round(gf,2),"assumed_goals_against_per_match":round(ga,2),
            "illustrative_goal_difference_per_match":round(gf-ga,2),"scoring_readiness_index":idx,
            "note":"What-if arithmetic only. Not a forecast; inputs are assumptions."}

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("backup_zip", help="Path to PitchBot backup ZIP")
    ap.add_argument("--out", help="Optional output JSON path")
    ap.add_argument("--simulate", action="store_true", help="Add transparent what-if scenarios")
    args=ap.parse_args()
    matches, logs, tracks, manifest=load_backup(args.backup_zip)
    report=analyse(matches,logs,tracks,manifest)
    if args.simulate:
        report["what_if_scenarios"]=[
          {"name":"current baseline",**scenario(report)},
          {"name":"more chances, same conversion",**scenario(report,shots_per_match=5)},
          {"name":"better finishing, same shots",**scenario(report,conversion=max(report["goals_per_shot"] or 0,0.18))},
          {"name":"chance creation + finishing improvement",**scenario(report,shots_per_match=5,conversion=max(report["goals_per_shot"] or 0,0.18),ga_per_match=max(0,report["goals_against_per_match"]-0.5))}
        ]
    rendered=json.dumps(report,indent=2)
    if args.out:
        Path(args.out).write_text(rendered+"\n",encoding="utf-8")
    print(rendered)
if __name__ == "__main__":
    main()
