// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>
#include <vector>

// The product-claims registry (#983) — what the corpus is ALLOWED to say the product can do.
//
// `product_knowledge_family` states capabilities in our own voice, in the model's weights, to
// every customer, on every turn. Every other corpus property is guarded — groundedness, negative
// controls, ask-form ownership, bare turns — and the one that reaches a customer's ears was not.
//
// A false claim in a meeting is made ONCE, to one person, and can be corrected in the next call.
// A false claim in the corpus is made by the model, forever, and it is fluent, confident and
// grounded-looking, which is exactly the failure profile that goes unnoticed. It also compounds:
// it becomes training data for the next checkpoint.
//
// The instance that motivated this is uncomfortable. The corpus says "You can run Python in a
// Pipeline step". The engine has `zetta::input::PythonFrontend` — documented in its own header as
// SIMULATED Python. In the customer meeting the claim was made accurately ("you can use SQL, you
// can use simulated Python, or you can use natural language"). **The meeting was more honest than
// the training data.**
namespace zetta::dataset {

enum class ClaimStatus {
    Shipped,      // true today, safe for the corpus to assert plainly
    Qualified,    // true only with a qualifier the corpus MUST carry (e.g. "simulated" Python)
    NotShipped,   // not true today; the corpus may not assert it at all
};

struct ProductClaim {
    std::string_view phrase;     // the phrase, as a corpus message would contain it
    ClaimStatus status;
    std::string_view qualifier;  // for Qualified: the word the assertion must also contain
    std::string_view evidence;   // where the truth of this was verified
};

// The registry. Deliberately small and evidence-backed rather than exhaustive: a registry padded
// with guesses is worse than none, because it launders assumptions into checks.
const std::vector<ProductClaim>& product_claims();

// Whether `message` makes a claim the registry forbids. Returns the offending phrase, or empty.
std::string forbidden_claim_in(std::string_view message);

}  // namespace zetta::dataset
