/*******************************************************************************
The MIT License (MIT)

Copyright (c) 2014 Dmitry "Dima" Korolev <dmitry.korolev@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*******************************************************************************/

#ifndef BRICKS_STRINGS_SPLIT_H
#define BRICKS_STRINGS_SPLIT_H

#include <algorithm>
#include <cctype>
#include <vector>
#include <string>
#include <type_traits>

#include "../exception.h"
#include "../template/enable_if.h"
#include "../template/decay.h"
#include "../template/weed.h"

namespace current {
namespace strings {

// Skip: Skip empty chunks, Keep: Keep empty fields.
enum class EmptyFields { Skip, Keep };

// Silent:  Silently ignore the key-value pairs that can not be parsed.
// Throw:   Throw an exception if the input string is malfomed.
enum class KeyValueParsing { Silent, Throw };
enum class ByWhitespace { UseIsSpace };
enum class ByLines { Use0Aor0D };

struct KeyValueNoValueException : Exception {};
struct KeyValueMultipleValuesException : Exception {};

namespace impl {

template <typename T>
struct MatchImpl {};

template <>
struct MatchImpl<char> {
  inline static bool Match(char a, char b) { return a == b; }
};

template <>
struct MatchImpl<ByWhitespace> {
  inline static bool Match(char c, ByWhitespace) { return !!(::isspace(c)); }
};

template <>
struct MatchImpl<ByLines> {
  inline static bool Match(char c, ByLines) { return c == '\n' || c == '\r'; }
};

template <>
struct MatchImpl<std::string> {
  inline static bool Match(char c, const std::string& s) { return s.find(c) != std::string::npos; }
};

template <size_t N>
struct MatchImpl<const char[N]> {
  static ENABLE_IF<(N > 0), bool> Match(char c, const char s[N]) { return std::find(s, s + N, c) != (s + N); }
};

template <typename T>
inline ENABLE_IF<!weed::call_with<T, char>::implemented, bool> Match(char a, T&& b) {
  return MatchImpl<rmref<T>>::Match(a, std::forward<T>(b));
}

template <typename T>
inline ENABLE_IF<weed::call_with<T, char>::implemented, bool> Match(char a, T&& b) {
  return !b(a);
}

template <typename T>
struct DefaultSeparator {};

template <>
struct DefaultSeparator<ByWhitespace> {
  static inline ByWhitespace value() { return ByWhitespace::UseIsSpace; }
};

template <>
struct DefaultSeparator<ByLines> {
  static inline ByLines value() { return ByLines::Use0Aor0D; }
};

}  // namespace impl

template <typename T_SEPARATOR, typename T_PROCESSOR>
inline size_t Split(const std::string& s,
                    T_SEPARATOR&& separator,
                    T_PROCESSOR&& processor,
                    EmptyFields empty_fields_strategy = EmptyFields::Skip) {
  size_t i = 0;
  size_t j = 0;
  size_t n = 0;
  const auto emit = [&]() {
    if (empty_fields_strategy == EmptyFields::Keep || i != j) {
      ++n;
      processor(s.substr(j, i - j));
    }
  };
  for (i = 0; i < s.size(); ++i) {
    if (impl::Match(s[i], std::forward<T_SEPARATOR>(separator))) {
      emit();
      j = i + 1;
    }
  }
  emit();
  return n;
}

template <typename T_SEPARATOR>
inline std::vector<std::string> Split(const std::string& s,
                                      T_SEPARATOR&& separator = impl::DefaultSeparator<T_SEPARATOR>::value(),
                                      EmptyFields empty_fields_strategy = EmptyFields::Skip) {
  std::vector<std::string> result;
  Split(s,
        std::forward<T_SEPARATOR>(separator),
        [&result](std::string&& chunk) { result.emplace_back(std::move(chunk)); },
        empty_fields_strategy);
  return result;
}

template <typename T_KEY_VALUE_SEPARATOR, typename T_FIELDS_SEPARATOR>
inline std::vector<std::pair<std::string, std::string>> SplitIntoKeyValuePairs(
    const std::string& s,
    T_KEY_VALUE_SEPARATOR&& key_value_separator,
    T_FIELDS_SEPARATOR&& fields_separator = impl::DefaultSeparator<T_FIELDS_SEPARATOR>::value(),
    KeyValueParsing throw_mode = KeyValueParsing::Silent) {
  std::vector<std::pair<std::string, std::string>> result;
  Split(s,
        std::forward<T_FIELDS_SEPARATOR>(fields_separator),
        [&result, &key_value_separator, &throw_mode](std::string&& key_and_value_as_one_string) {
          const std::vector<std::string> key_and_value =
              Split(key_and_value_as_one_string, std::forward<T_KEY_VALUE_SEPARATOR>(key_value_separator));
          if (key_and_value.size() >= 2) {
            if (key_and_value.size() == 2) {
              result.emplace_back(key_and_value[0], key_and_value[1]);
            } else if (throw_mode == KeyValueParsing::Throw) {
              CURRENT_THROW(KeyValueMultipleValuesException());
            }
          } else {
            if (throw_mode == KeyValueParsing::Throw) {
              CURRENT_THROW(KeyValueNoValueException());
            }
          }
        });
  return result;
}

template <typename T_KEY_VALUE_SEPARATOR>
inline std::vector<std::pair<std::string, std::string>> SplitIntoKeyValuePairs(
    const std::string& s,
    T_KEY_VALUE_SEPARATOR&& key_value_separator,
    KeyValueParsing throw_mode = KeyValueParsing::Silent) {
  return SplitIntoKeyValuePairs(s, key_value_separator, ByWhitespace::UseIsSpace, throw_mode);
}

}  // namespace strings
}  // namespace current

#endif  // BRICKS_STRINGS_SPLIT_H
