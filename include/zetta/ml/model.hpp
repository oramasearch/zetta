#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta::cluster { class ReplicationManager; }

namespace zetta::ml {

// influence (an explainability signal, e.g. a SHAP importance). `importance` can
// be signed (a feature can push a prediction up or down); drivers are ranked by
// |importance| descending, so the strongest influences read first.
struct ModelDriver {
    std::string feature;     // the input feature's name
    double importance = 0.0;  // its (possibly signed) influence on the prediction
};

// Extract a model's **ranked feature drivers** from its free-form
// `feature_schema`. Drivers are an *optional* explainability annotation: an
// `Explain` compute job (or any producer) may record them under
// `feature_schema.drivers` as `[{ "feature": <string>, "importance": <number> },
// …]`. This reads that array **defensively** — skipping anything not shaped like
// a driver — and returns the drivers sorted by |importance| descending. It adds
// **no new JSON boundary**: `feature_schema` is already free-form and was
// schema-validated as an object when the model entered (parse_model_value), so
// this is tolerant reading of trusted data, not a second validation gate. Returns
// an empty vector when no drivers are recorded.
std::vector<ModelDriver> ranked_drivers(const nlohmann::json& feature_schema);

// Fingerprint of a model's serialized weights. The weights themselves are NOT in
// the CRDT (they would bloat every node's document); like a large-object body
// they live on disk and transfer peer-to-peer, verified against this size +
// SHA-256. This small descriptor is the part that *does* ride in `model:<uuid>`.
struct ModelWeights {
    std::uintmax_t size = 0;  // bytes of the serialized weights body
    std::string sha256;       // lowercase-hex SHA-256 of the body
};

// The replicated/on-disk value of a model — everything the CRDT key
// `model:<uuid>` carries (the id is the key / folder name, so it isn't repeated).
// This is exactly the shape validated by the Model JSON Schema at every boundary.
//
// `feature_schema` and `metrics` are free-form JSON: `feature_schema` describes
// the model's input features (field → type / profile), `metrics` records the
// evaluation scores (name → number, e.g. {"auc": 0.91}). `project` / `run` are
// optional lineage refs to what produced the model. `weights` is present once a
// body has been attached (set_weights); absent on a freshly-created, body-less
// model — and present-but-not-yet-on-disk on a peer awaiting body transfer.
struct ModelValue {
    std::string name;     // optional human display label
    std::string kind;     // e.g. "classifier" / "regressor" (required, non-empty)
    std::string project;  // optional producing/owning project uuid
    std::string run;      // optional producing run uuid
    nlohmann::json feature_schema = nlohmann::json::object();  // free-form
    nlohmann::json metrics = nlohmann::json::object();         // free-form (name→number)
    std::string created;  // ISO-8601 UTC stamp at creation
    std::optional<ModelWeights> weights;  // set once a weights body is attached
};

// Serialize a model value to its JSON shape (no id). The inverse of
// parse_model_value; used for the CRDT value, model.json, and tests.
nlohmann::json to_json(const ModelValue& value);

// The Model JSON Schema (draft-07) — the single source of truth for the value
// shape, exposed so callers/tests can introspect it.
const nlohmann::json& model_value_schema();

// Boundary validation: check a JSON value against the Model JSON Schema. Returns
// true when valid; on failure returns false and (if `error` is non-null) fills
// it with a human-readable reason. This is the gate every stored/replicated
// model value passes through before it is trusted.
bool validate_model_value(const nlohmann::json& value,
                          std::string* error = nullptr);

// Parse + validate a JSON value (from model.json on disk or a `model:<id>` CRDT
// value) into a ModelValue. Returns nullopt when the value fails schema
// validation — so malformed data is rejected at the boundary with a clear reason
// (in `error`) instead of corrupting the in-memory tree.
std::optional<ModelValue> parse_model_value(const nlohmann::json& value,
                                            std::string* error = nullptr);

// A Model record: a trained model registered on the node (ENT-14 / FEAT-6). A
// top-level entity with no parent, modeled like zetta::pipeline::Run — **replicated state**
// (flat key `model:<uuid>`) whose in-memory + on-disk (`<model-uuid>/model.json`)
// forms are a projection of the Automerge CRDT document.
//
// The serialized **weights** are deliberately NOT in the document: like a large
// object's body they live on disk (`<model-uuid>/weights`) and move peer-to-peer
// over the existing large-object body seam (BlobStore / LOREQ-LORES), verified
// against the `size` + `sha256` in this model's metadata. The metadata replicates
// today and the body follows over that seam.
//
// The id is the folder name and never changes; `kind` / lineage refs are fixed at
// creation. set_weights / set_metrics capture into the CRDT (when replicated) and
// rewrite model.json. Reconciliation uses apply_value(), which updates the
// metadata projection from the document with NO capture (and never touches the
// weights body). Lookups hand back shared_ptr handles, like Run / Project. See
// docs/ml/models.md.
class Model {
public:
    Model(std::string id, ModelValue value, std::filesystem::path dir,
          zetta::cluster::ReplicationManager* replication = nullptr);

