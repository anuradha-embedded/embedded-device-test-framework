// Test registration and the global registry.
//
// The TEST_CASE / TEST_SUITE_F macros build TestCase records at static
// initialization time and add them to a single process-wide Registry. The
// runner later walks the registry, applies filters, and executes each case.
#ifndef EDTF_REGISTRY_HPP
#define EDTF_REGISTRY_HPP

#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace edtf {

// A fixture is any type with default-constructible state and optional
// set_up()/tear_down() hooks. Tests derive a body from it via TEST_SUITE_F.
// The base provides empty hooks so plain fixtures need not override them.
class Fixture {
public:
    virtual ~Fixture() = default;
    virtual void set_up() {}
    virtual void tear_down() {}
};

// One registered test. The body throws AssertionAbort on a fatal failure and
// otherwise returns normally; the runner is responsible for catching.
struct TestCase {
    std::string suite;
    std::string name;
    std::set<std::string> tags;
    std::chrono::milliseconds timeout{0};  // zero means "no per-test deadline"
    std::function<void()> body;

    std::string full_name() const { return suite + "." + name; }
    bool has_tag(const std::string& t) const { return tags.count(t) != 0; }
};

class Registry {
public:
    static Registry& instance();

    void add(TestCase tc) { cases_.push_back(std::move(tc)); }
    const std::vector<TestCase>& cases() const { return cases_; }

private:
    Registry() = default;
    std::vector<TestCase> cases_;
};

namespace detail {

// Splits a comma/space separated tag string into a tag set. Empty entries are
// dropped so trailing commas are harmless.
std::set<std::string> parse_tags(const std::string& spec);

// Helper that registers a case and returns a dummy int so it can be used to
// initialize a static variable at file scope.
int register_case(const std::string& suite,
                  const std::string& name,
                  const std::string& tag_spec,
                  long long timeout_ms,
                  std::function<void()> body);

}  // namespace detail
}  // namespace edtf

// --- Macro support ------------------------------------------------------------

#define EDTF_CONCAT_INNER(a, b) a##b
#define EDTF_CONCAT(a, b) EDTF_CONCAT_INNER(a, b)
#define EDTF_UNIQUE(prefix) EDTF_CONCAT(prefix, __LINE__)

// Optional trailing arguments to TEST_CASE / TEST_SUITE_F:
//   .tags("group,slow")  -> attach tags
//   .timeout_ms(250)     -> per-test deadline
// They are expressed through a small options builder so call sites stay
// readable, e.g. TEST_CASE(Math, adds, edtf::opts().tags("fast")).
namespace edtf {
struct CaseOptions {
    std::string tag_spec;
    long long timeout_ms = 0;
    CaseOptions& tags(const std::string& spec) {
        tag_spec = spec;
        return *this;
    }
    CaseOptions& timeout_ms_(long long ms) {
        timeout_ms = ms;
        return *this;
    }
};
inline CaseOptions opts() { return CaseOptions{}; }
}  // namespace edtf

// Resolves the optional trailing options argument: when omitted, the variadic
// pack is empty and we fall back to a default-constructed CaseOptions.
#define EDTF_OPTS_OR_DEFAULT(...) \
    (::edtf::detail::first_or_default(::edtf::opts(), ##__VA_ARGS__))

namespace edtf {
namespace detail {
inline CaseOptions first_or_default(CaseOptions fallback) { return fallback; }
inline CaseOptions first_or_default(CaseOptions, CaseOptions provided) {
    return provided;
}
}  // namespace detail
}  // namespace edtf

// Free-function test. Body follows the macro as a brace block.
#define TEST_CASE(suite_name, case_name, ...)                                  \
    static void EDTF_UNIQUE(edtf_body_)();                                     \
    namespace {                                                                \
    const ::edtf::CaseOptions EDTF_UNIQUE(edtf_opts_) =                        \
        EDTF_OPTS_OR_DEFAULT(__VA_ARGS__);                                     \
    const int EDTF_UNIQUE(edtf_reg_) = ::edtf::detail::register_case(          \
        #suite_name, #case_name, EDTF_UNIQUE(edtf_opts_).tag_spec,             \
        EDTF_UNIQUE(edtf_opts_).timeout_ms, &EDTF_UNIQUE(edtf_body_));         \
    }                                                                          \
    static void EDTF_UNIQUE(edtf_body_)()

// Fixture-backed test. The body runs as a member of a fresh fixture instance,
// so `this`, protected members, and helper methods are all available.
#define TEST_SUITE_F(fixture_type, case_name, ...)                             \
    namespace {                                                                \
    struct EDTF_CONCAT(fixture_type, case_name) : public fixture_type {        \
        void run_body();                                                       \
    };                                                                         \
    const ::edtf::CaseOptions EDTF_CONCAT(edtf_fopts_, case_name) =            \
        EDTF_OPTS_OR_DEFAULT(__VA_ARGS__);                                     \
    const int EDTF_CONCAT(edtf_freg_, case_name) =                            \
        ::edtf::detail::register_case(                                         \
            #fixture_type, #case_name,                                         \
            EDTF_CONCAT(edtf_fopts_, case_name).tag_spec,                      \
            EDTF_CONCAT(edtf_fopts_, case_name).timeout_ms, [] {               \
                EDTF_CONCAT(fixture_type, case_name) edtf_fx;                  \
                edtf_fx.set_up();                                              \
                try {                                                          \
                    edtf_fx.run_body();                                        \
                } catch (...) {                                                \
                    edtf_fx.tear_down();                                       \
                    throw;                                                     \
                }                                                              \
                edtf_fx.tear_down();                                           \
            });                                                                \
    }                                                                          \
    void EDTF_CONCAT(fixture_type, case_name)::run_body()

#endif  // EDTF_REGISTRY_HPP
