import importlib.util, pathlib, unittest
P=pathlib.Path(__file__).with_name("scoring_machine.py")
spec=importlib.util.spec_from_file_location("scoring_machine",P)
sm=importlib.util.module_from_spec(spec); spec.loader.exec_module(sm)

class ScoringMachineTests(unittest.TestCase):
    def test_empty_sample_is_bounded(self):
        r=sm.analyse([], "", [], {})
        self.assertEqual(r["sample_size"],0)
        self.assertGreaterEqual(r["scoring_readiness_index"],0)
        self.assertLessEqual(r["scoring_readiness_index"],100)
    def test_uses_summary_labels(self):
        m=[{"gf":2,"ga":1,"shots":4,"poss":60,"passAtt":10,"passOk":8,"losses":3}]
        r=sm.analyse(m,"",[],{"matches":1})
        self.assertEqual(r["goals_for"],2)
        self.assertEqual(r["goals_against"],1)
        self.assertEqual(r["shots_total"],4)
        self.assertEqual(r["pass_completion_pct"],80)
    def test_what_if_is_explicitly_not_forecast(self):
        r=sm.analyse([{"gf":1,"ga":1,"shots":4,"poss":50}], "", [], {})
        s=sm.scenario(r,shots_per_match=5,conversion=.2)
        self.assertEqual(s["illustrative_goals_per_match"],1)
        self.assertIn("not a forecast",s["note"].lower())
if __name__=="__main__": unittest.main()
