#include "gui/app.hpp"

#include "zetta/access_key.hpp"
#include "zetta/audit_event.hpp"
#include "zetta/bytes.hpp"
#include "zetta/capability.hpp"
#include "zetta/cluster.hpp"
#include "zetta/compute_job.hpp"
#include "zetta/database.hpp"
#include "zetta/deployment.hpp"
#include "zetta/deployment_target.hpp"
#include "zetta/duration.hpp"
#include "zetta/gate.hpp"
#include "zetta/import.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/serialization.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/large_object.hpp"
#include "zetta/model.hpp"
#include "zetta/n8n_import.hpp"
#include "zetta/node.hpp"
#include "zetta/principal.hpp"
#include "zetta/project.hpp"
#include "zetta/project_graph.hpp"
#include "zetta/query/service.hpp"
#include "zetta/run.hpp"
#include "zetta/scaling_policy.hpp"
#include "zetta/scope.hpp"
#include "zetta/signals.hpp"
#include "zetta/solution_catalog.hpp"
#include "zetta/table.hpp"
#include "zetta/timestamp.hpp"
#include "zetta/validation.hpp"
#include "zetta/version.hpp"

#include <SDL3/SDL.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <imgui_stdlib.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "zetta_font_embedded.hpp"

namespace zetta::gui {
namespace {

// Base logical font size; multiplied by the OS content scale per display.
constexpr float kBaseFontPixels = 15.0f;

// Inline-error text colour (soft red).
constexpr ImVec4 kErrorColor{1.0f, 0.4f, 0.4f, 1.0f};

// Inline-success text colour (soft green) — matches the cluster panel's
// "in sync" hue.
constexpr ImVec4 kSuccessColor{0.4f, 0.8f, 0.4f, 1.0f};

// Correctness-gate status colours: green pass, amber warn, red fail. The amber
// matches the "syncing" colour used in the cluster panel.
constexpr ImVec4 kPassColor{0.4f, 0.8f, 0.4f, 1.0f};
constexpr ImVec4 kWarnColor{0.9f, 0.7f, 0.3f, 1.0f};
constexpr ImVec4 kFailColor{0.95f, 0.4f, 0.4f, 1.0f};

// A small, valid execution graph (the IR's canonical JSON form) prefilled into
// the gate panel so the passing path is one click away — a churn-flavoured
// connect→clean→model→serve pipeline.
constexpr const char* kExampleGraphJson = R"({
  "nodes": [
    { "id": "load",  "kind": "connect", "name": "load_customers" },
    { "id": "clean", "kind": "clean",   "name": "dedupe" },
    { "id": "churn", "kind": "model",   "name": "churn_model" },
    { "id": "serve", "kind": "serve",   "name": "expose_scores" }
  ],
  "edges": [
    { "from": "load",  "to": "clean" },
    { "from": "clean", "to": "churn" },
    { "from": "churn", "to": "serve" }
  ]
})";

// An empty graph (the IR's canonical JSON form) — fails NonEmptyGraphCheck, so
// it demonstrates the deploy-blocking path.
constexpr const char* kEmptyGraphJson = R"({
  "nodes": [],
  "edges": []
})";

// The gate status as an uppercase badge ("PASS" / "WARN" / "FAIL").
std::string status_badge(CheckStatus status) {
    std::string text = to_string(status);
    for (char& character : text) {
        character =
            static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }
    return text;
}

// The text colour for a check status.
ImVec4 status_color(CheckStatus status) {
    switch (status) {
        case CheckStatus::Pass: return kPassColor;
        case CheckStatus::Warn: return kWarnColor;
        case CheckStatus::Fail: return kFailColor;
    }
    return kFailColor;
}

// "unsaved edits" / dirty marker colour (soft amber).
constexpr ImVec4 kUnsavedColor{0.9f, 0.7f, 0.3f, 1.0f};

// Graph-workspace canvas palette (ImU32, for the window draw list). Nodes are
// boxes laid out in columns by StepKind; edges are lines drawn underneath them.
constexpr ImU32 kCanvasHeaderColor = IM_COL32(150, 160, 180, 255);
constexpr ImU32 kNodeFill = IM_COL32(45, 55, 70, 255);
constexpr ImU32 kNodeFillHover = IM_COL32(60, 72, 92, 255);
constexpr ImU32 kNodeFillSelected = IM_COL32(48, 86, 130, 255);
constexpr ImU32 kNodeBorder = IM_COL32(120, 130, 150, 255);
constexpr ImU32 kNodeBorderSelected = IM_COL32(120, 180, 255, 255);
constexpr ImU32 kNodeTextColor = IM_COL32(230, 230, 235, 255);
constexpr ImU32 kDataEdgeColor = IM_COL32(150, 160, 180, 210);
constexpr ImU32 kControlEdgeColor = IM_COL32(200, 160, 110, 210);

float ui_content_scale(SDL_Window* window) {
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    if (display == 0) return 1.0f;
    const float s = SDL_GetDisplayContentScale(display);
    return s > 0.0f ? s : 1.0f;
}

float ui_pixel_density(SDL_Window* window) {
    const float d = SDL_GetWindowPixelDensity(window);
    return d > 0.0f ? d : 1.0f;
}

// A run's status, color-coded along its lifecycle: pending (neutral) → running
// (amber) → succeeded (green) | failed (red). Same palette as the cluster panel.
ImVec4 run_status_color(RunStatus status) {
    switch (status) {
        case RunStatus::Pending:   return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
        case RunStatus::Running:   return ImVec4(0.9f, 0.7f, 0.3f, 1.0f);
        case RunStatus::Succeeded: return ImVec4(0.4f, 0.8f, 0.4f, 1.0f);
        case RunStatus::Failed:    return kErrorColor;
    }
    return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
}

// A compute job's status, color-coded along its lifecycle: queued (neutral) →
// running (amber) → succeeded (green) | failed (red). Same palette as runs.
ImVec4 job_status_color(JobStatus status) {
    switch (status) {
        case JobStatus::Queued:    return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
        case JobStatus::Running:   return ImVec4(0.9f, 0.7f, 0.3f, 1.0f);
        case JobStatus::Succeeded: return ImVec4(0.4f, 0.8f, 0.4f, 1.0f);
        case JobStatus::Failed:    return kErrorColor;
    }
    return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
}

// A scaling action's color: scale-up green (more capacity), hold neutral,
// scale-down amber (shedding nodes). A quick read of the autoscaler's intent.
ImVec4 scaling_action_color(ScalingAction action) {
    switch (action) {
        case ScalingAction::ScaleUp:   return kSuccessColor;
        case ScalingAction::Hold:      return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
        case ScalingAction::ScaleDown: return kWarnColor;
    }
    return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
}

// A log line's severity color — error red, warn amber, anything else neutral.
ImVec4 log_level_color(const std::string& level) {
    if (level == "error") return kErrorColor;
    if (level == "warn")  return ImVec4(0.9f, 0.7f, 0.3f, 1.0f);
    return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
}

// Join lineage refs (sources / steps) for a single wrapped line.
std::string join_refs(const std::vector<std::string>& refs) {
    std::string joined;
    for (std::size_t i = 0; i < refs.size(); ++i) {
        if (i) joined += ", ";
        joined += refs[i];
    }
    return joined;
}

// Render one trained model produced by a run: its identity + weights fingerprint,
// its evaluation metrics (name→number), and its ranked feature drivers (the
// SHAP-style explainability surface). A pure view over an already-validated Model
// handle; metrics/drivers come from the model's free-form (already schema-
// validated) fields, so no new JSON boundary is introduced.
void render_model_card(const Model& model) {
    const std::string short_id = model.id().substr(0, model.id().find('-'));
    ImGui::SeparatorText(
        (model.name().empty() ? ("model " + short_id) : model.name()).c_str());
    ImGui::TextDisabled("kind: %s  ·  id: %s", model.kind().c_str(),
                        short_id.c_str());
    if (model.has_weights()) {
        ImGui::TextDisabled(
            "weights: %s%s", format_bytes(model.weights()->size).c_str(),
            model.has_weights_body() ? "" : " (awaiting body transfer)");
    } else {
        ImGui::TextDisabled("weights: (not yet attached)");
    }

    const ImGuiTableFlags table_flags = ImGuiTableFlags_Borders |
                                        ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_SizingStretchProp;

    // Metrics — the evaluation scores (auc / f1 / …). A flat name→number map; a
    // value in [0, 1] also gets a bar so a score reads at a glance.
    ImGui::TextDisabled("Metrics");
    if (!model.metrics().is_object() || model.metrics().empty()) {
        ImGui::TextDisabled("(no metrics recorded)");
    } else if (ImGui::BeginTable("##model_metrics", 2, table_flags)) {
        ImGui::TableSetupColumn("Metric");
        ImGui::TableSetupColumn("Value");
        ImGui::TableHeadersRow();
        for (const auto& [name, value] : model.metrics().items()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(name.c_str());
            ImGui::TableSetColumnIndex(1);
            const double number = value.is_number() ? value.get<double>() : 0.0;
            if (number >= 0.0 && number <= 1.0) {
                ImGui::ProgressBar(static_cast<float>(number), ImVec2(-FLT_MIN, 0));
            } else {
                ImGui::Text("%.4g", number);
            }
        }
        ImGui::EndTable();
    }

    // Ranked drivers — the feature importances, strongest |influence| first. An
    // optional explainability annotation in the model's feature_schema; absent
    // until an Explain job records it.
    ImGui::TextDisabled("Ranked drivers");
    const std::vector<ModelDriver> drivers = model.drivers();
    if (drivers.empty()) {
        ImGui::TextDisabled("(no ranked drivers recorded)");
    } else {
        // Normalize the bars against the strongest |importance| so the ranking
        // reads visually; a leading "+/-" keeps the sign (push up vs. down).
        const double top = std::abs(drivers.front().importance);
        if (ImGui::BeginTable("##model_drivers", 3, table_flags)) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Feature");
            ImGui::TableSetupColumn("Importance");
            ImGui::TableHeadersRow();
            int rank = 0;
            for (const ModelDriver& driver : drivers) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", ++rank);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(driver.feature.c_str());
                ImGui::TableSetColumnIndex(2);
                const float fraction =
                    top > 0.0 ? static_cast<float>(std::abs(driver.importance) / top)
                              : 0.0f;
                char overlay[32];
                std::snprintf(overlay, sizeof(overlay), "%+.3f", driver.importance);
                ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0), overlay);
            }
            ImGui::EndTable();
        }
    }
}

