# Embedded Device Test Framework

> A lightweight C++ test framework for embedded devices: fixtures, mock hardware, timeouts, JUnit/TAP.

[![CI](https://github.com/anuradha-embedded/embedded-device-test-framework/actions/workflows/ci.yml/badge.svg)](https://github.com/anuradha-embedded/embedded-device-test-framework/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C.svg)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-linux%20%7C%20macOS-lightgrey.svg)](#building)

## Overview

`edtf` is a small, dependency-free unit-test framework built for the way
embedded code is actually verified on a host: you don't have the silicon in
the loop on every commit, so you stand up *mock peripherals*, drive your
driver against them, and assert on both the computed results and the exact bus
traffic the driver produced.

It is deliberately compact. The runtime pieces compile to a few hundred lines
and the public surface lives almost entirely in headers, so it drops cleanly
into a cross-compiled firmware project or runs as a normal host-side build in
CI. There is no external test runner, no script harness, and nothing to
install: a test binary links one small static library and runs standalone.

### Motivation

Most general-purpose C++ test frameworks assume a desktop process model and
say nothing about hardware. Embedded verification needs a few extra things:

- A way to *simulate* memory-mapped registers, I2C devices, and serial ports
  so a driver can be exercised without a board attached.
- An expectation/verify mechanism so a test can assert that a driver issued
  exactly the right sequence of bus transactions.
- Hard per-test timeouts, because a buggy driver polling a status bit that
  never sets should be reported as a failure, not hang the suite.
- Machine-readable output (JUnit XML, TAP) that CI dashboards understand.

`edtf` provides exactly those, and nothing heavier.

## Features

- **Registration macros** — `TEST_CASE(suite, name)` for free-function tests
  and `TEST_SUITE_F(fixture, name)` for fixture-backed tests.
- **Fixtures** with `set_up()` / `tear_down()` hooks run around every case.
- **Rich assertions** — fatal `ASSERT_*` and non-fatal `EXPECT_*` families:
  `EQ`, `NE`, `LT/LE/GT/GE`, `TRUE/FALSE`, `NEAR` (floating-point tolerance),
  and `THROWS` (exception-type checking).
- **Per-test timeouts** — each test runs on its own worker thread; one that
  exceeds its deadline is reported as `TIMEOUT` instead of blocking the run.
- **Tagging and filtering** — tag cases and select them by tag or by a
  substring of `suite.name` at the command line.
- **Multiple reporters** — human-readable console (with color), TAP v13, and
  Surefire-style JUnit XML for CI.
- **Mock hardware** — `MockRegisterBank`, `MockI2CBus`, and `MockSerialPort`,
  the I2C bus with a scripted expectation queue and a `verify()` step.
- **Header-first** — include `edtf/edtf.hpp`, link the small `edtf` library,
  done.

## Design / Architecture

```
                +------------------------------------------+
   TEST_CASE /  |              Registry (global)           |
   TEST_SUITE_F |  collects TestCase{suite,name,tags,      |
   --(register)-> timeout,body} at static-init time        |
                +---------------------+--------------------+
                                      |
                                      v
                +------------------------------------------+
                |                 Runner                   |
                |  filter by tag / name                    |
                |  run each body on a worker thread with   |
                |  a deadline  -> Outcome + assertion stats|
                +---------------------+--------------------+
                                      |  TestResult
                 +--------------------+--------------------+
                 v                    v                    v
          ConsoleReporter        TapReporter         JunitReporter

   Test bodies use ASSERT_*/EXPECT_* which funnel through a thread-local
   CheckSink, and drive the mock peripherals under test:

          MockRegisterBank   MockI2CBus(+expect/verify)   MockSerialPort
```

Component summary:

| Component | Header | Responsibility |
|-----------|--------|----------------|
| Assertions | `edtf/assertions.hpp` | `ASSERT_*` / `EXPECT_*`, thread-local sink |
| Registration | `edtf/registry.hpp` | `TEST_CASE`, `TEST_SUITE_F`, fixtures, registry |
| Mocks | `edtf/mock_hardware.hpp` | register bank, I2C bus, serial port |
| Reporters | `edtf/reporter.hpp` | console / TAP / JUnit output |
| Runner | `edtf/runner.hpp` | filtering, timeouts, CLI parsing |

## Building

The library needs only a C++17 toolchain and a threads implementation
(`-pthread`). No third-party packages are required.

### CMake

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### Make

```sh
make            # builds example_suite and the self-tests
make test       # builds, runs the self-tests, and the example checks
make clean
```

## Usage

Run the demonstration suite (the `example_suite` binary). It drives a
simulated TMP102 temperature sensor over `MockI2CBus`, plus direct tests of
the register bank and serial-port mocks, and a timeout demo:

```sh
./build/example_suite
```

```text
Running 11 tests
  [PASS] SensorFixture.reads_room_temperature (0 ms)
  [PASS] SensorFixture.reads_negative_temperature (0 ms)
  [PASS] SensorFixture.one_shot_scripts_expected_bus_traffic (0 ms)
  [PASS] SensorFixture.high_limit_encodes_big_endian (0 ms)
  [PASS] SensorFixture.out_of_range_limit_throws (0 ms)
  [PASS] I2CBus.unknown_device_throws (0 ms)
  [PASS] RegisterBank.read_clears_status_bit (0 ms)
  [PASS] RegisterBank.out_of_range_access_throws (0 ms)
  [PASS] SerialPort.round_trip_buffers (0 ms)
  [PASS] Assertions.expect_family_is_non_fatal (0 ms)
  [TIMEOUT] Timeout.slow_test_is_killed (52 ms)
        exceeded deadline of 50 ms

Summary: 10/11 passed, 1 timed out (53 ms)
```

Filter by name or tag, list without running, or emit machine-readable output:

```sh
./build/example_suite --filter SensorFixture          # only the sensor tests
./build/example_suite --exclude-tag slow              # skip the timeout demo
./build/example_suite --list                          # list selected tests
./build/example_suite --reporter tap                  # TAP v13 to stdout
./build/example_suite --reporter junit -o results.xml # JUnit XML to a file
```

TAP output (`--reporter tap --filter SensorFixture`):

```text
TAP version 13
1..5
ok 1 - SensorFixture.reads_room_temperature
ok 2 - SensorFixture.reads_negative_temperature
ok 3 - SensorFixture.one_shot_scripts_expected_bus_traffic
ok 4 - SensorFixture.high_limit_encodes_big_endian
ok 5 - SensorFixture.out_of_range_limit_throws
# passed 5 of 5
```

### Writing a test

```cpp
#include "edtf/edtf.hpp"

using edtf::mock::MockI2CBus;

// A fixture: set_up() runs before every case that uses it.
class SensorFixture : public edtf::Fixture {
public:
    void set_up() override { bus.attach_device(0x48); }
protected:
    MockI2CBus bus;
};

TEST_SUITE_F(SensorFixture, reports_room_temperature) {
    bus.set_register(0x48, 0x00, 0x19);   // seed the TEMP register
    bus.set_register(0x48, 0x01, 0x00);
    TemperatureSensor sensor(bus, 0x48);
    ASSERT_NEAR(sensor.read_celsius(), 25.0, 0.01);
}

// Tag a case and give it a 50 ms deadline.
TEST_CASE(Timeout, must_finish, edtf::opts().tags("slow").timeout_ms_(50)) {
    ASSERT_TRUE(true);
}

int main(int argc, char** argv) { return edtf::run_all(argc, argv); }
```

The I2C mock can also assert *interactions*. Script the expected traffic, run
the driver, then `verify()`:

```cpp
bus.expect_read(0x48, 0x01, 0x60)        // driver will read CONFIG -> 0x60
   .expect_write(0x48, 0x01, 0xE1);      // ...then write it back as 0xE1
sensor.trigger_one_shot();
ASSERT_EQ(bus.verify(), std::string()); // empty == all expectations met, in order
```

## Project Structure

```
embedded-device-test-framework/
├── include/edtf/
│   ├── edtf.hpp              umbrella header
│   ├── assertions.hpp        ASSERT_* / EXPECT_* macros
│   ├── registry.hpp          TEST_CASE / TEST_SUITE_F, fixtures
│   ├── mock_hardware.hpp     MockRegisterBank / MockI2CBus / MockSerialPort
│   ├── reporter.hpp          reporter interface + factories
│   └── runner.hpp            runner + CLI
├── src/
│   ├── registry.cpp          registry, tag parsing, check sink
│   ├── reporter.cpp          console / TAP / JUnit reporters
│   └── runner.cpp            filtering, timeout execution, arg parsing
├── examples/
│   ├── temperature_sensor.hpp   simulated TMP102 driver
│   ├── example_suite.cpp        the main demonstration binary
│   └── tmp102_session.txt       sample register dump + scripted interactions
├── tests/
│   └── framework_tests.cpp      framework self-tests (assert-based)
├── CMakeLists.txt
├── Makefile
├── LICENSE
└── .github/workflows/ci.yml
```

## Testing

The framework is self-testing. `tests/framework_tests.cpp` registers a
controlled mix of cases — passing, deliberately failing, throwing, and
timing out — runs them through a `Runner`, and asserts the resulting summary
has exactly the expected pass / fail / error / timeout counts. It also
unit-tests each mock directly and checks that the JUnit and TAP reporters
produce well-formed output. Assertions stay enabled even in Release builds, so
the checks are real regardless of build type.

Two `ctest` cases additionally exercise the example binary: one confirms it
lists the expected tests, and one runs it with the timeout demo excluded and
expects a clean pass.

```sh
ctest --test-dir build --output-on-failure
```

```text
Test project build
    Start 1: framework_self_tests
1/3 Test #1: framework_self_tests .............   Passed
    Start 2: example_lists_tests
2/3 Test #2: example_lists_tests ..............   Passed
    Start 3: example_passes_without_slow
3/3 Test #3: example_passes_without_slow ......   Passed

100% tests passed, 0 tests failed out of 3
```

## Roadmap / Future Work

- Parameterized / data-driven test cases.
- A `MockSPIBus` peripheral and a generic transcript-replay device model.
- Optional parallel execution of independent tests across a thread pool.
- Cooperative timeout cancellation so over-running workers can be reclaimed
  rather than detached.
- A header-only single-include amalgamation for the simplest possible drop-in.

## License

Released under the MIT License. See [LICENSE](LICENSE).
