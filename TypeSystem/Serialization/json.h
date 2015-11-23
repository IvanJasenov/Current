/*******************************************************************************
The MIT License (MIT)

Copyright (c) 2015 Dmitry "Dima" Korolev <dmitry.korolev@gmail.com>

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

#ifndef CURRENT_TYPE_SYSTEM_SERIALIZATION_JSON_H
#define CURRENT_TYPE_SYSTEM_SERIALIZATION_JSON_H

#include <type_traits>
#include <limits>

#include "exceptions.h"

#include "../Reflection/reflection.h"

#include "../../Bricks/template/enable_if.h"
#include "../../Bricks/template/decay.h"
#include "../../Bricks/strings/strings.h"

// TODO(dkorolev): Use RapidJSON from outside Cereal.
#include "../../3rdparty/cereal/include/external/rapidjson/document.h"
#include "../../3rdparty/cereal/include/external/rapidjson/prettywriter.h"
#include "../../3rdparty/cereal/include/external/rapidjson/genericstream.h"

namespace current {
namespace serialization {

template <typename T>
struct AssignToRapidJSONValueImpl {
  static void WithDedicatedStringTreatment(rapidjson::Value& destination, const T& value) {
    destination = value;
  }
};

template <>
struct AssignToRapidJSONValueImpl<std::string> {
  static void WithDedicatedStringTreatment(rapidjson::Value& destination, const std::string& value) {
    destination.SetString(value.c_str(), value.length());
  }
};

template <typename T>
void AssignToRapidJSONValue(rapidjson::Value& destination, const T& value) {
  AssignToRapidJSONValueImpl<T>::WithDedicatedStringTreatment(destination, value);
}

template <typename T>
struct SaveIntoJSONImpl;

#define CURRENT_DECLARE_PRIMITIVE_TYPE(unused_typeid_index, cpp_type, unused_current_type) \
  template <>                                                                              \
  struct SaveIntoJSONImpl<cpp_type> {                                                      \
    static void Save(rapidjson::Value& destination,                                        \
                     rapidjson::Document::AllocatorType&,                                  \
                     const cpp_type& value) {                                              \
      AssignToRapidJSONValue(destination, value);                                          \
    }                                                                                      \
  };
#include "../primitive_types.dsl.h"
#undef CURRENT_DECLARE_PRIMITIVE_TYPE

template <typename T>
struct SaveIntoJSONImpl<std::vector<T>> {
  static void Save(rapidjson::Value& destination,
                   rapidjson::Document::AllocatorType& allocator,
                   const std::vector<T>& value) {
    destination.SetArray();
    rapidjson::Value element_to_push;
    for (const auto& element : value) {
      SaveIntoJSONImpl<T>::Save(element_to_push, allocator, element);
      destination.PushBack(element_to_push, allocator);
    }
  }
};

template <typename TF, typename TS>
struct SaveIntoJSONImpl<std::pair<TF, TS>> {
  static void Save(rapidjson::Value& destination,
                   rapidjson::Document::AllocatorType& allocator,
                   const std::pair<TF, TS>& value) {
    destination.SetArray();
    rapidjson::Value first_value;
    rapidjson::Value second_value;
    SaveIntoJSONImpl<TF>::Save(first_value, allocator, value.first);
    SaveIntoJSONImpl<TS>::Save(second_value, allocator, value.second);
    destination.PushBack(first_value, allocator);
    destination.PushBack(second_value, allocator);
  }
};

template <typename TK, typename TV>
struct SaveIntoJSONImpl<std::map<TK, TV>> {
  template <typename K = TK>
  static ENABLE_IF<std::is_same<K, std::string>::value> Save(rapidjson::Value& destination,
                                                             rapidjson::Document::AllocatorType& allocator,
                                                             const std::map<TK, TV>& value) {
    destination.SetObject();
    for (const auto& element : value) {
      rapidjson::Value populated_value;
      SaveIntoJSONImpl<TV>::Save(populated_value, allocator, element.second);
      // TODO(dkorolev): Be careful with this `.c_str()`; might have to be allocated within `allocator`.
      destination.AddMember(element.first.c_str(), populated_value, allocator);
    }
  }
  template <typename K = TK>
  static ENABLE_IF<!std::is_same<K, std::string>::value> Save(rapidjson::Value& destination,
                                                              rapidjson::Document::AllocatorType& allocator,
                                                              const std::map<TK, TV>& value) {
    destination.SetArray();
    for (const auto& element : value) {
      rapidjson::Value key_value_as_array;
      key_value_as_array.SetArray();
      rapidjson::Value populated_key;
      rapidjson::Value populated_value;
      SaveIntoJSONImpl<TK>::Save(populated_key, allocator, element.first);
      SaveIntoJSONImpl<TV>::Save(populated_value, allocator, element.second);
      key_value_as_array.PushBack(populated_key, allocator);
      key_value_as_array.PushBack(populated_value, allocator);
      destination.PushBack(key_value_as_array, allocator);
    }
  }
};

template <typename T>
struct SaveIntoJSONImpl<Optional<T>> {
  static void Save(rapidjson::Value& destination,
                   rapidjson::Document::AllocatorType& allocator,
                   const Optional<T>& value) {
    if (Exists(value)) {
      SaveIntoJSONImpl<T>::Save(destination, allocator, Value(value));
    } else {
      // Current's default JSON parser would accept a missing field as well for no value,
      // but output it as `null` nonetheless, for clarity.
      destination.SetNull_();
    }
  }
};

// TODO(dkorolev): A smart `enable_if` to not treat any non-primitive type as a `CURRENT_STRUCT`?
template <typename T>
struct SaveIntoJSONImpl {
  struct SaveFieldVisitor {
    rapidjson::Value& destination_;
    rapidjson::Document::AllocatorType& allocator_;

    SaveFieldVisitor(rapidjson::Value& destination, rapidjson::Document::AllocatorType& allocator)
        : destination_(destination), allocator_(allocator) {}

    // IMPORTANT: Pass in `const char* name`, as `const std::string& name`
    // would fail memory-allocation-wise due to over-smartness of RapidJSON.
    template <typename U>
    void operator()(const char* name, const U& source) const {
      rapidjson::Value placeholder;
      SaveIntoJSONImpl<U>::Save(placeholder, allocator_, source);
      destination_.AddMember(name, placeholder, allocator_);
    }
  };

  // `CURRENT_STRUCT`.
  template <typename TT = T>
  static ENABLE_IF<IS_CURRENT_STRUCT(TT)> Save(rapidjson::Value& destination,
                                               rapidjson::Document::AllocatorType& allocator,
                                               const T& source) {
    destination.SetObject();

    SaveFieldVisitor visitor(destination, allocator);
    current::reflection::VisitAllFields<bricks::decay<T>,
                                        current::reflection::FieldNameAndImmutableValue>::WithObject(source,
                                                                                                     visitor);
  }

  // `enum` and `enum class`.
  template <typename TT = T>
  static ENABLE_IF<std::is_enum<TT>::value> Save(rapidjson::Value& destination,
                                                 rapidjson::Document::AllocatorType&,
                                                 const T& value) {
    AssignToRapidJSONValue(destination, static_cast<typename std::underlying_type<T>::type>(value));
  }
};

template <typename T>
std::string CreateJSONViaRapidJSON(const T& value) {
  rapidjson::Document document;
  rapidjson::Value& destination = document;

  SaveIntoJSONImpl<T>::Save(destination, document.GetAllocator(), value);

  std::ostringstream os;
  auto stream = rapidjson::GenericWriteStream(os);
  auto writer = rapidjson::Writer<rapidjson::GenericWriteStream>(stream);
  document.Accept(writer);

  return os.str();
}

// TODO(dkorolev): A smart `enable_if` to not treat any non-primitive type as a `CURRENT_STRUCT`?
template <typename T>
struct LoadFromJSONImpl {
  struct LoadFieldVisitor {
    rapidjson::Value& source_;
    const std::string& path_;

    explicit LoadFieldVisitor(rapidjson::Value& source, const std::string& path)
        : source_(source), path_(path) {}

    // IMPORTANT: Pass in `const char* name`, as `const std::string& name`
    // would fail memory-allocation-wise due to over-smartness of RapidJSON.
    template <typename U>
    void operator()(const char* name, U& value) const {
      if (source_.HasMember(name)) {
        LoadFromJSONImpl<U>::Load(&source_[name], value, path_ + '.' + name);
      } else {
        LoadFromJSONImpl<U>::Load(nullptr, value, path_ + '.' + name);
      }
    }
  };

  // `CURRENT_STRUCT`.
  template <typename TT = T>
  static ENABLE_IF<IS_CURRENT_STRUCT(TT)> Load(rapidjson::Value* source,
                                               T& destination,
                                               const std::string& path) {
    //  static void Load(rapidjson::Value* source, T& destination, const std::string& path) {
    if (source && source->IsObject()) {
      LoadFieldVisitor visitor(*source, path);
      current::reflection::VisitAllFields<bricks::decay<T>, current::reflection::FieldNameAndMutableValue>::
          WithObject(destination, visitor);
    } else {
      throw JSONSchemaException("object", source, path);
    }
  }

  // `uint*_t`.
  template <typename TT = T>
  static ENABLE_IF<std::numeric_limits<TT>::is_integer && !std::numeric_limits<TT>::is_signed &&
                   !std::is_same<TT, bool>::value>
  Load(rapidjson::Value* source, T& destination, const std::string& path) {
    if (source && source->IsNumber()) {
      destination = static_cast<T>(source->GetUint64());
    } else {
      throw JSONSchemaException("number", source, path);
    }
  }

  // `int*_t`
  template <typename TT = T>
  static ENABLE_IF<std::numeric_limits<TT>::is_integer && std::numeric_limits<TT>::is_signed> Load(
      rapidjson::Value* source, T& destination, const std::string& path) {
    if (source && source->IsNumber()) {
      destination = static_cast<T>(source->GetInt64());
    } else {
      throw JSONSchemaException("number", source, path);
    }
  }

  // `float`.
  static void Load(rapidjson::Value* source, float& destination, const std::string& path) {
    if (source && source->IsNumber()) {
      destination = static_cast<float>(source->GetDouble());
    } else {
      throw JSONSchemaException("float", source, path);
    }
  }

  // `double`.
  static void Load(rapidjson::Value* source, double& destination, const std::string& path) {
    if (source && source->IsNumber()) {
      destination = source->GetDouble();
    } else {
      throw JSONSchemaException("double", source, path);
    }
  }

  // `enum` and `enum class`.
  template <typename TT = T>
  static ENABLE_IF<std::is_enum<TT>::value> Load(rapidjson::Value* source,
                                                 T& destination,
                                                 const std::string& path) {
    if (source && source->IsNumber()) {
      if (std::numeric_limits<typename std::underlying_type<T>::type>::is_signed) {
        destination = static_cast<T>(source->GetInt64());
      } else {
        destination = static_cast<T>(source->GetUint64());
      }
    } else {
      throw JSONSchemaException("number", source, path);
    }
  }
};

template <>
struct LoadFromJSONImpl<std::string> {
  static void Load(rapidjson::Value* source, std::string& destination, const std::string& path) {
    if (source && source->IsString()) {
      destination.assign(source->GetString(), source->GetStringLength());
    } else {
      throw JSONSchemaException("string", source, path);
    }
  }
};

template <>
struct LoadFromJSONImpl<bool> {
  static void Load(rapidjson::Value* source, bool& destination, const std::string& path) {
    if (source && (source->IsTrue() || source->IsFalse())) {
      destination = source->IsTrue();
    } else {
      throw JSONSchemaException("bool", source, path);
    }
  }
};

template <typename T>
struct LoadFromJSONImpl<std::vector<T>> {
  static void Load(rapidjson::Value* source, std::vector<T>& destination, const std::string& path) {
    if (source && source->IsArray()) {
      const size_t n = source->Size();
      destination.resize(n);
      for (rapidjson::SizeType i = 0; i < static_cast<rapidjson::SizeType>(n); ++i) {
        LoadFromJSONImpl<T>::Load(&((*source)[i]), destination[i], path + '[' + std::to_string(i) + ']');
      }
    } else {
      throw JSONSchemaException("array", source, path);
    }
  }
};

template <typename TF, typename TS>
struct LoadFromJSONImpl<std::pair<TF, TS>> {
  static void Load(rapidjson::Value* source, std::pair<TF, TS>& destination, const std::string& path) {
    if (source && source->IsArray() && source->Size() == 2u) {
      LoadFromJSONImpl<TF>::Load(&((*source)[static_cast<rapidjson::SizeType>(0)]), destination.first, path);
      LoadFromJSONImpl<TS>::Load(&((*source)[static_cast<rapidjson::SizeType>(1)]), destination.second, path);
    } else {
      throw JSONSchemaException("pair as array", source, path);
    }
  }
};

template <typename TK, typename TV>
struct LoadFromJSONImpl<std::map<TK, TV>> {
  template <typename K = TK>
  static ENABLE_IF<std::is_same<std::string, K>::value> Load(rapidjson::Value* source,
                                                             std::map<TK, TV>& destination,
                                                             const std::string& path) {
    if (source && source->IsObject()) {
      destination.clear();
      for (rapidjson::Value::MemberIterator cit = source->MemberBegin(); cit != source->MemberEnd(); ++cit) {
        std::pair<TK, TV> entry;  // TODO(dkorolev): Investigate.
        LoadFromJSONImpl<TK>::Load(&cit->name, entry.first, path);
        LoadFromJSONImpl<TV>::Load(&cit->value, entry.second, path);
        destination.insert(entry);
      }
    } else {
      throw JSONSchemaException("map as object", source, path);
    }
  }
  template <typename K = TK>
  static ENABLE_IF<!std::is_same<std::string, K>::value> Load(rapidjson::Value* source,
                                                              std::map<TK, TV>& destination,
                                                              const std::string& path) {
    if (source && source->IsArray()) {
      destination.clear();
      for (rapidjson::Value::ValueIterator cit = source->Begin(); cit != source->End(); ++cit) {
        std::pair<TK, TV> entry;  // TODO(dkorolev): Investigate.
        if (!cit->IsArray()) {
          throw JSONSchemaException("map entry as array", source, path);
        }
        if (cit->Size() != 2u) {
          throw JSONSchemaException("map entry as array of two elements", source, path);
        }
        LoadFromJSONImpl<TK>::Load(&(*cit)[static_cast<rapidjson::SizeType>(0)], entry.first, path);
        LoadFromJSONImpl<TV>::Load(&(*cit)[static_cast<rapidjson::SizeType>(1)], entry.second, path);
        destination.insert(entry);
      }
    } else {
      throw JSONSchemaException("map as array", source, path);
    }
  }
};

template <typename T>
struct LoadFromJSONImpl<Optional<T>> {
  static void Load(rapidjson::Value* source, Optional<T>& destination, const std::string& path) {
    if (!source || source->IsNull_()) {
      destination = nullptr;
    } else {
      destination = T();
      LoadFromJSONImpl<T>::Load(source, Value(destination), path);
    }
  }
};

template <typename T>
void ParseJSONViaRapidJSON(const std::string& json, T& destination) {
  rapidjson::Document document;

  if (document.Parse<0>(json.c_str()).HasParseError()) {
    throw InvalidJSONException(json);
  }

  LoadFromJSONImpl<T>::Load(&document, destination, "");
}

// External user-facing `JSON` and `ParseJSON` methods. Since RapidJSON is not friendly
// with serializing bare integers, strings, and booleans, make it happen explicitly.

template <typename T, bool BYPASS_RAPIDJSON>
struct ViaRapidJSONOrNatively {
  static std::string Create(const T& source) { return CreateJSONViaRapidJSON(source); }
  static void Parse(const std::string& source, T& destination) { ParseJSONViaRapidJSON(source, destination); }
};

template <typename T>
struct ViaRapidJSONOrNatively<T, true> {
  static std::string Create(const T& source) { return bricks::strings::ToString(source); }
  static void Parse(const std::string& source, T& destination) {
    bricks::strings::FromString(source, destination);
  }
};

template <typename T>
struct BypassRapidJSON {
  static constexpr bool bypass = std::is_integral<T>::value;
};

// Note: Bare strings will not be escaped during serialization / deserialization.
// Thus, not proper JSON. But one-to-one convertible to and from binary. -- D.K.
template <>
struct BypassRapidJSON<std::string> {
  static constexpr bool bypass = true;
};

template <typename T>
inline std::string JSON(const T& source) {
  using DECAYED_T = bricks::decay<T>;
  return ViaRapidJSONOrNatively<DECAYED_T, BypassRapidJSON<DECAYED_T>::bypass>::Create(source);
}

inline std::string JSON(const char* special_case_bare_c_string) { return special_case_bare_c_string; }

template <typename T>
inline T ParseJSON(const std::string& source, T& destination) {
  using DECAYED_T = bricks::decay<T>;
  ViaRapidJSONOrNatively<DECAYED_T, BypassRapidJSON<DECAYED_T>::bypass>::Parse(source, destination);
}

template <typename T>
inline T ParseJSON(const std::string& source) {
  T result;
  ViaRapidJSONOrNatively<T, BypassRapidJSON<T>::bypass>::Parse(source, result);
  return result;
}

}  // namespace serialization
}  // namespace current

// Inject into global namespace.
using current::serialization::JSON;
using current::serialization::ParseJSON;
using current::serialization::TypeSystemParseJSONException;
using current::serialization::JSONSchemaException;
using current::serialization::InvalidJSONException;

#endif  // CURRENT_TYPE_SYSTEM_SERIALIZATION_JSON_H