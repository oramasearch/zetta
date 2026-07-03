// The Cmd+K command palette (#625, docs/gui/command-palette.md): keyboard-first
// navigation and actions. One overlay, three entry sources, all live:
//
//   Views    — the view registry (app_views.cpp), the same rows the sidebar
//              renders; a fourth consumer of the one list.
//   Entities — projects / datasets / runs / models / jobs / cluster nodes,
//              read from the node each frame (thin renderer, no copies);
//              activating one deep-links via navigate_to(view, entity_id).
//   Actions  — the capability registry (docs/api/registry-design.md), the SAME
//              list the MCP server serves, rendered for humans. Activating one
//              runs Registry::call on this (owning) thread, so schema
//              validation, authorization, audit, spans, and the consent gate
//              are the one shared path — a cloud-touching entry is refused
//              with the registry's consent reason, exactly like an MCP caller.
//
// Matching is zetta::util::fuzzy_score (zetta-core, unit-tested headless);
// this TU is presentation only.

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <nlohmann/json.hpp>

#include "gui/app.hpp"
#include "gui/app_internal.hpp"
#include "gui/app_operations_internal.hpp"
#include "gui/tones.hpp"
#include "zetta/api/background.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/data/dataset.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/util/fuzzy.hpp"

namespace zetta::gui {
namespace {

constexpr const char* kPalettePopup = "##command_palette";

// Up/Down while the query input keeps focus move the list selection instead
// of the text cursor (ImGui's history-callback seam; the REPL console uses
// the same mechanism for its history).
int palette_history_callback(ImGuiInputTextCallbackData* data) {
    int* move = static_cast<int*>(data->UserData);
    if (data->EventKey == ImGuiKey_UpArrow) *move = -1;
    if (data->EventKey == ImGuiKey_DownArrow) *move = 1;
    return 0;
}

// Coerce one staged text field to the JSON type its schema property declares.
// Text that fails to parse passes through as the raw string so the registry's
// own schema validation reports the path-qualified reason — the palette adds
// no second validator in front of the one boundary.
nlohmann::json coerce_argument(const nlohmann::json& property,
                               const std::string& text) {
    const std::string type = property.value("type", "string");
    if (type == "integer") {
        char* end = nullptr;
        const long long value = std::strtoll(text.c_str(), &end, 10);
        if (end != text.c_str() && end && *end == '\0') return value;
    } else if (type == "number") {
        char* end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        if (end != text.c_str() && end && *end == '\0') return value;
    } else if (type == "boolean") {
        if (text == "true") return true;
        if (text == "false") return false;
    } else if (type == "array" || type == "object") {
        const nlohmann::json parsed =
            nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (!parsed.is_discarded()) return parsed;
    }
    return text;
}

bool schema_requires(const nlohmann::json& schema, const std::string& field) {
    const auto required = schema.find("required");
    if (required == schema.end() || !required->is_array()) return false;
    for (const auto& entry : *required) {
        if (entry.is_string() && entry.get<std::string>() == field) return true;
    }
    return false;
}

bool schema_has_required_fields(const nlohmann::json& schema) {
    const auto required = schema.find("required");
    return required != schema.end() && required->is_array() && !required->empty();
}

// The action row's mono meta: the entry's kind, with the consent marker on
// cloud-touching entries so the gate is visible before anything runs.
std::string capability_meta(const api::Capability& capability) {
    std::string meta{api::to_string(capability.kind)};
    if (capability.requires_consent) meta += " · consent";
    return meta;
}

// Right-aligned muted mono annotation drawn over the last item — the
// master-detail row / sidebar-badge pattern.
void draw_row_meta(const std::string& meta) {
    if (meta.empty()) return;
    ImFont* meta_font = g_mono_font ? g_mono_font : ImGui::GetFont();
    const float meta_font_size = meta_font->FontSize;
    const ImVec2 meta_size = meta_font->CalcTextSizeA(meta_font_size, FLT_MAX,
                                                      0.0f, meta.c_str());
    const ImVec2 row_min = ImGui::GetItemRectMin();
    const ImVec2 row_max = ImGui::GetItemRectMax();
    const ImVec2 meta_pos(
        row_max.x - meta_size.x - ImGui::GetStyle().FramePadding.x,
        row_min.y + (row_max.y - row_min.y - meta_size.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(meta_font, meta_font_size, meta_pos,
                                        ImGui::GetColorU32(kInkQuiet),
                                        meta.c_str());
}

}  // namespace

void App::run_palette_capability(const std::string& capability_name) {
    palette_status_.clear();
    palette_failed_ = false;
    palette_trace_.clear();
    palette_result_.clear();
    if (!capability_registry_) return;
    const auto capability = capability_registry_->find(capability_name);
    if (!capability) {
        palette_failed_ = true;
        palette_status_ = "no action named '" + capability_name + "'";
        return;
    }

    // Assemble the staged fields into the call input. Empty fields are
    // omitted, so a missing required argument fails schema validation with
    // the validator's own path-qualified reason.
    nlohmann::json input = nlohmann::json::object();
    const auto properties = capability->input_schema.find("properties");
    if (properties != capability->input_schema.end() && properties->is_object()) {
        for (const auto& [field, property] : properties->items()) {
            const auto staged = palette_args_.find(field);
            if (staged == palette_args_.end() || staged->second.empty()) continue;
            input[field] = coerce_argument(property, staged->second);
        }
    }

    // The GUI runs on the loop that owns the Node, so it calls the registry
    // directly (no bridge hop) — the same validate → authorize → audit →
    // handler path every other rendering takes. The palette acts as the
    // audited "unauthenticated local" session (the governed-console
    // precedent), and the span lands in the node's ring beside MCP calls.
    // The lazily-built background owner lets the kind:job entries (run /
    // testkit_run, #617) launch from the palette too; poll_background_ops()
    // prunes finished workers each frame.
    if (!palette_background_) {
        palette_background_ = std::make_unique<api::BackgroundActions>();
    }
    api::ApiContext context{.node = node_,
                            .cluster = &cluster_,
                            .acting_key = {},
                            .trace = zetta::observability::make_root_trace(),
                            .recorder = &node_.span_recorder(),
                            .background = palette_background_.get(),
                            .provisioners = [this](const std::string& target) {
                                return provisioner_for(target);
                            }};
    // The registry is shared with the MCP server, whose activity hook feeds
    // the external-activity strip (#620) — a palette call is self-initiated,
    // so suppress it for the synchronous duration of the call (the hook fires
    // on this thread, inside call; Registry::call is non-throwing).
    palette_call_in_flight_ = true;
    const api::ApiResult result =
        capability_registry_->call(capability_name, input, context);
    palette_call_in_flight_ = false;

    palette_trace_ = result.trace_id;
    if (result.ok) {
        char duration[32];
        std::snprintf(duration, sizeof(duration), "%.1f", result.duration_ms);
        palette_status_ = capability->title + ": ok in " + duration + " ms";
        palette_result_ = result.value.dump(2);
    } else {
        // The engine's reason verbatim (banner rule #632), never a laundered
        // "operation failed". A consent-gated entry lands here with the
        // registry's consent_required reason; its challenge then awaits a
        // decision on the Admin panel's approval surface (#621).
        palette_failed_ = true;
        palette_status_ = result.error.code + ": " + result.error.message;
        if (!result.error.path.empty()) {
            palette_status_ += " (" + result.error.path + ")";
        }
    }
}

zetta::api::Registry& App::ensure_capability_registry() {
    if (!capability_registry_) {
        owned_capability_registry_ = std::make_unique<api::Registry>();
        api::register_core_capabilities(*owned_capability_registry_);
        capability_registry_ = owned_capability_registry_.get();
    }
    return *capability_registry_;
}

void App::draw_command_palette() {
    // Cmd+K toggles (ImGuiMod_Shortcut: Cmd on macOS, Ctrl elsewhere). The
    // View menu sets palette_requested_ for discoverability. A chord while
    // another popup is up (New project, About) is ignored — one modal at a
    // time.
    const bool chord =
        ImGui::IsKeyChordPressed(ImGuiMod_Shortcut | ImGuiKey_K);
    const bool already_open = ImGui::IsPopupOpen(kPalettePopup);
    bool opened_this_frame = false;
    if ((palette_requested_ || chord) && !already_open &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId |
                                    ImGuiPopupFlags_AnyPopupLevel)) {
        // The palette renders/executes the API server's registry; when main
        // wired none (--no-mcp) it builds its own from the SAME core
        // registration, so the action list never forks (one registry, many
        // renderings).
        ensure_capability_registry();
        palette_query_.clear();
        palette_selected_ = 0;
        palette_capability_.clear();
        palette_args_.clear();
        palette_status_.clear();
        palette_failed_ = false;
        palette_trace_.clear();
        palette_result_.clear();
        ImGui::OpenPopup(kPalettePopup);
        opened_this_frame = true;
    }
    palette_requested_ = false;

    // Spotlight geometry: fixed width, top-centred, height from content.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = ImGui::GetFontSize() * 42.0f;
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
               viewport->WorkPos.y + viewport->WorkSize.y * 0.14f),
        ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f),
                                        ImVec2(width, FLT_MAX));
    if (!ImGui::BeginPopupModal(kPalettePopup, nullptr,
                                ImGuiWindowFlags_NoTitleBar |
                                    ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoMove)) {
        return;
    }
    if (chord && !opened_this_frame) ImGui::CloseCurrentPopup();

    if (!palette_capability_.empty()) {
        draw_palette_argument_step();
    } else {
        draw_palette_list_step();
    }

    // The last call's outcome, shared by both steps: the status banner (the
    // engine's reason verbatim on failure), the trace id (readable back via
    // the Traces view / get_trace), and the result JSON.
    banner(palette_failed_ ? Tone::Error : Tone::Ok, palette_status_);
    if (!palette_trace_.empty()) {
        mono_line(kInkQuiet, "trace ", palette_trace_);
    }
    if (!palette_result_.empty()) {
        ImGui::BeginChild("##palette_result",
                          ImVec2(-FLT_MIN,
                                 ImGui::GetTextLineHeightWithSpacing() * 10.0f),
                          ImGuiChildFlags_Borders);
        if (g_mono_font) ImGui::PushFont(g_mono_font);
        ImGui::TextUnformatted(palette_result_.c_str());
        if (g_mono_font) ImGui::PopFont();
        ImGui::EndChild();
    }

    ImGui::EndPopup();
}

