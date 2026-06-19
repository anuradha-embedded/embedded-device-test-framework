#include "edtf/reporter.hpp"

#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>

namespace edtf {

const char* to_string(Outcome o) {
    switch (o) {
        case Outcome::Passed:  return "PASS";
        case Outcome::Failed:  return "FAIL";
        case Outcome::Timeout: return "TIMEOUT";
        case Outcome::Error:   return "ERROR";
    }
    return "?";
}

namespace {

// ANSI color helpers. Disabled when color is off so piped/CI output is clean.
struct Palette {
    bool on;
    const char* code(const char* c) const { return on ? c : ""; }
    const char* green() const { return code("\033[32m"); }
    const char* red() const { return code("\033[31m"); }
    const char* yellow() const { return code("\033[33m"); }
    const char* dim() const { return code("\033[2m"); }
    const char* bold() const { return code("\033[1m"); }
    const char* reset() const { return code("\033[0m"); }
};

// Indents a possibly multi-line block by a fixed prefix.
std::string indent(const std::string& text, const std::string& prefix) {
    std::string out;
    std::istringstream is(text);
    std::string line;
    bool first = true;
    while (std::getline(is, line)) {
        if (!first) out += "\n";
        out += prefix + line;
        first = false;
    }
    return out;
}

// XML attribute/text escaping for the JUnit reporter.
std::string xml_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c; break;
        }
    }
    return out;
}

std::string seconds(std::chrono::milliseconds ms) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(3)
       << (static_cast<double>(ms.count()) / 1000.0);
    return os.str();
}

// --- Console -----------------------------------------------------------------
class ConsoleReporter : public Reporter {
public:
    ConsoleReporter(std::ostream& os, bool color) : os_(os), pal_{color} {}

    void begin_run(int total) override {
        os_ << pal_.bold() << "Running " << total << " test"
            << (total == 1 ? "" : "s") << pal_.reset() << "\n";
    }

    void on_result(const TestResult& r) override {
        const char* col = pal_.green();
        if (r.outcome == Outcome::Failed) col = pal_.red();
        else if (r.outcome == Outcome::Timeout) col = pal_.yellow();
        else if (r.outcome == Outcome::Error) col = pal_.red();

        os_ << "  [" << col << to_string(r.outcome) << pal_.reset() << "] "
            << r.full_name() << " " << pal_.dim() << "("
            << r.duration.count() << " ms)" << pal_.reset() << "\n";

        if (!r.ok() && !r.detail.empty()) {
            os_ << indent(r.detail, "        ") << "\n";
        }
    }

    void end_run(const RunSummary& s) override {
        os_ << pal_.bold() << "\nSummary: " << pal_.reset()
            << s.passed << "/" << s.total << " passed";
        if (s.failed) os_ << ", " << pal_.red() << s.failed << " failed"
                          << pal_.reset();
        if (s.timed_out) os_ << ", " << pal_.yellow() << s.timed_out
                            << " timed out" << pal_.reset();
        if (s.errored) os_ << ", " << pal_.red() << s.errored << " errored"
                          << pal_.reset();
        os_ << pal_.dim() << " (" << s.duration.count() << " ms)"
            << pal_.reset() << "\n";
    }

private:
    std::ostream& os_;
    Palette pal_;
};

// --- TAP (Test Anything Protocol, version 13) --------------------------------
class TapReporter : public Reporter {
public:
    explicit TapReporter(std::ostream& os) : os_(os) {}

    void begin_run(int total) override {
        os_ << "TAP version 13\n";
        os_ << "1.." << total << "\n";
    }

    void on_result(const TestResult& r) override {
        ++index_;
        os_ << (r.ok() ? "ok " : "not ok ") << index_ << " - "
            << r.full_name();
        if (r.outcome == Outcome::Timeout) os_ << " # TIMEOUT";
        os_ << "\n";
        if (!r.ok() && !r.detail.empty()) {
            // YAML diagnostic block per the TAP 13 spec.
            os_ << "  ---\n";
            os_ << "  outcome: " << to_string(r.outcome) << "\n";
            os_ << "  message: |\n" << indent(r.detail, "    ") << "\n";
            os_ << "  ...\n";
        }
    }

    void end_run(const RunSummary& s) override {
        os_ << "# passed " << s.passed << " of " << s.total << "\n";
    }

private:
    std::ostream& os_;
    int index_ = 0;
};

// --- JUnit XML (Surefire-style, consumed by CI dashboards) -------------------
class JunitReporter : public Reporter {
public:
    JunitReporter(std::ostream& os, std::string suite)
        : os_(os), suite_(std::move(suite)) {}

    void begin_run(int /*total*/) override {}

    void on_result(const TestResult& r) override { results_.push_back(r); }

    void end_run(const RunSummary& s) override {
        os_ << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        os_ << "<testsuites tests=\"" << s.total << "\" failures=\""
            << s.failed << "\" errors=\"" << (s.errored + s.timed_out)
            << "\" time=\"" << seconds(s.duration) << "\">\n";
        os_ << "  <testsuite name=\"" << xml_escape(suite_) << "\" tests=\""
            << s.total << "\" failures=\"" << s.failed << "\" errors=\""
            << (s.errored + s.timed_out) << "\" time=\""
            << seconds(s.duration) << "\">\n";

        for (const auto& r : results_) {
            os_ << "    <testcase classname=\"" << xml_escape(r.suite)
                << "\" name=\"" << xml_escape(r.name) << "\" time=\""
                << seconds(r.duration) << "\"";
            if (r.ok()) {
                os_ << "/>\n";
                continue;
            }
            os_ << ">\n";
            if (r.outcome == Outcome::Failed) {
                os_ << "      <failure message=\"assertion failure\">"
                    << xml_escape(r.detail) << "</failure>\n";
            } else if (r.outcome == Outcome::Timeout) {
                os_ << "      <error type=\"timeout\" message=\""
                    << xml_escape(r.detail) << "\"/>\n";
            } else {
                os_ << "      <error type=\"exception\">"
                    << xml_escape(r.detail) << "</error>\n";
            }
            os_ << "    </testcase>\n";
        }

        os_ << "  </testsuite>\n";
        os_ << "</testsuites>\n";
    }

private:
    std::ostream& os_;
    std::string suite_;
    std::vector<TestResult> results_;
};

}  // namespace

std::unique_ptr<Reporter> make_console_reporter(std::ostream& os, bool color) {
    return std::unique_ptr<Reporter>(new ConsoleReporter(os, color));
}
std::unique_ptr<Reporter> make_tap_reporter(std::ostream& os) {
    return std::unique_ptr<Reporter>(new TapReporter(os));
}
std::unique_ptr<Reporter> make_junit_reporter(std::ostream& os,
                                              std::string suite_name) {
    return std::unique_ptr<Reporter>(
        new JunitReporter(os, std::move(suite_name)));
}

}  // namespace edtf
