#include "edtf/runner.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "edtf/assertions.hpp"

namespace edtf {

namespace {

bool contains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    return haystack.find(needle) != std::string::npos;
}

bool any_tag_in(const std::set<std::string>& tags,
                const std::set<std::string>& wanted) {
    for (const auto& t : wanted) {
        if (tags.count(t)) return true;
    }
    return false;
}

}  // namespace

bool Runner::selected(const TestCase& tc) const {
    if (!contains(tc.full_name(), opts_.name_filter)) return false;
    if (!opts_.include_tags.empty() &&
        !any_tag_in(tc.tags, opts_.include_tags)) {
        return false;
    }
    if (!opts_.exclude_tags.empty() &&
        any_tag_in(tc.tags, opts_.exclude_tags)) {
        return false;
    }
    return true;
}

TestResult Runner::run_one(const TestCase& tc) const {
    using clock = std::chrono::steady_clock;

    TestResult result;
    result.suite = tc.suite;
    result.name = tc.name;

    std::chrono::milliseconds deadline = tc.timeout;
    if (deadline.count() == 0) deadline = opts_.default_timeout;

    // The body runs on a dedicated worker thread so an over-running or hung
    // test can be declared a timeout without taking the runner down with it.
    //
    // On timeout the worker cannot be safely interrupted mid-flight, so it is
    // detached and left to finish in the background. To make that safe, every
    // piece of state the worker touches lives in a heap-allocated, refcounted
    // block captured by value -- it outlives this stack frame for as long as
    // the detached worker keeps running.
    struct Shared {
        detail::CheckSink sink;
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        Outcome outcome = Outcome::Passed;
        std::string detail;
    };
    auto state = std::make_shared<Shared>();

    auto start = clock::now();

    std::thread worker([state, &tc] {
        // Bind this worker's thread-local sink so assertion macros target it.
        detail::active_sink() = &state->sink;
        try {
            tc.body();
        } catch (const AssertionAbort&) {
            // Already recorded in the sink; nothing extra to do.
        } catch (const std::exception& e) {
            state->outcome = Outcome::Error;
            state->detail = std::string("unhandled std::exception: ") + e.what();
        } catch (...) {
            state->outcome = Outcome::Error;
            state->detail = "unhandled non-standard exception";
        }
        detail::active_sink() = nullptr;
        {
            std::lock_guard<std::mutex> lk(state->m);
            state->done = true;
        }
        state->cv.notify_one();
    });

    bool timed_out = false;
    {
        std::unique_lock<std::mutex> lk(state->m);
        if (deadline.count() > 0) {
            timed_out =
                !state->cv.wait_for(lk, deadline, [&] { return state->done; });
        } else {
            state->cv.wait(lk, [&] { return state->done; });
        }
    }

    if (timed_out) {
        worker.detach();
        result.outcome = Outcome::Timeout;
        result.detail = "exceeded deadline of " +
                        std::to_string(deadline.count()) + " ms";
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            clock::now() - start);
        return result;
    }

    worker.join();
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        clock::now() - start);
    result.assertions_passed = state->sink.passed();
    result.assertions_failed = state->sink.failed();

    if (state->outcome == Outcome::Error) {
        result.outcome = Outcome::Error;
        result.detail = state->detail;
    } else if (state->sink.failed() > 0) {
        result.outcome = Outcome::Failed;
        result.detail = state->sink.failures();
    } else {
        result.outcome = Outcome::Passed;
    }
    return result;
}

bool Runner::run(std::ostream& console_err) {
    // Route the report to the configured file, or stdout when none is set.
    std::ofstream file;
    if (!opts_.output_path.empty()) {
        file.open(opts_.output_path);
        if (!file) {
            console_err << "error: cannot open output file: "
                        << opts_.output_path << "\n";
            return false;
        }
        return run(file, console_err);
    }
    return run(std::cout, console_err);
}

