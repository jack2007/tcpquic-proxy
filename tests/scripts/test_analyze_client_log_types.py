import importlib.util
import pathlib
import unittest


SCRIPT_PATH = (
    pathlib.Path(__file__).resolve().parents[2]
    / "scripts"
    / "analyze_client_log_types.py"
)


def load_module():
    spec = importlib.util.spec_from_file_location("analyze_client_log_types", SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class AnalyzeClientLogTypesTest(unittest.TestCase):
    def test_classifies_event_before_message_prefix_and_sorts_by_count(self):
        module = load_module()
        lines = [
            "[2026-06-27 21:43:03.252] [info] event=stats_tick global: quic_conns=1\n",
            "[2026-06-27 21:43:03.253] [info] event=stats_tick global: quic_conns=1\n",
            "[2026-06-27 21:43:03.254] [info]   quic_metrics:\n",
            "[2026-06-27 21:43:03.255] [info]   quic_metrics:\n",
            "[2026-06-27 21:43:03.256] [info] tcpquic-proxy runtime: rtt=66ms\n",
            "[2026-06-27 21:43:03.257] [warning] plain warning without event\n",
        ]

        ranking = module.rank_log_types(lines)

        self.assertEqual(
            ranking,
            [
                ("event=stats_tick", 2),
                ("quic_metrics", 2),
                ("tcpquic-proxy runtime", 1),
                ("plain warning without event", 1),
            ],
        )

    def test_uses_level_when_line_does_not_match_log_format(self):
        module = load_module()

        ranking = module.rank_log_types(["raw unstructured line\n"])

        self.assertEqual(ranking, [("unparsed", 1)])

    def test_classifies_metric_key_value_line_by_first_key(self):
        module = load_module()

        log_type = module.classify_line(
            "[2026-06-27 21:43:03.268] [info]   congestion_events=0 cwnd=24099 mtu=1500\n"
        )

        self.assertEqual(log_type, "congestion_events")


if __name__ == "__main__":
    unittest.main()
