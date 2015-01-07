/*******************************************************************************
The MIT License (MIT)

Copyright (c) 2014 Dmitry "Dima" Korolev, <dmitry.korolev@gmail.com>.

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

#ifndef BRICKS_NET_URL_URL_H
#define BRICKS_NET_URL_URL_H

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../../exception.h"
#include "../../strings/printf.h"
#include "../../strings/split.h"

namespace bricks {
namespace net {
namespace url {

struct EmptyURLException : Exception {};
// struct EmptyURLHostException : Exception {};

// URL manages the mapping between the string and parsed representations of the URL. It manages:
//
// * host    (string)
// * path    (string, defaults to "/", never empty.)
// * scheme  (defaults to "http", empty only if set explicitly in constructor.)
// * port    (defaults to the default port for supported schemes, zero/unset for unknown ones.)
//
// When handling redirects, the previous URL can be provided to properly handle host/port/scheme.

namespace impl {

namespace {
const char* const kDefaultScheme = "http";
}

struct URLWithoutParameters {
  std::string host = "";
  std::string path = "/";
  std::string scheme = kDefaultScheme;
  int port = 0;

  URLWithoutParameters() = default;

  // Extra parameters for previous host and port are provided in the constructor to handle redirects.
  URLWithoutParameters(const std::string& url,
                       const std::string& previous_scheme = kDefaultScheme,
                       const std::string& previous_host = "",
                       const int previous_port = 0) {
    if (url.empty()) {
      throw EmptyURLException();
    }
    scheme = "";
    size_t offset_past_scheme = 0;
    const size_t i = url.find("://");
    if (i != std::string::npos) {
      scheme = url.substr(0, i);
      offset_past_scheme = i + 3;
    }

    // TODO(dkorolev): Support `http://user:pass@host:80/` in the future.
    const size_t colon = url.find(':', offset_past_scheme);
    const size_t slash = url.find('/', offset_past_scheme);
    host = url.substr(offset_past_scheme, std::min(colon, slash) - offset_past_scheme);
    if (host.empty()) {
      host = previous_host;
    }

    if (colon < slash) {
      port = atoi(url.c_str() + colon + 1);
    } else {
      port = previous_port;
    }

    if (slash != std::string::npos) {
      path = url.substr(slash);
    } else {
      path = "";
    }
    if (path.empty()) {
      path = "/";
    }

    if (scheme.empty()) {
      if (!previous_scheme.empty()) {
        scheme = previous_scheme;
      } else {
        scheme = DefaultSchemeForPort(port);
      }
    }

    if (port == 0) {
      port = DefaultPortForScheme(scheme);
    }
  }

  URLWithoutParameters(const std::string& url, const URLWithoutParameters& previous)
      : URLWithoutParameters(url, previous.scheme, previous.host, previous.port) {}

  std::string ComposeURL() const {
    if (!host.empty()) {
      std::ostringstream os;
      if (!scheme.empty()) {
        os << scheme << "://";
      }
      os << host;
      if (port != DefaultPortForScheme(scheme)) {
        os << ':' << port;
      }
      os << path;
      return os.str();
    } else {
      // If no host is specified, it's just the path: No need to put scheme and port.
      return path;
    }
  }

  static int DefaultPortForScheme(const std::string& scheme) {
    // We don't really "support" other schemes yet -- D.K.
    if (scheme == "http") {
      return 80;
    } else if (scheme == "https") {
      return 443;
    } else {
      return 0;
    }
  }

  static std::string DefaultSchemeForPort(int port) { return port == 80 ? "http" : ""; }
};

struct URLParameters {
  URLParameters() = default;
  URLParameters(std::string url) {
    const size_t pound_sign_index = url.find('#');
    if (pound_sign_index != std::string::npos) {
      fragment = url.substr(pound_sign_index + 1);
      url = url.substr(0, pound_sign_index);
    }
    const size_t question_mark_index = url.find('?');
    if (question_mark_index != std::string::npos) {
      parameters_vector = strings::SplitIntoKeyValuePairs(url.substr(question_mark_index + 1), '=', '&');
      for (auto& it : parameters_vector) {
        it.second = DecodeURIComponent(it.second);
      }
      parameters.insert(parameters_vector.begin(), parameters_vector.end());
      url = url.substr(0, question_mark_index);
    }
    url_without_parameters = url;
  }

  std::string operator()(const std::string& key, const std::string& default_value) const {
    const auto cit = parameters.find(key);
    if (cit != parameters.end()) {
      return cit->second;
    } else {
      return default_value;
    }
  }

  std::string operator[](const std::string& key) const { return operator()(key, ""); }

  bool HasParameter(const std::string& key) const { return parameters.find(key) != parameters.end(); }

  static std::string DecodeURIComponent(const std::string& encoded) {
    std::string decoded;
    for (size_t i = 0; i < encoded.length(); ++i) {
      if (i + 3 <= encoded.length() && encoded[i] == '%') {
        decoded += static_cast<char>(std::stoi(encoded.substr(i + 1, 2).c_str(), nullptr, 16));
        i += 2;
      } else {
        decoded += encoded[i];
      }
    }
    return decoded;
  }

  static std::string EncodeURIComponent(const std::string& decoded) {
    std::string encoded;
    for (const char c : decoded) {
      if (::isalpha(c) || ::isdigit(c)) {
        encoded += c;
      } else {
        encoded += strings::Printf("%%%02X", static_cast<int>(c));
      }
    }
    return encoded;
  }

  std::string ComposeParameters() const {
    std::string composed_parameters;
    if (!parameters_vector.empty()) {
      for (size_t i = 0; i < parameters_vector.size(); ++i) {
        composed_parameters += "?&"[i];
        composed_parameters += EncodeURIComponent(parameters_vector[i].first) + '=' +
                               EncodeURIComponent(parameters_vector[i].second);
      }
    }
    if (!fragment.empty()) {
      composed_parameters += "#" + fragment;
    }
    return composed_parameters;
  }

  std::vector<std::pair<std::string, std::string>> parameters_vector;
  std::map<std::string, std::string> parameters;
  std::string fragment;
  std::string url_without_parameters;
};

struct URL : URLParameters, URLWithoutParameters {
  URL() = default;

  // Extra parameters for previous host and port are provided in the constructor to handle redirects.
  URL(const std::string& url,
      const std::string& previous_scheme = kDefaultScheme,
      const std::string& previous_host = "",
      const int previous_port = 0)
      : URLParameters(url),
        URLWithoutParameters(
            URLParameters::url_without_parameters, previous_scheme, previous_host, previous_port) {}

  URL(const std::string& url, const URLWithoutParameters& previous)
      : URLParameters(url), URLWithoutParameters(URLParameters::url_without_parameters, previous) {}

  std::string ComposeURL() const {
    return URLWithoutParameters::ComposeURL() + URLParameters::ComposeParameters();
  }
};

}  // namespace impl

typedef impl::URL URL;

}  // namespace url
}  // namespace net
}  // namespace bricks

#endif  // BRICKS_NET_URL_URL_H
