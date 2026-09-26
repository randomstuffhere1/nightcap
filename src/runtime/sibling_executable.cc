// Modified by vii from komaruworld/mocktail. See README "About this fork".

#include "runtime/sibling_executable.h"

#include <unistd.h>

#include <system_error>

namespace mocktail::runtime {

std::filesystem::path SiblingExecutableFor(
    const std::filesystem::path& self_path, std::string_view program_name) {
  if (self_path.empty() || program_name.empty()) {
    return {};
  }
  // read_symlink rather than a plain read: /proc/self/exe is itself a symlink to
  // the real binary, and on a few systems it points through an intermediate
  // path. Resolving it first means the sibling is looked for beside the program
  // that is actually running.
  std::error_code error;
  std::filesystem::path resolved = std::filesystem::read_symlink(self_path, error);
  if (error) {
    resolved = self_path;
  }
  const std::filesystem::path candidate =
      resolved.parent_path() / std::string(program_name);
  if (!std::filesystem::exists(candidate, error) || error) {
    return {};
  }
  return candidate;
}

std::filesystem::path SiblingExecutable(std::string_view program_name) {
  return SiblingExecutableFor("/proc/self/exe", program_name);
}

}  // namespace mocktail::runtime