// Render the full detail of one run: identity + status, logs, outputs, the
// trained model(s) it produced (metrics + ranked drivers), and lineage. A pure
// view over the (already schema-validated) Run handle — no mutation. `models` are
// the node's models whose producing-run ref is this run (resolved by the caller).
void render_run_detail(const Run& run,
                       const std::vector<std::shared_ptr<Model>>& models) {
    ImGui::Text("Run %s", run.id().c_str());
    ImGui::SameLine();
    ImGui::TextColored(run_status_color(run.status()), "[%s]",
                       std::string(to_string(run.status())).c_str());
    if (!run.project().empty()) {
        ImGui::TextDisabled("project: %s", run.project().c_str());
    }
    if (!run.graph().empty()) {
        ImGui::TextDisabled("graph: %s", run.graph().c_str());
    }
    ImGui::TextDisabled("created: %s", run.created().c_str());
    if (run.updated() != run.created()) {
        ImGui::TextDisabled("updated: %s", run.updated().c_str());
    }

    const ImGuiTableFlags table_flags = ImGuiTableFlags_Borders |
                                        ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_SizingStretchProp;

    // Logs — time + level (color-coded) + message; the failing step + reason of
    // a failed run live here.
    ImGui::SeparatorText("Logs");
    if (run.logs().empty()) {
        ImGui::TextDisabled("(no log entries)");
    } else {
        for (const RunLogEntry& entry : run.logs()) {
            ImGui::TextDisabled("%s", entry.time.c_str());
            ImGui::SameLine();
            ImGui::TextColored(log_level_color(entry.level), "%-5s",
                               entry.level.c_str());
            ImGui::SameLine();
            ImGui::TextWrapped("%s", entry.message.c_str());
        }
    }

    // Outputs — the produced datasets / models / indexes.
    ImGui::SeparatorText("Outputs");
    if (run.outputs().empty()) {
        ImGui::TextDisabled("(no outputs)");
    } else if (ImGui::BeginTable("##run_outputs", 3, table_flags)) {
        ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Ref");
        ImGui::TableHeadersRow();
        for (const RunOutput& output : run.outputs()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(output.kind.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(output.name.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(output.ref.c_str());
        }
        ImGui::EndTable();
    }

    // Trained models — the model(s) this run produced, with their metrics and
    // ranked drivers (the FEAT-5 "surface the trained Model + metrics + ranked
    // drivers" slice). Linked by the Model's producing-run ref (Model::run() ==
    // this run's id), the durable edge — the executor's logical model output ref
    // is just a placeholder handle in R1.
    if (!models.empty()) {
        ImGui::SeparatorText("Trained models");
        for (const std::shared_ptr<Model>& model : models) {
            ImGui::PushID(model->id().c_str());
            render_model_card(*model);
            ImGui::PopID();
        }
    }

    // Lineage — where the results came from (sources, steps), how fresh, and how
    // confident (a 0..1 score, shown as a bar).
    ImGui::SeparatorText("Lineage");
    const RunLineage& lineage = run.lineage();
    const bool has_lineage = !lineage.sources.empty() || !lineage.steps.empty() ||
                             !lineage.freshness.empty() || lineage.confidence != 0.0;
    if (!has_lineage) {
        ImGui::TextDisabled("(no lineage)");
        return;
    }
    ImGui::TextDisabled("sources:");
    ImGui::SameLine();
    ImGui::TextWrapped("%s", lineage.sources.empty()
                                 ? "—"
                                 : join_refs(lineage.sources).c_str());
    ImGui::TextDisabled("steps:");
    ImGui::SameLine();
    ImGui::TextWrapped(
        "%s", lineage.steps.empty() ? "—" : join_refs(lineage.steps).c_str());
    ImGui::TextDisabled("freshness: %s",
                        lineage.freshness.empty() ? "—" : lineage.freshness.c_str());
    ImGui::TextDisabled("confidence:");
    ImGui::SameLine();
    ImGui::ProgressBar(static_cast<float>(lineage.confidence), ImVec2(-FLT_MIN, 0));
}

// Render one access key: its short id + created stamp, the resources its **scope**
// reaches (deny-by-default), and the rights its **capabilities** grant (each TTL'd;
// an expired grant is flagged red). A pure view over the already-validated key —
// `now` is the current ISO-8601 stamp the capability TTLs are compared against.
void render_access_key(const AccessKey& key, const std::string& now) {
    const std::string short_id = key.id().substr(0, key.id().find('-'));
    ImGui::SeparatorText(("key " + short_id).c_str());
    ImGui::TextDisabled("created: %s", key.created().c_str());

    // Scope — which resources this key may touch (the resource axis, RULE-3).
    const Scope& scope = key.scope();
    if (scope.unrestricted) {
        ImGui::TextColored(kWarnColor, "scope: unrestricted (admin)");
    } else if (scope.grants.empty()) {
        ImGui::TextDisabled("scope: (no resources — denies every resource)");
    } else {
        ImGui::TextDisabled("scope:");
        ImGui::Indent();
        for (const Resource& resource : scope.grants) {
            ImGui::BulletText("%s: %s",
                              std::string(to_string(resource.kind)).c_str(),
                              resource.name.c_str());
        }
        ImGui::Unindent();
    }

    // Capabilities — which rights this key may exercise (the verb axis, ENT-22),
    // optionally TTL'd. No wildcard: a key with none denies every operation.
    const CapabilitySet& capabilities = key.capabilities();
    if (capabilities.grants.empty()) {
        ImGui::TextDisabled("capabilities: (none — denies every operation)");
    } else {
        ImGui::TextDisabled("capabilities:");
        ImGui::Indent();
        for (const Capability& capability : capabilities.grants) {
            const std::string right =
                std::string(to_string(capability.domain)) + ":" + capability.action;
            if (capability.expires_at.empty()) {
                ImGui::BulletText("%s", right.c_str());
            } else if (capability.expired(now)) {
                ImGui::Bullet();
                ImGui::SameLine();
                ImGui::TextColored(kFailColor, "%s (expired %s)", right.c_str(),
                                   capability.expires_at.c_str());
            } else {
                ImGui::BulletText("%s (expires %s)", right.c_str(),
                                  capability.expires_at.c_str());
            }
        }
        ImGui::Unindent();
    }
}

// Render the full detail of one principal: identity + its access keys (each with
// its scope + capability grants). A pure view — no mutation.
void render_principal_detail(const Principal& principal) {
    ImGui::Text("%s", principal.name().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("[%s]",
                        std::string(to_string(principal.kind())).c_str());
    ImGui::TextDisabled("id: %s", principal.id().c_str());
    ImGui::TextDisabled("created: %s", principal.created().c_str());

    const std::vector<std::shared_ptr<AccessKey>>& keys = principal.keys();
    ImGui::Spacing();
    ImGui::TextDisabled("%zu access key(s)", keys.size());
    if (keys.empty()) {
        ImGui::TextDisabled("(no keys — this principal can authenticate nothing)");
        return;
    }
    const std::string now = now_iso8601();
    for (const std::shared_ptr<AccessKey>& key : keys) {
        ImGui::PushID(key->id().c_str());
        render_access_key(*key, now);
        ImGui::PopID();
    }
}

// Render the immutable audit trail as a scrolling table: each governed action
// (graph change / deploy / data access) with its time, actor, target, the
// allow/deny outcome (color-coded), and the human-readable detail. Chronological,
// matching Node::audit_events(); a pure view over already-validated events.
void render_audit_log(const std::vector<std::shared_ptr<AuditEvent>>& events) {
    ImGui::TextDisabled("%zu event(s)", events.size());
    if (events.empty()) {
        ImGui::TextDisabled("(no audit events)");
        return;
    }
    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
    const float table_h = ImGui::GetTextLineHeightWithSpacing() * 12.0f;
    if (ImGui::BeginTable("##audit_log", 6, table_flags, ImVec2(0, table_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);  // keep the header row pinned
        ImGui::TableSetupColumn("Time");
        ImGui::TableSetupColumn("Action");
        ImGui::TableSetupColumn("Actor");
        ImGui::TableSetupColumn("Target");
        ImGui::TableSetupColumn("Outcome");
        ImGui::TableSetupColumn("Detail");
        ImGui::TableHeadersRow();
        for (const std::shared_ptr<AuditEvent>& event : events) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(event->time().c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(
                std::string(to_string(event->action())).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(
                event->actor().empty() ? "—" : event->actor().c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(
                event->target().empty() ? "—" : event->target().c_str());
            ImGui::TableSetColumnIndex(4);
            if (event->allowed()) {
                ImGui::TextColored(kSuccessColor, "allow");
            } else {
                ImGui::TextColored(kErrorColor, "deny");
            }
            ImGui::TableSetColumnIndex(5);
            ImGui::TextWrapped(
                "%s", event->detail().empty() ? "—" : event->detail().c_str());
        }
        ImGui::EndTable();
    }
}

}  // namespace

App::App(Node& node, Cluster& cluster)
    : node_(node),
      cluster_(cluster),
      query_(std::make_unique<query::QueryService>(node, &cluster)),
      gate_graph_input_(kExampleGraphJson) {}

App::~App() {
    if (initialized_) {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    if (initialized_) SDL_Quit();
}

bool App::init() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    // Open at 70% of the primary display's usable area (excluding the macOS
    // menu bar + dock), with floors so the layout still has room on a small
    // display.
    int win_w = 1280, win_h = 800;
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) &&
        usable.w > 0 && usable.h > 0) {
        win_w = static_cast<int>(usable.w * 0.70f);
        win_h = static_cast<int>(usable.h * 0.70f);
        if (win_w < 900) win_w = 900;
        if (win_h < 600) win_h = 600;
    }

    const std::string title = std::string{"zetta "} + kVersion;
    window_ = SDL_CreateWindow(
        title.c_str(), win_w, win_h,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window_ == nullptr) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }

    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (renderer_ == nullptr) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        SDL_Quit();
        return false;
    }
    SDL_SetRenderVSync(renderer_, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();

    ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
    ImGui_ImplSDLRenderer3_Init(renderer_);

    base_style_ = std::make_unique<ImGuiStyle>(ImGui::GetStyle());
    initialized_ = true;
    apply_display_scale();
    return true;
}

// DPI / per-monitor UI scaling.
//
// Two distinct things conflate here, and SDL3 exposes them separately:
//   1) Pixel density (Retina, HiDPI) — already handled by the
//      imgui_impl_sdlrenderer3 backend, which reads io.DisplayFramebufferScale
//      and calls SDL_SetRenderScale so logical-pixel drawing renders crisply
//      at native pixels. Don't multiply this in again.
//   2) Content scale — the OS "make the UI ~Nx bigger on this display" hint
//      (macOS scaled modes, Windows DPI scaling, GNOME text-scale). ImGui will
//      NOT apply it; that's on us.
//
// SDL_GetWindowDisplayScale = pixel_density * content_scale, so using it
// directly double-scales on Retina. We read SDL_GetDisplayContentScale for the
// content factor only and re-rasterize the font atlas at framebuffer-pixel
// density so text stays sharp when the display changes.
void App::apply_display_scale() {
    const float content = ui_content_scale(window_);
    const float density = ui_pixel_density(window_);
    if (content == applied_content_scale_ &&
        density == applied_pixel_density_) return;
    applied_content_scale_ = content;
    applied_pixel_density_ = density;

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig cfg;
    cfg.SizePixels = kBaseFontPixels * content;
    cfg.RasterizerDensity = density;
    cfg.FontDataOwnedByAtlas = false;  // bytes live in the embedded header
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    io.Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(kDroidSans),
        static_cast<int>(kDroidSans_len),
        cfg.SizePixels, &cfg);
    io.Fonts->Build();
    ImGui_ImplSDLRenderer3_DestroyFontsTexture();
    ImGui_ImplSDLRenderer3_CreateFontsTexture();

    ImGuiStyle& style = ImGui::GetStyle();
    style = *base_style_;
    style.ScaleAllSizes(content);
}

void App::draw_menu_bar(bool& running) {
    if (!ImGui::BeginMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Project...")) {
            new_project_modal_requested_ = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Cmd+Q")) running = false;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("ImGui demo window", nullptr, &show_demo_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("About zetta");
        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();
}

void App::draw_dashboard() {
    ImGui::Text("Node: %s", node_.status_line().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("|  data dir: %s", node_.config().data_dir.c_str());
    if (!io_status_.empty()) {
        ImGui::TextDisabled("%s", io_status_.c_str());
    }
    ImGui::Spacing();
}

void App::draw_explorer() {
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float col_w =
        (ImGui::GetContentRegionAvail().x - 4.0f * spacing) / 5.0f;

    ImGui::BeginChild("##projects", ImVec2(col_w, 0), ImGuiChildFlags_Borders);
    draw_projects();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##databases", ImVec2(col_w, 0), ImGuiChildFlags_Borders);
    draw_databases();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##tables", ImVec2(col_w, 0), ImGuiChildFlags_Borders);
    draw_tables();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##rows", ImVec2(col_w, 0), ImGuiChildFlags_Borders);
    draw_rows();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##largeobjects", ImVec2(0, 0), ImGuiChildFlags_Borders);
    draw_large_objects();
    ImGui::EndChild();
}

void App::draw_projects() {
    ImGui::SeparatorText("Projects");

    // The full create flow — name + NL intent + n8n artifact import — lives in
    // the New Project modal (SCR-2); this button is its front door. Rename /
    // select / delete for existing projects stay inline below.
    if (ImGui::Button("New Project...")) {
        new_project_modal_requested_ = true;
    }
    if (!project_error_.empty()) {
        ImGui::TextColored(kErrorColor, "%s", project_error_.c_str());
    }

    ImGui::Separator();

    for (const std::string& name : node_.project_names()) {
        ImGui::PushID(name.c_str());
        if (ImGui::SmallButton("x")) {
            node_.drop_project(name);
            if (selected_project_ == name) selected_project_.clear();
        }
        ImGui::SameLine();
        if (ImGui::Selectable(name.c_str(), selected_project_ == name)) {
            selected_project_ = name;
        }
        ImGui::PopID();
    }

    // Details + rename for the selected project. Re-resolved by name each frame
    // (like the database selection) so a dropped/renamed project can't leave a
    // dangling handle.
    const auto project = node_.find_project(selected_project_);
    if (!project) return;

    ImGui::Separator();
    // First UUID segment — enough to identify the project at a glance.
    const std::string short_id =
        project->id().substr(0, project->id().find('-'));
    ImGui::TextDisabled("id: %s", short_id.c_str());
    ImGui::TextDisabled("owner: %s",
                        project->owner().empty() ? "—" : project->owner().c_str());
    ImGui::TextDisabled("created: %s", project->created().c_str());
    if (project->updated() != project->created()) {
        ImGui::TextDisabled("updated: %s", project->updated().c_str());
    }

    ImGui::Separator();
    ImGui::TextDisabled("Rename '%s' to:", project->name().c_str());
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool rename = ImGui::InputTextWithHint(
        "##renproject", "new name", &rename_project_input_,
        ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::Button("Rename") || rename) {
        if (rename_project_input_.empty()) {
            project_error_ = "name required";
        } else if (!node_.rename_project(selected_project_,
                                         rename_project_input_)) {
            project_error_ = "rename failed (name taken?)";
        } else {
            selected_project_ = rename_project_input_;
            rename_project_input_.clear();
            project_error_.clear();
        }
    }
}

void App::draw_databases() {
    ImGui::SeparatorText("Databases");

    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool submit = ImGui::InputTextWithHint(
        "##newdb", "new database name", &new_db_input_,
        ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::Button("Create database") || submit) {
        if (new_db_input_.empty()) {
            db_error_ = "name required";
        } else if (!node_.create_database(new_db_input_)) {
            db_error_ = "database already exists";
        } else {
            selected_db_ = new_db_input_;
            selected_table_.clear();
            new_db_input_.clear();
            db_error_.clear();
        }
    }
    if (!db_error_.empty()) {
        ImGui::TextColored(kErrorColor, "%s", db_error_.c_str());
    }

    ImGui::Separator();

    for (const std::string& name : node_.database_names()) {
        ImGui::PushID(name.c_str());
        if (ImGui::SmallButton("x")) {
            node_.drop_database(name);
            if (selected_db_ == name) {
                selected_db_.clear();
                selected_table_.clear();
            }
        }
        ImGui::SameLine();
        if (ImGui::Selectable(name.c_str(), selected_db_ == name)) {
            selected_db_ = name;
            selected_table_.clear();
        }
        ImGui::PopID();
    }

    // The virtual, read-only `system` database (computed system tables like
    // system.zetta_nodes). It isn't a real on-disk database, so it's listed
    // apart and can't be renamed/dropped.
    ImGui::Separator();
    if (ImGui::Selectable("system  (virtual)", selected_db_ == "system")) {
        selected_db_ = "system";
        selected_table_.clear();
    }

    // Rename (the "U" in CRUD) the selected database.
    if (const auto db = node_.find_database(selected_db_)) {
        ImGui::Separator();
        ImGui::TextDisabled("Rename '%s' to:", db->name().c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool rename = ImGui::InputTextWithHint(
            "##rendb", "new name", &rename_db_input_,
            ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("Rename") || rename) {
            if (rename_db_input_.empty()) {
                db_error_ = "name required";
            } else if (!node_.rename_database(selected_db_, rename_db_input_)) {
                db_error_ = "rename failed (name taken?)";
            } else {
                selected_db_ = rename_db_input_;
                rename_db_input_.clear();
                db_error_.clear();
            }
        }
    }
}

void App::draw_tables() {
    ImGui::SeparatorText("Tables");

    if (selected_db_ == "system") {
        ImGui::TextDisabled("in 'system' (read-only)");
        ImGui::Separator();
        for (const char* table_name : {"zetta_nodes"}) {
            if (ImGui::Selectable(table_name, selected_table_ == table_name)) {
                selected_table_ = table_name;
            }
        }
        return;
    }

    const auto db = node_.find_database(selected_db_);
    if (!db) {
        ImGui::TextDisabled("Select a database.");
        return;
    }

    ImGui::TextDisabled("in '%s'", db->name().c_str());

    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool submit = ImGui::InputTextWithHint(
        "##newtable", "new table name", &new_table_input_,
        ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::Button("Create table") || submit) {
        if (new_table_input_.empty()) {
            table_error_ = "name required";
        } else if (!db->create_table(new_table_input_)) {
            table_error_ = "table already exists";
        } else {
            selected_table_ = new_table_input_;
            new_table_input_.clear();
            table_error_.clear();
        }
    }
    if (!table_error_.empty()) {
        ImGui::TextColored(kErrorColor, "%s", table_error_.c_str());
    }

    ImGui::Separator();

    for (const std::string& name : db->table_names()) {
        ImGui::PushID(name.c_str());
        if (ImGui::SmallButton("x")) {
            db->drop_table(name);
            if (selected_table_ == name) selected_table_.clear();
        }
        ImGui::SameLine();
        const auto table = db->find_table(name);
        const std::size_t rows = table ? table->row_count() : 0;
        const std::string label = name + "  (" + std::to_string(rows) + ")";
        if (ImGui::Selectable(label.c_str(), selected_table_ == name)) {
            selected_table_ = name;
        }
        ImGui::PopID();
    }
}

void App::draw_rows() {
    ImGui::SeparatorText("Rows");

    // System tables are computed + read-only: query them and show the result.
    if (selected_db_ == "system") {
        if (selected_table_.empty()) {
            ImGui::TextDisabled("Select a system table.");
            return;
        }
        const query::QueryService::Outcome outcome = query_->run(
            "SELECT * FROM system." + selected_table_ + ";", selected_db_);
        if (!outcome.ok()) {
            ImGui::TextColored(kErrorColor, "%s", outcome.error.c_str());
            return;
        }
        ImGui::TextDisabled("system.%s  (%zu rows)", selected_table_.c_str(),
                            outcome.rows.size());
        for (std::size_t i = 0; i < outcome.rows.size(); ++i) {
            ImGui::TextWrapped("%zu  %s", i, outcome.rows[i].dump().c_str());
        }
        return;
    }

    const auto db = node_.find_database(selected_db_);
    if (!db) {
        ImGui::TextDisabled("Select a database.");
        return;
    }
    const auto table = db->find_table(selected_table_);
    if (!table) {
        ImGui::TextDisabled("Select or create a table.");
        return;
    }

    ImGui::TextDisabled("%s.%s  (%zu rows)", db->name().c_str(),
                        table->name().c_str(), table->row_count());

    if (ImGui::Button("Import JSON/JSONL...")) {
        static const SDL_DialogFileFilter filters[] = {
            {"JSON / JSON Lines", "json;jsonl"}, {"All files", "*"}};
        SDL_ShowOpenFileDialog(&App::on_open_records, this, window_, filters, 2,
                               nullptr, false);
    }

    ImGui::TextUnformatted("Add a JSON object:");
    ImGui::InputTextMultiline("##doc", &doc_input_,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 5));
    if (ImGui::Button("Add object")) {
        // Parse without exceptions — a malformed doc yields a discarded value
        // we report inline rather than throwing across the UI loop.
        auto doc = nlohmann::json::parse(doc_input_, nullptr,
                                         /*allow_exceptions=*/false);
        if (doc.is_discarded()) {
            row_error_ = "invalid JSON — not added";
        } else {
            table->add_row(std::move(doc));
            row_error_.clear();
        }
    }
    if (!row_error_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kErrorColor, "%s", row_error_.c_str());
    }

    ImGui::Spacing();
    const StorageEngine& store = table->store();
    if (store.size() == 0) {
        ImGui::TextDisabled("(empty)");
    }
    // Cap how many rows we render — a persisted table can hold far more than is
    // useful to draw every frame.
    constexpr std::size_t kMaxShown = 500;
    const std::size_t shown = std::min(store.size(), kMaxShown);
    for (std::size_t i = 0; i < shown; ++i) {
        ImGui::TextWrapped("%zu  %s", i, store.at(i).dump().c_str());
    }
    if (store.size() > shown) {
        ImGui::TextDisabled("... and %zu more", store.size() - shown);
    }
}

void App::draw_query() {
    if (!ImGui::CollapsingHeader("Query (postgres)",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    ImGui::TextDisabled(
        "Unqualified names use the selected database (%s); use db.table to "
        "target another.",
        selected_db_.empty() ? "none" : selected_db_.c_str());

    ImGui::InputTextMultiline("##query", &query_input_,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 6));

    if (ImGui::Button("Run")) {
        const query::QueryService::Outcome outcome =
            query_->run(query_input_, selected_db_);
        query_status_.clear();
        query_error_.clear();
        query_result_columns_.clear();
        query_result_rows_.clear();
        query_time_ = format_duration(outcome.elapsed);
        if (!outcome.ok()) {
            query_error_ = outcome.error;
        } else {
            for (std::size_t i = 0; i < outcome.messages.size(); ++i) {
                if (i) query_status_ += "   ";
                query_status_ += outcome.messages[i];
            }
            query_result_columns_ = outcome.columns;
            for (const nlohmann::json& row : outcome.rows) {
                query_result_rows_.push_back(row.dump());
            }
        }
    }

    if (!query_error_.empty()) {
        ImGui::TextColored(kErrorColor, "%s", query_error_.c_str());
    } else if (!query_status_.empty()) {
        ImGui::TextDisabled("%s", query_status_.c_str());
    }
    if (!query_time_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", query_time_.c_str());
    }

    if (ImGui::BeginChild("##query_results", ImVec2(0, 160),
                          ImGuiChildFlags_Borders)) {
        if (!query_result_columns_.empty()) {
            std::string cols;
            for (std::size_t i = 0; i < query_result_columns_.size(); ++i) {
                if (i) cols += ", ";
                cols += query_result_columns_[i];
            }
            ImGui::TextDisabled("columns: %s", cols.c_str());
        }
        constexpr std::size_t kMaxShown = 500;
        const std::size_t shown = std::min(query_result_rows_.size(), kMaxShown);
        for (std::size_t i = 0; i < shown; ++i) {
            ImGui::TextWrapped("%s", query_result_rows_[i].c_str());
        }
        if (query_result_rows_.size() > shown) {
            ImGui::TextDisabled("... and %zu more",
                                query_result_rows_.size() - shown);
        }
    }
    ImGui::EndChild();
}

void App::draw_gate() {
    if (!ImGui::CollapsingHeader("Correctness gate",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    ImGui::TextDisabled(
        "Validate an execution graph before deploy. RULE-1: no deploy without a "
        "passing gate.");

    // Graph source. Fill a preset, load a stored graph from the selected
    // project, or edit the IR JSON directly below. ir::from_json validates the
    // document at the boundary, so a malformed graph is rejected with a reason.
    if (ImGui::SmallButton("Passing example")) {
        gate_graph_input_ = kExampleGraphJson;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Empty (fails)")) {
        gate_graph_input_ = kEmptyGraphJson;
    }

    // Load a graph from the selected project's stored graphs (the real
    // ProjectGraph data path), when the project has any.
    if (const auto project = node_.find_project(selected_project_)) {
        const std::vector<std::string> graph_names = project->graph_names();
        if (!graph_names.empty()) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
            if (ImGui::BeginCombo("##loadgraph", "Load from project…")) {
                for (const std::string& name : graph_names) {
                    if (ImGui::Selectable(name.c_str())) {
                        if (const auto graph = project->find_graph(name)) {
                            gate_graph_input_ =
                                ir::to_json(graph->graph()).dump(2);
                        }
                    }
                }
                ImGui::EndCombo();
            }
        }
    }

    ImGui::InputTextMultiline("##gategraph", &gate_graph_input_,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 8));

    if (ImGui::Button("Run gate")) {
        gate_error_.clear();
        gate_report_.reset();
        // Parse + validate at the boundary without exceptions: a malformed
        // document yields a reason we show inline, never a thrown stack trace.
        auto document = nlohmann::json::parse(gate_graph_input_, nullptr,
                                              /*allow_exceptions=*/false);
        if (document.is_discarded()) {
            gate_error_ = "invalid JSON — could not parse the graph";
        } else {
            std::string parse_error;
            std::optional<ir::ExecutionGraph> graph =
                ir::from_json(document, &parse_error);
            if (!graph) {
                gate_error_ = "invalid graph: " + parse_error;
            } else {
                gate_report_ = std::make_unique<ValidationReport>(
                    default_gate().run(*graph));
            }
        }
    }

    if (!gate_error_.empty()) {
        ImGui::TextColored(kErrorColor, "%s", gate_error_.c_str());
    }

    if (!gate_report_) return;
    const ValidationReport& report = *gate_report_;

    // The block-on-fail banner is the headline state this panel exists to show.
    ImGui::Spacing();
    if (report.block_deploy) {
        std::size_t failures = 0;
        for (const CheckResult& check : report.checks) {
            if (check.status == CheckStatus::Fail) ++failures;
        }
        ImGui::TextColored(kFailColor,
                           "Deploy blocked — %zu check(s) failed (verdict: %s)",
                           failures, to_string(report.verdict()).c_str());
    } else {
        ImGui::TextColored(kPassColor,
                           "Deploy allowed — gate passed (verdict: %s)",
                           to_string(report.verdict()).c_str());
    }

    // Per-check results: the check, its pass/warn/fail status, and the
    // human-readable reason (never a stack trace).
    const ImGuiTableFlags table_flags = ImGuiTableFlags_Borders |
                                        ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("gate_checks", 3, table_flags)) {
        ImGui::TableSetupColumn("Check", ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch,
                                0.15f);
        ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthStretch,
                                0.55f);
        ImGui::TableHeadersRow();
        for (const CheckResult& check : report.checks) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(check.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(status_color(check.status), "%s",
                               status_badge(check.status).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextWrapped("%s", check.reason.c_str());
        }
        ImGui::EndTable();
    }
    if (report.checks.empty()) {
        ImGui::TextDisabled("(the gate ran no checks)");
    }
}

void App::draw_large_objects() {
    ImGui::SeparatorText("Large Objects");
    const auto db = node_.find_database(selected_db_);
    if (!db) {
        ImGui::TextDisabled("Select a database.");
        return;
    }
    if (ImGui::Button("Store file...")) {
        SDL_ShowOpenFileDialog(&App::on_open_large_object, this, window_,
                               nullptr, 0, nullptr, false);
    }
    ImGui::TextDisabled("or drop any file on the window");
    ImGui::Separator();

    const std::vector<LargeObject> objects = db->large_objects().list();
    if (objects.empty()) ImGui::TextDisabled("(none)");
    for (const LargeObject& lo : objects) {
        ImGui::PushID(lo.id.c_str());
        if (ImGui::SmallButton("x")) db->large_objects().unlink(lo.id);
        ImGui::SameLine();
        ImGui::TextWrapped("%s  (%s)", lo.name.c_str(),
                           format_bytes(lo.size).c_str());
        // The metadata replicates immediately; the body transfers separately,
        // so a freshly-replicated object may not have its bytes here yet.
        if (!lo.body_present) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "(syncing body)");
        }
        ImGui::PopID();
    }
}

