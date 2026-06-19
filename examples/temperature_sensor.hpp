// A small, self-contained temperature-sensor driver modeled on the TI TMP102.
//
// The driver talks to a bus abstraction (here, MockI2CBus) and exposes a tidy
// C++ API on top of the device's raw register layout. It exists to demonstrate
// driving the framework's mocks against realistic embedded code; the same
// driver would compile against a real I2C HAL with an equivalent surface.
#ifndef EDTF_EXAMPLES_TEMP_SENSOR_HPP
#define EDTF_EXAMPLES_TEMP_SENSOR_HPP

#include <cstdint>
#include <stdexcept>

#include "edtf/mock_hardware.hpp"

namespace example {

// Register map (subset) of the TMP102.
namespace tmp102 {
constexpr uint8_t kDefaultAddr = 0x48;
constexpr uint8_t kRegTemp = 0x00;     // 12-bit temperature, big-endian
constexpr uint8_t kRegConfig = 0x01;   // configuration
constexpr uint8_t kRegTLow = 0x02;
constexpr uint8_t kRegTHigh = 0x03;

// Configuration bits (high byte).
constexpr uint8_t kCfgShutdown = 0x01;       // SD: low-power one-shot mode
constexpr uint8_t kCfgOneShot = 0x80;        // OS: trigger a single conversion
}  // namespace tmp102

struct SensorError : std::runtime_error {
    explicit SensorError(const std::string& w) : std::runtime_error(w) {}
};

class TemperatureSensor {
public:
    TemperatureSensor(edtf::mock::MockI2CBus& bus, uint8_t addr = tmp102::kDefaultAddr)
        : bus_(bus), addr_(addr) {}

    // Reads the 12-bit temperature register and converts to degrees Celsius.
    // The TMP102 reports temperature as a signed 12-bit value in the upper
    // bits of a big-endian 16-bit word, with a resolution of 0.0625 C/LSB.
    double read_celsius() {
        uint8_t hi = bus_.read_register(addr_, tmp102::kRegTemp);
        uint8_t lo = bus_.read_register(addr_, tmp102::kRegTemp + 1);
        // The 12-bit reading is left-justified in a big-endian 16-bit word.
        // Reconstructing it as a signed 16-bit value and arithmetic-shifting
        // right by four both right-justifies and sign-extends in one step.
        int16_t raw = static_cast<int16_t>((hi << 8) | lo);
        int value = raw >> 4;
        return value * 0.0625;
    }

    // Triggers a one-shot conversion: set OS bit while in shutdown mode.
    void trigger_one_shot() {
        uint8_t cfg = bus_.read_register(addr_, tmp102::kRegConfig);
        cfg |= tmp102::kCfgShutdown | tmp102::kCfgOneShot;
        bus_.write_register(addr_, tmp102::kRegConfig, cfg);
    }

    void set_high_limit_celsius(double celsius) {
        write_limit(tmp102::kRegTHigh, celsius);
    }
    void set_low_limit_celsius(double celsius) {
        write_limit(tmp102::kRegTLow, celsius);
    }

    uint8_t address() const { return addr_; }

private:
    void write_limit(uint8_t reg, double celsius) {
        int counts = static_cast<int>(celsius / 0.0625);
        if (counts > 2047 || counts < -2048) {
            throw SensorError("temperature limit out of representable range");
        }
        uint16_t raw = static_cast<uint16_t>((counts & 0x0FFF) << 4);
        // The 16-bit limit occupies the register pair reg / reg+1, big-endian.
        bus_.write_register(addr_, reg, static_cast<uint8_t>(raw >> 8));
        bus_.write_register(addr_, reg + 1, static_cast<uint8_t>(raw & 0xFF));
    }

    edtf::mock::MockI2CBus& bus_;
    uint8_t addr_;
};

}  // namespace example

#endif  // EDTF_EXAMPLES_TEMP_SENSOR_HPP
