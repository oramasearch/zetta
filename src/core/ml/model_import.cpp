#include "zetta/ml/model_import.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "protobuf_wire.hpp"

// lake#681. See include/zetta/ml/model_import.hpp and docs/ml/model-import.md.
namespace zetta::ml {

namespace {

constexpr double kInfinity = std::numeric_limits<double>::infinity();

template <typename Value>
std::optional<Value> refuse(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
    return std::nullopt;
}

// --- Splits: from the source's comparison to the engine's strict `<` ---------------

// How the source compares a feature value with a threshold.
enum class Precision { Float32, Float64 };
enum class Comparison { Less, LessEqual, Greater, GreaterEqual };

// One node of a source tree, already in the engine's terms: x < bound goes to
// `below`, anything else to `above`, a missing value to `below` when missing_below.
struct SourceNode {
    bool present = false;
    bool leaf = true;
    int feature = -1;
    double bound = 0.0;
    int below = -1;
    int above = -1;
    bool missing_below = false;
    double value = 0.0;
    double cover = 0.0;
};

// The bound for "x OP threshold is true", and whether the true branch is `below`.
// false when the threshold cannot be compared exactly (not finite).
bool convert_split(Comparison comparison, double threshold, Precision precision,
                   double* bound, bool* true_is_below) {
    if (!std::isfinite(threshold)) return false;
    const auto less_than = [&](double value) {
        return precision == Precision::Float32 ? float_less_bound(static_cast<float>(value))
                                               : value;
    };
    const auto less_equal = [&](double value) {
        if (precision == Precision::Float32) {
            return float_less_bound(
                std::nextafter(static_cast<float>(value), std::numeric_limits<float>::infinity()));
        }
        return std::nextafter(value, kInfinity);
    };
    switch (comparison) {
        case Comparison::Less:
            *bound = less_than(threshold);
            *true_is_below = true;
            break;
        case Comparison::LessEqual:
            *bound = less_equal(threshold);
            *true_is_below = true;
            break;
        case Comparison::GreaterEqual:  // x >= t  <=>  !(x < t)
            *bound = less_than(threshold);
            *true_is_below = false;
            break;
        case Comparison::Greater:  // x > t  <=>  !(x <= t)
            *bound = less_equal(threshold);
            *true_is_below = false;
            break;
    }
    return true;
}

// Build the engine's tree from source nodes, root first and every child after its
// parent. Refuses a child out of range, a node reached twice (a cycle or a shared
// child) and an internal node that reads a feature the model does not have.
std::optional<RegressionTree> build_tree(const std::vector<SourceNode>& nodes, int root,
                                         std::size_t feature_count, bool covers_known,
                                         std::string* error) {
    const auto count = static_cast<int>(nodes.size());
    if (root < 0 || root >= count || !nodes[static_cast<std::size_t>(root)].present) {
        return refuse<RegressionTree>(error, "a tree has no root node");
    }
    RegressionTree tree;
    std::vector<int> placed(nodes.size(), -1);
    std::vector<int> pending{root};
    placed[static_cast<std::size_t>(root)] = 0;
    tree.nodes.emplace_back();
    while (!pending.empty()) {
        const int source_index = pending.back();
        pending.pop_back();
        const SourceNode& source = nodes[static_cast<std::size_t>(source_index)];
        const auto target = static_cast<std::size_t>(placed[static_cast<std::size_t>(source_index)]);
        TreeNode node;
        node.value = source.value;
        node.cover = source.cover;
        if (!source.leaf) {
            if (source.feature < 0 || static_cast<std::size_t>(source.feature) >= feature_count) {
                return refuse<RegressionTree>(
                    error, "a split reads feature " + std::to_string(source.feature) +
                               ", and the model has " + std::to_string(feature_count));
            }
            node.feature = source.feature;
            node.threshold = source.bound;
            node.missing_left = source.missing_below;
            for (const int child : {source.below, source.above}) {
                if (child < 0 || child >= count || !nodes[static_cast<std::size_t>(child)].present) {
                    return refuse<RegressionTree>(error, "a split points at a node that is not there");
                }
                if (placed[static_cast<std::size_t>(child)] >= 0) {
                    return refuse<RegressionTree>(error, "a tree reaches one node twice");
                }
                placed[static_cast<std::size_t>(child)] = static_cast<int>(tree.nodes.size());
                tree.nodes.emplace_back();
                pending.push_back(child);
            }
            node.left = placed[static_cast<std::size_t>(source.below)];
            node.right = placed[static_cast<std::size_t>(source.above)];
        }
        tree.nodes[target] = node;
    }
    // A source that keeps no training weight (ONNX) weighs each branch by its leaves,
    // so TreeSHAP still has a background distribution. Children come after their
    // parent, so one backward pass sums them.
    if (!covers_known) {
        for (std::size_t index = tree.nodes.size(); index-- > 0;) {
            TreeNode& node = tree.nodes[index];
            node.cover = node.is_leaf()
                             ? 1.0
                             : tree.nodes[static_cast<std::size_t>(node.left)].cover +
                                   tree.nodes[static_cast<std::size_t>(node.right)].cover;
        }
    }
    return tree;
}

std::vector<std::string> default_feature_names(std::size_t count) {
    std::vector<std::string> names;
    names.reserve(count);
    for (std::size_t index = 0; index < count; ++index) names.push_back("f" + std::to_string(index));
    return names;
}

// The names the model reads: the caller's, else the file's, else f0, f1, ….
std::optional<std::vector<std::string>> resolve_feature_names(
    const ModelImportOptions& options, std::vector<std::string> from_file,
    std::size_t feature_count, std::string* error) {
    if (!options.feature_names.empty()) {
        if (options.feature_names.size() != feature_count) {
            return refuse<std::vector<std::string>>(
                error, "feature_names names " + std::to_string(options.feature_names.size()) +
                           " columns and the model reads " + std::to_string(feature_count));
        }
        from_file = options.feature_names;
    }
    if (from_file.empty()) return default_feature_names(feature_count);
    if (from_file.size() != feature_count) {
        return refuse<std::vector<std::string>>(
            error, "the file names " + std::to_string(from_file.size()) +
                       " features and its trees read " + std::to_string(feature_count));
    }
    std::set<std::string> seen;
    for (const std::string& name : from_file) {
        if (name.empty() || !seen.insert(name).second) {
            return refuse<std::vector<std::string>>(
                error, "feature names must be distinct and not empty ('" + name + "')");
        }
    }
    return from_file;
}

double logit(double probability) { return std::log(probability / (1.0 - probability)); }

// --- ONNX ------------------------------------------------------------------------

constexpr int kOnnxFloat = 1;
constexpr int kOnnxInt32 = 6;
constexpr int kOnnxInt64 = 7;
constexpr int kOnnxDouble = 11;

struct OnnxTensor {
    int data_type = 0;
    std::vector<std::int64_t> dims;
    std::vector<double> values;
};

struct OnnxAttribute {
    std::optional<double> number;
    std::optional<std::int64_t> integer;
    std::string text;
    std::vector<double> floats;
    std::vector<std::int64_t> ints;
    std::vector<std::string> strings;
    std::optional<OnnxTensor> tensor;
};

struct OnnxNode {
    std::string op_type;
    std::string domain;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    std::map<std::string, OnnxAttribute> attributes;
};

struct OnnxValueInfo {
    std::string name;
    int element_type = 0;
    std::vector<std::int64_t> dims;  // -1 for a named or unknown dimension
};

struct OnnxModel {
    std::vector<OnnxNode> nodes;
    std::map<std::string, OnnxTensor> initializers;
    std::vector<OnnxValueInfo> inputs;
    std::map<std::string, std::string> metadata;
};

bool append_varints(const protobuf::Field& field, std::vector<std::int64_t>& into) {
    if (field.wire_type == protobuf::WireType::Varint) {
        into.push_back(field.as_int64());
        return true;
    }
    if (field.wire_type != protobuf::WireType::LengthDelimited) return false;
    const auto values = protobuf::Reader::packed_varints(field.bytes);
    if (!values) return false;
    into.insert(into.end(), values->begin(), values->end());
    return true;
}

bool append_floats(const protobuf::Field& field, std::vector<double>& into) {
    if (field.wire_type == protobuf::WireType::Fixed32) {
        into.push_back(field.as_float());
        return true;
    }
    if (field.wire_type != protobuf::WireType::LengthDelimited) return false;
    const auto values = protobuf::Reader::packed_floats(field.bytes);
    if (!values) return false;
    into.insert(into.end(), values->begin(), values->end());
    return true;
}

bool append_doubles(const protobuf::Field& field, std::vector<double>& into) {
    if (field.wire_type == protobuf::WireType::Fixed64) {
        into.push_back(field.as_double());
        return true;
    }
    if (field.wire_type != protobuf::WireType::LengthDelimited) return false;
    const auto values = protobuf::Reader::packed_doubles(field.bytes);
    if (!values) return false;
    into.insert(into.end(), values->begin(), values->end());
    return true;
}

std::optional<OnnxTensor> parse_tensor(std::string_view bytes) {
    OnnxTensor tensor;
    std::string_view raw;
    std::vector<double> float_data;
    std::vector<double> double_data;
    std::vector<std::int64_t> integer_data;
    protobuf::Reader reader(bytes);
    while (const auto field = reader.next()) {
        bool ok = true;
        switch (field->number) {
            case 1: ok = append_varints(*field, tensor.dims); break;
            case 2: tensor.data_type = static_cast<int>(field->integer); break;
            case 4: ok = append_floats(*field, float_data); break;
            case 5: ok = append_varints(*field, integer_data); break;
            case 7: ok = append_varints(*field, integer_data); break;
            case 9: raw = field->bytes; break;
            case 10: ok = append_doubles(*field, double_data); break;
            default: break;
        }
        if (!ok) return std::nullopt;
    }
    if (reader.failed()) return std::nullopt;
    if (!raw.empty()) {
        if (tensor.data_type == kOnnxFloat) {
            const auto values = protobuf::Reader::packed_floats(raw);
            if (!values) return std::nullopt;
            tensor.values.assign(values->begin(), values->end());
        } else if (tensor.data_type == kOnnxDouble) {
            const auto values = protobuf::Reader::packed_doubles(raw);
            if (!values) return std::nullopt;
            tensor.values = *values;
        } else if (tensor.data_type == kOnnxInt64) {
            if (raw.size() % 8 != 0) return std::nullopt;
            for (std::size_t offset = 0; offset < raw.size(); offset += 8) {
                std::int64_t value = 0;
                std::memcpy(&value, raw.data() + offset, 8);  // little-endian hosts only
                tensor.values.push_back(static_cast<double>(value));
            }
        } else {
            return std::nullopt;
        }
    } else if (tensor.data_type == kOnnxFloat) {
        tensor.values = std::move(float_data);
    } else if (tensor.data_type == kOnnxDouble) {
        tensor.values = std::move(double_data);
    } else if (tensor.data_type == kOnnxInt64 || tensor.data_type == kOnnxInt32) {
        for (const std::int64_t value : integer_data) tensor.values.push_back(static_cast<double>(value));
    }
    return tensor;
}

std::optional<std::pair<std::string, OnnxAttribute>> parse_attribute(std::string_view bytes) {
    std::string name;
    OnnxAttribute attribute;
    protobuf::Reader reader(bytes);
    while (const auto field = reader.next()) {
        bool ok = true;
        switch (field->number) {
            case 1: name = std::string(field->bytes); break;
            case 2: attribute.number = field->as_float(); break;
            case 3: attribute.integer = field->as_int64(); break;
            case 4: attribute.text = std::string(field->bytes); break;
            case 5: {
                attribute.tensor = parse_tensor(field->bytes);
                ok = attribute.tensor.has_value();
                break;
            }
            case 7: ok = append_floats(*field, attribute.floats); break;
            case 8: ok = append_varints(*field, attribute.ints); break;
            case 9: attribute.strings.emplace_back(field->bytes); break;
            default: break;
        }
        if (!ok) return std::nullopt;
    }
    if (reader.failed() || name.empty()) return std::nullopt;
    return std::pair{std::move(name), std::move(attribute)};
}

std::optional<OnnxNode> parse_node(std::string_view bytes) {
    OnnxNode node;
    protobuf::Reader reader(bytes);
    while (const auto field = reader.next()) {
        switch (field->number) {
            case 1: node.inputs.emplace_back(field->bytes); break;
            case 2: node.outputs.emplace_back(field->bytes); break;
            case 4: node.op_type = std::string(field->bytes); break;
            case 7: node.domain = std::string(field->bytes); break;
            case 5: {
                auto attribute = parse_attribute(field->bytes);
                if (!attribute) return std::nullopt;
                node.attributes.insert_or_assign(attribute->first, std::move(attribute->second));
                break;
            }
            default: break;
        }
    }
    if (reader.failed()) return std::nullopt;
    return node;
}

std::optional<OnnxValueInfo> parse_value_info(std::string_view bytes) {
    OnnxValueInfo info;
    protobuf::Reader reader(bytes);
    while (const auto field = reader.next()) {
        if (field->number == 1) {
            info.name = std::string(field->bytes);
        } else if (field->number == 2) {  // TypeProto
            protobuf::Reader type_reader(field->bytes);
            while (const auto type_field = type_reader.next()) {
                if (type_field->number != 1) continue;  // tensor_type
                protobuf::Reader tensor_reader(type_field->bytes);
                while (const auto tensor_field = tensor_reader.next()) {
                    if (tensor_field->number == 1) {
                        info.element_type = static_cast<int>(tensor_field->integer);
                    } else if (tensor_field->number == 2) {  // TensorShapeProto
                        protobuf::Reader shape_reader(tensor_field->bytes);
                        while (const auto dimension = shape_reader.next()) {
                            if (dimension->number != 1) continue;
                            std::int64_t value = -1;
                            protobuf::Reader dimension_reader(dimension->bytes);
                            while (const auto part = dimension_reader.next()) {
                                if (part->number == 1) value = part->as_int64();
                            }
                            info.dims.push_back(value > 0 ? value : -1);
                        }
                    }
                }
            }
        }
    }
    if (reader.failed()) return std::nullopt;
    return info;
}

std::optional<OnnxModel> parse_onnx(std::string_view bytes) {
    OnnxModel model;
    bool has_graph = false;
    protobuf::Reader reader(bytes);
    while (const auto field = reader.next()) {
        if (field->number == 7 && field->wire_type == protobuf::WireType::LengthDelimited) {
            has_graph = true;
            protobuf::Reader graph(field->bytes);
            while (const auto graph_field = graph.next()) {
                if (graph_field->number == 1) {
                    auto node = parse_node(graph_field->bytes);
                    if (!node) return std::nullopt;
                    model.nodes.push_back(std::move(*node));
                } else if (graph_field->number == 5) {
                    auto tensor = parse_tensor(graph_field->bytes);
                    if (!tensor) return std::nullopt;
                    // The name is field 8; read it separately to keep parse_tensor small.
                    std::string name;
                    protobuf::Reader names(graph_field->bytes);
                    while (const auto part = names.next()) {
                        if (part->number == 8) name = std::string(part->bytes);
                    }
                    model.initializers.insert_or_assign(name, std::move(*tensor));
                } else if (graph_field->number == 11) {
                    auto info = parse_value_info(graph_field->bytes);
                    if (!info) return std::nullopt;
                    model.inputs.push_back(std::move(*info));
                }
            }
            if (graph.failed()) return std::nullopt;
        } else if (field->number == 14 && field->wire_type == protobuf::WireType::LengthDelimited) {
            std::string key;
            std::string value;
            protobuf::Reader entry(field->bytes);
            while (const auto part = entry.next()) {
                if (part->number == 1) key = std::string(part->bytes);
                if (part->number == 2) value = std::string(part->bytes);
            }
            model.metadata.insert_or_assign(key, value);
        }
    }
    if (reader.failed() || !has_graph || model.nodes.empty()) return std::nullopt;
    return model;
}

// A numeric list attribute, from its list form or its `<name>_as_tensor` form.
// `precision` says whether the values were 32-bit floats.
std::vector<double> onnx_numbers(const OnnxNode& node, const std::string& name,
                                 Precision* precision = nullptr) {
    if (const auto tensor = node.attributes.find(name + "_as_tensor");
        tensor != node.attributes.end() && tensor->second.tensor) {
        if (precision != nullptr) {
            *precision = tensor->second.tensor->data_type == kOnnxDouble ? Precision::Float64
                                                                         : Precision::Float32;
        }
        return tensor->second.tensor->values;
    }
    if (precision != nullptr) *precision = Precision::Float32;
    const auto found = node.attributes.find(name);
    return found == node.attributes.end() ? std::vector<double>{} : found->second.floats;
}

std::vector<std::int64_t> onnx_ints(const OnnxNode& node, const std::string& name) {
    const auto found = node.attributes.find(name);
    return found == node.attributes.end() ? std::vector<std::int64_t>{} : found->second.ints;
}

std::string onnx_text(const OnnxNode& node, const std::string& name, std::string fallback) {
    const auto found = node.attributes.find(name);
    return found == node.attributes.end() || found->second.text.empty() ? fallback
                                                                        : found->second.text;
}

bool is_tree_ensemble(const OnnxNode& node) {
    return node.op_type == "TreeEnsembleRegressor" || node.op_type == "TreeEnsembleClassifier";
}

// The nodes a converter puts around a tree ensemble that change no value.
bool is_plumbing(const OnnxNode& node, const OnnxModel& model) {
    if (node.op_type == "Identity" || node.op_type == "Cast" || node.op_type == "ZipMap") {
        return true;
    }
    if (node.op_type == "Mul") {  // onnxmltools multiplies LightGBM's output by a constant 1
        for (const std::string& input : node.inputs) {
            const auto constant = model.initializers.find(input);
            if (constant == model.initializers.end()) continue;
            return !constant->second.values.empty() &&
                   std::ranges::all_of(constant->second.values,
                                       [](double value) { return value == 1.0; });
        }
    }
    return false;
}

std::optional<ImportedModel> import_onnx(std::string_view bytes, const ModelImportOptions& options,
                                         std::string* error) {
    const std::optional<OnnxModel> onnx = parse_onnx(bytes);
    if (!onnx) {
        return refuse<ImportedModel>(error, "this file is not a model the engine reads. " +
                                                supported_model_formats());
    }
    const OnnxNode* ensemble = nullptr;
    for (const OnnxNode& node : onnx->nodes) {
        if (is_tree_ensemble(node) && (node.domain == "ai.onnx.ml")) {
            if (ensemble != nullptr) {
                return refuse<ImportedModel>(error, "this ONNX graph has more than one tree "
                                                    "ensemble; the engine reads one");
            }
            ensemble = &node;
        }
    }
    if (ensemble == nullptr) {
        const std::string first = onnx->nodes.front().op_type;
        if (first == "TreeEnsemble") {
            return refuse<ImportedModel>(
                error, "this ONNX graph uses the opset 5 TreeEnsemble operator, which the engine "
                       "does not read yet; export with TreeEnsembleRegressor or "
                       "TreeEnsembleClassifier (ai.onnx.ml opset 1 to 3)");
        }
        return refuse<ImportedModel>(
            error, "this ONNX graph is not a tree ensemble (its first operator is " + first +
                       "): a neural network or another kind of model cannot be brought. " +
                       supported_model_formats());
    }
    for (const OnnxNode& node : onnx->nodes) {
        if (&node == ensemble || is_plumbing(node, *onnx)) continue;
        return refuse<ImportedModel>(
            error, "this ONNX graph has a " + node.op_type +
                       " step beside its tree ensemble; the engine reads a graph that is only "
                       "the ensemble (bring the model without its preprocessing, and prepare "
                       "the columns in the dataset)");
    }
    if (ensemble->inputs.empty()) return refuse<ImportedModel>(error, "the tree ensemble has no input");
    const auto input = std::ranges::find(onnx->inputs, ensemble->inputs.front(), &OnnxValueInfo::name);
    if (input == onnx->inputs.end() || onnx->initializers.contains(input->name)) {
        return refuse<ImportedModel>(error, "the tree ensemble does not read the graph's input "
                                            "directly");
    }
    Precision precision = Precision::Float32;
    if (input->element_type == kOnnxDouble) {
        precision = Precision::Float64;
    } else if (input->element_type != kOnnxFloat) {
        return refuse<ImportedModel>(error, "the graph's input is not a float or double tensor");
    }

    const bool classifier = ensemble->op_type == "TreeEnsembleClassifier";
    const std::string post_transform = onnx_text(*ensemble, "post_transform", "NONE");
    std::string objective;
    std::string kind;
    if (classifier) {
        const std::size_t labels = std::max(onnx_ints(*ensemble, "classlabels_int64s").size(),
                                            ensemble->attributes.contains("classlabels_strings")
                                                ? ensemble->attributes.at("classlabels_strings").strings.size()
                                                : std::size_t{0});
        if (labels != 2) {
            return refuse<ImportedModel>(error, "this classifier has " + std::to_string(labels) +
                                                    " classes; the engine reads binary "
                                                    "classifiers and regressors");
        }
        if (post_transform == "NONE") {
            return refuse<ImportedModel>(
                error, "this classifier averages probabilities over its trees (a random forest "
                       "or a decision tree); the engine's classifiers score through a logistic "
                       "link, so it cannot be brought yet. A gradient-boosted classifier or any "
                       "tree regressor can");
        }
        if (post_transform != "LOGISTIC") {
            return refuse<ImportedModel>(error, "this classifier's output is " + post_transform +
                                                    "; the engine reads a logistic one");
        }
        objective = "binary_logistic";
        kind = "classifier";
    } else {
        const auto targets = ensemble->attributes.find("n_targets");
        if (targets != ensemble->attributes.end() && targets->second.integer.value_or(1) != 1) {
            return refuse<ImportedModel>(error, "this regressor predicts several targets; the "
                                                "engine reads one");
        }
        if (post_transform != "NONE") {
            return refuse<ImportedModel>(error, "this regressor's output is " + post_transform +
                                                    "; the engine reads one with none");
        }
        objective = "squared_error";
        kind = "regressor";
    }
    const std::string aggregate = onnx_text(*ensemble, "aggregate_function", "SUM");
    if (aggregate != "SUM" && aggregate != "AVERAGE") {
        return refuse<ImportedModel>(error, "this ensemble combines its trees by " + aggregate +
                                                "; the engine reads SUM and AVERAGE");
    }

    const std::vector<std::int64_t> tree_ids = onnx_ints(*ensemble, "nodes_treeids");
    const std::vector<std::int64_t> node_ids = onnx_ints(*ensemble, "nodes_nodeids");
    const std::vector<std::int64_t> feature_ids = onnx_ints(*ensemble, "nodes_featureids");
    const std::vector<std::int64_t> true_ids = onnx_ints(*ensemble, "nodes_truenodeids");
    const std::vector<std::int64_t> false_ids = onnx_ints(*ensemble, "nodes_falsenodeids");
    std::vector<std::int64_t> tracks_true = onnx_ints(*ensemble, "nodes_missing_value_tracks_true");
    const std::vector<double> thresholds = onnx_numbers(*ensemble, "nodes_values");
    const auto modes_found = ensemble->attributes.find("nodes_modes");
    const std::vector<std::string> modes =
        modes_found == ensemble->attributes.end() ? std::vector<std::string>{}
                                                  : modes_found->second.strings;
    const std::size_t node_count = tree_ids.size();
    if (tracks_true.empty()) tracks_true.assign(node_count, 0);
    if (node_count == 0 || node_ids.size() != node_count || feature_ids.size() != node_count ||
        true_ids.size() != node_count || false_ids.size() != node_count ||
        modes.size() != node_count || thresholds.size() != node_count ||
        tracks_true.size() != node_count) {
        return refuse<ImportedModel>(error, "the tree ensemble's node attributes do not agree "
                                            "in length");
    }
    const std::string prefix = classifier ? "class_" : "target_";
    const std::vector<std::int64_t> leaf_trees = onnx_ints(*ensemble, prefix + "treeids");
    const std::vector<std::int64_t> leaf_nodes = onnx_ints(*ensemble, prefix + "nodeids");
    const std::vector<std::int64_t> leaf_ids = onnx_ints(*ensemble, prefix + "ids");
    const std::vector<double> leaf_weights = onnx_numbers(*ensemble, prefix + "weights");
    if (leaf_trees.size() != leaf_nodes.size() || leaf_ids.size() != leaf_nodes.size() ||
        leaf_weights.size() != leaf_nodes.size()) {
        return refuse<ImportedModel>(error, "the tree ensemble's leaf attributes do not agree "
                                            "in length");
    }
    if (!leaf_ids.empty() && std::ranges::any_of(leaf_ids, [&](std::int64_t id) {
            return id != leaf_ids.front();
        })) {
        return refuse<ImportedModel>(
            error, classifier ? "this classifier keeps a score for each class; the engine reads "
                                "a binary classifier with one score"
                              : "this regressor predicts several targets; the engine reads one");
    }
    const std::vector<double> base_values = onnx_numbers(*ensemble, "base_values");
    if (base_values.size() > 1) {
        return refuse<ImportedModel>(error, "the ensemble has " + std::to_string(base_values.size()) +
                                                " base values; the engine reads one");
    }

    // Trees, in the order their ids first appear.
    std::vector<std::int64_t> tree_order;
    std::map<std::int64_t, std::map<std::int64_t, std::size_t>> positions;  // tree → node id → index
    for (std::size_t index = 0; index < node_count; ++index) {
        auto& tree = positions[tree_ids[index]];
        if (tree.empty()) tree_order.push_back(tree_ids[index]);
        if (!tree.emplace(node_ids[index], index).second) {
            return refuse<ImportedModel>(error, "a tree repeats a node id");
        }
    }
    std::size_t feature_count = 0;
    if (input->dims.size() == 2 && input->dims[1] > 0) {
        feature_count = static_cast<std::size_t>(input->dims[1]);
    } else {
        for (std::size_t index = 0; index < node_count; ++index) {
            if (modes[index] != "LEAF") {
                feature_count = std::max(feature_count, static_cast<std::size_t>(feature_ids[index] + 1));
            }
        }
    }
    const double divisor = aggregate == "AVERAGE" ? static_cast<double>(tree_order.size()) : 1.0;
    std::map<std::pair<std::int64_t, std::int64_t>, double> leaf_values;
    for (std::size_t index = 0; index < leaf_nodes.size(); ++index) {
        leaf_values[{leaf_trees[index], leaf_nodes[index]}] += leaf_weights[index];
    }

    GbdtModel model;
    model.objective = objective;
    model.base_score = base_values.empty() ? 0.0 : base_values.front();
    model.learning_rate = 1.0;
    for (const std::int64_t tree_id : tree_order) {
        const auto& index_of = positions.at(tree_id);
        std::vector<SourceNode> nodes(index_of.size());
        std::unordered_map<std::int64_t, int> local;  // node id → position in `nodes`
        int next = 0;
        for (const auto& [node_id, index] : index_of) local[node_id] = next++;
        std::set<int> children;
        const auto local_of = [&](std::int64_t node_id) {
            const auto found = local.find(node_id);
            return found == local.end() ? -1 : found->second;
        };
        for (const auto& [node_id, index] : index_of) {
            SourceNode& node = nodes[static_cast<std::size_t>(local.at(node_id))];
            node.present = true;
            const std::string& mode = modes[index];
            if (mode == "LEAF") {
                const auto value = leaf_values.find({tree_id, node_id});
                node.value = value == leaf_values.end() ? 0.0 : value->second / divisor;
                continue;
            }
            Comparison comparison;
            if (mode == "BRANCH_LT") comparison = Comparison::Less;
            else if (mode == "BRANCH_LEQ") comparison = Comparison::LessEqual;
            else if (mode == "BRANCH_GT") comparison = Comparison::Greater;
            else if (mode == "BRANCH_GTE") comparison = Comparison::GreaterEqual;
            else {
                return refuse<ImportedModel>(error, "a split is " + mode +
                                                        "; the engine reads <, <=, > and >= splits");
            }
            const int true_child = local_of(true_ids[index]);
            const int false_child = local_of(false_ids[index]);
            bool true_is_below = true;
            if (!convert_split(comparison, thresholds[index], precision, &node.bound, &true_is_below)) {
                return refuse<ImportedModel>(error, "a split's threshold is not a finite number");
            }
            node.leaf = false;
            node.feature = static_cast<int>(feature_ids[index]);
            node.below = true_is_below ? true_child : false_child;
            node.above = true_is_below ? false_child : true_child;
            // onnxruntime: NaN takes the true branch when tracked, and otherwise every
            // comparison with NaN is false.
            const int missing_child = tracks_true[index] != 0 ? true_child : false_child;
            node.missing_below = missing_child == node.below;
            children.insert(true_child);
            children.insert(false_child);
        }
        int root = -1;
        for (int position = 0; position < static_cast<int>(nodes.size()); ++position) {
            if (children.contains(position)) continue;
            if (root >= 0) return refuse<ImportedModel>(error, "a tree has more than one root");
            root = position;
        }
        std::optional<RegressionTree> tree =
            build_tree(nodes, root, feature_count, /*covers_known=*/false, error);
        if (!tree) return std::nullopt;
        model.trees.push_back(std::move(*tree));
    }
    std::vector<std::string> named;
    if (const auto names = onnx->metadata.find("feature_names"); names != onnx->metadata.end()) {
        const nlohmann::json parsed = nlohmann::json::parse(names->second, nullptr, false);
        if (parsed.is_array()) {
            for (const nlohmann::json& name : parsed) {
                if (name.is_string()) named.push_back(name.get<std::string>());
            }
        }
    }
    auto feature_names = resolve_feature_names(options, std::move(named), feature_count, error);
    if (!feature_names) return std::nullopt;
    model.feature_names = std::move(*feature_names);
    return ImportedModel{std::move(model), ModelFileFormat::Onnx, kind};
}

// --- XGBoost -----------------------------------------------------------------------

// A number XGBoost writes as a string, sometimes in brackets: "5E-1", "[5E-1]".
std::optional<double> xgboost_number(const nlohmann::json& value) {
    if (value.is_number()) return value.get<double>();
    if (!value.is_string()) return std::nullopt;
    std::string text = value.get<std::string>();
    std::erase_if(text, [](char character) { return character == '[' || character == ']' || character == ' '; });
    if (text.find(',') != std::string::npos) return std::nullopt;
    double number = 0.0;
    const auto [end, status] = std::from_chars(text.data(), text.data() + text.size(), number);
    if (status != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return number;
}

std::optional<ImportedModel> import_xgboost(const nlohmann::json& document, ModelFileFormat format,
                                            const ModelImportOptions& options, std::string* error) {
    const auto fail = [&](const std::string& message) {
        return refuse<ImportedModel>(error, "XGBoost model: " + message);
    };
    if (!document.is_object() || !document.contains("learner") || !document.at("learner").is_object()) {
        return fail("no learner");
    }
    const nlohmann::json& learner = document.at("learner");
    const nlohmann::json booster = learner.value("gradient_booster", nlohmann::json::object());
    const std::string booster_name = booster.value("name", std::string{});
    if (booster_name != "gbtree") {
        return fail("the booster is '" + booster_name + "'; the engine reads gbtree (not dart or "
                    "gblinear)");
    }
    const std::string objective =
        learner.value("objective", nlohmann::json::object()).value("name", std::string{});
    std::string engine_objective;
    std::string kind;
    if (objective == "binary:logistic" || objective == "reg:logistic") {
        engine_objective = "binary_logistic";
        kind = "classifier";
    } else if (objective == "reg:squarederror" || objective == "reg:linear" ||
               objective == "reg:absoluteerror" || objective == "reg:pseudohubererror" ||
               objective == "reg:squaredlogerror" || objective == "reg:quantileerror") {
        engine_objective = "squared_error";
        kind = "regressor";
    } else {
        return fail("the objective is '" + objective + "'; the engine reads binary:logistic and "
                    "the reg: objectives whose prediction is the sum of the trees");
    }
    const nlohmann::json parameters = learner.value("learner_model_param", nlohmann::json::object());
    const auto count_of = [&](const char* name) {
        return xgboost_number(parameters.value(name, nlohmann::json("0"))).value_or(0.0);
    };
    if (count_of("num_class") > 1.0 || count_of("num_target") > 1.0) {
        return fail("it predicts several classes or targets; the engine reads one");
    }
    const std::optional<double> base_score =
        xgboost_number(parameters.value("base_score", nlohmann::json("0.5")));
    if (!base_score) return fail("its base_score is not one number");
    if (const auto types = learner.find("feature_types"); types != learner.end() && types->is_array()) {
        for (const nlohmann::json& type : *types) {
            if (type == "c") return fail("it has categorical features; the engine reads numeric ones");
        }
    }
    std::vector<std::string> names;
    if (const auto found = learner.find("feature_names"); found != learner.end() && found->is_array()) {
        for (const nlohmann::json& name : *found) names.push_back(name.get<std::string>());
    }
    const auto feature_count = static_cast<std::size_t>(count_of("num_feature"));

    GbdtModel model;
    model.objective = engine_objective;
    model.learning_rate = 1.0;
    if (engine_objective == "binary_logistic") {
        if (*base_score <= 0.0 || *base_score >= 1.0) return fail("its base_score is not a probability");
        // XGBoost keeps the base score as a 32-bit float probability and adds its logit.
        model.base_score = logit(static_cast<float>(*base_score));
    } else {
        model.base_score = static_cast<float>(*base_score);
    }
    const nlohmann::json trees = booster.value("model", nlohmann::json::object())
                                     .value("trees", nlohmann::json::array());
    for (const nlohmann::json& tree_json : trees) {
        const auto array = [&](const char* name) {
            return tree_json.value(name, nlohmann::json::array());
        };
        const nlohmann::json left = array("left_children");
        const nlohmann::json right = array("right_children");
        const nlohmann::json features = array("split_indices");
        const nlohmann::json conditions = array("split_conditions");
        const nlohmann::json default_left = array("default_left");
        const nlohmann::json split_types = array("split_type");
        const nlohmann::json hessians = array("sum_hessian");
        const std::size_t count = left.size();
        if (count == 0 || right.size() != count || features.size() != count ||
            conditions.size() != count || default_left.size() != count) {
            return fail("a tree's arrays do not agree in length");
        }
        const nlohmann::json tree_parameters = tree_json.value("tree_param", nlohmann::json::object());
        if (xgboost_number(tree_parameters.value("size_leaf_vector", nlohmann::json("1"))).value_or(1.0) > 1.0) {
            return fail("its leaves hold vectors; the engine reads one value per leaf");
        }
        const bool covers_known = hessians.size() == count;
        std::vector<SourceNode> nodes(count);
        for (std::size_t index = 0; index < count; ++index) {
            SourceNode& node = nodes[index];
            node.present = true;
            if (covers_known) node.cover = hessians[index].get<double>();
            const int left_child = left[index].get<int>();
            if (left_child < 0) {
                node.value = conditions[index].get<double>();
                continue;
            }
            if (split_types.size() == count && split_types[index].get<int>() != 0) {
                return fail("it has a categorical split; the engine reads numeric ones");
            }
            bool true_is_below = true;
            // XGBoost compares 32-bit floats: x < split_condition goes left.
            if (!convert_split(Comparison::Less, conditions[index].get<double>(), Precision::Float32,
                               &node.bound, &true_is_below)) {
                return fail("a split's threshold is not a finite number");
            }
            node.leaf = false;
            node.feature = features[index].get<int>();
            node.below = left_child;
            node.above = right[index].get<int>();
            node.missing_below = default_left[index].get<int>() != 0;
        }
        std::optional<RegressionTree> tree = build_tree(
            nodes, 0, std::max(feature_count, names.size()), covers_known, error);
        if (!tree) {
            if (error != nullptr) *error = "XGBoost model: " + *error;
            return std::nullopt;
        }
        model.trees.push_back(std::move(*tree));
    }
    auto feature_names =
        resolve_feature_names(options, std::move(names), std::max<std::size_t>(feature_count, 1), error);
    if (!feature_names) return std::nullopt;
    model.feature_names = std::move(*feature_names);
    return ImportedModel{std::move(model), format, kind};
}

// --- LightGBM ----------------------------------------------------------------------

std::vector<std::string_view> split_words(std::string_view text) {
    std::vector<std::string_view> words;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find(' ', start);
        const std::size_t stop = end == std::string_view::npos ? text.size() : end;
        if (stop > start) words.push_back(text.substr(start, stop - start));
        start = stop + 1;
    }
    return words;
}

template <typename Number>
std::optional<std::vector<Number>> parse_numbers(std::string_view text) {
    std::vector<Number> values;
    for (const std::string_view word : split_words(text)) {
        Number value{};
        const auto [end, status] = std::from_chars(word.data(), word.data() + word.size(), value);
        if (status != std::errc{} || end != word.data() + word.size()) return std::nullopt;
        values.push_back(value);
    }
    return values;
}

std::optional<ImportedModel> import_lightgbm(std::string_view text, const ModelImportOptions& options,
                                             std::string* error) {
    const auto fail = [&](const std::string& message) {
        return refuse<ImportedModel>(error, "LightGBM model: " + message);
    };
    std::map<std::string, std::string, std::less<>> header;
    std::vector<std::map<std::string, std::string, std::less<>>> trees;
    bool average_output = false;
    bool in_parameters = false;
    bool reg_sqrt = false;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line == "end of trees") continue;
        if (line == "parameters:") in_parameters = true;
        if (line == "end of parameters") in_parameters = false;
        if (in_parameters) {
            if (line == "[reg_sqrt: 1]") reg_sqrt = true;
            continue;
        }
        if (line == "average_output") {
            average_output = true;
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        const std::string key(line.substr(0, equals));
        const std::string value(line.substr(equals + 1));
        if (key == "Tree") {
            trees.emplace_back();
            continue;
        }
        if (key == "feature_importances" || key == "pandas_categorical") continue;
        if (trees.empty()) header[key] = value;
        else trees.back()[key] = value;
        if (end == text.size()) break;
    }
    const auto header_value = [&](std::string_view key) {
        const auto found = header.find(key);
        return found == header.end() ? std::string{} : found->second;
    };
    if (header_value("num_class") != "1" || header_value("num_tree_per_iteration") != "1") {
        return fail("it predicts several classes; the engine reads one score");
    }
    if (reg_sqrt) return fail("it was trained with reg_sqrt, whose prediction is squared");
    const std::vector<std::string_view> objective_words = split_words(header_value("objective"));
    const std::string objective =
        objective_words.empty() ? std::string{} : std::string(objective_words.front());
    std::string engine_objective;
    std::string kind;
    double sigmoid = 1.0;
    if (objective == "binary" || objective == "cross_entropy" || objective == "xentropy") {
        engine_objective = "binary_logistic";
        kind = "classifier";
        for (const std::string_view word : objective_words) {
            if (word.starts_with("sigmoid:")) {
                const auto value = parse_numbers<double>(word.substr(8));
                if (!value || value->size() != 1) return fail("its sigmoid is not a number");
                sigmoid = value->front();
            }
        }
    } else if (objective == "regression" || objective == "regression_l2" || objective == "l2" ||
               objective == "mean_squared_error" || objective == "mse" || objective == "l2_root" ||
               objective == "root_mean_squared_error" || objective == "rmse" ||
               objective == "regression_l1" || objective == "l1" || objective == "mae" ||
               objective == "mean_absolute_error" || objective == "huber" || objective == "fair" ||
               objective == "quantile" || objective == "mape") {
        engine_objective = "squared_error";
        kind = "regressor";
    } else {
        return fail("the objective is '" + objective + "'; the engine reads binary and the "
                    "regression objectives whose prediction is the sum of the trees");
    }
    const auto max_feature = parse_numbers<int>(header_value("max_feature_idx"));
    if (!max_feature || max_feature->size() != 1 || max_feature->front() < 0) {
        return fail("it has no max_feature_idx");
    }
    const auto feature_count = static_cast<std::size_t>(max_feature->front() + 1);
    std::vector<std::string> names;
    for (const std::string_view word : split_words(header_value("feature_names"))) {
        names.emplace_back(word);
    }

