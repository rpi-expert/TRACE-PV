import unittest

import server


class ComponentConfigurationTest(unittest.TestCase):
    def test_loads_actual_simulation_model_part_numbers(self):
        configuration = server.load_component_configuration(
            server.DEFAULT_SIMULATION_INPUT["model"]
        )
        self.assertEqual(
            configuration,
            {
                "pv_panel": "CS6U-330P",
                "pv_inverter": "SMA_Sunny_Tripower_30000TL-US-10",
                "grid": "GRID001",
                "power_module": "F3L75R12W1H3_B11",
                "capacitor": "Rubycon_475VXG330MEFCSN30X55",
                "fan_cooling": "NMB 09238RE-24N-GU",
                "pcb": "PCB001",
            },
        )

    def test_missing_model_returns_empty_configuration(self):
        self.assertEqual(
            server.load_component_configuration("does/not/exist.json"),
            {},
        )


if __name__ == "__main__":
    unittest.main()