void App::enqueue_file(std::string path, FileIntent intent) {
    const std::lock_guard<std::mutex> lock(incoming_mutex_);
    incoming_files_.push_back({std::move(path), intent});
}

void App::on_open_records(void* userdata, const char* const* files, int) {
    if (files && files[0]) {
        static_cast<App*>(userdata)->enqueue_file(files[0],
                                                  FileIntent::Records);
    }
}

void App::on_open_large_object(void* userdata, const char* const* files, int) {
    if (files && files[0]) {
        static_cast<App*>(userdata)->enqueue_file(files[0],
                                                  FileIntent::LargeObject);
    }
}

void App::on_open_n8n(void* userdata, const char* const* files, int) {
    if (files && files[0]) {
        static_cast<App*>(userdata)->enqueue_file(files[0],
                                                  FileIntent::N8nArtifact);
    }
}

void App::handle_incoming_files() {
    std::vector<IncomingFile> files;
    {
        const std::lock_guard<std::mutex> lock(incoming_mutex_);
        if (incoming_files_.empty()) return;
        files.swap(incoming_files_);
    }
    for (const IncomingFile& f : files) {
        const std::filesystem::path path = f.path;
        FileIntent intent = f.intent;
        // An n8n artifact is staged for the New Project dialog, not ingested:
        // validate it and remember the path so Create can import it into the
        // new project (see stage_n8n_artifact / draw_new_project_modal).
        if (intent == FileIntent::N8nArtifact) {
            stage_n8n_artifact(f.path);
            continue;
        }
        if (intent == FileIntent::Auto) {
            std::string ext = path.extension().string();
            for (char& c : ext) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            intent = (ext == ".json" || ext == ".jsonl") ? FileIntent::Records
                                                         : FileIntent::LargeObject;
        }
        if (intent == FileIntent::Records) {
            auto db = node_.find_database(selected_db_);
            auto table = db ? db->find_table(selected_table_) : nullptr;
            if (!table) {
                io_status_ = "Import: select a table first (" +
                             path.filename().string() + ")";
                continue;
            }
            const ImportResult r = import_records(*table, path);
            io_status_ = r.ok() ? "Imported " + std::to_string(r.added) +
                                      " record(s) into " + table->name()
                                : "Import failed: " + r.error;
        } else {
            auto db = node_.find_database(selected_db_);
            if (!db) {
                io_status_ = "Store: select a database first (" +
                             path.filename().string() + ")";
                continue;
            }
            const auto lo = db->large_objects().import_file(path);
            io_status_ = lo ? "Stored " + lo->name + " (" +
                                  format_bytes(lo->size) + ") as a large object"
                            : "Store failed: " + path.string();
        }
    }
}