    GbdtModel model;
    model.objective = engine_objective;
    model.base_score = 0.0;  // LightGBM folds its initial score into the first tree
    model.learning_rate = 1.0;
    const double scale = sigmoid / (average_output ? static_cast<double>(std::max<std::size_t>(trees.size(), 1)) : 1.0);
    for (const auto& tree_fields : trees) {
        const auto field = [&](std::string_view key) {
            const auto found = tree_fields.find(key);
            return found == tree_fields.end() ? std::string{} : found->second;
        };
        const auto leaves = parse_numbers<int>(field("num_leaves"));
        if (!leaves || leaves->size() != 1 || leaves->front() < 1) return fail("a tree has no num_leaves");
        const int leaf_count = leaves->front();
        if (field("num_cat") != "" && field("num_cat") != "0") {
            return fail("it has categorical splits; the engine reads numeric ones");
        }
        if (field("is_linear") == "1") return fail("it has linear trees; the engine reads constant leaves");
        const auto leaf_values = parse_numbers<double>(field("leaf_value"));
        if (!leaf_values || static_cast<int>(leaf_values->size()) != leaf_count) {
            return fail("a tree's leaf_value does not match num_leaves");
        }
        const int internal_count = leaf_count - 1;
        std::vector<SourceNode> nodes(static_cast<std::size_t>(internal_count + leaf_count));
        const auto leaf_weights = parse_numbers<double>(field("leaf_weight"));
        const auto internal_weights = parse_numbers<double>(field("internal_weight"));
        bool covers_known = leaf_weights && internal_weights &&
                            static_cast<int>(leaf_weights->size()) == leaf_count &&
                            static_cast<int>(internal_weights->size()) == internal_count;
        if (covers_known) {
            covers_known = std::ranges::all_of(*leaf_weights, [](double weight) { return weight > 0.0; });
        }
        for (int leaf = 0; leaf < leaf_count; ++leaf) {
            SourceNode& node = nodes[static_cast<std::size_t>(internal_count + leaf)];
            node.present = true;
            node.value = (*leaf_values)[static_cast<std::size_t>(leaf)] * scale;
            if (covers_known) node.cover = (*leaf_weights)[static_cast<std::size_t>(leaf)];
        }
        if (internal_count > 0) {
            const auto features = parse_numbers<int>(field("split_feature"));
            const auto thresholds = parse_numbers<double>(field("threshold"));
            const auto decisions = parse_numbers<int>(field("decision_type"));
            const auto lefts = parse_numbers<int>(field("left_child"));
            const auto rights = parse_numbers<int>(field("right_child"));
            const auto size = static_cast<std::size_t>(internal_count);
            if (!features || !thresholds || !decisions || !lefts || !rights ||
                features->size() != size || thresholds->size() != size ||
                decisions->size() != size || lefts->size() != size || rights->size() != size) {
                return fail("a tree's split arrays do not match num_leaves");
            }
            const auto position = [&](int child) { return child >= 0 ? child : internal_count + ~child; };
            for (std::size_t index = 0; index < size; ++index) {
                SourceNode& node = nodes[index];
                node.present = true;
                node.leaf = false;
                if (covers_known) node.cover = (*internal_weights)[index];
                const int decision = (*decisions)[index];
                if ((decision & 1) != 0) return fail("it has a categorical split; the engine reads numeric ones");
                const bool default_left = (decision & 2) != 0;
                const int missing_type = (decision >> 2) & 3;
                const double threshold = (*thresholds)[index];
                bool true_is_below = true;
                // LightGBM compares doubles: x <= threshold goes left.
                if (!convert_split(Comparison::LessEqual, threshold, Precision::Float64, &node.bound,
                                   &true_is_below)) {
                    return fail("a split's threshold is not a finite number");
                }
                node.feature = (*features)[index];
                node.below = position((*lefts)[index]);
                node.above = position((*rights)[index]);
                // LightGBM's NumericalDecision: a NaN becomes 0 unless the missing type
                // is NaN; a NaN-typed missing value, or a zero under the Zero type,
                // takes the default direction.
                constexpr double kZero = 1e-35;  // LightGBM's kZeroThreshold
                if (missing_type == 2) {
                    node.missing_below = default_left;
                } else if (missing_type == 1) {
                    const bool zeros_agree = default_left ? threshold >= kZero : threshold < -kZero;
                    if (!zeros_agree) {
                        return fail("a split sends zero, as missing, against its own threshold; "
                                    "train with zero_as_missing=false");
                    }
                    node.missing_below = default_left;
                } else {
                    node.missing_below = 0.0 < node.bound;
                }
            }
        }
        std::optional<RegressionTree> tree =
            build_tree(nodes, internal_count > 0 ? 0 : internal_count, feature_count, covers_known, error);
        if (!tree) {
            if (error != nullptr) *error = "LightGBM model: " + *error;
            return std::nullopt;
        }
        model.trees.push_back(std::move(*tree));
    }
    if (trees.empty()) return fail("it has no trees");
    auto feature_names = resolve_feature_names(options, std::move(names), feature_count, error);
    if (!feature_names) return std::nullopt;
    model.feature_names = std::move(*feature_names);
    return ImportedModel{std::move(model), ModelFileFormat::LightgbmText, kind};
}

// --- The engine's own JSON -----------------------------------------------------------

std::optional<ImportedModel> import_zetta(const nlohmann::json& body, const ModelImportOptions& options,
                                          std::string* error) {
    std::string reason;
    std::optional<GbdtModel> model = GbdtModel::from_json(body, &reason);
    if (!model) {
        return refuse<ImportedModel>(error, "the engine's own JSON weights format: " + reason);
    }
    auto names = resolve_feature_names(options, model->feature_names, model->feature_count(), error);
    if (!names) return std::nullopt;
    model->feature_names = std::move(*names);
    const std::string kind = model->objective == "squared_error" ? "regressor" : "classifier";
    return ImportedModel{std::move(*model), ModelFileFormat::Zetta, kind};
}

std::string_view trim_start(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\n' || text.front() == '\r' ||
                             text.front() == '\t')) {
        text.remove_prefix(1);
    }
    return text;
}

