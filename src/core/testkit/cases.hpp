#pragma once

// Internal: the per-category case registrars assembled by TestSuite::builtin().
// Each appends its canned cases to the suite. Keeping them in separate translation
// units keeps each category's includes local and the files readable.
namespace zetta::testkit {

class TestSuite;

void register_compute_cases(TestSuite& suite);
void register_ddf_cases(TestSuite& suite);
void register_parquet_cases(TestSuite& suite);
void register_ml_cases(TestSuite& suite);
void register_distributed_cases(TestSuite& suite);
void register_governance_cases(TestSuite& suite);
void register_pipeline_cases(TestSuite& suite);
void register_example_cases(TestSuite& suite);

}  // namespace zetta::testkit
