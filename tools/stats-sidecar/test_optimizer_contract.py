import unittest
import numpy as np
import stats_sidecar as sidecar


class OptimizerContractTests(unittest.TestCase):
    def request(self):
        return {"method":"bo_next","domains":{"x":{"kind":"linear_numeric","min":0,"max":10}},
                "design":[{"x":0},{"x":10}],"observations":[{"run_index":0,"value":0},{"run_index":1,"value":1}]}

    def test_bounded_candidates_cover_each_axis(self):
        domains={key:{"kind":"linear_numeric","min":0,"max":10} for key in ['a','b','c']}
        rows=sidecar._candidate_grid(sorted(domains),domains,128,np.random.default_rng(7))
        self.assertEqual(len(rows),128)
        for key in domains:
            self.assertLess(min(row[key][0] for row in rows),0.1)
            self.assertGreater(max(row[key][0] for row in rows),0.9)
        self.assertEqual(rows,sidecar._candidate_grid(sorted(domains),domains,128,np.random.default_rng(7)))

    def test_invalid_pairing_and_domains_are_rejected(self):
        cases=[]
        for index in [-1,2,True]:
            value=self.request(); value['observations'][0]['run_index']=index; cases.append(value)
        value=self.request(); value['observations'][1]['run_index']=0; cases.append(value)
        for invalid in [-1,11,True,'1']:
            value=self.request(); value['design'][0]['x']=invalid; cases.append(value)
        value=self.request(); value['backend']='unregistered'; cases.append(value)
        value=self.request(); value['observations'][0]['value']=True; cases.append(value)
        for value in cases:
            with self.subTest(value=value):
                payload,code=sidecar.handle(value)
                self.assertNotEqual(code,0)
                self.assertFalse(payload['ok'])

    def test_flat_fallback_describes_what_it_returns(self):
        value=self.request(); value['observations'][1]['value']=0
        payload,code=sidecar.handle(value)
        self.assertEqual(code,0)
        self.assertEqual(payload['surrogate'],'builtin_grid')
        self.assertNotIn('kept_best_run',payload['notes'])


if __name__=='__main__': unittest.main()