// --- ONNX export ---------------------------------------------------------------------

protobuf::Writer ints_attribute(const std::string& name, const std::vector<std::int64_t>& values) {
    protobuf::Writer attribute;
    attribute.bytes_field(1, name).varint_field(20, 7);  // INTS
    for (const std::int64_t value : values) attribute.varint_field(8, static_cast<std::uint64_t>(value));
    return attribute;
}

protobuf::Writer strings_attribute(const std::string& name, const std::vector<std::string>& values) {
    protobuf::Writer attribute;
    attribute.bytes_field(1, name).varint_field(20, 8);  // STRINGS
    for (const std::string& value : values) attribute.bytes_field(9, value);
    return attribute;
}

protobuf::Writer string_attribute(const std::string& name, const std::string& value) {
    protobuf::Writer attribute;
    attribute.bytes_field(1, name).varint_field(20, 3).bytes_field(4, value);  // STRING
    return attribute;
}

protobuf::Writer int_attribute(const std::string& name, std::int64_t value) {
    protobuf::Writer attribute;
    attribute.bytes_field(1, name).varint_field(20, 2).varint_field(3, static_cast<std::uint64_t>(value));
    return attribute;
}

protobuf::Writer doubles_attribute(const std::string& name, const std::vector<double>& values) {
    protobuf::Writer tensor;
    tensor.varint_field(1, values.size()).varint_field(2, kOnnxDouble).bytes_field(
        9, protobuf::Writer::raw_doubles(values));
    protobuf::Writer attribute;
    attribute.bytes_field(1, name).varint_field(20, 4).message_field(5, tensor);  // TENSOR
    return attribute;
}

