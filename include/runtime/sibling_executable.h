// Modified by vii from komaruworld/mocktail. See README "About this fork".
#ifndef MOCKTAIL_RUNTIME_SIBLING_EXECUTABLE_H_
#define MOCKTAIL_RUNTIME_SIBLING_EXECUTABLE_H_

#include <filesystem>
#include <string_view>

namespace mocktail::runtime {

// The path of a program installed into the same directory as this one.
//
// Nightcap and its helpers are installed side by side, and two of them need to
// find each other: `mocktail --settings` execs mocktail_settings, and the
// settings window's Restart button execs mocktail back. Resolving that from
// /proc/self/exe rather than from PATH is deliberate, because the two may have
// been launched from a desktop entry or a session whose PATH lacks the install
// prefix.
//
// Returns an empty path when the sibling does not exist, so a caller reports a
// missing program rather than exec'ing something and failing obscurely.
std::filesystem::path SiblingExecutable(std::string_view program_name);

// The same lookup for a known self path, which is what makes it testable
// without a process to interrogate.
std::filesystem::path SiblingExecutableFor(
    const std::filesystem::path& self_path, std::string_view program_name);

}  // namespace mocktail::runtime
#endif  // MOCKTAIL_RUNTIME_SIBLING_EXECUTABLE_H_
