#include "gui/app.hpp"
#include "gui/app_internal.hpp"

#include "zetta/agent/admission_gate.hpp"
#include "zetta/agent/corpus_io.hpp"
#include "zetta/dataset/dataset_generator_factory.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <cfloat>
#include <fstream>
#include <memory>
#include <string>

namespace zetta::gui {

// The Data generators view (#873): the native, in-Zetta training-dataset generators (the base
// agent's corpus, Auto-Suggest, …), each growing + gating a corpus grounded in the solutions
// plan (ADR-17). A thin renderer over DatasetGeneratorFactory::builtin() — Generate runs the
// generator, gates it with admit_corpus, and (with a path) writes the JSONL. Grounded in core;
// generation is fast today (churn + search) so it runs inline.
void App::draw_datagen() {
    page_header("Data generators",
                "Native training-dataset generators — grow + gate a corpus grounded in the "
                "solutions plan, inside Zetta");

    // Shared options for a Generate run.
    ImGui::TextUnformatted("Variants per subject");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::InputInt("##dg_variants", &datagen_variants_) && datagen_variants_ < 1) {
        datagen_variants_ = 1;
    }
    ImGui::TextUnformatted("Write JSONL to (optional)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##dg_path", "a file path on this node, e.g. data/generated.jsonl",
                             &datagen_path_);
    ImGui::Spacing();
    banner(datagen_failed_ ? Tone::Error : Tone::Ok, datagen_status_);

    for (const std::shared_ptr<const zetta::dataset::DatasetGenerator>& generator :
         zetta::dataset::DatasetGeneratorFactory::builtin().generators()) {
        ImGui::PushID(generator->kind().c_str());
        ImGui::SeparatorText(generator->title().c_str());
        mono_line(kInkQuiet, "kind:  ", generator->kind());
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(generator->description().c_str());
        ImGui::PopTextWrapPos();

        if (primary_button("Generate")) {
            datagen_status_.clear();
            zetta::dataset::DatasetGenSpec spec;
            spec.variants_per_subject = datagen_variants_ < 1 ? 1 : datagen_variants_;
            const zetta::dataset::GeneratedDataset dataset = generator->generate(spec);
            const zetta::agent::AdmissionResult gate =
                zetta::agent::admit_corpus(dataset.cases);
            datagen_failed_ = !gate.admitted;
            if (!gate.admitted) {
                datagen_status_ =
                    std::string("Gate failed: ") +
                    std::string(zetta::agent::to_string(gate.failures.front().kind)) + " (task '" +
                    gate.failures.front().task_id + "')";
            } else {
                std::string line = "Generated " + std::to_string(dataset.cases.size()) +
                                   " gated cases over " +
                                   std::to_string(dataset.subjects.size()) + " subjects";
                // Write the JSONL when a path is given (the engine's reason surfaces on failure).
                if (!datagen_path_.empty()) {
                    std::ofstream file(datagen_path_);
                    if (!file) {
                        datagen_failed_ = true;
                        datagen_status_ = "Could not open '" + datagen_path_ + "' for writing";
                    } else {
                        file << zetta::agent::dump_corpus_jsonl(dataset.cases);
                        line += " \xE2\x86\x92 " + datagen_path_;
                    }
                }
                if (!datagen_failed_) datagen_status_ = line;
            }
        }
        ImGui::PopID();
    }
}

}  // namespace zetta::gui
