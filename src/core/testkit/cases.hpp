#pragma once

#include <string>

// Internal: the per-category case registrars assembled by TestSuite::builtin().
// Each appends its canned cases to the suite. Keeping them in separate translation
// units keeps each category's includes local and the files readable.
namespace zetta::testkit { class TestSuite; }

namespace zetta::testkit {

void register_compute_cases(TestSuite& suite);
void register_ddf_cases(TestSuite& suite);
void register_parquet_cases(TestSuite& suite);
void register_ml_cases(TestSuite& suite);
void register_distributed_cases(TestSuite& suite);
void register_fulltext_cases(TestSuite& suite);
void register_governance_cases(TestSuite& suite);
void register_pipeline_cases(TestSuite& suite);
void register_uc1_scale_cases(TestSuite& suite);
void register_example_cases(TestSuite& suite);

// The canonical UC-1-at-scale sim-Python program (TC-12 / #531), defined in
// cases_uc1_scale.cpp. Shared so the lightweight examples.uc1_at_scale example
// (cases_examples.cpp) and the real cluster.uc1_at_scale case run one source.
const std::string& uc1_at_scale_script();

}  // namespace zetta::testkit
