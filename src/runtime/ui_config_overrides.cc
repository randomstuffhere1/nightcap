// Modified by vii from komaruworld/mocktail. See README "About this fork".

#include "runtime/ui_config_overrides.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <yaml.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::size_t kMaximumUiConfigBytes = 64U * 1024U;
// Matches the loader's own limit for a device name, so a value the window
// accepts is one the loader will also accept.
constexpr std::size_t kMaximumFreeTextBytes = 512;
constexpr mode_t kUiConfigMode = 0600;

std::string Scalar(const yaml_node_t* node) {
  if (node == nullptr || node->type != YAML_SCALAR_NODE) {
    return {};
  }
  return std::string(reinterpret_cast<const char*>(node->data.scalar.value),
                     node->data.scalar.length);
}

// "graphics.vsync" to { "graphics", "vsync" }.
std::vector<std::string> SplitKey(std::string_view key) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= key.size()) {
    const std::size_t dot = key.find('.', start);
    if (dot == std::string_view::npos) {
      parts.emplace_back(key.substr(start));
      break;
    }
    parts.emplace_back(key.substr(start, dot - start));
    start = dot + 1;
  }
  return parts;
}

}  // namespace

const std::string* UiConfigOverrides::Find(std::string_view key) const {
  for (const auto& [existing, value] : values) {
    if (existing == key) {
      return &value;
    }
  }
  return nullptr;
}

void UiConfigOverrides::Set(std::string key, std::string value) {
  for (auto& [existing, current] : values) {
    if (existing == key) {
      current = std::move(value);
      return;
    }
  }
  values.emplace_back(std::move(key), std::move(value));
}

void UiConfigOverrides::Clear(std::string_view key) {
  values.erase(std::remove_if(values.begin(), values.end(),
                              [key](const auto& entry) {
                                return entry.first == key;
                              }),
               values.end());
}

std::vector<std::string> UiConfigTabs() {
  return {"Graphics", "Input", "Audio", "Integrations", "Advanced"};
}

