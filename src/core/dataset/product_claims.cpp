#include "zetta/dataset/product_claims.hpp"

#include <algorithm>
#include <cctype>

namespace zetta::dataset {
namespace {

std::string lowered(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char character : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return out;
}

}  // namespace

const std::vector<ProductClaim>& product_claims() {
    static const std::vector<ProductClaim> kClaims = {
        {"run python", ClaimStatus::Qualified, "simulated",
         "zetta::input::PythonFrontend — its own header says simulated Python"},
        {"connect to postgres", ClaimStatus::Shipped, "",
         "src/core/connect/postgres_runner.cpp"},
        {"connect to snowflake", ClaimStatus::Shipped, "",
         "src/core/connect/snowflake_protocol.cpp"},
        {"auto-suggest", ClaimStatus::NotShipped, "",
         "no Auto-Suggest checkpoint exists; all 17 in data/checkpoints/ are base-agent-*"},
    };
    return kClaims;
}

std::string forbidden_claim_in(std::string_view message) {
    const std::string haystack = lowered(message);
    for (const ProductClaim& claim : product_claims()) {
        if (haystack.find(lowered(claim.phrase)) == std::string::npos) continue;
        switch (claim.status) {
            case ClaimStatus::Shipped:
                break;
            case ClaimStatus::NotShipped:
                return std::string(claim.phrase);
            case ClaimStatus::Qualified:
                // The claim is permitted only when the qualifier travels with it. Dropping the
                // qualifier is how "simulated Python" becomes "Python" between the meeting and
                // the weights.
                if (haystack.find(lowered(claim.qualifier)) == std::string::npos) {
                    return std::string(claim.phrase);
                }
                break;
        }
    }
    return {};
}

}  // namespace zetta::dataset