void App::stage_n8n_artifact(const std::string& path) {
    const std::filesystem::path file = path;
    new_project_artifact_path_ = path;
    new_project_artifact_name_ = file.filename().string();

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        new_project_artifact_valid_ = false;
        new_project_artifact_status_ = "cannot open file";
        return;
    }
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());

    // Validate against the n8n workflow schema at the staging boundary so the
    // user gets fail-fast feedback here; import_n8n_workflow_file re-validates at
    // the real storage boundary on Create. (No new format is introduced — this
    // reuses the existing n8n_workflow_schema.)
    std::string error;
    if (!validate_n8n_workflow(content, &error)) {
        new_project_artifact_valid_ = false;
        new_project_artifact_status_ = error;
        return;
    }
    new_project_artifact_valid_ = true;
    const auto document =
        nlohmann::json::parse(content, nullptr, /*allow_exceptions=*/false);
    const std::size_t node_count =
        (!document.is_discarded() && document.contains("nodes") &&
         document["nodes"].is_array())
            ? document["nodes"].size()
            : 0;
    new_project_artifact_status_ =
        "valid n8n workflow — " + std::to_string(node_count) + " node(s)";
}

void App::reset_new_project_form() {
    new_project_name_.clear();
    new_project_intent_.clear();
    new_project_artifact_path_.clear();
    new_project_artifact_name_.clear();
    new_project_artifact_status_.clear();
    new_project_artifact_valid_ = false;
    new_project_error_.clear();
}