std::vector<UiConfigField> UiConfigFields() {
  // Written with named members rather than aggregate initializers: a field
  // that gains a property would otherwise silently shift every value after it
  // and be caught only by a compile error a long way from the change.
  std::vector<UiConfigField> fields;

  const auto choice = [&fields](std::string key, std::string fallback,
                                std::vector<std::string> allowed,
                                std::string help) {
    UiConfigField field;
    field.key = std::move(key);
    field.default_value = std::move(fallback);
    field.allowed_values = std::move(allowed);
    field.help = std::move(help);
    fields.push_back(std::move(field));
  };
  const auto toggle = [&fields](std::string key, std::string fallback,
                                std::string help) {
    UiConfigField field;
    field.key = std::move(key);
    field.default_value = std::move(fallback);
    field.boolean = true;
    field.help = std::move(help);
    fields.push_back(std::move(field));
  };
  const auto number = [&fields](std::string key, std::string fallback,
                                std::int64_t minimum, std::int64_t maximum,
                                std::string help) {
    UiConfigField field;
    field.key = std::move(key);
    field.default_value = std::move(fallback);
    field.numeric = true;
    field.minimum = minimum;
    field.maximum = maximum;
    field.help = std::move(help);
    fields.push_back(std::move(field));
  };
  const auto text = [&fields](std::string key, std::string fallback,
                              std::string help) {
    UiConfigField field;
    field.key = std::move(key);
    field.default_value = std::move(fallback);
    field.free_text = true;
    field.help = std::move(help);
    fields.push_back(std::move(field));
  };

  // Graphics.
  choice("graphics.backend", "auto",
         {"auto", "system", "opengl", "gles", "vulkan", "angle-vulkan",
          "angle-swiftshader"},
         "How Nightcap talks to the GPU. Leave on auto unless Vulkan is "
         "failing on your hardware.");
  choice("graphics.frame_rate_limit", "-1",
         {"-1", "display", "30", "60", "90", "120", "144", "165", "240"},
         "display unlocks the frame rate to your monitor's rate. A number caps "
         "it. A frame cap hides frame-time gains, so record it when measuring.");
  choice("graphics.vsync", "auto", {"auto", "on", "off"},
         "off gives the lowest input lag, at the cost of tearing.");
  choice("graphics.quality", "auto", {"auto", "1", "2", "3", "4", "5", "6"},
         "Roblox's own graphics level. A higher level asks for more shadow and "
         "lighting work, so it lowers the frame rate rather than sharpening the "
         "image. auto leaves the choice to the client.");
  choice("graphics.pipeline_cache", "on", {"on", "off", "clear"},
         "A fallback copy of the driver's pipeline cache. The client already "
         "keeps its own, so on a stock client this does nothing. clear "
         "discards it and runs the next session uncached.");
  number("performance.texture_memory_mb", "0", 0, 2047,
         "Texture memory budget told to Roblox. 0 decides from system memory, "
         "which is usually right. The ceiling is 2047 because the client "
         "reads it through a signed 32-bit setting.");
  number("performance.small_texture_upscale", "4", 1, 8,
         "Redraws small compressed textures at N times their size so they are "
         "not blurry on a desktop display. Higher values cost GPU memory and "
         "bandwidth on every frame that uses one. 1 turns it off.");
  toggle("performance.multithreaded_rendering", "false",
         "Size Roblox's worker queues from every physical core. A place's Lua "
         "thread can still remain serial.");
  choice("performance.physics_worker_mode", "throughput",
         {"auto", "latency", "throughput"},
         "throughput gives every core to Roblox's worker pools. latency keeps "
         "Roblox's own pool sizes, which lowers CPU use at the cost of some "
         "physics throughput.");
  number("performance.memory_limit_mb", "0", 0, 131072,
         "Hard RAM cap for the process tree. 0 disables it.");
  choice("performance.gamemode", "auto", {"auto", "on", "off"},
         "Request Feral GameMode when its daemon is available.");

  // Input.
  toggle("input.raw_mouse", "false",
         "Send mouse look deltas in the host's own units. Turn this on when "
         "first-person aiming feels too slow or too fast.");
  toggle("input.touch_enabled", "false",
         "Report a touchscreen to the client. Only meaningful with a touch "
         "device profile.");
  toggle("compatibility.desktop_playability", "true",
         "Let the client believe it is on a desktop platform.");

  // Audio.
  text("audio.output_device", "default",
       "Leave on default to follow the system output. To pin one, copy the "
       "exact device name printed at startup.");
  text("audio.input_device", "default",
       "Microphone to use. Leave on default to follow the system.");
  number("audio.buffer_samples", "0", 0, 8192,
         "Intended playback buffer. SDL3 cannot set this per device, so the "
         "value is advisory: startup reports the size actually in use and "
         "reports a starved-refill count at exit.");

  // Integrations.
  toggle("integrations.discord_rpc.enabled", "true",
         "Show what you are playing in Discord.");
  toggle("integrations.discord_rpc.show_place_name", "true",
         "Include the experience's name in the Discord status.");
  toggle("integrations.discord_rpc.show_elapsed_time", "false",
         "Include how long you have been playing.");
  toggle("integrations.discord_rpc.join.enabled", "false",
         "Show a Join button in the Discord presence.");
  toggle("integrations.discord_rpc.join.public_servers_only", "true",
         "Restrict the Join button to public servers. Turning this off lets a "
         "Discord user request a private server link.");

  // Advanced.
  choice("appearance.theme", "roblox",
         {"roblox", "system", "light", "dark"}, "Window and dialog theme.");
  toggle("network.use_system_proxy", "true",
         "Use the desktop's proxy configuration.");
  toggle("runtime.headless", "false",
         "Start with no window. Useful for testing, not for playing.");

  return fields;
}

namespace {

const UiConfigField* FindField(std::string_view key) {
  static const std::vector<UiConfigField> fields = UiConfigFields();
  for (const UiConfigField& field : fields) {
    if (field.key == key) {
      return &field;
    }
  }
  return nullptr;
}

bool ParseBoundedNumber(std::string_view value, std::int64_t minimum,
                        std::int64_t maximum, std::int64_t* parsed) {
  if (value.empty() || value.size() > 20) {
    return false;
  }
  std::int64_t result = 0;
  const auto outcome =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (outcome.ec != std::errc() || outcome.ptr != value.data() + value.size()) {
    return false;
  }
  if (result < minimum || result > maximum) {
    return false;
  }
  *parsed = result;
  return true;
}

}  // namespace

