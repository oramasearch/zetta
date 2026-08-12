
// --- model_health (#983, the first Monitor capability) --------------------------------------
//
// The agent is meant to watch a deployed model and say whether it can still be trusted. Until
// this it had no tool that answers that, so it could only DESCRIBE monitoring — which is #976 at
// the methodology level: teaching a verb the agent has no vocabulary for.

TEST_F(CapabilityFixture, ModelHealthRefusesToTrustAModelWithNoHoldout) {
    // The most important thing it can say. Without a holdout every figure was measured on the
    // rows the model learned from — an in-sample 0.868 reached a client report once where the
    // held-out figure for the same run was 0.726.
    nlohmann::json metrics{{"in_sample_auc", 0.868}};
    const std::string id = install_model_with_metrics(metrics);

    const ApiResult result = call("model_health", {{"model_id", id}});
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_FALSE(result.value.at("trustworthy").get<bool>());

    const auto findings = result.value.at("findings");
    ASSERT_FALSE(findings.empty());
    bool named_the_fix = false;
    for (const auto& finding : findings) {
        if (finding.get<std::string>().find("holdout_fraction") != std::string::npos) {
            named_the_fix = true;
        }
    }
    EXPECT_TRUE(named_the_fix) << "a finding must say what to do, not only what is wrong";
}

TEST_F(CapabilityFixture, ModelHealthReportsTheOverfittingGap) {
    nlohmann::json metrics{{"in_sample_auc", 0.95}, {"held_out_auc", 0.72}};
    const std::string id = install_model_with_metrics(metrics);

    const ApiResult result = call("model_health", {{"model_id", id}});
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_NEAR(result.value.at("overfit_gap").get<double>(), 0.23, 1e-9);
    EXPECT_FALSE(result.value.at("trustworthy").get<bool>())
        << "a 0.23 gap is memorization, and the held-out figure is the only one describing new "
           "accounts";
}

TEST_F(CapabilityFixture, ModelHealthTrustsAModelThatGeneralizes) {
    nlohmann::json metrics{{"in_sample_auc", 0.78}, {"held_out_auc", 0.75}};
    const std::string id = install_model_with_metrics(metrics);

    const ApiResult result = call("model_health", {{"model_id", id}});
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_TRUE(result.value.at("trustworthy").get<bool>());
}

TEST_F(CapabilityFixture, ModelHealthIsAReadAndReachableByTheAgent) {
    // Reads are GET, actions are POST (#954) — and a capability the agent cannot call is one that
    // may as well not exist (`score` shipped and was unanswerable in chat until it was allowlisted).
    const Capability* capability = registry().find("model_health");
    ASSERT_NE(capability, nullptr);
    EXPECT_EQ(capability->kind, CapabilityKind::Read);
    EXPECT_FALSE(capability->mutates);
}
