import unittest

from tools import isolation


def cpu(package, die, core, *siblings):
    return isolation.Cpu(package, die, core, None, siblings)


class LayoutTests(unittest.TestCase):
    def setUp(self):
        self.topology = {
            0: cpu(0, 0, 0, 0, 1),
            1: cpu(0, 0, 0, 0, 1),
            2: cpu(0, 0, 1, 2, 3),
            3: cpu(0, 0, 1, 2, 3),
            12: cpu(0, 1, 0, 12, 13),
            13: cpu(0, 1, 0, 12, 13),
            14: cpu(0, 1, 1, 14, 15),
            15: cpu(0, 1, 1, 14, 15),
            255: cpu(1, 0, 0, 255),
            256: cpu(1, 1, 0, 256),
        }

    def test_linux_cpu_list_round_trip(self):
        cpus = {0, 2, 3, 11, 12, 13, 255}
        self.assertEqual(isolation.format_cpus(cpus), '0,2-3,11-13,255')
        self.assertEqual(isolation.cpu_list('0,2-3,11-13,255'), cpus)
        with self.assertRaises(ValueError):
            isolation.cpu_list('13-11')

    def test_same_die_and_cross_die_allow_repeated_core_ids(self):
        pool = set(self.topology)
        self.assertEqual(isolation.choose_cpus(self.topology, pool, pool, 'same-die'), (12, 14, 255))
        self.assertEqual(isolation.choose_cpus(self.topology, {2, 14}, {0}, 'cross-die'), (2, 14, 0))
        self.assertEqual(isolation.choose_cpus(self.topology, pool, pool, 'cross-die'), (14, 255, 12))
        self.assertEqual(isolation.choose_cpus(self.topology, {0, 2}, {12}, 'same-die'), (0, 2, 12))

    def test_smt_siblings_are_not_distinct_cores(self):
        pool = set(self.topology)
        with self.assertRaisesRegex(ValueError, 'distinct physical cores'):
            isolation.choose_cpus(self.topology, pool, pool, 'any', (12, 13, 0))

    def test_same_core_id_on_different_packages_without_die_id(self):
        first = cpu(0, None, 0, 2)
        second = cpu(1, None, 0, 14)
        self.assertTrue(isolation.pair_matches(first, second, 'cross-die'))
        self.assertFalse(isolation.pair_matches(first, second, 'same-die'))

    def test_boot_keeps_system_housekeeping_even_above_core_id_limit(self):
        self.assertEqual(isolation.boot_parameters(self.topology, (12, 14, 0)),
                         'nosmt isolcpus=domain,managed_irq,12,14 nohz_full=12,14 '
                         'rcu_nocbs=12,14 irqaffinity=0,2,255-256')
        with self.assertRaisesRegex(ValueError, 'CPU 0'):
            isolation.boot_parameters(self.topology, (0, 14, 2))
        with self.assertRaisesRegex(ValueError, 'SMT threads'):
            isolation.boot_parameters(self.topology, (13, 14, 0))

    def test_isolated_run_checks_live_nohz_and_online_siblings(self):
        online = {2: cpu(0, 0, 1, 2), 12: cpu(0, 1, 0, 12), 14: cpu(0, 1, 1, 14)}
        self.assertEqual(isolation.run_layout(online, {12, 14}, {12, 14}, set(online),
                                              'isolated', 'any', None), (12, 14, 2))
        with self.assertRaises(ValueError):
            isolation.run_layout(online, {12, 14}, {12}, set(online), 'isolated', 'any', (12, 14, 2))
        with self.assertRaises(ValueError):
            isolation.run_layout(self.topology, {12, 14}, {12, 14}, set(self.topology),
                                 'isolated', 'any', (12, 14, 0))

    def test_auto_prefers_eligible_isolated_pair_and_honors_housekeeping_override(self):
        online = {cpu_id: cpu(0, 0, cpu_id, cpu_id) for cpu_id in (0, 2, 4, 12, 14)}
        reserved = {12, 14}
        layout = (online, reserved, reserved, set(online), 'auto', 'any')
        self.assertEqual(isolation.select_run_layout(*layout, None), ('isolated', (12, 14, 4)))
        self.assertEqual(isolation.select_run_layout(*layout, (0, 2, 4)), ('housekeeping', (0, 2, 4)))
        with self.assertRaisesRegex(ValueError, 'both workers'):
            isolation.select_run_layout(*layout, (12, 2, 4))

    def test_auto_falls_back_when_only_one_isolated_worker_is_eligible(self):
        online = {cpu_id: cpu(0, 0, cpu_id, cpu_id) for cpu_id in (0, 2, 4, 12, 14)}
        layout = (online, {12, 14}, {12}, set(online))
        self.assertEqual(isolation.select_run_layout(*layout, 'auto', 'any', None), ('housekeeping', (2, 4, 0)))
        with self.assertRaises(ValueError):
            isolation.select_run_layout(*layout, 'isolated', 'any', None)
        with self.assertRaises(ValueError):
            isolation.select_run_layout(*layout, 'auto', 'any', (12, 14, 0))


if __name__ == '__main__':
    unittest.main()