bool ValidateUiConfigValue(std::string_view key, std::string_view value,
                           std::string* error) {
  const auto fail = [error](std::string message) {
    if (error != nullptr) {
      *error = std::move(message);
    }
    return false;
  };
  const UiConfigField* field = FindField(key);
  if (field == nullptr) {
    return fail("the settings window cannot write unknown key: " +
                std::string(key));
  }
  if (field->boolean) {
    if (value == "true" || value == "false") {
      return true;
    }
    return fail(std::string(key) + " must be true or false");
  }
  if (field->numeric) {
    std::int64_t parsed = 0;
    if (!ParseBoundedNumber(value, field->minimum, field->maximum, &parsed)) {
      return fail(std::string(key) + " must be a whole number from " +
                  std::to_string(field->minimum) + " to " +
                  std::to_string(field->maximum));
    }
    return true;
  }
  if (field->free_text) {
    // A device name only exists on the user's machine, so the window cannot
    // enumerate it. The length is still bounded because the value goes into
    // the environment and a config key.
    if (value.empty() || value.size() > kMaximumFreeTextBytes) {
      return fail(std::string(key) + " must be between 1 and " +
                  std::to_string(kMaximumFreeTextBytes) + " characters");
    }
    // A newline would corrupt the one-key-per-line file the writer produces.
    if (value.find('\n') != std::string_view::npos ||
        value.find('\r') != std::string_view::npos) {
      return fail(std::string(key) + " cannot contain a line break");
    }
    return true;
  }
  const std::unordered_set<std::string> allowed(field->allowed_values.begin(),
                                                field->allowed_values.end());
  if (allowed.find(std::string(value)) == allowed.end()) {
    std::string accepted;
    for (const std::string& candidate : field->allowed_values) {
      if (!accepted.empty()) {
        accepted += ", ";
      }
      accepted += candidate;
    }
    return fail(std::string(key) + " must be one of: " + accepted);
  }
  return true;
}

std::string RenderUiConfigYaml(const UiConfigOverrides& overrides) {
  // Only the keys the user set are written, grouped by section, so the file
  // stays short enough to read at a glance. It carries no comments by design:
  // the documentation lives in config.yaml and in the window itself.
  //
  // Sorted rather than written in insertion order, so toggling one setting does
  // not reshuffle the file and produce a confusing diff if it is ever kept
  // under version control.
  std::vector<std::pair<std::string, std::string>> sorted = overrides.values;
  std::sort(sorted.begin(), sorted.end());
  std::string document =
      "# Written by the Nightcap settings window. Edits here override\n"
      "# config.yaml, and both are overridden by the environment.\n"
      "# Clear a setting in the window to fall back to config.yaml.\n";
  std::string section;
  for (const auto& [key, value] : sorted) {
    const std::vector<std::string> parts = SplitKey(key);
    if (parts.size() < 2) {
      continue;
    }
    if (parts[0] != section) {
      section = parts[0];
      document += "\n" + section + ":\n";
    }
    const std::string leaf = parts.back();
    document += "  " + leaf + ": " + value + "\n";
  }
  return document;
}

namespace {

bool ReadWholeFile(const std::filesystem::path& path, std::string* contents,
                   std::string* error) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    *error = "cannot open " + path.string() + ": " + std::strerror(errno);
    return false;
  }
  char buffer[4096];
  while (true) {
    const ssize_t bytes = ::read(descriptor, buffer, sizeof(buffer));
    if (bytes < 0) {
      if (errno == EINTR) {
        continue;
      }
      *error = "cannot read " + path.string() + ": " + std::strerror(errno);
      close(descriptor);
      return false;
    }
    if (bytes == 0) {
      break;
    }
    if (contents->size() + static_cast<std::size_t>(bytes) >
        kMaximumUiConfigBytes) {
      *error = path.string() + " is larger than 64 KiB";
      close(descriptor);
      return false;
    }
    contents->append(buffer, static_cast<std::size_t>(bytes));
  }
  close(descriptor);
  return true;
}

}  // namespace

