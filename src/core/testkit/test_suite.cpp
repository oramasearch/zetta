#include "zetta/testkit/test_suite.hpp"

#include <chrono>
#include <exception>
#include <utility>

#include "cases.hpp"

namespace zetta::testkit {

// ----- enum names ------------------------------------------------------------

std::string_view to_string(TestCategory category) {
    switch (category) {
        case TestCategory::Compute:     return "compute";
        case TestCategory::Ddf:         return "ddf";
        case TestCategory::Ml:          return "ml";
        case TestCategory::Distributed: return "distributed";
        case TestCategory::Governance:  return "governance";
        case TestCategory::Pipeline:    return "pipeline";
        case TestCategory::Examples:    return "examples";
    }
    return "unknown";
}

std::string_view category_title(TestCategory category) {
    switch (category) {
        case TestCategory::Compute:     return "Compute & device";
        case TestCategory::Ddf:         return "DDF dataframe ops";
        case TestCategory::Ml:          return "Machine learning";
        case TestCategory::Distributed: return "Distributed & cluster";
        case TestCategory::Governance:  return "Governance";
        case TestCategory::Pipeline:    return "UC-1 pipeline";
        case TestCategory::Examples:    return "Example scripts";
    }
    return "Other";
}

std::string_view to_string(TestOutcome outcome) {
    switch (outcome) {
        case TestOutcome::Pass:    return "pass";
        case TestOutcome::Fail:    return "fail";
        case TestOutcome::Skipped: return "skipped";
        case TestOutcome::Error:   return "error";
    }
    return "unknown";
}

// ----- TestOption factories --------------------------------------------------

TestOption TestOption::boolean(std::string key, std::string label, bool value,
                               std::string help) {
    TestOption option;
    option.key = std::move(key);
    option.label = std::move(label);
    option.help = std::move(help);
    option.type = OptionType::Bool;
    option.default_bool = value;
    return option;
}

TestOption TestOption::integer(std::string key, std::string label, long value,
                               long min_value, long max_value, std::string help) {
    TestOption option;
    option.key = std::move(key);
    option.label = std::move(label);
    option.help = std::move(help);
    option.type = OptionType::Int;
    option.default_int = value;
    option.min_int = min_value;
    option.max_int = max_value;
    return option;
}

TestOption TestOption::real(std::string key, std::string label, double value,
                            double min_value, double max_value, std::string help) {
    TestOption option;
    option.key = std::move(key);
    option.label = std::move(label);
    option.help = std::move(help);
    option.type = OptionType::Double;
    option.default_double = value;
    option.min_double = min_value;
    option.max_double = max_value;
    return option;
}

TestOption TestOption::choice(std::string key, std::string label,
                              std::vector<std::string> options, int selected,
                              std::string help) {
    TestOption option;
    option.key = std::move(key);
    option.label = std::move(label);
    option.help = std::move(help);
    option.type = OptionType::Choice;
    option.choices = std::move(options);
    option.default_int = selected;
    return option;
}

TestOption TestOption::text(std::string key, std::string label, std::string value,
                            std::string help) {
    TestOption option;
    option.key = std::move(key);
    option.label = std::move(label);
    option.help = std::move(help);
    option.type = OptionType::Text;
    option.default_text = std::move(value);
    return option;
}

// ----- TestOptions -----------------------------------------------------------

void TestOptions::set_bool(const std::string& key, bool value) {
    Value& slot = values_[key];
    slot.type = OptionType::Bool;
    slot.boolean = value;
}
void TestOptions::set_int(const std::string& key, long value) {
    Value& slot = values_[key];
    slot.type = OptionType::Int;
    slot.integer = value;
}
void TestOptions::set_double(const std::string& key, double value) {
    Value& slot = values_[key];
    slot.type = OptionType::Double;
    slot.real = value;
}
void TestOptions::set_choice(const std::string& key, int index) {
    Value& slot = values_[key];
    slot.type = OptionType::Choice;
    slot.choice = index;
}
void TestOptions::set_text(const std::string& key, std::string value) {
    Value& slot = values_[key];
    slot.type = OptionType::Text;
    slot.text = std::move(value);
}

bool TestOptions::get_bool(const std::string& key, bool fallback) const {
    const auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second.boolean;
}
long TestOptions::get_int(const std::string& key, long fallback) const {
    const auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second.integer;
}
double TestOptions::get_double(const std::string& key, double fallback) const {
    const auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second.real;
}
int TestOptions::get_choice(const std::string& key, int fallback) const {
    const auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second.choice;
}
std::string TestOptions::get_text(const std::string& key,
                                  std::string fallback) const {
    const auto it = values_.find(key);
    return it == values_.end() ? std::move(fallback) : it->second.text;
}

// ----- TestResult builders ---------------------------------------------------

TestResult TestResult::pass(std::string summary) {
    return TestResult{TestOutcome::Pass, std::move(summary), {}, {}, {}, 0.0};
}
TestResult TestResult::fail(std::string summary) {
    TestResult result;
    result.outcome = TestOutcome::Fail;
    result.summary = std::move(summary);
    result.error = result.summary;
    return result;
}
TestResult TestResult::skip(std::string summary) {
    return TestResult{TestOutcome::Skipped, std::move(summary), {}, {}, {}, 0.0};
}
TestResult TestResult::err(std::string summary) {
    TestResult result;
    result.outcome = TestOutcome::Error;
    result.summary = std::move(summary);
    result.error = result.summary;
    return result;
}

TestResult& TestResult::metric(std::string label, std::string value) {
    metrics.push_back(TestMetric{std::move(label), std::move(value)});
    return *this;
}
TestResult& TestResult::note(std::string line) {
    log.push_back(std::move(line));
    return *this;
}

// ----- TestCase --------------------------------------------------------------

TestOptions TestCase::defaults() const {
    TestOptions out;
    for (const TestOption& option : options) {
        switch (option.type) {
            case OptionType::Bool:   out.set_bool(option.key, option.default_bool); break;
            case OptionType::Int:    out.set_int(option.key, option.default_int); break;
            case OptionType::Double: out.set_double(option.key, option.default_double); break;
            case OptionType::Choice: out.set_choice(option.key, static_cast<int>(option.default_int)); break;
            case OptionType::Text:   out.set_text(option.key, option.default_text); break;
        }
    }
    return out;
}

// ----- TestSuite -------------------------------------------------------------

void TestSuite::add(TestCase test_case) { cases_.push_back(std::move(test_case)); }

const TestCase* TestSuite::find(const std::string& id) const {
    for (const TestCase& test_case : cases_) {
        if (test_case.id == id) return &test_case;
    }
    return nullptr;
}

std::vector<const TestCase*> TestSuite::in_category(TestCategory category) const {
    std::vector<const TestCase*> out;
    for (const TestCase& test_case : cases_) {
        if (test_case.category == category) out.push_back(&test_case);
    }
    return out;
}

const TestSuite& TestSuite::builtin() {
    static const TestSuite suite = [] {
        TestSuite built;
        register_compute_cases(built);
        register_ddf_cases(built);
        register_parquet_cases(built);
        register_ml_cases(built);
        register_distributed_cases(built);
        register_governance_cases(built);
        register_pipeline_cases(built);
        register_example_cases(built);
        return built;
    }();
    return suite;
}

// ----- the runner ------------------------------------------------------------

TestResult run_test_case(const TestCase& test_case, const TestOptions& options,
                         TestContext& context) {
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    TestResult result;
    if (!test_case.run) {
        result = TestResult::err("test case has no run body");
    } else {
        // The body may call any engine primitive; convert a thrown exception into a
        // clean Error so a misbehaving case can never propagate across the seam.
        try {
            result = test_case.run(options, context);
        } catch (const std::exception& error) {
            result = TestResult::err(std::string("exception: ") + error.what());
        } catch (...) {
            result = TestResult::err("unknown exception");
        }
    }
    const auto finished = Clock::now();
    result.elapsed_seconds =
        std::chrono::duration<double>(finished - started).count();
    return result;
}

}  // namespace zetta::testkit