// A tensor value: element type and shape (0 = a named dimension "N").
protobuf::Writer value_info(const std::string& name, int element_type,
                            const std::vector<std::int64_t>& dims) {
    protobuf::Writer shape;
    for (const std::int64_t dim : dims) {
        protobuf::Writer dimension;
        if (dim > 0) dimension.varint_field(1, static_cast<std::uint64_t>(dim));
        else dimension.bytes_field(2, "N");
        shape.message_field(1, dimension);
    }
    protobuf::Writer tensor_type;
    tensor_type.varint_field(1, static_cast<std::uint64_t>(element_type)).message_field(2, shape);
    protobuf::Writer type;
    type.message_field(1, tensor_type);
    protobuf::Writer info;
    info.bytes_field(1, name).message_field(2, type);
    return info;
}

}  // namespace

std::string to_string(ModelFileFormat format) {
    switch (format) {
        case ModelFileFormat::Zetta: return "zetta";
        case ModelFileFormat::Onnx: return "onnx";
        case ModelFileFormat::XgboostJson: return "xgboost_json";
        case ModelFileFormat::XgboostUbjson: return "xgboost_ubjson";
        case ModelFileFormat::LightgbmText: return "lightgbm";
    }
    return "zetta";
}

std::optional<ModelFileFormat> model_file_format_from_string(std::string_view name) {
    for (const ModelFileFormat format :
         {ModelFileFormat::Zetta, ModelFileFormat::Onnx, ModelFileFormat::XgboostJson,
          ModelFileFormat::XgboostUbjson, ModelFileFormat::LightgbmText}) {
        if (to_string(format) == name) return format;
    }
    return std::nullopt;
}

