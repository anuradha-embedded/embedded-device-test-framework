// Assertion macros for the embedded device test framework.
//
// Two families are provided:
//   ASSERT_* : fatal. On failure the current test body is aborted via an
//              exception that the runner catches and records.
//   EXPECT_* : non-fatal. On failure the failure is recorded but the test
//              body keeps executing so multiple problems can be reported in
//              a single run.
//
// All checks funnel through edtf::detail::CheckSink, which the runner binds
// to the test currently executing. This keeps the macros free of any global
// mutable state of their own.
#ifndef EDTF_ASSERTIONS_HPP
#define EDTF_ASSERTIONS_HPP

#include <cmath>
#include <sstream>
#include <string>

namespace edtf {

// Thrown by a fatal assertion to unwind out of the test body. The runner
// catches it; user code generally should not.
struct AssertionAbort {
    std::string message;
};

namespace detail {

// Receives the outcome of every assertion in the active test. The runner
// installs a thread-local pointer to the sink for the test under execution.
class CheckSink {
public:
    void record_pass() { ++passed_; }

    void record_failure(const std::string& expr,
                         const std::string& detail,
                         const char* file,
                         int line) {
        ++failed_;
        std::ostringstream os;
        os << file << ":" << line << ": " << expr;
        if (!detail.empty()) {
            os << "\n    " << detail;
        }
        if (!failures_.empty()) {
            failures_ += "\n";
        }
        failures_ += os.str();
    }

    int passed() const { return passed_; }
    int failed() const { return failed_; }
    const std::string& failures() const { return failures_; }

private:
    int passed_ = 0;
    int failed_ = 0;
    std::string failures_;
};

// Active sink for the calling thread. Tests run on a worker thread, so this
// is intentionally thread-local rather than a single global.
CheckSink*& active_sink();

// Stream helper that renders most values, falling back gracefully for types
// that are not stream-insertable.
template <typename T>
std::string stringify(const T& value) {
    std::ostringstream os;
    os << value;
    return os.str();
}

inline std::string stringify(bool value) { return value ? "true" : "false"; }
inline std::string stringify(const char* value) {
    return value ? std::string("\"") + value + "\"" : std::string("nullptr");
}
inline std::string stringify(const std::string& value) {
    return "\"" + value + "\"";
}

}  // namespace detail
}  // namespace edtf

// --- Internal plumbing shared by the public macros ----------------------------

#define EDTF_RECORD_PASS() (::edtf::detail::active_sink()->record_pass())

#define EDTF_FAIL_IMPL(expr, detail_text, fatal)                               \
    do {                                                                       \
        ::edtf::detail::active_sink()->record_failure((expr), (detail_text),   \
                                                      __FILE__, __LINE__);      \
        if (fatal) {                                                           \
            throw ::edtf::AssertionAbort{std::string(__FILE__) + ":" +         \
                                         std::to_string(__LINE__) + ": " +     \
                                         (expr)};                              \
        }                                                                      \
    } while (false)