void App::draw_new_project_modal() {
    if (new_project_modal_requested_) {
        ImGui::OpenPopup("New Project");
        new_project_modal_requested_ = false;
        new_project_error_.clear();
    }

    // Centre the dialog on first appearance; let it auto-size to its content.
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 32.0f, 0.0f),
                             ImGuiCond_Appearing);

    bool stay_open = true;  // the window's close (x) button toggles this
    if (!ImGui::BeginPopupModal("New Project", &stay_open,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        new_project_modal_open_ = false;
        return;
    }
    // Mirror the open state so run() routes file drops to the n8n staging area
    // while this dialog is up (see the drop handling in run()).
    new_project_modal_open_ = true;

    ImGui::TextDisabled(
        "Name the project, describe what it should do, and optionally import an "
        "n8n workflow as a starting artifact.");
    ImGui::Spacing();

    // 1) Name (required).
    ImGui::TextUnformatted("Name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##np_name", "e.g. churn-risk", &new_project_name_);

    // 2) Natural-language intent (optional — captured via Project::capture_intent,
    //    which itself rejects empty/whitespace-only text).
    ImGui::Spacing();
    ImGui::TextUnformatted("Intent");
    ImGui::InputTextMultiline("##np_intent", &new_project_intent_,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4.0f));
    ImGui::TextDisabled(
        "Optional. e.g. \"Score every active account for churn risk and tell me "
        "why.\"");

    // 3) n8n workflow artifact (optional). Import via the native dialog or by
    //    dropping a .json on this dialog; staged + validated, imported on Create.
    ImGui::Spacing();
    ImGui::TextUnformatted("n8n workflow (artifact)");
    if (ImGui::Button("Import n8n workflow...")) {
        static const SDL_DialogFileFilter filters[] = {
            {"n8n workflow (JSON)", "json"}, {"All files", "*"}};
        SDL_ShowOpenFileDialog(&App::on_open_n8n, this, window_, filters, 2,
                               nullptr, false);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("or drop a workflow .json on this dialog");

    ImGui::BeginChild("##np_dropzone",
                      ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * 2.4f),
                      ImGuiChildFlags_Borders);
    if (new_project_artifact_path_.empty()) {
        ImGui::TextDisabled("(no workflow staged)");
        ImGui::TextDisabled("Drop an n8n workflow JSON here, or use the button.");
    } else {
        ImGui::TextUnformatted(new_project_artifact_name_.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("clear")) {
            new_project_artifact_path_.clear();
            new_project_artifact_name_.clear();
            new_project_artifact_status_.clear();
            new_project_artifact_valid_ = false;
        }
        if (!new_project_artifact_status_.empty()) {
            ImGui::TextColored(
                new_project_artifact_valid_ ? kSuccessColor : kErrorColor, "%s",
                new_project_artifact_status_.c_str());
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::Separator();
    if (!new_project_error_.empty()) {
        ImGui::TextColored(kErrorColor, "%s", new_project_error_.c_str());
    }

    // Create runs the whole flow atomically. A staged-but-invalid artifact
    // blocks the create so the project is never built with a broken artifact.
    const bool artifact_blocks =
        !new_project_artifact_path_.empty() && !new_project_artifact_valid_;
    if (ImGui::Button("Create Project")) {
        if (new_project_name_.empty()) {
            new_project_error_ = "name required";
        } else if (artifact_blocks) {
            new_project_error_ =
                "the staged n8n file is not a valid workflow — fix or clear it";
        } else if (auto project = node_.create_project(new_project_name_)) {
            std::string summary = "Created project '" + project->name() + "'";
            if (project->capture_intent(new_project_intent_)) {
                summary += " + intent";
            }
            if (!new_project_artifact_path_.empty()) {
                const N8nImportResult imported = import_n8n_workflow_file(
                    *project, new_project_artifact_path_);
                summary += imported.ok()
                               ? " + n8n artifact (" + new_project_artifact_name_ +
                                     ")"
                               : " (artifact import failed: " + imported.error +
                                     ")";
            }
            selected_project_ = project->name();
            io_status_ = summary;
            reset_new_project_form();
            ImGui::CloseCurrentPopup();
        } else {
            new_project_error_ = "a project by that name already exists";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        reset_new_project_form();
        ImGui::CloseCurrentPopup();
    }

    if (!stay_open) {  // window close (x)
        reset_new_project_form();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void App::draw_deploy() {
    if (!ImGui::CollapsingHeader("Deploy", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    ImGui::TextDisabled(
        "Pick an AI solution, choose where to run it, and deploy. The "
        "correctness gate must pass first — no deploy without it.");

    const SolutionCatalog& catalog = SolutionCatalog::builtin();

    // 1) Solution picker — the AI Solution Library blueprints (the GUI's source
    //    of deployable graphs until a graph editor lands). Selection is by id.
    const Solution* selected = catalog.find(selected_solution_);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##solution",
                          selected ? selected->name.c_str()
                                   : "Select an AI solution...")) {
        for (const Solution& solution : catalog.solutions()) {
            const bool is_selected = solution.id == selected_solution_;
            if (ImGui::Selectable(solution.name.c_str(), is_selected)) {
                selected_solution_ = solution.id;
                deploy_status_.clear();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s — %s", solution.category.c_str(),
                                  solution.summary.c_str());
            }
            if (is_selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    selected = catalog.find(selected_solution_);
    if (selected) {
        ImGui::TextDisabled("%s · %zu steps · %zu edges",
                            selected->category.c_str(),
                            selected->graph.node_count(),
                            selected->graph.edge_count());
    }

    // 2) Placement dial — the local→kubernetes→vpc→cloud progression. R1 ships
    //    Local only; the rest are shown disabled and labelled "(planned)" so the
    //    dial communicates the roadmap without being choosable. The combo index
    //    mirrors DeploymentKind's declaration order; deploy still guards on Local.
    static constexpr std::array<DeploymentKind, 4> kDial{
        DeploymentKind::Local, DeploymentKind::Kubernetes, DeploymentKind::Vpc,
        DeploymentKind::Cloud};
    const auto dial_label = [](DeploymentKind kind) -> std::string {
        std::string label = to_string(kind);
        if (!label.empty()) label[0] = static_cast<char>(std::toupper(label[0]));
        return kind == DeploymentKind::Local ? label : label + " (planned)";
    };
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##placement",
                          dial_label(kDial[deploy_placement_]).c_str())) {
        for (int index = 0; index < static_cast<int>(kDial.size()); ++index) {
            const bool functional = kDial[index] == DeploymentKind::Local;
            ImGui::BeginDisabled(!functional);
            const bool is_selected = index == deploy_placement_;
            if (ImGui::Selectable(dial_label(kDial[index]).c_str(),
                                  is_selected)) {
                deploy_placement_ = index;
                deploy_status_.clear();
            }
            ImGui::EndDisabled();
            if (is_selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // 3) One-button deploy: run the correctness gate (RULE-1 — no deploy without
    //    a passing gate), then place the blueprint onto the chosen target.
    ImGui::BeginDisabled(selected == nullptr);
    if (ImGui::Button("Deploy")) {
        const ir::ExecutionGraph& graph = selected->graph;
        const ValidationReport report = default_gate().run(graph);
        if (report.block_deploy) {
            std::string reason;
            for (const CheckResult& check : report.checks) {
                if (check.status == CheckStatus::Fail) {
                    reason = check.name + ": " + check.reason;
                    break;
                }
            }
            deploy_failed_ = true;
            deploy_status_ = "Gate blocked deploy — " +
                             (reason.empty() ? "graph failed validation" : reason);
        } else {
            const DeploymentTarget target{kDial[deploy_placement_], {}, {}};
            const std::shared_ptr<Deployment> deployment =
                node_.deploy_graph(graph, target, selected->name);
            if (deployment) {
                deploy_failed_ = false;
                deploy_status_ = "Deployed \"" + selected->name + "\" to " +
                                 to_string(target.kind) + " (deployment " +
                                 deployment->id().substr(0, 8) + ")";
            } else {
                deploy_failed_ = true;
                deploy_status_ =
                    target.is_local()
                        ? "Deploy failed — the gate passed but placement was "
                          "rejected"
                        : "Deploy failed — only the local target is available "
                          "in R1";
            }
        }
    }
    ImGui::EndDisabled();
    if (!deploy_status_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(deploy_failed_ ? kErrorColor : kSuccessColor, "%s",
                           deploy_status_.c_str());
    }

    // 4) Deployment status — the recorded placements (acceptance: "see its
    //    status"). Sourced from the replicated deployments() list.
    ImGui::SeparatorText("Deployments");
    const std::vector<std::shared_ptr<Deployment>> deployments =
        node_.deployments();
    if (deployments.empty()) {
        ImGui::TextDisabled("(none yet)");
        return;
    }
    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("deployments", 5, table_flags)) {
        ImGui::TableSetupColumn("Graph");
        ImGui::TableSetupColumn("Target");
        ImGui::TableSetupColumn("Steps");
        ImGui::TableSetupColumn("Edges");
        ImGui::TableSetupColumn("Created");
        ImGui::TableHeadersRow();
        for (const std::shared_ptr<Deployment>& deployment : deployments) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(deployment->graph_name().c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(to_string(deployment->target().kind).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%zu", deployment->node_count());
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%zu", deployment->edge_count());
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(deployment->created().c_str());
        }
        ImGui::EndTable();
    }
}

void App::draw_cluster() {
    if (!ImGui::CollapsingHeader("Cluster", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    ImGui::Text("Node: %s", cluster_.node_id().c_str());
    for (const std::string& address : cluster_.listen_addresses()) {
        ImGui::Text("Listening on: %s", address.c_str());
    }
    ImGui::TextDisabled(
        "All nodes are equal — leader election + failover are planned.");

    ImGui::TextUnformatted("Add another node with:");
    std::string command = cluster_.peer_command();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##peer_command", &command, ImGuiInputTextFlags_ReadOnly);
    if (ImGui::Button("Copy command")) {
        ImGui::SetClipboardText(command.c_str());
    }

    ImGui::SeparatorText("Nodes & replication");
    const std::vector<NodeStatus> nodes = cluster_.nodes();
    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("nodes", 6, table_flags)) {
        ImGui::TableSetupColumn("Node");
        ImGui::TableSetupColumn("Address");
        ImGui::TableSetupColumn("Role");
        ImGui::TableSetupColumn("Entries");
        ImGui::TableSetupColumn("Version");
        ImGui::TableSetupColumn("Replication");
        ImGui::TableHeadersRow();
        for (const NodeStatus& node : nodes) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            // First UUID segment — enough to tell nodes apart at a glance.
            const std::string id = node.node_id.substr(0, node.node_id.find('-'));
            ImGui::TextUnformatted(node.is_self ? (id + " (self)").c_str()
                                                : id.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(node.address.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(to_string(node.role).c_str());
            ImGui::TableSetColumnIndex(3);
            if (node.replication.known) {
                ImGui::Text("%zu", node.replication.entries);
            } else {
                ImGui::TextDisabled("—");
            }
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(node.replication.known
                                       ? node.replication.version.c_str()
                                       : "—");
            ImGui::TableSetColumnIndex(5);
            if (node.is_self) {
                ImGui::TextDisabled("—");
            } else if (!node.replication.known) {
                ImGui::TextDisabled("no status yet");
            } else if (node.replication.in_sync) {
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f),
                                   "in sync (%.1fs)",
                                   node.replication.seconds_since_sync);
            } else {
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f),
                                   "syncing (%.1fs)",
                                   node.replication.seconds_since_sync);
            }
        }
        ImGui::EndTable();
    }
}

void App::draw_runs() {
    if (!ImGui::CollapsingHeader("Runs / Outputs / Lineage",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    // A run is the record of one execution of a deployed graph (status / logs /
    // outputs / lineage). This panel is a read-only viewer: runs arrive via the
    // API, tests, or replication from a peer (executing a graph — StartRun — is
    // a later slice). Held by id and re-resolved each frame, like the explorer.
    const std::vector<std::shared_ptr<Run>> runs = node_.runs();
    ImGui::TextDisabled("%zu run(s)", runs.size());

    const float list_w = ImGui::GetContentRegionAvail().x * 0.28f;
    const float panel_h = ImGui::GetTextLineHeightWithSpacing() * 14.0f;

    // Left: the run list. Runs have no human name, so each row is a status badge
    // plus the first segment of the UUID — enough to tell them apart.
    ImGui::BeginChild("##run_list", ImVec2(list_w, panel_h),
                      ImGuiChildFlags_Borders);
    if (runs.empty()) ImGui::TextDisabled("(no runs)");
    for (const std::shared_ptr<Run>& run : runs) {
        ImGui::PushID(run->id().c_str());
        ImGui::TextColored(run_status_color(run->status()), "%-9s",
                           std::string(to_string(run->status())).c_str());
        ImGui::SameLine();
        const std::string short_id = run->id().substr(0, run->id().find('-'));
        if (ImGui::Selectable(short_id.c_str(), selected_run_ == run->id())) {
            selected_run_ = run->id();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    // Right: the selected run's status / logs / outputs / lineage.
    ImGui::SameLine();
    ImGui::BeginChild("##run_detail", ImVec2(0, panel_h),
                      ImGuiChildFlags_Borders);
    if (const auto run = node_.find_run(selected_run_)) {
        // Resolve the trained model(s) this run produced via the durable reverse
        // link (Model::run() == run id), so the detail pane can surface their
        // metrics + ranked drivers alongside the run's outputs.
        std::vector<std::shared_ptr<Model>> run_models;
        for (const std::shared_ptr<Model>& model : node_.models()) {
            if (model->run() == run->id()) run_models.push_back(model);
        }
        render_run_detail(*run, run_models);
    } else {
        ImGui::TextDisabled(
            "Select a run to view its status, logs, outputs, models, and "
            "lineage.");
    }
    ImGui::EndChild();
}

void App::reset_scaling_controls() {
    scaling_objective_ = 0;          // minimize_cost
    scaling_min_nodes_ = 1;
    scaling_max_nodes_ = 8;
    scaling_cost_cap_ = 0.0;
    scaling_target_seconds_ = 600.0;
    scaling_nodes_ = 2;
    scaling_progress_ = 0.4;
    scaling_elapsed_ = 120.0;
    scaling_cost_ = 50.0;
}

void App::draw_job_scaling_controls() {
    // Per-job objective/scaling controls — a LIVE PREVIEW over the built
    // ScalingPolicy::evaluate(). It is a "what-if" calculator, not a write path:
    // ScalingPolicy is not persisted and the autoscaler is unbuilt (both later
    // slices), so this scores an editable policy + sample with the real, pure
    // evaluate() and shows the resulting decision — set the reward objective +
    // bounds, add/remove nodes, watch the recommendation move. Nothing here
    // mutates the job.
    ImGui::SeparatorText("Objective & scaling (preview)");
    ImGui::TextDisabled(
        "Score a reward objective against a sample — no autoscaler is wired yet, "
        "so this previews ScalingPolicy::evaluate(); it doesn't change the job.");

    // 1) Objective — "set the reward". The three reward models the autoscaler can
    //    optimize, in declaration order (the combo index maps to ScalingObjective).
    static constexpr std::array<ScalingObjective, 3> kObjectives{
        ScalingObjective::MinimizeCost, ScalingObjective::RuntimeTarget,
        ScalingObjective::DataShape};
    if (scaling_objective_ < 0 ||
        scaling_objective_ >= static_cast<int>(kObjectives.size())) {
        scaling_objective_ = 0;
    }
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::BeginCombo(
            "Reward objective",
            std::string(to_string(kObjectives[scaling_objective_])).c_str())) {
        for (int index = 0; index < static_cast<int>(kObjectives.size());
             ++index) {
            const bool is_selected = index == scaling_objective_;
            if (ImGui::Selectable(
                    std::string(to_string(kObjectives[index])).c_str(),
                    is_selected)) {
                scaling_objective_ = index;
            }
            if (is_selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    const ScalingObjective objective = kObjectives[scaling_objective_];

    // 2) Bounds — the hard envelope the autoscaler must stay inside (min/max node
    //    count + a cost cap; 0 = unlimited). A runtime target only matters for the
    //    RuntimeTarget objective, so it is shown only then.
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("Min nodes", &scaling_min_nodes_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("Max nodes", &scaling_max_nodes_);
    if (scaling_min_nodes_ < 1) scaling_min_nodes_ = 1;
    if (scaling_max_nodes_ < scaling_min_nodes_) {
        scaling_max_nodes_ = scaling_min_nodes_;
    }
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputDouble("Cost cap (0 = unlimited)", &scaling_cost_cap_, 0.0, 0.0,
                       "%.1f");
    if (scaling_cost_cap_ < 0.0) scaling_cost_cap_ = 0.0;
    if (objective == ScalingObjective::RuntimeTarget) {
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputDouble("Target seconds", &scaling_target_seconds_, 0.0, 0.0,
                           "%.1f");
        if (scaling_target_seconds_ < 0.0) scaling_target_seconds_ = 0.0;
    }

    // 3) Sample — the live signals the autoscaler scores (progress, elapsed,
    //    cost) plus the current allocation. "Add/remove nodes" are the +/- buttons
    //    that move the sample's node count within the bounds.
    ImGui::Spacing();
    ImGui::TextDisabled("Allocated nodes");
    ImGui::SameLine();
    if (ImGui::SmallButton("- node") && scaling_nodes_ > 0) --scaling_nodes_;
    ImGui::SameLine();
    ImGui::Text("%d", scaling_nodes_);
    ImGui::SameLine();
    if (ImGui::SmallButton("+ node")) ++scaling_nodes_;
    if (scaling_nodes_ < 0) scaling_nodes_ = 0;

    ImGui::SetNextItemWidth(220.0f);
    float progress = static_cast<float>(scaling_progress_);
    if (ImGui::SliderFloat("Progress", &progress, 0.0f, 1.0f, "%.2f")) {
        scaling_progress_ = progress;
    }
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputDouble("Elapsed seconds", &scaling_elapsed_, 0.0, 0.0, "%.1f");
    if (scaling_elapsed_ < 0.0) scaling_elapsed_ = 0.0;
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputDouble("Accrued cost", &scaling_cost_, 0.0, 0.0, "%.1f");
    if (scaling_cost_ < 0.0) scaling_cost_ = 0.0;

    // 4) Score it — the real, pure decision. reward (higher is better),
    //    recommended node count, the scale up/hold/down action relative to the
    //    sample, whether the cost cap holds, and the human-readable reason.
    ScalingPolicy policy;
    policy.objective = objective;
    policy.bounds.min_nodes = static_cast<std::uint32_t>(scaling_min_nodes_);
    policy.bounds.max_nodes = static_cast<std::uint32_t>(scaling_max_nodes_);
    policy.bounds.cost_cap = scaling_cost_cap_;
    policy.target_seconds = scaling_target_seconds_;
    const ScalingSample sample{scaling_progress_, scaling_elapsed_, scaling_cost_,
                               static_cast<std::uint32_t>(scaling_nodes_)};
    const ScalingDecision decision = policy.evaluate(sample);

    ImGui::SeparatorText("Decision");
    ImGui::Text("recommended nodes: %u", decision.recommended_nodes);
    ImGui::SameLine();
    ImGui::TextColored(scaling_action_color(decision.action), "(%s)",
                       std::string(to_string(decision.action)).c_str());
    ImGui::Text("reward: %.4g", decision.reward);
    if (!decision.within_cost_cap) {
        ImGui::SameLine();
        ImGui::TextColored(kErrorColor, "  cost cap breached");
    }
    ImGui::TextWrapped("%s", decision.reason.c_str());
}

void App::draw_jobs() {
    if (!ImGui::CollapsingHeader("Compute Jobs",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    // The GUI form of system.zetta_jobs: the live compute-job registry (one row
    // per unit of native distributed accelerated compute — featurize / train /
    // explain / score / a DDF op — with its lifecycle status). Read-only over the
    // replicated, already-validated ComputeJob handles, like the Runs panel —
    // creating/scheduling a job is a later slice. The detail pane adds the per-job
    // objective/scaling preview (draw_job_scaling_controls).
    const std::vector<std::shared_ptr<ComputeJob>> jobs = node_.jobs();
    ImGui::TextDisabled("%zu job(s)", jobs.size());

    const float list_w = ImGui::GetContentRegionAvail().x * 0.28f;
    const float panel_h = ImGui::GetTextLineHeightWithSpacing() * 22.0f;

    // Left: the job list. Jobs have no human name, so each row is a color-coded
    // status badge plus the kind and the first UUID segment.
    ImGui::BeginChild("##job_list", ImVec2(list_w, panel_h),
                      ImGuiChildFlags_Borders);
    if (jobs.empty()) ImGui::TextDisabled("(no jobs)");
    for (const std::shared_ptr<ComputeJob>& job : jobs) {
        ImGui::PushID(job->id().c_str());
        ImGui::TextColored(job_status_color(job->status()), "%-9s",
                           std::string(to_string(job->status())).c_str());
        ImGui::SameLine();
        const std::string short_id = job->id().substr(0, job->id().find('-'));
        const std::string label =
            std::string(to_string(job->kind())) + " · " + short_id;
        if (ImGui::Selectable(label.c_str(), selected_job_ == job->id())) {
            selected_job_ = job->id();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    // Right: the selected job's detail + the objective/scaling preview.
    ImGui::SameLine();
    ImGui::BeginChild("##job_detail", ImVec2(0, panel_h),
                      ImGuiChildFlags_Borders);
    if (const std::shared_ptr<ComputeJob> job = node_.find_job(selected_job_)) {
        // Reset the scaling controls whenever the selection changes, so they
        // start from sensible defaults per job rather than leaking the previous
        // job's edits.
        if (scaling_loaded_job_ != job->id()) {
            reset_scaling_controls();
            scaling_loaded_job_ = job->id();
        }

        ImGui::Text("Job %s", job->id().c_str());
        ImGui::SameLine();
        ImGui::TextColored(job_status_color(job->status()), "[%s]",
                           std::string(to_string(job->status())).c_str());
        ImGui::TextDisabled("kind: %s",
                            std::string(to_string(job->kind())).c_str());
        ImGui::TextDisabled("created: %s", job->created().c_str());
        if (job->updated() != job->created()) {
            ImGui::TextDisabled("updated: %s", job->updated().c_str());
        }
        if (!job->detail().empty()) {
            ImGui::TextWrapped("detail: %s", job->detail().c_str());
        }

        // Id references — the job's place in the pipeline (project / graph / run /
        // scaling-policy reward). Shown only when set.
        const auto ref_line = [](const char* label, const std::string& value) {
            if (!value.empty()) ImGui::TextDisabled("%s: %s", label, value.c_str());
        };
        ref_line("project", job->project());
        ref_line("graph", job->graph());
        ref_line("run", job->run());
        ref_line("scaling_policy", job->scaling_policy());

        draw_job_scaling_controls();
    } else {
        ImGui::TextDisabled(
            "Select a job to view its status, references, and scaling controls.");
    }
    ImGui::EndChild();
}

void App::draw_admin() {
    if (!ImGui::CollapsingHeader("Admin & Settings",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    // The governance surface (SCR-7 / CMP-5, the GUI slice of FEAT-9): a
    // **read-only** viewer over the node's already-validated governance state —
    // principals + their access keys (scope + capability grants) and the
    // immutable audit trail. Mutation (issuing keys, granting capabilities) is a
    // later slice; this realizes the "view principals/keys, grants, and audit
    // events" acceptance. Reads in-memory handles validated when they entered
    // from disk / the CRDT, so it introduces no new JSON boundary.

    // --- Principals & access keys (list + detail, like the Runs panel) ------
    ImGui::SeparatorText("Principals & access keys");
    const std::vector<std::string> principal_names = node_.principal_names();
    ImGui::TextDisabled("%zu principal(s)", principal_names.size());

    const float list_w = ImGui::GetContentRegionAvail().x * 0.28f;
    const float panel_h = ImGui::GetTextLineHeightWithSpacing() * 12.0f;

    // Left: the principals. Each row is a kind badge plus the principal's name;
    // selecting one (held by name, re-resolved each frame) inspects its keys.
    ImGui::BeginChild("##principal_list", ImVec2(list_w, panel_h),
                      ImGuiChildFlags_Borders);
    if (principal_names.empty()) ImGui::TextDisabled("(no principals)");
    for (const std::string& name : principal_names) {
        const std::shared_ptr<Principal> principal = node_.find_principal(name);
        if (!principal) continue;
        ImGui::PushID(name.c_str());
        ImGui::TextDisabled("%-7s",
                            std::string(to_string(principal->kind())).c_str());
        ImGui::SameLine();
        if (ImGui::Selectable(name.c_str(), selected_principal_ == name)) {
            selected_principal_ = name;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    // Right: the selected principal's keys (each with its scope + grants).
    ImGui::SameLine();
    ImGui::BeginChild("##principal_detail", ImVec2(0, panel_h),
                      ImGuiChildFlags_Borders);
    if (const std::shared_ptr<Principal> principal =
            node_.find_principal(selected_principal_)) {
        render_principal_detail(*principal);
    } else {
        ImGui::TextDisabled(
            "Select a principal to view its keys, scope, and capability grants.");
    }
    ImGui::EndChild();

    // --- Audit log (the immutable, append-only trail) -----------------------
    ImGui::SeparatorText("Audit log");
    render_audit_log(node_.audit_events());
}

void App::open_graph(const ProjectGraph& stored) {
    // A fresh working copy: edits apply here and only land in the IR on Save, so
    // an edit is a deliberate, atomic act (and Revert is just "drop this copy").
    editing_graph_ = std::make_unique<ir::ExecutionGraph>(stored.graph());
    selected_graph_ = stored.name();
    graph_loaded_id_ = stored.id();
    graph_dirty_ = false;
    selected_node_id_.clear();
    graph_error_.clear();
    rename_graph_input_ = stored.name();
    rename_node_input_.clear();
    add_node_name_input_.clear();
    add_edge_from_.clear();
    add_edge_to_.clear();
}

void App::draw_graph_workspace() {
    if (!ImGui::CollapsingHeader("Graph Workspace",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    // The workspace edits the *currently selected* project's graphs (the same
    // selection the explorer's Projects column drives). Re-resolved by name each
    // frame so a dropped/renamed project never leaves a dangling handle.
    const auto project = node_.find_project(selected_project_);
    if (!project) {
        editing_graph_.reset();
        graph_panel_project_.clear();
        selected_graph_.clear();
        ImGui::TextDisabled("Select a project to view its execution graphs.");
        return;
    }

    // When the selected project changes, drop any open working copy so we never
    // show one project's graph under another.
    if (graph_panel_project_ != project->id()) {
        graph_panel_project_ = project->id();
        editing_graph_.reset();
        selected_graph_.clear();
        graph_loaded_id_.clear();
        selected_node_id_.clear();
        graph_dirty_ = false;
        graph_error_.clear();
    }

    // --- graph list + create ----------------------------------------------
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    const bool submit = ImGui::InputTextWithHint(
        "##newgraph", "new graph name", &new_graph_input_,
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("New graph") || submit) {
        if (new_graph_input_.empty()) {
            graph_error_ = "graph name required";
        } else if (auto created =
                       project->create_graph(new_graph_input_, ir::ExecutionGraph{})) {
            open_graph(*created);
            new_graph_input_.clear();
        } else {
            graph_error_ = "a graph by that name already exists";
        }
    }

    const std::vector<std::string> names = project->graph_names();
    if (names.empty()) {
        ImGui::TextDisabled("(this project has no graphs yet)");
    }
    for (const std::string& name : names) {
        ImGui::PushID(name.c_str());
        if (ImGui::SmallButton("x")) {
            project->drop_graph(name);
            if (selected_graph_ == name) {
                editing_graph_.reset();
                selected_graph_.clear();
                graph_loaded_id_.clear();
                selected_node_id_.clear();
                graph_dirty_ = false;
            }
            ImGui::PopID();
            continue;  // this name is gone; don't draw a selectable for it
        }
        ImGui::SameLine();
        const bool is_open = editing_graph_ && selected_graph_ == name;
        if (ImGui::Selectable(name.c_str(), is_open)) {
            const auto stored = project->find_graph(name);
            // Re-clicking the open graph keeps unsaved edits; opening a
            // different one loads a fresh working copy (discarding edits).
            if (stored && stored->id() != graph_loaded_id_) open_graph(*stored);
        }
        ImGui::PopID();
    }

    if (!editing_graph_) {
        if (!graph_error_.empty()) {
            ImGui::TextColored(kErrorColor, "%s", graph_error_.c_str());
        }
        ImGui::TextDisabled("Select or create a graph to view and edit it.");
        return;
    }

    ImGui::Separator();
    draw_graph_canvas();
    draw_graph_editor(*project);
}

void App::draw_graph_canvas() {
    ir::ExecutionGraph& graph = *editing_graph_;

    const float font = ImGui::GetFontSize();
    const float node_w = font * 9.0f;
    const float node_h = font * 2.4f;
    const float col_gap = font * 3.0f;
    const float row_gap = font * 0.9f;
    const float header_h = font * 1.8f;
    const float pad = font * 0.6f;
    const float rounding = font * 0.3f;

    if (ImGui::BeginChild("##graph_canvas", ImVec2(0, font * 18.0f),
                          ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        if (graph.empty()) {
            ImGui::TextDisabled("(empty graph — add a step below)");
        } else {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 origin = ImGui::GetCursorScreenPos();

            // Pass 1: place each node in its StepKind column and remember its
            // rect + center (so edges can be drawn between centers underneath).
            struct Placed {
                ImVec2 top_left;
                ImVec2 bottom_right;
                std::shared_ptr<ir::Node> node;
            };
            std::vector<Placed> placed;
            std::unordered_map<std::string, ImVec2> center_of;
            float content_right = origin.x;
            float content_bottom = origin.y;

            const auto kinds = ir::all_step_kinds();
            for (std::size_t column = 0; column < kinds.size(); ++column) {
                const float x =
                    origin.x + static_cast<float>(column) * (node_w + col_gap);
                const std::string header{ir::to_string(kinds[column])};
                draw->AddText(ImVec2(x, origin.y), kCanvasHeaderColor,
                              header.c_str());
                float y = origin.y + header_h;
                for (const auto& node : graph.nodes_of_kind(kinds[column])) {
                    const ImVec2 top_left(x, y);
                    const ImVec2 bottom_right(x + node_w, y + node_h);
                    placed.push_back({top_left, bottom_right, node});
                    center_of[node->id] =
                        ImVec2(x + node_w * 0.5f, y + node_h * 0.5f);
                    y += node_h + row_gap;
                    content_bottom = std::max(content_bottom, y);
                }
                content_right = std::max(content_right, x + node_w);
            }

            // Pass 2: edges, clipped to each box's border with an arrowhead at
            // the `to` end so direction reads even when an edge points backward.
            const ImVec2 half(node_w * 0.5f, node_h * 0.5f);
            const auto border_point = [&](ImVec2 center, ImVec2 toward) {
                const ImVec2 d(toward.x - center.x, toward.y - center.y);
                if (d.x == 0.0f && d.y == 0.0f) return center;
                const float scale_x =
                    d.x != 0.0f ? half.x / std::abs(d.x) : FLT_MAX;
                const float scale_y =
                    d.y != 0.0f ? half.y / std::abs(d.y) : FLT_MAX;
                const float scale = std::min(scale_x, scale_y);
                return ImVec2(center.x + d.x * scale, center.y + d.y * scale);
            };
            for (const ir::Edge& edge : graph.edges()) {
                const auto from = center_of.find(edge.from);
                const auto to = center_of.find(edge.to);
                if (from == center_of.end() || to == center_of.end()) continue;
                const ImU32 color = edge.kind == ir::EdgeKind::Control
                                        ? kControlEdgeColor
                                        : kDataEdgeColor;
                const ImVec2 start = border_point(from->second, to->second);
                const ImVec2 end = border_point(to->second, from->second);
                draw->AddLine(start, end, color, font * 0.1f);
                ImVec2 dir(end.x - start.x, end.y - start.y);
                const float length =
                    std::sqrt(dir.x * dir.x + dir.y * dir.y);
                if (length > 0.001f) {
                    dir.x /= length;
                    dir.y /= length;
                    const ImVec2 normal(-dir.y, dir.x);
                    const float head = font * 0.55f;
                    const ImVec2 base(end.x - dir.x * head, end.y - dir.y * head);
                    draw->AddTriangleFilled(
                        end,
                        ImVec2(base.x + normal.x * head * 0.5f,
                               base.y + normal.y * head * 0.5f),
                        ImVec2(base.x - normal.x * head * 0.5f,
                               base.y - normal.y * head * 0.5f),
                        color);
                }
            }

            // Pass 3: node boxes + click-to-select (invisible buttons hit-test).
            for (const Placed& item : placed) {
                ImGui::SetCursorScreenPos(item.top_left);
                ImGui::InvisibleButton(item.node->id.c_str(),
                                       ImVec2(node_w, node_h));
                const bool hovered = ImGui::IsItemHovered();
                if (ImGui::IsItemClicked()) {
                    selected_node_id_ = item.node->id;
                    rename_node_input_ = item.node->name;
                }
                const bool selected = item.node->id == selected_node_id_;
                const ImU32 fill = selected ? kNodeFillSelected
                                            : (hovered ? kNodeFillHover : kNodeFill);
                draw->AddRectFilled(item.top_left, item.bottom_right, fill,
                                    rounding);
                draw->AddRect(item.top_left, item.bottom_right,
                              selected ? kNodeBorderSelected : kNodeBorder,
                              rounding, 0, selected ? 2.0f : 1.0f);
                draw->PushClipRect(item.top_left, item.bottom_right, true);
                draw->AddText(
                    ImVec2(item.top_left.x + pad,
                           item.top_left.y + (node_h - font) * 0.5f),
                    kNodeTextColor, item.node->name.c_str());
                draw->PopClipRect();
            }

            // Reserve the laid-out area so the child scrolls to fit it.
            ImGui::SetCursorScreenPos(origin);
            ImGui::Dummy(ImVec2(content_right - origin.x + col_gap,
                                content_bottom - origin.y + row_gap));
        }
    }
    ImGui::EndChild();
}

void App::draw_graph_editor(Project& project) {
    ir::ExecutionGraph& graph = *editing_graph_;
    const float font = ImGui::GetFontSize();

    // --- summary, rename, save / revert -----------------------------------
    ImGui::Text("%zu step(s), %zu edge(s)", graph.node_count(),
                graph.edge_count());
    if (graph_dirty_) {
        ImGui::SameLine();
        ImGui::TextColored(kUnsavedColor, "● unsaved edits");
    }

    ImGui::SetNextItemWidth(font * 12.0f);
    ImGui::InputTextWithHint("##rengraph", "graph name", &rename_graph_input_);
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        // Persist the working copy back to the IR (disk + CRDT) via update_graph.
        // A name change in the field becomes a rename in the same call.
        std::string new_name = rename_graph_input_ == selected_graph_
                                   ? std::string{}
                                   : rename_graph_input_;
        if (auto updated =
                project.update_graph(selected_graph_, graph, std::move(new_name))) {
            selected_graph_ = updated->name();
            graph_loaded_id_ = updated->id();
            rename_graph_input_ = updated->name();
            graph_dirty_ = false;
            graph_error_.clear();
            io_status_ = "Saved graph '" + updated->name() + "' (" +
                         std::to_string(updated->graph().node_count()) +
                         " steps, " +
                         std::to_string(updated->graph().edge_count()) +
                         " edges)";
        } else {
            graph_error_ = "save failed — name empty or already taken";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) {
        if (const auto stored = project.find_graph_by_id(graph_loaded_id_)) {
            open_graph(*stored);
        } else {
            editing_graph_.reset();
            selected_graph_.clear();
            graph_loaded_id_.clear();
        }
    }
    if (!graph_error_.empty()) {
        ImGui::TextColored(kErrorColor, "%s", graph_error_.c_str());
    }
    if (!editing_graph_) return;  // Revert may have closed the graph

    // --- add a step --------------------------------------------------------
    ImGui::SeparatorText("Add a step");
    ImGui::SetNextItemWidth(font * 10.0f);
    ImGui::InputTextWithHint("##nodename", "step name", &add_node_name_input_);
    ImGui::SameLine();
    const auto kinds = ir::all_step_kinds();
    ImGui::SetNextItemWidth(font * 7.0f);
    if (ImGui::BeginCombo("##nodekind",
                          std::string{ir::to_string(kinds[add_node_kind_])}
                              .c_str())) {
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            const std::string label{ir::to_string(kinds[i])};
            if (ImGui::Selectable(label.c_str(),
                                  static_cast<int>(i) == add_node_kind_)) {
                add_node_kind_ = static_cast<int>(i);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add step")) {
        if (add_node_name_input_.empty()) {
            graph_error_ = "step name required";
        } else if (auto added = graph.add_node(kinds[add_node_kind_],
                                               add_node_name_input_)) {
            selected_node_id_ = added->id;
            rename_node_input_ = added->name;
            add_node_name_input_.clear();
            graph_dirty_ = true;
            graph_error_.clear();
        } else {
            graph_error_ = "could not add step";
        }
    }

    // A picker over the graph's nodes, labelled "<name> [<kind>]".
    const auto node_label = [](const ir::Node& node) {
        return node.name + "  [" + std::string{ir::to_string(node.kind)} + "]";
    };
    const auto node_picker = [&](const char* combo_id, const char* placeholder,
                                 std::string& selection) {
        const auto current = graph.find_node(selection);
        const std::string preview =
            current ? node_label(*current) : std::string{placeholder};
        ImGui::SetNextItemWidth(font * 11.0f);
        if (ImGui::BeginCombo(combo_id, preview.c_str())) {
            for (const auto& node : graph.nodes()) {
                if (ImGui::Selectable(node_label(*node).c_str(),
                                      node->id == selection)) {
                    selection = node->id;
                }
            }
            ImGui::EndCombo();
        }
    };

    // --- add an edge -------------------------------------------------------
    ImGui::SeparatorText("Add an edge");
    if (graph.node_count() < 1) {
        ImGui::TextDisabled("Add steps first.");
    } else {
        node_picker("##edgefrom", "(from)", add_edge_from_);
        ImGui::SameLine();
        ImGui::TextUnformatted("→");
        ImGui::SameLine();
        node_picker("##edgeto", "(to)", add_edge_to_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(font * 7.0f);
        const char* edge_kind_names[] = {"data_flow", "control"};
        ImGui::Combo("##edgekind", &add_edge_kind_, edge_kind_names, 2);
        ImGui::SameLine();
        if (ImGui::Button("Add edge")) {
            const ir::EdgeKind kind = add_edge_kind_ == 1
                                          ? ir::EdgeKind::Control
                                          : ir::EdgeKind::DataFlow;
            if (graph.add_edge(add_edge_from_, add_edge_to_, kind)) {
                graph_dirty_ = true;
                graph_error_.clear();
            } else {
                graph_error_ =
                    "edge rejected — unknown step or duplicate edge";
            }
        }
    }

    // Existing edges, each with a delete button (remove_edge is the inverse).
    if (!graph.edges().empty()) {
        int remove_at = -1;
        for (std::size_t i = 0; i < graph.edges().size(); ++i) {
            const ir::Edge& edge = graph.edges()[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton("x")) remove_at = static_cast<int>(i);
            ImGui::SameLine();
            const auto from = graph.find_node(edge.from);
            const auto to = graph.find_node(edge.to);
            ImGui::TextDisabled(
                "%s → %s  (%s)",
                from ? from->name.c_str() : edge.from.c_str(),
                to ? to->name.c_str() : edge.to.c_str(),
                std::string{ir::to_string(edge.kind)}.c_str());
            ImGui::PopID();
        }
        if (remove_at >= 0) {
            const ir::Edge edge = graph.edges()[remove_at];
            graph.remove_edge(edge.from, edge.to, edge.kind);
            graph_dirty_ = true;
        }
    }

    // --- selected step -----------------------------------------------------
    ImGui::SeparatorText("Selected step");
    const auto selected = graph.find_node(selected_node_id_);
    if (!selected) {
        ImGui::TextDisabled("Click a step in the canvas to select it.");
    } else {
        const std::string short_id =
            selected_node_id_.substr(0, selected_node_id_.find('-'));
        ImGui::TextDisabled("id: %s   kind: %s", short_id.c_str(),
                            std::string{ir::to_string(selected->kind)}.c_str());
        ImGui::SetNextItemWidth(font * 12.0f);
        const bool rename = ImGui::InputTextWithHint(
            "##rennode", "step name", &rename_node_input_,
            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Rename step") || rename) {
            if (rename_node_input_.empty()) {
                graph_error_ = "step name required";
            } else {
                selected->name = rename_node_input_;  // mutate the working copy
                graph_dirty_ = true;
                graph_error_.clear();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete step")) {
            graph.remove_node(selected_node_id_);
            selected_node_id_.clear();
            graph_dirty_ = true;
        }
    }
}

void App::run() {
    ImGuiIO& io = ImGui::GetIO();

    // Install our handlers last (after SDL_Init may have set its own) so
    // terminal Ctrl-C quits the headed app too, matching headless. The flag is
    // polled below; SDL_WaitEventTimeout's cap bounds the response to ~33ms.
    install_signal_handlers();

    bool running = true;
    while (running) {
        if (stop_requested()) break;
        node_.tick();

        // Sleep until the next input arrives or kIdleFrameMs elapses, so an
        // idle UI doesn't peg the CPU redrawing identical frames. Wait wakes
        // immediately on input, so interaction stays full-speed.
        constexpr int kIdleFrameMs = 33;  // ~30fps when idle
        SDL_Event event;
        if (!SDL_WaitEventTimeout(&event, kIdleFrameMs)) {
            event.type = 0;  // timed out — skip the drain below
        }
        do {
            if (event.type == 0) break;
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED ||
                       event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED) {
                apply_display_scale();
            } else if (event.type == SDL_EVENT_DROP_FILE) {
                if (event.drop.data) {
                    // While the New Project dialog is up, a dropped file is its
                    // n8n artifact (the rest of the UI is blocked by the modal);
                    // otherwise it routes by extension as before.
                    enqueue_file(event.drop.data,
                                 new_project_modal_open_
                                     ? FileIntent::N8nArtifact
                                     : FileIntent::Auto);
                }
            }
        } while (SDL_PollEvent(&event));

        // Ingest any files dropped or picked since the last frame (the dialog
        // callback runs on another thread; this drains on the main thread).
        handle_incoming_files();

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        constexpr ImGuiWindowFlags kRootFlags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_MenuBar;
        ImGui::Begin("##zetta_root", nullptr, kRootFlags);

        draw_menu_bar(running);
        draw_dashboard();
        draw_query();
        draw_gate();
        draw_deploy();
        draw_cluster();
        draw_runs();
        draw_jobs();
        draw_admin();
        draw_graph_workspace();
        ImGui::Separator();
        draw_explorer();

        // The New Project dialog (SCR-2) is a modal over the root window; it must
        // be issued every frame so ImGui keeps it alive while it's open.
        draw_new_project_modal();

        ImGui::End();

        if (show_demo_) ImGui::ShowDemoWindow(&show_demo_);

        ImGui::Render();
        // SDL_WINDOW_HIGH_PIXEL_DENSITY gives a framebuffer larger than the
        // logical window. ImGui draws in logical coords and the SDLRenderer3
        // backend does NOT set the render scale for us, so without this the UI
        // paints into the top-left logical-sized quadrant only.
        SDL_SetRenderScale(renderer_,
                           io.DisplayFramebufferScale.x,
                           io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer_, 30, 30, 30, 255);
        SDL_RenderClear(renderer_);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
        SDL_RenderPresent(renderer_);
    }
}

}  // namespace zetta::gui