std::string supported_model_formats() {
    return "The engine reads gradient-boosted and other tree models as ONNX (TreeEnsembleRegressor "
           "or TreeEnsembleClassifier, from scikit-learn, XGBoost or LightGBM), XGBoost's own JSON "
           "or UBJSON model, LightGBM's text model, and its own JSON weights";
}

double float_less_bound(float upper) {
    constexpr float kFloatInfinity = std::numeric_limits<float>::infinity();
    if (std::isnan(upper)) return std::numeric_limits<double>::quiet_NaN();
    if (upper == -kFloatInfinity) return -kInfinity;
    // float(x) < upper  <=>  float(x) <= below, the float just under upper.
    const float below = std::nextafter(upper, -kFloatInfinity);
    // The doubles that round to `below` or lower end at the midpoint between below
    // and upper; at the midpoint itself, round-to-nearest-even picks whichever of the
    // two has an even significand. An infinite neighbour stands at 2^128, where a
    // finite float would be if the exponent went one higher.
    const double two_to_128 = std::ldexp(1.0, 128);
    const double low = std::isinf(below) ? -two_to_128 : static_cast<double>(below);
    const double high = std::isinf(upper) ? two_to_128 : static_cast<double>(upper);
    const double middle = low + (high - low) / 2.0;
    std::uint32_t bits = 0;
    std::memcpy(&bits, &below, sizeof bits);
    const bool below_is_even = (bits & 1U) == 0;
    return below_is_even ? std::nextafter(middle, kInfinity) : middle;
}

