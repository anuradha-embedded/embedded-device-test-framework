// Per-test result and reporter interface.
#ifndef EDTF_REPORTER_HPP
#define EDTF_REPORTER_HPP

#include <chrono>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace edtf {

enum class Outcome {
    Passed,
    Failed,   // one or more assertions failed
    Timeout,  // exceeded its per-test deadline
    Error     // threw something other than AssertionAbort
};

struct TestResult {
    std::string suite;
    std::string name;
    Outcome outcome = Outcome::Passed;
    int assertions_passed = 0;
    int assertions_failed = 0;
    std::chrono::milliseconds duration{0};
    std::string detail;  // failure text / exception message

    std::string full_name() const { return suite + "." + name; }
    bool ok() const { return outcome == Outcome::Passed; }
};

struct RunSummary {
    int total = 0;
    int passed = 0;
    int failed = 0;
    int timed_out = 0;
    int errored = 0;
    std::chrono::milliseconds duration{0};

    int not_passed() const { return failed + timed_out + errored; }
};

// Reporters render results in a particular format. The runner calls them in
// order: begin_run, then on_result per test, then end_run.
class Reporter {
public:
    virtual ~Reporter() = default;
    virtual void begin_run(int total_tests) = 0;
    virtual void on_result(const TestResult& r) = 0;
    virtual void end_run(const RunSummary& s) = 0;
};

std::unique_ptr<Reporter> make_console_reporter(std::ostream& os, bool color);
std::unique_ptr<Reporter> make_tap_reporter(std::ostream& os);
std::unique_ptr<Reporter> make_junit_reporter(std::ostream& os,
                                               std::string suite_name);

const char* to_string(Outcome o);

}  // namespace edtf

#endif  // EDTF_REPORTER_HPP
