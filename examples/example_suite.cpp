// example_suite: the framework's flagship demonstration binary.
//
// It registers a battery of tests that drive the simulated TMP102 temperature
// sensor through MockI2CBus, exercises fixtures, the assertion families, the
// per-test timeout mechanism, and the mock expectation/verify flow. Run it
// directly to see human-readable output, or pass --reporter junit/tap for CI.
#include <chrono>
#include <thread>

#include "edtf/edtf.hpp"
#include "temperature_sensor.hpp"

using edtf::mock::MockI2CBus;
using edtf::mock::MockSerialPort;
using edtf::mock::MockRegisterBank;
using example::TemperatureSensor;
namespace tmp102 = example::tmp102;

// ---------------------------------------------------------------------------
// Fixture: a bus pre-populated with a TMP102 at its default address, plus a
// driver bound to it. set_up runs before every test using this fixture.
// ---------------------------------------------------------------------------
class SensorFixture : public edtf::Fixture {
public:
    void set_up() override {
        bus.attach_device(tmp102::kDefaultAddr);
        bus.set_register(tmp102::kDefaultAddr, tmp102::kRegConfig, 0x60);
        sensor.reset(new TemperatureSensor(bus, tmp102::kDefaultAddr));
    }

    void tear_down() override { sensor.reset(); }

protected:
    // Helper to load a known temperature into the sensor's registers.
    void load_temperature(int raw12) {
        uint16_t word = static_cast<uint16_t>((raw12 & 0x0FFF) << 4);
        bus.set_register(tmp102::kDefaultAddr, tmp102::kRegTemp,
                         static_cast<uint8_t>(word >> 8));
        bus.set_register(tmp102::kDefaultAddr, tmp102::kRegTemp + 1,
                         static_cast<uint8_t>(word & 0xFF));
    }

    MockI2CBus bus;
    std::unique_ptr<TemperatureSensor> sensor;
};

// 25.0 C -> 400 counts (25 / 0.0625). Verify the conversion math.
TEST_SUITE_F(SensorFixture, reads_room_temperature) {
    load_temperature(400);
    ASSERT_NEAR(sensor->read_celsius(), 25.0, 0.01);
}

// A negative temperature exercises the 12-bit two's-complement sign extension.
TEST_SUITE_F(SensorFixture, reads_negative_temperature) {
    load_temperature(-160);  // -160 * 0.0625 = -10.0 C
    ASSERT_NEAR(sensor->read_celsius(), -10.0, 0.01);
}

// The one-shot trigger must read-modify-write the config register, setting the
// shutdown and one-shot bits. We script the exact bus traffic and verify it.
TEST_SUITE_F(SensorFixture, one_shot_scripts_expected_bus_traffic) {
    bus.expect_read(tmp102::kDefaultAddr, tmp102::kRegConfig, 0x60)
        .expect_write(tmp102::kDefaultAddr, tmp102::kRegConfig,
                      0x60 | tmp102::kCfgShutdown | tmp102::kCfgOneShot);

    sensor->trigger_one_shot();

    ASSERT_EQ(bus.verify(), std::string());
    ASSERT_EQ(bus.log().size(), static_cast<std::size_t>(2));
}

// Writing a high limit must produce a big-endian register pair.
TEST_SUITE_F(SensorFixture, high_limit_encodes_big_endian) {
    sensor->set_high_limit_celsius(80.0);  // 80 / 0.0625 = 1280 = 0x500
    // 0x500 << 4 = 0x5000 -> hi 0x50, lo 0x00
    ASSERT_EQ(bus.get_register(tmp102::kDefaultAddr, tmp102::kRegTHigh),
              static_cast<uint8_t>(0x50));
    ASSERT_EQ(bus.get_register(tmp102::kDefaultAddr, tmp102::kRegTHigh + 1),
              static_cast<uint8_t>(0x00));
}

// Out-of-range limits should throw a SensorError; exercises ASSERT_THROWS.
TEST_SUITE_F(SensorFixture, out_of_range_limit_throws) {
    ASSERT_THROWS(sensor->set_high_limit_celsius(1000.0), example::SensorError);
}

// Talking to a device that is not on the bus is a hardware fault.
TEST_CASE(I2CBus, unknown_device_throws) {
    MockI2CBus bus;
    bus.attach_device(0x48);
    TemperatureSensor stray(bus, 0x49);
    ASSERT_THROWS(stray.read_celsius(), edtf::mock::HardwareError);
}

// MockRegisterBank with a read-clears-on-read status register.
TEST_CASE(RegisterBank, read_clears_status_bit) {
    MockRegisterBank bank(8);
    constexpr uint32_t kStatus = 0x02;
    bank.poke(kStatus, 0x1);  // a latched interrupt flag
    bank.set_read_hook([&](uint32_t addr, uint32_t stored) -> uint32_t {
        if (addr == kStatus) {
            bank.poke(kStatus, 0);  // clear on read
        }
        return stored;
    });
    ASSERT_EQ(bank.read(kStatus), 0x1u);
    ASSERT_EQ(bank.read(kStatus), 0x0u);
    ASSERT_GE(bank.read_count(), static_cast<std::size_t>(2));
}

// Out-of-range register access is rejected.
TEST_CASE(RegisterBank, out_of_range_access_throws) {
    MockRegisterBank bank(4);
    ASSERT_THROWS(bank.read(4), edtf::mock::HardwareError);
    ASSERT_THROWS(bank.write(99, 0), edtf::mock::HardwareError);
}

// MockSerialPort round trip: inject RX, drain it, capture TX.
TEST_CASE(SerialPort, round_trip_buffers) {
    MockSerialPort port;
    port.inject_rx_string("OK\r\n");
    ASSERT_EQ(port.read_byte(), static_cast<int>('O'));
    ASSERT_EQ(port.read_byte(), static_cast<int>('K'));

    port.write_byte('A');
    port.write_byte('T');
    ASSERT_EQ(port.tx_string(), std::string("AT"));
    ASSERT_EQ(port.rx_remaining(), static_cast<std::size_t>(2));
}

// Demonstrates the non-fatal EXPECT_* family: both checks run even though the
// first passes; all are expected to pass here.
TEST_CASE(Assertions, expect_family_is_non_fatal, edtf::opts().tags("framework")) {
    EXPECT_TRUE(1 + 1 == 2);
    EXPECT_EQ(std::string("edtf"), std::string("edtf"));
    EXPECT_NEAR(3.14159, 3.14, 0.01);
}

// A deliberately slow test guarded by a tight per-test deadline. The runner
// must report this as a TIMEOUT, not a crash. Tagged so CI can opt out.
TEST_CASE(Timeout, slow_test_is_killed,
          edtf::opts().tags("slow").timeout_ms_(50)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    ASSERT_TRUE(true);  // never reached before the deadline fires
}

int main(int argc, char** argv) {
    return edtf::run_all(argc, argv);
}