namespace {

// Recognise the format and run its reader. import_model checks what comes back.
std::optional<ImportedModel> read_model(std::string_view bytes, const ModelImportOptions& options,
                                        std::string* error) {
    const std::string_view text = trim_start(bytes);
    if (text.empty()) return refuse<ImportedModel>(error, "the file is empty");
    std::optional<ModelFileFormat> format = options.format;
    nlohmann::json document;
    if (!format) {
        const auto first = static_cast<unsigned char>(bytes.front());
        if (first == 0x80 && bytes.size() > 1 && static_cast<unsigned char>(bytes[1]) <= 5) {
            return refuse<ImportedModel>(
                error, "this is a Python pickle (a .pkl or joblib file), which the engine never "
                       "unpickles: loading one runs code. Save the model as ONNX, or with "
                       "XGBoost's or LightGBM's own save_model. " + supported_model_formats());
        }
        if (text.starts_with("tree\n") || text.starts_with("tree\r\n")) {
            format = ModelFileFormat::LightgbmText;
        } else if (text.front() == '{') {
            document = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
            if (document.is_discarded()) {
                document = nlohmann::json::from_ubjson(bytes, /*strict=*/true, /*allow_exceptions=*/false);
                if (!document.is_discarded() && document.is_object() && document.contains("learner")) {
                    format = ModelFileFormat::XgboostUbjson;
                }
            } else if (document.is_object() && document.contains("learner")) {
                format = ModelFileFormat::XgboostJson;
            } else if (document.is_object() && document.contains("trees")) {
                format = ModelFileFormat::Zetta;
            } else {
                return refuse<ImportedModel>(error, "this JSON file is not a model the engine reads. " +
                                                        supported_model_formats());
            }
        }
        if (!format) {
            // Anything else: ONNX, whose reader says so when it is not.
            format = ModelFileFormat::Onnx;
        }
    }
    switch (*format) {
        case ModelFileFormat::Onnx: return import_onnx(bytes, options, error);
        case ModelFileFormat::LightgbmText: return import_lightgbm(bytes, options, error);
        case ModelFileFormat::XgboostJson:
        case ModelFileFormat::Zetta: {
            if (document.is_null()) document = nlohmann::json::parse(text, nullptr, false);
            if (document.is_discarded()) {
                return refuse<ImportedModel>(error, "the file is not JSON. " + supported_model_formats());
            }
            return *format == ModelFileFormat::Zetta ? import_zetta(document, options, error)
                                                     : import_xgboost(document, *format, options, error);
        }
        case ModelFileFormat::XgboostUbjson: {
            if (document.is_null()) {
                document = nlohmann::json::from_ubjson(bytes, true, false);
            }
            if (document.is_discarded()) {
                return refuse<ImportedModel>(error, "the file is not UBJSON. " + supported_model_formats());
            }
            return import_xgboost(document, *format, options, error);
        }
    }
    return refuse<ImportedModel>(error, supported_model_formats());
}

}  // namespace

