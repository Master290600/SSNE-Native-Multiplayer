#pragma once
#include <atomic>
#include <filesystem>
#include <string>

namespace launcher {
// Apply only hash-bound sparse function edits to a separately generated tree.
// Verifies all input/output files and functions before committing output_dir.
// Existing output survives cancellation/validation failure; input is read-only.
// Recipe contains edits/hashes/header includes, never complete game bodies.
bool apply_translation_recipe(const std::filesystem::path& input_dir,
                              const std::filesystem::path& output_dir,
                              const std::filesystem::path& recipe,
                              std::atomic<bool>& cancel,std::string& error);
}