#define EDTF_CHECK_TRUE(cond, fatal)                                           \
    do {                                                                       \
        if (cond) {                                                            \
            EDTF_RECORD_PASS();                                                \
        } else {                                                               \
            EDTF_FAIL_IMPL(std::string("expected true: ") + #cond, "", fatal); \
        }                                                                      \
    } while (false)

#define EDTF_CHECK_FALSE(cond, fatal)                                          \
    do {                                                                       \
        if (!(cond)) {                                                         \
            EDTF_RECORD_PASS();                                                \
        } else {                                                               \
            EDTF_FAIL_IMPL(std::string("expected false: ") + #cond, "",        \
                           fatal);                                             \
        }                                                                      \
    } while (false)

#define EDTF_CHECK_BINOP(a, b, op, name, fatal)                                \
    do {                                                                       \
        auto edtf_lhs = (a);                                                   \
        auto edtf_rhs = (b);                                                   \
        if (edtf_lhs op edtf_rhs) {                                            \
            EDTF_RECORD_PASS();                                                \
        } else {                                                               \
            std::string edtf_detail =                                          \
                "lhs = " + ::edtf::detail::stringify(edtf_lhs) +               \
                "\n    rhs = " + ::edtf::detail::stringify(edtf_rhs);          \
            EDTF_FAIL_IMPL(std::string(name) + ": " #a " " #op " " #b,         \
                           edtf_detail, fatal);                                \
        }                                                                      \
    } while (false)

#define EDTF_CHECK_NEAR(a, b, tol, fatal)                                      \
    do {                                                                       \
        double edtf_a = static_cast<double>(a);                               \
        double edtf_b = static_cast<double>(b);                               \
        double edtf_t = static_cast<double>(tol);                             \
        double edtf_d = std::fabs(edtf_a - edtf_b);                            \
        if (edtf_d <= edtf_t) {                                                \
            EDTF_RECORD_PASS();                                                \
        } else {                                                               \
            std::string edtf_detail =                                          \
                "lhs = " + ::edtf::detail::stringify(edtf_a) +                 \
                "\n    rhs = " + ::edtf::detail::stringify(edtf_b) +           \
                "\n    |diff| = " + ::edtf::detail::stringify(edtf_d) +        \
                " > tol " + ::edtf::detail::stringify(edtf_t);                 \
            EDTF_FAIL_IMPL(std::string("ASSERT_NEAR: " #a " ~= " #b),          \
                           edtf_detail, fatal);                                \
        }                                                                      \
    } while (false)

#define EDTF_CHECK_THROWS(stmt, ex_type, fatal)                                \
    do {                                                                       \
        bool edtf_threw = false;                                               \
        try {                                                                  \
            stmt;                                                              \
        } catch (const ex_type&) {                                            \
            edtf_threw = true;                                                 \
        } catch (...) {                                                        \
            EDTF_FAIL_IMPL(std::string("expected " #ex_type " from: " #stmt),  \
                           "a different exception type was thrown", fatal);    \
            break;                                                             \
        }                                                                      \
        if (edtf_threw) {                                                      \
            EDTF_RECORD_PASS();                                                \
        } else {                                                               \
            EDTF_FAIL_IMPL(std::string("expected " #ex_type " from: " #stmt),  \
                           "no exception was thrown", fatal);                  \
        }                                                                      \
    } while (false)

// --- Public assertion macros --------------------------------------------------

#define ASSERT_TRUE(cond) EDTF_CHECK_TRUE(cond, true)
#define ASSERT_FALSE(cond) EDTF_CHECK_FALSE(cond, true)
#define ASSERT_EQ(a, b) EDTF_CHECK_BINOP(a, b, ==, "ASSERT_EQ", true)
#define ASSERT_NE(a, b) EDTF_CHECK_BINOP(a, b, !=, "ASSERT_NE", true)
#define ASSERT_LT(a, b) EDTF_CHECK_BINOP(a, b, <, "ASSERT_LT", true)
#define ASSERT_LE(a, b) EDTF_CHECK_BINOP(a, b, <=, "ASSERT_LE", true)
#define ASSERT_GT(a, b) EDTF_CHECK_BINOP(a, b, >, "ASSERT_GT", true)
#define ASSERT_GE(a, b) EDTF_CHECK_BINOP(a, b, >=, "ASSERT_GE", true)
#define ASSERT_NEAR(a, b, tol) EDTF_CHECK_NEAR(a, b, tol, true)
#define ASSERT_THROWS(stmt, ex_type) EDTF_CHECK_THROWS(stmt, ex_type, true)

#define EXPECT_TRUE(cond) EDTF_CHECK_TRUE(cond, false)
#define EXPECT_FALSE(cond) EDTF_CHECK_FALSE(cond, false)
#define EXPECT_EQ(a, b) EDTF_CHECK_BINOP(a, b, ==, "EXPECT_EQ", false)
#define EXPECT_NE(a, b) EDTF_CHECK_BINOP(a, b, !=, "EXPECT_NE", false)
#define EXPECT_LT(a, b) EDTF_CHECK_BINOP(a, b, <, "EXPECT_LT", false)
#define EXPECT_LE(a, b) EDTF_CHECK_BINOP(a, b, <=, "EXPECT_LE", false)
#define EXPECT_GT(a, b) EDTF_CHECK_BINOP(a, b, >, "EXPECT_GT", false)
#define EXPECT_GE(a, b) EDTF_CHECK_BINOP(a, b, >=, "EXPECT_GE", false)
#define EXPECT_NEAR(a, b, tol) EDTF_CHECK_NEAR(a, b, tol, false)
#define EXPECT_THROWS(stmt, ex_type) EDTF_CHECK_THROWS(stmt, ex_type, false)

// Unconditionally fail the current test with a message.
#define ADD_FAILURE(msg) EDTF_FAIL_IMPL(std::string("FAIL"), std::string(msg), false)
#define FAIL(msg) EDTF_FAIL_IMPL(std::string("FAIL"), std::string(msg), true)

#endif  // EDTF_ASSERTIONS_HPP
