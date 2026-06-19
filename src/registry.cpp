#include "edtf/assertions.hpp"
#include "edtf/registry.hpp"

#include <cctype>
#include <utility>

namespace edtf {

Registry& Registry::instance() {
    static Registry reg;
    return reg;
}

namespace detail {

CheckSink*& active_sink() {
    // Thread-local: each test runs on its own worker thread and binds its own
    // sink before invoking the body.
    static thread_local CheckSink* sink = nullptr;
    return sink;
}

std::set<std::string> parse_tags(const std::string& spec) {
    std::set<std::string> out;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty()) {
            out.insert(cur);
            cur.clear();
        }
    };
    for (char c : spec) {
        if (c == ',' || c == ' ' || c == '\t') {
            flush();
        } else {
            cur += c;
        }
    }
    flush();
    return out;
}

int register_case(const std::string& suite,
                  const std::string& name,
                  const std::string& tag_spec,
                  long long timeout_ms,
                  std::function<void()> body) {
    TestCase tc;
    tc.suite = suite;
    tc.name = name;
    tc.tags = parse_tags(tag_spec);
    tc.timeout = std::chrono::milliseconds(timeout_ms);
    tc.body = std::move(body);
    Registry::instance().add(std::move(tc));
    return 0;
}

}  // namespace detail
}  // namespace edtf
