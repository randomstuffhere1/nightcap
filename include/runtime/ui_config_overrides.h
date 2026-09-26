// Modified by vii from komaruworld/mocktail. See README "About this fork".
#ifndef MOCKTAIL_RUNTIME_UI_CONFIG_OVERRIDES_H_
#define MOCKTAIL_RUNTIME_UI_CONFIG_OVERRIDES_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mocktail {
namespace runtime {

// The settings window writes the values you change here rather than into
// config.yaml.
//
// config.yaml is created once, mode 0600, and is 74% comments that are its own
// documentation; the bootstrap refuses to follow a symlink at it and states
// that Nightcap never replaces your edits. Rewriting it from a UI would break
// that promise and throw away the documentation, and preserving the comments
// through a rewrite is a large amount of work for a file that is mostly
// comments. So the window keeps its own small file of only the keys you
// actually set, and config.yaml is left exactly as you wrote it.
//
// The two files layer, highest first:
//
//   1. the process environment, unchanged
//   2. config.ui.yaml, written by the settings window
//   3. config.yaml, hand-edited
//   4. built-in defaults
//
// The consequence worth knowing: a value the window set outranks a later
// hand-edit of the same key in config.yaml, because the window's file is the
// more deliberate of the two. Clear the setting in the window and the
// hand-edited value takes effect again. The window shows which values come
// from where for this reason.

// One setting the window can change, with the values it accepts. The accepted
// list is what lets a bad value be reported at save time rather than at next
// launch, which is the whole point of a settings window.
struct UiConfigField {
  // The dotted config.yaml path, such as "graphics.vsync".
  std::string key;
  // The value used when the user has not set this field. Empty means "do not
  // write this key at all", which is how a field the window does not manage is
  // left alone.
  std::string default_value;
  // Every value the field accepts, in the order a combo box should show them.
  // Empty for a boolean, and empty with `free_text` set for a value the window
  // cannot enumerate, such as an audio device name that only exists on the
  // user's machine.
  std::vector<std::string> allowed_values;
  // A free-text field accepts any value within a length limit. Used for device
  // names, which the window cannot know in advance; the user types the exact
  // name the app prints at startup.
  bool free_text = false;
  // A boolean field is rendered as a switch rather than a combo.
  bool boolean = false;
  // A field whose value is a whole number is rendered as a spin button and
  // range-checked between these, inclusive.
  bool numeric = false;
  std::int64_t minimum = 0;
  std::int64_t maximum = 0;
  // Shown under the control. Says what the setting does and, where relevant,
  // that it is a request rather than a measured improvement.
  std::string help;
};

// Every field the window manages, grouped by tab. This is the single list the
// loader, the writer and the UI all read, so a setting cannot be added to the
// window without also being added to the merge and to the tests.
std::vector<UiConfigField> UiConfigFields();
std::vector<std::string> UiConfigTabs();

// A validated set of values, keyed by the field's dotted path. Only fields the
// user has actually set are present; an absent field means "not set here", and
// never overwrites a lower layer.
struct UiConfigOverrides {
  std::vector<std::pair<std::string, std::string>> values;

  const std::string* Find(std::string_view key) const;
  void Set(std::string key, std::string value);
  void Clear(std::string_view key);
  bool empty() const { return values.empty(); }
};

// Rejects a value the named field does not accept, so a bad value is reported
// where it was typed rather than at next launch. An unknown key is an error
// too: the window must not be able to write a key the loader will reject.
bool ValidateUiConfigValue(std::string_view key, std::string_view value,
                           std::string* error);

// Reads config.ui.yaml. A missing file is not an error: it is what a user who
// has never opened the window has. Returns false only for a file that exists
// and cannot be understood, which is reported rather than silently ignored,
// since ignoring it would make the window's own settings vanish.
bool LoadUiConfigOverrides(const std::filesystem::path& file,
                           UiConfigOverrides* overrides, bool* file_loaded,
                           std::string* error);

// Writes config.ui.yaml atomically, creating its directory. An empty set
// removes the file instead of leaving an empty one behind, so a user who
// clears every setting is back to a clean state. The file is mode 0600 like
// config.yaml, since it can hold a proxy host.
bool SaveUiConfigOverrides(const std::filesystem::path& file,
                           const UiConfigOverrides& overrides,
                           std::string* error);

// Renders the file the writer would produce. Exposed so the exact bytes can be
// asserted in a test without writing anything, and so the format stays
// reviewable in one place.
std::string RenderUiConfigYaml(const UiConfigOverrides& overrides);

// The file name the settings window uses, next to config.yaml.
inline constexpr std::string_view kUiConfigFileName = "config.ui.yaml";

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_UI_CONFIG_OVERRIDES_H_