bool Runner::run(std::ostream& out_ref, std::ostream& console_err) {
    using clock = std::chrono::steady_clock;
    (void)console_err;

    std::vector<const TestCase*> selected;
    for (const auto& tc : Registry::instance().cases()) {
        if (this->selected(tc)) selected.push_back(&tc);
    }

    std::ostream* out = &out_ref;
    if (opts_.list_only) {
        for (const auto* tc : selected) {
            *out << tc->full_name();
            if (!tc->tags.empty()) {
                *out << "  [";
                bool first = true;
                for (const auto& t : tc->tags) {
                    if (!first) *out << ",";
                    *out << t;
                    first = false;
                }
                *out << "]";
            }
            *out << "\n";
        }
        return true;
    }

    std::unique_ptr<Reporter> reporter;
    switch (opts_.format) {
        case RunOptions::Format::Tap:
            reporter = make_tap_reporter(*out);
            break;
        case RunOptions::Format::Junit:
            reporter = make_junit_reporter(*out, opts_.junit_suite);
            break;
        case RunOptions::Format::Console:
        default:
            reporter = make_console_reporter(*out, opts_.color);
            break;
    }

    auto run_start = clock::now();
    reporter->begin_run(static_cast<int>(selected.size()));

    summary_ = RunSummary{};
    summary_.total = static_cast<int>(selected.size());

    for (const auto* tc : selected) {
        TestResult r = run_one(*tc);
        results_.push_back(r);
        reporter->on_result(r);
        switch (r.outcome) {
            case Outcome::Passed:  ++summary_.passed; break;
            case Outcome::Failed:  ++summary_.failed; break;
            case Outcome::Timeout: ++summary_.timed_out; break;
            case Outcome::Error:   ++summary_.errored; break;
        }
    }

    summary_.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        clock::now() - run_start);
    reporter->end_run(summary_);

    return summary_.not_passed() == 0;
}

namespace {

void print_usage(std::ostream& os, const char* prog) {
    os << "Usage: " << prog << " [options]\n\n"
       << "Options:\n"
       << "  -f, --filter <substr>   Run only tests whose suite.name contains <substr>\n"
       << "  -t, --tag <tag>         Include only tests with this tag (repeatable)\n"
       << "  -x, --exclude-tag <tag> Skip tests with this tag (repeatable)\n"
       << "      --timeout <ms>      Default per-test timeout when none is set\n"
       << "  -r, --reporter <kind>   Output format: console | tap | junit\n"
       << "  -o, --output <path>     Write report to file instead of stdout\n"
       << "      --suite <name>      JUnit testsuite name (default: edtf)\n"
       << "  -l, --list              List selected tests and exit\n"
       << "      --no-color          Disable ANSI colors in console output\n"
       << "  -h, --help              Show this help\n";
}

}  // namespace

RunOptions parse_args(int argc, char** argv, std::ostream& out,
                      std::ostream& err, bool& should_exit, int& exit_code) {
    RunOptions opts;
    should_exit = false;
    exit_code = 0;

    auto need_value = [&](int& i, const char* flag) -> const char* {
        if (i + 1 >= argc) {
            err << "error: " << flag << " requires a value\n";
            should_exit = true;
            exit_code = 2;
            return nullptr;
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            print_usage(out, argv[0]);
            should_exit = true;
            exit_code = 0;
            return opts;
        } else if (a == "-f" || a == "--filter") {
            const char* v = need_value(i, "--filter");
            if (!v) return opts;
            opts.name_filter = v;
        } else if (a == "-t" || a == "--tag") {
            const char* v = need_value(i, "--tag");
            if (!v) return opts;
            opts.include_tags.insert(v);
        } else if (a == "-x" || a == "--exclude-tag") {
            const char* v = need_value(i, "--exclude-tag");
            if (!v) return opts;
            opts.exclude_tags.insert(v);
        } else if (a == "--timeout") {
            const char* v = need_value(i, "--timeout");
            if (!v) return opts;
            opts.default_timeout = std::chrono::milliseconds(std::atoll(v));
        } else if (a == "-r" || a == "--reporter") {
            const char* v = need_value(i, "--reporter");
            if (!v) return opts;
            std::string kind = v;
            if (kind == "console") {
                opts.format = RunOptions::Format::Console;
            } else if (kind == "tap") {
                opts.format = RunOptions::Format::Tap;
            } else if (kind == "junit") {
                opts.format = RunOptions::Format::Junit;
            } else {
                err << "error: unknown reporter '" << kind << "'\n";
                should_exit = true;
                exit_code = 2;
                return opts;
            }
        } else if (a == "-o" || a == "--output") {
            const char* v = need_value(i, "--output");
            if (!v) return opts;
            opts.output_path = v;
        } else if (a == "--suite") {
            const char* v = need_value(i, "--suite");
            if (!v) return opts;
            opts.junit_suite = v;
        } else if (a == "-l" || a == "--list") {
            opts.list_only = true;
        } else if (a == "--no-color") {
            opts.color = false;
        } else {
            err << "error: unknown argument '" << a << "'\n";
            print_usage(err, argv[0]);
            should_exit = true;
            exit_code = 2;
            return opts;
        }
    }
    return opts;
}

int run_all(int argc, char** argv) {
    bool should_exit = false;
    int exit_code = 0;
    RunOptions opts =
        parse_args(argc, argv, std::cout, std::cerr, should_exit, exit_code);
    if (should_exit) return exit_code;

    Runner runner(std::move(opts));
    bool ok = runner.run(std::cerr);
    return ok ? 0 : 1;
}

}  // namespace edtf
