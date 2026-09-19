#include "multi_physics_simulator/thermal_simulation/simplified_loss_thermal.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_close(double actual, double expected, const std::string& message) {
    require(std::isfinite(actual) && std::abs(actual - expected) < 1e-12,
            message);
}

// These are compatibility tests for the still-active fast/fallback path.
// Preserving its outputs does not validate these approximations physically.
void test_capacitor_loss() {
    require_close(calculate_capacitor_loss(20.0, 0.01).capacitor_loss,
                  4.0, "capacitor loss must retain I_rms squared times ESR");
    require_close(calculate_capacitor_loss(10.0, 0.02).capacitor_loss,
                  2.0, "capacitor loss must use the supplied effective ESR");
    require_close(calculate_capacitor_loss(0.0, 0.01).capacitor_loss,
                  0.0, "zero current must produce zero capacitor loss");
    require_close(calculate_capacitor_loss(20.0, 0.0).capacitor_loss,
                  0.0, "zero ESR must produce zero capacitor loss");
    require_close(calculate_capacitor_loss(-20.0, 0.01).capacitor_loss,
                  0.0, "negative current must retain the legacy zero result");
    require_close(calculate_capacitor_loss(20.0, -0.01).capacitor_loss,
                  0.0, "negative ESR must retain the legacy zero result");
    require_close(calculate_capacitor_loss(-20.0, -0.01).capacitor_loss,
                  0.0, "two negative inputs must not bypass the loss guard");
}

void test_capacitor_thermal() {
    const CapacitorThermalResult loaded =
        calculate_capacitor_thermal(4.0, 30.0, 2.5, 1.0);
    require_close(loaded.capacitor_hotspot_temperature, 40.0,
                  "hotspot must retain internal temperature plus loss times Rth");
    require_close(loaded.capacitor_surface_temperature, 34.0,
                  "surface must retain its independently supplied resistance");

    const CapacitorThermalResult no_loss =
        calculate_capacitor_thermal(0.0, -10.0, 2.5, 1.0);
    require_close(no_loss.capacitor_hotspot_temperature, -10.0,
                  "zero loss hotspot must equal internal temperature");
    require_close(no_loss.capacitor_surface_temperature, -10.0,
                  "zero loss surface must equal internal temperature");

    const CapacitorThermalResult no_resistance =
        calculate_capacitor_thermal(4.0, 30.0, 0.0, 0.0);
    require_close(no_resistance.capacitor_hotspot_temperature, 30.0,
                  "zero hotspot resistance must produce zero temperature rise");
    require_close(no_resistance.capacitor_surface_temperature, 30.0,
                  "zero surface resistance must produce zero temperature rise");
}

void test_power_module_thermal() {
    require_close(calculate_power_module_thermal(30000.0, 45.0).junction_temperature,
                  100.0, "rated-power thermal approximation must remain unchanged");
    require_close(calculate_power_module_thermal(15000.0, 30.0).junction_temperature,
                  57.5, "partial-power thermal approximation must remain linear");
    require_close(calculate_power_module_thermal(0.0, -10.0).junction_temperature,
                  -10.0, "zero AC power must retain ambient temperature");
}

void test_uncomputed_power_module_loss() {
    const PowerModuleLossResult result;
    require(!result.valid, "uncomputed power-module loss must not be marked valid");
    require_close(result.power_module_loss, 0.0,
                  "uncomputed power-module loss must retain zero initialization");
    require(result.message.empty(),
            "uncomputed power-module loss must retain its empty default message");
}

} // namespace

int main() {
    test_capacitor_loss();
    test_capacitor_thermal();
    test_power_module_thermal();
    test_uncomputed_power_module_loss();
    std::cout << "simplified loss/thermal compatibility tests passed\n";
    return 0;
}
