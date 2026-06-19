// The test runner: filtering, per-test timeout execution, and reporting.
#ifndef EDTF_RUNNER_HPP
#define EDTF_RUNNER_HPP

#include <chrono>
#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <vector>

#include "edtf/registry.hpp"
#include "edtf/reporter.hpp"

namespace edtf {

// Selection / output options for a run. Constructed by parse_args or filled
// directly by embedding code.
struct RunOptions {
    std::string name_filter;            // substring match on suite.name
    std::set<std::string> include_tags; // run only cases with any of these tags
    std::set<std::string> exclude_tags; // skip cases with any of these tags
    std::chrono::milliseconds default_timeout{0};  // applied when a case has none
    bool list_only = false;
    bool color = true;

    enum class Format { Console, Tap, Junit } format = Format::Console;
    std::string junit_suite = "edtf";
    std::string output_path;            // empty -> stdout
};

class Runner {
public:
    explicit Runner(RunOptions opts) : opts_(std::move(opts)) {}

    // Runs the selected tests, writing the formatted report to `report_out`
    // and any diagnostics to `err`. Returns true when every selected test
    // passed.
    bool run(std::ostream& report_out, std::ostream& err);

    // Convenience overload: routes the report to the configured output file,
    // or to std::cout when none is set. `err` receives diagnostics.
    bool run(std::ostream& err);

    const std::vector<TestResult>& results() const { return results_; }
    const RunSummary& summary() const { return summary_; }

private:
    bool selected(const TestCase& tc) const;
    TestResult run_one(const TestCase& tc) const;

    RunOptions opts_;
    std::vector<TestResult> results_;
    RunSummary summary_;
};

// Parse argv into RunOptions. On --help / parse error, prints to `out`/`err`
// and sets `should_exit` with the desired process exit code.
RunOptions parse_args(int argc, char** argv, std::ostream& out,
                      std::ostream& err, bool& should_exit, int& exit_code);

// Convenience entry point: parse args, run, return a process exit code.
int run_all(int argc, char** argv);

}  // namespace edtf

#endif  // EDTF_RUNNER_HPP
