// Self-tests for the framework's own machinery.
//
// This binary registers a controlled mix of tests -- some passing, one with a
// deliberate failure, one that throws, and one that overruns a tight timeout
// -- then runs them through a Runner and asserts the resulting RunSummary has
// exactly the pass/fail/timeout/error counts we expect. It also unit-tests the
// mocks and reporters directly.
//
// Because it drives the global Registry, it does NOT link the example suite;
// only the tests defined here are present, so the counts are deterministic.

// The self-tests assert on real expected values, so they must stay active even
// in Release builds (where CMake defines NDEBUG and would otherwise disable
// <cassert>). Force assertions on before including the header.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "edtf/edtf.hpp"

using namespace edtf;

// ---- Tests fed to the Runner ------------------------------------------------
// Six cases below: 3 pass, 1 assertion-fail, 1 throw (error), 1 timeout.

TEST_CASE(Self, passes_plain) { ASSERT_EQ(2 + 2, 4); }

TEST_CASE(Self, passes_with_expect) {
    EXPECT_TRUE(true);
    EXPECT_NEAR(1.0, 1.0001, 0.001);
}

TEST_CASE(Self, passes_throws_assertion) {
    ASSERT_THROWS(throw std::runtime_error("boom"), std::runtime_error);
}

TEST_CASE(Self, fails_on_purpose, edtf::opts().tags("expected-fail")) {
    ASSERT_EQ(1, 2);          // fatal: aborts here
    ASSERT_TRUE(false);       // not reached
}

TEST_CASE(Self, errors_on_purpose, edtf::opts().tags("expected-fail")) {
    throw std::logic_error("unhandled in body");
}

TEST_CASE(Self, times_out_on_purpose,
          edtf::opts().tags("expected-fail").timeout_ms_(30)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
}

// ---- Direct unit tests of components ---------------------------------------

static void test_register_bank() {
    mock::MockRegisterBank bank(4);
    bank.write(0, 0xDEAD);
    assert(bank.read(0) == 0xDEAD);
    assert(bank.write_count() == 1);
    assert(bank.read_count() == 1);

    bool threw = false;
    try {
        bank.read(10);
    } catch (const mock::HardwareError&) {
        threw = true;
    }
    assert(threw);

    // Write hook implementing write-one-to-clear on bit 0.
    bank.poke(1, 0x3);
    bank.set_write_hook([](uint32_t, uint32_t cur, uint32_t incoming) {
        return cur & ~incoming;  // clear the bits set in the incoming word
    });
    bank.write(1, 0x1);
    assert(bank.peek(1) == 0x2);
}

static void test_i2c_expectations() {
    mock::MockI2CBus bus;
    bus.attach_device(0x50);
    bus.expect_write(0x50, 0x01, 0xAB)
       .expect_read(0x50, 0x02, 0x7F);

    bus.write_register(0x50, 0x01, 0xAB);
    uint8_t v = bus.read_register(0x50, 0x02);
    assert(v == 0x7F);
    assert(bus.verify().empty());
    assert(bus.log().size() == 2);

    // A mismatch must be reported.
    mock::MockI2CBus bus2;
    bus2.attach_device(0x50);
    bus2.expect_write(0x50, 0x01, 0xAB);
    bool mismatch = false;
    try {
        bus2.write_register(0x50, 0x01, 0x00);  // wrong value
    } catch (const mock::HardwareError&) {
        mismatch = true;
    }
    assert(mismatch);

    // An unmet expectation must be surfaced by verify().
    mock::MockI2CBus bus3;
    bus3.attach_device(0x50);
    bus3.expect_read(0x50, 0x09, 0x11);
    assert(!bus3.verify().empty());
}

static void test_serial_port() {
    mock::MockSerialPort port;
    port.inject_rx({0x01, 0x02, 0x03});
    std::vector<uint8_t> out;
    std::size_t n = port.read(out, 2);
    assert(n == 2);
    assert(out.size() == 2 && out[0] == 0x01 && out[1] == 0x02);
    assert(port.rx_remaining() == 1);

    port.write({'h', 'i'});
    assert(port.tx_string() == "hi");
}

