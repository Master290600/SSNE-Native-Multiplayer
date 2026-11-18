#pragma once
#include "gc/editor_effect.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace mapedit {
using Matrix = std::array<float, 12>;
struct Object {
  uint32_t offset = 0, transform = 0, type = 0;
  std::string name;
  bool added = false;
  Matrix initial{};
};
struct WaveRow {
  uint32_t enemy_offset = 0;
  unsigned count = 3;
};
struct WaveTrigger {
  float radius = 5, delay = 0;
  bool repeat = false;
  unsigned max_activations = 16;
  std::vector<WaveRow> rows;
};
enum class LogicAction { Trigger, Enable, Disable };
enum class LogicKind { Area, Relay, Counter, Timer, Gate, KeyBranch, Button, Toggle, Light, Explosion };
struct LogicSpec {
  LogicKind kind = LogicKind::Relay;
  std::string name = "New logic";
  bool enabled = true, start_on_load = false, reusable = true;
  bool repeat = false;
  unsigned threshold = 2, activation_limit = 0;
  float delay = 1, radius = 3, poll_interval = .1f;
  int key = 0;
  gc::editor::Effect effect;
  bool operator==(const LogicSpec &) const = default;
};
const char *logic_kind_name(LogicKind);
bool logic_target_supports(uint32_t type, LogicAction action);
bool logic_template_type(uint32_t type);
size_t logic_template_config_size(uint32_t type);
struct LogicSlot {
  uint32_t source = 0, owner = 0;
  unsigned slot = 0;
  std::string label;
  std::optional<uint32_t> target;
  LogicAction action = LogicAction::Trigger;
  bool action_editable = false;
};
bool wave_helper(uint32_t, const std::string &);
class Document {
public:
  void load(const std::filesystem::path &);
  void parse(std::vector<uint8_t>);
  const std::vector<Object> &objects() const { return objects_; }
  Matrix matrix(size_t index) const;
  Matrix original_matrix(size_t index) const;
  void set_matrix(size_t index, const Matrix &);
  uint32_t transform_type(size_t index) const;
  void move(size_t index, std::array<float, 3>);
  void rotate_y(size_t index, float degrees);
  bool can_duplicate(size_t) const;
  bool can_erase(size_t) const;
  size_t duplicate(size_t, std::array<float, 3>, bool place_active = true);
  void erase(size_t);
  size_t import_object(const Document &, size_t, std::array<float, 3>);
  uint64_t structure_revision() const { return structure_revision_; }
  bool structural() const;
  size_t add_wave_trigger(std::array<float, 3>, const WaveTrigger &);
  void set_wave_trigger(size_t, const WaveTrigger &);
  std::optional<WaveTrigger> wave_trigger(size_t) const;
  // Stable authored connection slots. Editor areas expose their private
  // relay's outputs; their internal area-to-relay link stays protected.
  std::vector<LogicSlot> logic_slots(uint32_t source_id) const;
  bool can_logic_target(uint32_t target_id, LogicAction) const;
  void add_logic_link(uint32_t source_id, uint32_t target_id, LogicAction);
  void set_logic_link(uint32_t source_id, unsigned slot,
                      std::optional<uint32_t> target_id, LogicAction);
  uint32_t link_wave_enemy(uint32_t source_id, uint32_t enemy_offset,
                           unsigned count = 3);
  size_t add_logic(std::array<float, 3>, const LogicSpec &);
  // Converts a proven original switch brush to a portable native model and
  // pairs it with the original interactive switch actor and output relay.
  size_t add_button(const Document &original, size_t brush,
                    std::array<float, 3>, const LogicSpec &);
  std::optional<LogicSpec> logic_settings(uint32_t source_id) const;
  void set_logic_settings(uint32_t source_id, const LogicSpec &);
  bool is_editor_logic(size_t index) const;
  bool is_logic_visual(size_t index) const;
  // Returns the paired 4180 controller for a private visible button mesh.
  // Unpaired objects return -1; this never guesses ownership from a name.
  int logic_owner_index(size_t index) const;
  bool undo();
  bool redo();
  bool can_undo() const { return !undo_.empty(); }
  bool can_redo() const { return !redo_.empty(); }
  bool changed() const {
    return data_ != saved_ || relocations_ != saved_relocations_;
  }
  void mark_saved() {
    saved_ = data_;
    saved_relocations_ = relocations_;
  }
  void clear_history() {
    undo_.clear();
    redo_.clear();
  }
  size_t edited_objects() const;
  std::vector<uint8_t> export_bytes() const;
  void write_copy(const std::filesystem::path &) const;
  const std::filesystem::path &source() const { return source_; }
  std::string source_hash() const;
  const std::vector<uint8_t> &payload() const { return data_; }

private:
  friend struct Project;
  struct Patch {
    uint32_t offset;
    std::vector<uint8_t> before, after;
  };
  struct ImportSegment {
    uint32_t source, end, target;
  };
  struct ImportedMap {
    std::string hash;
    uint32_t size;
    std::vector<ImportSegment> segments;
  };
  std::vector<ImportedMap> imports_;
  struct Change {
    std::vector<ImportedMap> before_imports, after_imports;
    size_t before_size = 0, after_size = 0;
    std::vector<Patch> patches;
    std::vector<uint8_t> tail;
    std::vector<Object> before_objects, after_objects;
    std::vector<uint32_t> added_relocations, removed_relocations;
    bool structural = false;
  };
  std::filesystem::path source_;
  std::vector<uint8_t> file_, base_, data_, saved_;
  std::vector<Object> objects_;
  std::vector<Change> undo_, redo_;
  std::set<uint32_t> relocations_, base_relocations_, saved_relocations_;
  uint64_t structure_revision_ = 0;
  uint32_t next_identifier_ = 0;
  void put(uint32_t, const Matrix &);
  void transaction(const std::function<void()> &, bool structural = false);
  void apply(const Change &, bool forward);
  void rebuild_objects();
  uint32_t copy_block(uint32_t, size_t);
  uint32_t append_string(const std::string &);
  void pointer(uint32_t, uint32_t);
  uint32_t allocate_identifier();
  uint32_t clone_entity(uint32_t, std::array<float, 3>, bool active,
                        uint32_t external_visual = 0,
                        uint32_t external_destruction = 0);
  void move_prop_bounds(size_t, const Matrix &, const Matrix &);
  uint32_t append_zero(size_t);
  uint32_t append_entity(uint32_t, const std::string &, uint32_t, uint32_t);
  void unlink_entity(uint32_t);
  void write_wave(uint32_t, const WaveTrigger &);
  void erase_wave(size_t);
  void adopt_snapshot(std::vector<uint8_t>);
  uint32_t own_logic_config(uint32_t entity, size_t minimum);
  unsigned transaction_depth_ = 0;
  mutable std::map<uint32_t,uint32_t> logic_lookup_;
  mutable bool logic_lookup_valid_ = false;
  void write_logic(uint32_t entity, const LogicSpec &);
  size_t duplicate_editor_logic(size_t, std::array<float, 3>);
  void erase_editor_logic(size_t);
  void copy_camera_path(uint32_t entity, std::array<float,3> delta, bool detach_events);
  void move_logic_path(size_t index, const Matrix &before, const Matrix &after);
};
const char *category(uint32_t type);
uint32_t adler32(const std::vector<uint8_t> &);
std::vector<uint8_t> stored_zlib(const std::vector<uint8_t> &);
void atomic_write(const std::filesystem::path &, const std::vector<uint8_t> &);
struct Project {
  std::filesystem::path folder;
  std::string level, title;
  static Project create(const std::filesystem::path &projects,
                        const std::string &name, const std::string &level);
  void save(Document &) const;
  static Project open(const std::filesystem::path &folder,
                      const std::filesystem::path &original_data, Document &);
  // Files in this private test tree share read-only assets, but the edited
  // map is always an independently replaced file. No original is opened for
  // writing.
  std::filesystem::path prepare_test(const std::filesystem::path &original_data,
                                     const std::filesystem::path &game,
                                     const std::filesystem::path &card) const;
  std::filesystem::path
  prepare_survival_test(const std::filesystem::path &,
                        const std::filesystem::path &,
                        const std::filesystem::path &) const;
};
} // namespace mapedit