    const std::string& id() const { return id_; }
    const std::string& name() const { return value_.name; }
    const std::string& kind() const { return value_.kind; }
    const std::string& project() const { return value_.project; }
    const std::string& run() const { return value_.run; }
    const nlohmann::json& feature_schema() const { return value_.feature_schema; }
    const nlohmann::json& metrics() const { return value_.metrics; }

    // This model's ranked feature drivers (explainability), read from its
    // free-form feature_schema (see ranked_drivers()). Empty when none recorded.
    std::vector<ModelDriver> drivers() const {
        return ranked_drivers(value_.feature_schema);
    }

    const std::string& created() const { return value_.created; }
    const std::optional<ModelWeights>& weights() const { return value_.weights; }
    const ModelValue& value() const { return value_; }
    const std::filesystem::path& dir() const { return dir_; }

    // Whether weights metadata has been attached to this model (independent of
    // whether the body is on disk here yet — see has_weights_body()).
    bool has_weights() const { return value_.weights.has_value(); }

    // Where the serialized weights body lives on disk (dir/weights).
    std::filesystem::path weights_path() const { return dir_ / "weights"; }
    // Is the weights body present on disk here? (False on a peer that has the
    // metadata but not yet the body — body transfer is still in flight.)
    bool has_weights_body() const;
    // Read the raw weights bytes; nullopt if the body isn't present locally.
    std::optional<std::string> read_weights() const;

    // Attach (or replace) the serialized weights: compute the size + SHA-256,
    // write the body to disk atomically, then capture `model:<id>` into the CRDT
    // (when replicated) and rewrite model.json. The metadata (size + hash)
    // replicates; peers fetch the body over the large-object body seam.
    bool set_weights(std::string_view bytes);

    // Update the evaluation metrics (free-form name→number object), then capture
    // + persist. The weights body is untouched.
    void set_metrics(nlohmann::json metrics);

    // Replace the metadata to match a (replicated) document value, then rewrite
    // model.json — with NO capture and WITHOUT touching the weights body (used by
    // reconciliation; the body arrives separately over the body seam).
    void apply_value(ModelValue value);

    // Write this model's metadata (+ id) to dir/model.json.
    void persist_meta() const;

    // The replicated/on-disk value shape (no id). Equivalent to to_json(value()).
    nlohmann::json metadata() const;

private:
    void capture_and_persist();   // capture (if replicated), rewrite model.json
    bool write_weights_body(std::string_view bytes) const;  // atomic body write

    std::string id_;
    ModelValue value_;
    std::filesystem::path dir_;
    zetta::cluster::ReplicationManager* replication_;  // null = standalone (not replicated)
};

}  // namespace zetta::ml