bool LoadUiConfigOverrides(const std::filesystem::path& file,
                           UiConfigOverrides* overrides, bool* file_loaded,
                           std::string* error) {
  if (overrides == nullptr) {
    return false;
  }
  overrides->values.clear();
  if (file_loaded != nullptr) {
    *file_loaded = false;
  }
  std::error_code filesystem_error;
  if (!std::filesystem::exists(file, filesystem_error) || filesystem_error) {
    // Never having opened the window is the normal case, not a failure.
    return true;
  }
  std::string contents;
  if (!ReadWholeFile(file, &contents, error)) {
    return false;
  }
  yaml_parser_t parser;
  if (!yaml_parser_initialize(&parser)) {
    if (error != nullptr) {
      *error = "cannot initialize YAML parser";
    }
    return false;
  }
  yaml_parser_set_input_string(
      &parser, reinterpret_cast<const unsigned char*>(contents.data()),
      contents.size());
  yaml_document_t document;
  if (yaml_parser_load(&parser, &document) == 0) {
    yaml_parser_delete(&parser);
    if (error != nullptr) {
      *error = "invalid YAML in " + file.string();
    }
    return false;
  }
  if (file_loaded != nullptr) {
    *file_loaded = true;
  }
  yaml_node_t* root = yaml_document_get_root_node(&document);
  bool valid = true;
  if (root == nullptr || root->type != YAML_MAPPING_NODE) {
    if (error != nullptr) {
      *error = file.string() + " must be a mapping of sections";
    }
    valid = false;
  }
  for (yaml_node_pair_t* pair =
       valid ? root->data.mapping.pairs.start : nullptr;
       valid && pair != root->data.mapping.pairs.top; ++pair) {
    const std::string section =
        Scalar(yaml_document_get_node(&document, pair->key));
    const yaml_node_t* body = yaml_document_get_node(&document, pair->value);
    if (section.empty() || body == nullptr ||
        body->type != YAML_MAPPING_NODE) {
      if (error != nullptr) {
        *error = file.string() + " has a section that is not a mapping";
      }
      valid = false;
      break;
    }
    for (yaml_node_pair_t* entry = body->data.mapping.pairs.start;
         entry != body->data.mapping.pairs.top; ++entry) {
      const std::string leaf =
          Scalar(yaml_document_get_node(&document, entry->key));
      const std::string value =
          Scalar(yaml_document_get_node(&document, entry->value));
      const std::string key = section + "." + leaf;
      if (key.empty() || leaf.empty()) {
        if (error != nullptr) {
          *error = file.string() + " has an empty key";
        }
        valid = false;
        break;
      }
      if (!ValidateUiConfigValue(key, value, error)) {
        valid = false;
        break;
      }
      overrides->Set(key, value);
    }
  }
  yaml_document_delete(&document);
  yaml_parser_delete(&parser);
  if (!valid) {
    // A file the window wrote must never be unreadable, so a bad value here
    // means a hand-edit. It is reported rather than ignored, because ignoring
    // it would silently drop settings the user believes are active.
    overrides->values.clear();
    if (file_loaded != nullptr) {
      *file_loaded = false;
    }
    return false;
  }
  return true;
}

bool SaveUiConfigOverrides(const std::filesystem::path& file,
                           const UiConfigOverrides& overrides,
                           std::string* error) {
  const auto fail = [error](std::string message) {
    if (error != nullptr) {
      *error = std::move(message);
    }
    return false;
  };
  // Everything is validated before anything is written, so a rejected value
  // cannot leave a half-saved file behind.
  for (const auto& [key, value] : overrides.values) {
    if (!ValidateUiConfigValue(key, value, error)) {
      return false;
    }
  }
  std::error_code filesystem_error;
  if (overrides.empty()) {
    // No settings left means no file. Leaving an empty one behind would make
    // "did I clear everything?" unanswerable from the filesystem.
    std::filesystem::remove(file, filesystem_error);
    return true;
  }
  const std::filesystem::path parent = file.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, filesystem_error);
    filesystem_error.clear();
  }
  const std::string document = RenderUiConfigYaml(overrides);
  // Written to a temporary file and renamed, so a crash mid-write cannot leave
  // a truncated file that fails to parse at next launch.
  const std::filesystem::path temporary = file.string() + ".tmp";
  const int descriptor =
      ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
             kUiConfigMode);
  if (descriptor < 0) {
    return fail("cannot write " + temporary.string() + ": " +
                std::strerror(errno));
  }
  std::size_t offset = 0;
  bool written = true;
  while (offset < document.size()) {
    const ssize_t bytes = ::write(descriptor, document.data() + offset,
                                  document.size() - offset);
    if (bytes < 0) {
      if (errno == EINTR) {
        continue;
      }
      written = false;
      break;
    }
    offset += static_cast<std::size_t>(bytes);
  }
  const bool synced = written && fsync(descriptor) == 0;
  close(descriptor);
  if (!synced) {
    std::error_code remove_error;
    std::filesystem::remove(temporary, remove_error);
    return fail("cannot write " + temporary.string());
  }
  std::filesystem::rename(temporary, file, filesystem_error);
  if (filesystem_error) {
    std::error_code remove_error;
    std::filesystem::remove(temporary, remove_error);
    return fail("cannot publish " + file.string() + ": " +
                filesystem_error.message());
  }
  return true;
}

}  // namespace runtime
}  // namespace mocktail
