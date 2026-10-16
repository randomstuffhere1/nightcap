#ifndef MOCKTAIL_RUNTIME_SECURE_CLEAR_H_
#define MOCKTAIL_RUNTIME_SECURE_CLEAR_H_

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace mocktail::runtime {

// Wipes sensitive buffers -- chat text, password fields, IME composition --
// before they are released.
//
// explicit_bzero rather than a loop through a volatile pointer. Both stop the
// compiler eliding the wipe, which is the entire point of the function, but
// volatile also forbids vectorising and unrolling, so the loop ran at roughly
// one byte per iteration: 8.3 million serial stores to clear a 1920x1080
// overlay buffer. explicit_bzero carries the same guarantee -- the compiler may
// not assume the memory is dead, so the call survives as a call -- and runs at
// memset speed.
//
// The string overloads swap in a fresh string rather than clear()ing, so the
// old buffer is freed only after it has been wiped and so the wipe covers the
// size the caller had rather than a capacity the buffer may no longer have.

inline void SecureClearBytes(std::vector<std::uint8_t>* bytes) {
  if (bytes == nullptr) {
    return;
  }
  if (!bytes->empty()) {
    explicit_bzero(bytes->data(), bytes->size());
  }
  bytes->clear();
}

inline void SecureClearText(std::string* text) {
  if (text == nullptr) {
    return;
  }
  if (!text->empty()) {
    explicit_bzero(&(*text)[0], text->size());
  }
  std::string empty;
  text->swap(empty);
}

inline void SecureClearTextInPlace(std::string* value) {
  if (value == nullptr) {
    return;
  }
  if (!value->empty()) {
    explicit_bzero(value->data(), value->size());
  }
  value->clear();
}

}  // namespace mocktail::runtime

#endif  // MOCKTAIL_RUNTIME_SECURE_CLEAR_H_