std::optional<ImportedModel> import_model(std::string_view bytes, const ModelImportOptions& options,
                                          std::string* error) {
    std::optional<ImportedModel> imported = read_model(bytes, options, error);
    if (!imported) return std::nullopt;
    // Every reader checks splits as it goes, but against the counts it saw at the
    // time; the feature names can still change after that (the caller's list
    // replaces the file's). Check the finished model once more against its own,
    // final feature list (#1390): a split past the end of it would read past the
    // row at scoring and write past the explanation in TreeSHAP.
    std::string reason;
    if (!imported->model.validate_structure(&reason)) {
        return refuse<ImportedModel>(error, "the model does not hold together: " + reason);
    }
    return imported;
}

std::optional<std::string> export_onnx(const GbdtModel& model, std::string* error) {
    const bool classifier = model.objective == "binary_logistic";
    if (!classifier && model.objective != "squared_error") {
        return refuse<std::string>(error, "a " + model.objective + " model cannot be written as "
                                          "ONNX; download its weights in the engine's format");
    }
    double scale = 1.0;
    double offset = 0.0;
    if (model.calibration && classifier) {
        if (model.calibration->method != CalibrationMethod::Platt) {
            return refuse<std::string>(error, "this model's isotonic calibration cannot be written "
                                              "as ONNX; download its weights in the engine's format");
        }
        scale = model.calibration->platt_a;  // sigmoid(a * raw + b), folded into the leaves
        offset = model.calibration->platt_b;
    }
    std::vector<std::int64_t> tree_ids, node_ids, feature_ids, true_ids, false_ids, missing;
    std::vector<std::string> modes;
    std::vector<double> thresholds;
    std::vector<std::int64_t> leaf_trees, leaf_nodes, leaf_ids;
    std::vector<double> leaf_weights;
    for (std::size_t tree_index = 0; tree_index < model.trees.size(); ++tree_index) {
        const auto tree_id = static_cast<std::int64_t>(tree_index);
        const std::vector<TreeNode>& nodes = model.trees[tree_index].nodes;
        if (nodes.empty()) {  // contributes 0: one leaf
            tree_ids.push_back(tree_id);
            node_ids.push_back(0);
            feature_ids.push_back(0);
            true_ids.push_back(0);
            false_ids.push_back(0);
            missing.push_back(0);
            modes.emplace_back("LEAF");
            thresholds.push_back(0.0);
            leaf_trees.push_back(tree_id);
            leaf_nodes.push_back(0);
            leaf_ids.push_back(0);
            leaf_weights.push_back(0.0);
            continue;
        }
        for (std::size_t index = 0; index < nodes.size(); ++index) {
            const TreeNode& node = nodes[index];
            tree_ids.push_back(tree_id);
            node_ids.push_back(static_cast<std::int64_t>(index));
            if (node.is_leaf()) {
                feature_ids.push_back(0);
                true_ids.push_back(0);
                false_ids.push_back(0);
                missing.push_back(0);
                modes.emplace_back("LEAF");
                thresholds.push_back(0.0);
                leaf_trees.push_back(tree_id);
                leaf_nodes.push_back(static_cast<std::int64_t>(index));
                leaf_ids.push_back(0);
                leaf_weights.push_back(node.value * scale);
            } else {
                feature_ids.push_back(node.feature);
                true_ids.push_back(node.left);
                false_ids.push_back(node.right);
                missing.push_back(node.missing_left ? 1 : 0);
                modes.emplace_back("BRANCH_LT");
                thresholds.push_back(node.threshold);
            }
        }
    }
    const std::string prefix = classifier ? "class_" : "target_";
    protobuf::Writer node;
    node.bytes_field(1, "input");
    if (classifier) {
        node.bytes_field(2, "label").bytes_field(2, "probabilities");
    } else {
        node.bytes_field(2, "variable");
    }
    node.bytes_field(3, "zetta_trees")
        .bytes_field(4, classifier ? "TreeEnsembleClassifier" : "TreeEnsembleRegressor")
        .bytes_field(7, "ai.onnx.ml")
        .message_field(5, ints_attribute("nodes_treeids", tree_ids))
        .message_field(5, ints_attribute("nodes_nodeids", node_ids))
        .message_field(5, ints_attribute("nodes_featureids", feature_ids))
        .message_field(5, ints_attribute("nodes_truenodeids", true_ids))
        .message_field(5, ints_attribute("nodes_falsenodeids", false_ids))
        .message_field(5, ints_attribute("nodes_missing_value_tracks_true", missing))
        .message_field(5, strings_attribute("nodes_modes", modes))
        .message_field(5, doubles_attribute("nodes_values_as_tensor", thresholds))
        .message_field(5, ints_attribute(prefix + "treeids", leaf_trees))
        .message_field(5, ints_attribute(prefix + "nodeids", leaf_nodes))
        .message_field(5, ints_attribute(prefix + "ids", leaf_ids))
        .message_field(5, doubles_attribute(prefix + "weights_as_tensor", leaf_weights))
        .message_field(5, doubles_attribute("base_values_as_tensor",
                                            {model.base_score * scale + offset}))
        .message_field(5, string_attribute("post_transform", classifier ? "LOGISTIC" : "NONE"));
    if (classifier) {
        node.message_field(5, ints_attribute("classlabels_int64s", {0, 1}));
    } else {
        node.message_field(5, int_attribute("n_targets", 1))
            .message_field(5, string_attribute("aggregate_function", "SUM"));
    }
    const auto features = static_cast<std::int64_t>(model.feature_count());
    protobuf::Writer graph;
    graph.message_field(1, node)
        .bytes_field(2, "zetta_model")
        .message_field(11, value_info("input", kOnnxDouble, {0, features}));
    if (classifier) {
        graph.message_field(12, value_info("label", kOnnxInt64, {0}))
            .message_field(12, value_info("probabilities", kOnnxFloat, {0, 2}));
    } else {
        graph.message_field(12, value_info("variable", kOnnxFloat, {0, 1}));
    }
    protobuf::Writer default_opset;
    default_opset.bytes_field(1, "").varint_field(2, 17);
    protobuf::Writer ml_opset;
    ml_opset.bytes_field(1, "ai.onnx.ml").varint_field(2, 3);
    protobuf::Writer names;
    names.bytes_field(1, "feature_names").bytes_field(2, nlohmann::json(model.feature_names).dump());
    protobuf::Writer onnx;
    onnx.varint_field(1, 8)  // ir_version
        .bytes_field(2, "zetta")
        .message_field(7, graph)
        .message_field(8, default_opset)
        .message_field(8, ml_opset)
        .message_field(14, names);
    return onnx.bytes();
}

}  // namespace zetta::ml