static void test_runner_summary() {
    RunOptions opts;
    opts.format = RunOptions::Format::Console;
    opts.color = false;
    Runner runner(std::move(opts));

    std::ostringstream sink;  // capture the report; we inspect the summary
    std::ostringstream err;
    bool ok = runner.run(sink, err);

    const RunSummary& s = runner.summary();
    assert(s.total == 6);
    assert(s.passed == 3);
    assert(s.failed == 1);
    assert(s.errored == 1);
    assert(s.timed_out == 1);
    assert(!ok);  // the suite as a whole did not pass

    // The console reporter should have emitted output for each case.
    std::string text = sink.str();
    assert(text.find("Self.passes_plain") != std::string::npos);
    assert(text.find("TIMEOUT") != std::string::npos);
}

static void test_tag_filtering() {
    // Only the deliberately-failing cases carry the expected-fail tag.
    RunOptions opts;
    opts.color = false;
    opts.exclude_tags.insert("expected-fail");
    Runner runner(std::move(opts));

    std::ostringstream sink;
    std::ostringstream err;
    bool ok = runner.run(sink, err);
    const RunSummary& s = runner.summary();
    assert(s.total == 3);   // the three passing cases remain
    assert(s.passed == 3);
    assert(ok);
}

static void test_junit_output_is_well_formed() {
    RunOptions opts;
    opts.color = false;
    opts.include_tags.insert("expected-fail");  // the 3 non-passing cases
    opts.format = RunOptions::Format::Junit;
    opts.junit_suite = "selftest";
    Runner runner(std::move(opts));

    std::ostringstream sink;
    std::ostringstream err;
    runner.run(sink, err);
    std::string xml = sink.str();

    assert(xml.find("<?xml") != std::string::npos);
    assert(xml.find("<testsuites") != std::string::npos);
    assert(xml.find("name=\"selftest\"") != std::string::npos);
    assert(xml.find("<failure") != std::string::npos);
    assert(xml.find("type=\"timeout\"") != std::string::npos);
    assert(xml.find("type=\"exception\"") != std::string::npos);
    // tests=3, failures=1, errors=2 (timeout counts as an error in JUnit).
    assert(xml.find("tests=\"3\"") != std::string::npos);
}

static void test_tap_output() {
    RunOptions opts;
    opts.color = false;
    opts.name_filter = "Self.passes_plain";
    opts.format = RunOptions::Format::Tap;
    Runner runner(std::move(opts));

    std::ostringstream sink;
    std::ostringstream err;
    bool ok = runner.run(sink, err);
    std::string tap = sink.str();
    assert(ok);
    assert(tap.find("TAP version 13") != std::string::npos);
    assert(tap.find("1..1") != std::string::npos);
    assert(tap.find("ok 1 - Self.passes_plain") != std::string::npos);
}

static void test_arg_parsing() {
    const char* argv[] = {"prog", "--reporter", "tap", "--tag", "slow",
                          "--timeout", "250", "--no-color"};
    int argc = static_cast<int>(sizeof(argv) / sizeof(argv[0]));
    bool should_exit = false;
    int code = 0;
    std::ostringstream out, err;
    RunOptions o = parse_args(argc, const_cast<char**>(argv), out, err,
                              should_exit, code);
    assert(!should_exit);
    assert(o.format == RunOptions::Format::Tap);
    assert(o.include_tags.count("slow") == 1);
    assert(o.default_timeout == std::chrono::milliseconds(250));
    assert(o.color == false);

    // Unknown flag -> should request exit with code 2.
    const char* bad[] = {"prog", "--nope"};
    std::ostringstream o2, e2;
    parse_args(2, const_cast<char**>(bad), o2, e2, should_exit, code);
    assert(should_exit && code == 2);
}

int main() {
    test_register_bank();
    test_i2c_expectations();
    test_serial_port();
    test_tag_filtering();
    test_junit_output_is_well_formed();
    test_tap_output();
    test_arg_parsing();
    // Run the full mixed suite last: it consumes the same global registry but
    // does not mutate it, so ordering relative to the filtered runs is safe.
    test_runner_summary();

    std::cout << "all framework self-tests passed\n";
    return 0;
}