void App::draw_palette_list_step() {
    // The query input owns the keyboard: type to filter, Up/Down to select
    // (the history callback), Enter to activate, Esc to close (the modal's
    // default).
    int move = 0;
    if (ImGui::IsWindowAppearing() || palette_refocus_) {
        ImGui::SetKeyboardFocusHere();
        palette_refocus_ = false;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool submitted = ImGui::InputTextWithHint(
        "##palette_query", "Search views, entities, and actions",
        &palette_query_,
        ImGuiInputTextFlags_EnterReturnsTrue |
            ImGuiInputTextFlags_CallbackHistory,
        palette_history_callback, &move);
    if (ImGui::IsItemEdited()) palette_selected_ = 0;

    // One row the palette can select. Navigation rows carry a view (+ an
    // optional entity deep link); action rows carry a registry entry name.
    struct PaletteEntry {
        std::string title;
        std::string alias;  // secondary match text (snake_case name, full id)
        std::string meta;   // right-aligned mono annotation
        std::string section;
        int section_rank = 0;
        Tone tone = Tone::Muted;
        bool mono_title = false;
        View view = View::Home;
        std::string entity_id;
        std::string capability;  // "" = navigation
        int score = 0;
    };
    std::vector<PaletteEntry> entries;
    const std::string& query = palette_query_;
    const auto add = [&entries, &query](PaletteEntry entry) {
        const std::optional<int> title_score =
            zetta::util::fuzzy_score(query, entry.title);
        const std::optional<int> alias_score =
            entry.alias.empty() ? std::nullopt
                                : zetta::util::fuzzy_score(query, entry.alias);
        if (!title_score && !alias_score) return;
        constexpr int kNoScore = std::numeric_limits<int>::min();
        entry.score = std::max(title_score.value_or(kNoScore),
                               alias_score.value_or(kNoScore));
        entries.push_back(std::move(entry));
    };

    // Views — the registry's rows (its fourth consumer).
    for (const ViewInfo& view : view_registry()) {
        add({.title = view.name,
             .alias = view.section,
             .meta = "view",
             .section = "Views",
             .section_rank = 0,
             .view = view.id});
    }
    // Entities appear as you type (the browse state stays views + actions, so
    // an idle open does not walk every run on the node).
    if (!query.empty()) {
        for (const std::string& name : node_.project_names()) {
            add({.title = name,
                 .meta = "project",
                 .section = "Projects",
                 .section_rank = 1,
                 .view = View::Explorer,
                 .entity_id = name});
        }
        for (const zetta::data::DatasetSummary& dataset : node_.datasets()) {
            add({.title = dataset.name,
                 .alias = dataset.database,
                 .meta = dataset.format + " · " + dataset.database,
                 .section = "Datasets",
                 .section_rank = 2,
                 .mono_title = true,
                 .view = View::Datasets,
                 .entity_id = dataset.table_id});
        }
        for (const auto& run : node_.runs()) {
            if (!run) continue;
            add({.title = entity_short_id(run->id()),
                 .alias = run->id(),
                 .meta = std::string(to_string(run->status())),
                 .section = "Runs",
                 .section_rank = 3,
                 .tone = tone_of(run->status()),
                 .mono_title = true,
                 .view = View::Runs,
                 .entity_id = run->id()});
        }
        for (const auto& model : node_.models()) {
            if (!model) continue;
            add({.title = model->name().empty() ? entity_short_id(model->id())
                                                : model->name(),
                 .alias = model->id(),
                 .meta = model->kind(),
                 .section = "Models",
                 .section_rank = 4,
                 .mono_title = model->name().empty(),
                 .view = View::Models,
                 .entity_id = model->id()});
        }
        for (const auto& job : node_.jobs()) {
            if (!job) continue;
            add({.title = entity_short_id(job->id()),
                 .alias = job->id(),
                 .meta = std::string(to_string(job->status())),
                 .section = "Jobs",
                 .section_rank = 5,
                 .tone = tone_of(job->status()),
                 .mono_title = true,
                 .view = View::Jobs,
                 .entity_id = job->id()});
        }
        for (const zetta::cluster::NodeStatus& peer : cluster_.nodes()) {
            add({.title = entity_short_id(peer.node_id),
                 .alias = peer.node_id,
                 .meta = peer.address,
                 .section = "Nodes",
                 .section_rank = 6,
                 .tone = tone_of(peer),
                 .mono_title = true,
                 .view = View::Cluster,
                 .entity_id = peer.node_id});
        }
    }
    // Actions — the capability registry rendered for humans. Adding a
    // capability once adds it here (the #625 acceptance rule).
    if (capability_registry_) {
        for (const auto& capability : capability_registry_->list()) {
            add({.title = capability->title,
                 .alias = capability->name,
                 .meta = capability_meta(*capability),
                 .section = "Actions",
                 .section_rank = 7,
                 .capability = capability->name});
        }
    }

    // Best match first within each section; sections keep the fixed order
    // above (views → entities → actions).
    std::stable_sort(entries.begin(), entries.end(),
                     [](const PaletteEntry& left, const PaletteEntry& right) {
                         if (left.section_rank != right.section_rank) {
                             return left.section_rank < right.section_rank;
                         }
                         return left.score > right.score;
                     });

    if (entries.empty()) {
        palette_selected_ = 0;
    } else {
        palette_selected_ =
            std::clamp(palette_selected_ + move, 0,
                       static_cast<int>(entries.size()) - 1);
    }

    ImGui::BeginChild("##palette_list",
                      ImVec2(-FLT_MIN,
                             ImGui::GetTextLineHeightWithSpacing() * 14.0f),
                      ImGuiChildFlags_Borders);
    if (entries.empty()) ImGui::TextDisabled("(no matches)");
    bool activated = false;
    std::string_view current_section;
    for (int index = 0; index < static_cast<int>(entries.size()); ++index) {
        const PaletteEntry& entry = entries[static_cast<std::size_t>(index)];
        if (entry.section != current_section) {
            if (index != 0) ImGui::Spacing();
            current_section = entry.section;
            eyebrow(g_mono_font, to_upper(entry.section).c_str());
        }
        ImGui::PushID(index);
        status_dot(entry.tone);
        const bool selected = index == palette_selected_;
        if (entry.mono_title && g_mono_font) ImGui::PushFont(g_mono_font);
        if (ImGui::Selectable(entry.title.c_str(), selected)) {
            palette_selected_ = index;
            activated = true;
        }
        if (entry.mono_title && g_mono_font) ImGui::PopFont();
        if (selected && move != 0) ImGui::SetScrollHereY(0.5f);
        draw_row_meta(entry.meta);
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (g_mono_font) ImGui::PushFont(g_mono_font);
    ImGui::TextColored(kInkQuiet,
                       "up/down select · enter open or run · esc close");
    if (g_mono_font) ImGui::PopFont();

    if ((submitted || activated) && !entries.empty()) {
        const PaletteEntry& chosen =
            entries[static_cast<std::size_t>(palette_selected_)];
        if (chosen.capability.empty()) {
            navigate_to(chosen.view, chosen.entity_id);
            ImGui::CloseCurrentPopup();
        } else {
            palette_status_.clear();
            palette_failed_ = false;
            palette_trace_.clear();
            palette_result_.clear();
            palette_args_.clear();
            palette_refocus_ = true;
            const auto capability = capability_registry_
                                        ? capability_registry_->find(chosen.capability)
                                        : nullptr;
            if (capability &&
                schema_has_required_fields(capability->input_schema)) {
                // Required arguments: switch to the schema-driven form.
                palette_capability_ = chosen.capability;
            } else {
                // Nothing required: run it now, stay open to show the result.
                run_palette_capability(chosen.capability);
            }
        }
    } else if (submitted) {
        // Enter with no match: keep typing (refocus the query).
        palette_refocus_ = true;
    }
}

void App::draw_palette_argument_step() {
    const auto capability = capability_registry_
                                ? capability_registry_->find(palette_capability_)
                                : nullptr;
    if (!capability) {
        palette_capability_.clear();
        return;
    }

    // The entry's own words: title, description (side effects included, per
    // the registry's description rule), and the mono name · kind line.
    ImGui::TextColored(kInkPrimary, "%s", capability->title.c_str());
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kInkMuted, "%s", capability->description.c_str());
    ImGui::PopTextWrapPos();
    mono_line(kInkQuiet, "",
              capability->name + " · " + capability_meta(*capability));
    ImGui::Spacing();

    // One input per schema property — the form IS the input schema, so a new
    // capability's arguments render here with no palette change. Values are
    // validated by Registry::call against that same schema (the one
    // boundary); empty optional fields are omitted from the call.
    bool run_now = false;
    bool first_field = true;
    const auto properties = capability->input_schema.find("properties");
    if (properties != capability->input_schema.end() && properties->is_object()) {
        for (const auto& [field, property] : properties->items()) {
            if (g_mono_font) ImGui::PushFont(g_mono_font);
            ImGui::TextColored(kInkSecondary, "%s", field.c_str());
            if (g_mono_font) ImGui::PopFont();
            if (!schema_requires(capability->input_schema, field)) {
                ImGui::SameLine();
                ImGui::TextColored(kInkQuiet, "optional");
            }
            if (palette_refocus_ && first_field) {
                ImGui::SetKeyboardFocusHere();
                palette_refocus_ = false;
            }
            first_field = false;
            const std::string hint = property.value("description", "");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputTextWithHint(("##palette_arg_" + field).c_str(),
                                         hint.c_str(), &palette_args_[field],
                                         ImGuiInputTextFlags_EnterReturnsTrue)) {
                run_now = true;
            }
            ImGui::Spacing();
        }
    }

    if (primary_button("Run") || run_now) {
        run_palette_capability(palette_capability_);
    }
    ImGui::SameLine();
    if (ImGui::Button("Back")) {
        palette_capability_.clear();
        palette_args_.clear();
        palette_status_.clear();
        palette_failed_ = false;
        palette_trace_.clear();
        palette_result_.clear();
        palette_refocus_ = true;
    }
    ImGui::Spacing();
}

}  // namespace zetta::gui
