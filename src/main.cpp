// zetta — entry point.
//
// Parses arguments, builds a zetta::Node, and dispatches to one of two run
// modes. Headed (the default) renders the node in an SDL3 + Dear ImGui window;
// headless runs the same node with no UI, for servers with no display. All the
// real work lives behind zetta::Node — main only picks how to drive it.

#include "zetta/headless.hpp"
#include "zetta/node.hpp"
#include "zetta/version.hpp"

#include "gui/app.hpp"

#include <cstdio>
#include <string>

namespace {

enum class Mode { Headed, Headless };

struct Options {
    Mode mode = Mode::Headed;  // headed by default
    zetta::NodeConfig node;
    bool show_help = false;
    bool bad_usage = false;
};

void print_usage(const char* argv0) {
    std::printf(
        "zetta %s — a database node\n\n"
        "Usage: %s [options]\n\n"
        "Options:\n"
        "  --headed         Run with the SDL3 + ImGui UI (default)\n"
        "  --headless       Run without a UI (server only)\n"
        "  --data-dir PATH  Database directory (default: %s)\n"
        "  -h, --help       Show this help\n",
        zetta::kVersion, argv0, zetta::NodeConfig{}.data_dir.c_str());
}

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--headless") {
            o.mode = Mode::Headless;
        } else if (arg == "--headed") {
            o.mode = Mode::Headed;
        } else if (arg == "--data-dir") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "zetta: --data-dir needs a path\n");
                o.bad_usage = true;
                break;
            }
            o.node.data_dir = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            o.show_help = true;
        } else {
            std::fprintf(stderr, "zetta: unknown option '%s'\n", arg.c_str());
            o.bad_usage = true;
            break;
        }
    }
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    const Options opts = parse_args(argc, argv);
    if (opts.bad_usage) {
        print_usage(argv[0]);
        return 2;
    }
    if (opts.show_help) {
        print_usage(argv[0]);
        return 0;
    }

    zetta::Node node(opts.node);
    if (!node.start()) {
        std::fprintf(stderr, "zetta: failed to start node\n");
        return 1;
    }

    int rc = 0;
    if (opts.mode == Mode::Headless) {
        rc = zetta::run_headless(node);
    } else {
        zetta::gui::App app(node);
        if (!app.init()) {
            node.stop();
            return 1;
        }
        app.run();
    }

    node.stop();
    return rc;
}
