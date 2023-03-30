// Copyright 2021 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <grpc/support/port_platform.h>

#include "src/core/lib/security/authorization/rbac_translator.h"

#include <stddef.h>

#include <algorithm>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/strip.h"

#include "src/core/lib/gpr/useful.h"
#include "src/core/lib/json/json.h"
#include "src/core/lib/matchers/matchers.h"

namespace grpc_core {

namespace {

absl::string_view GetMatcherType(absl::string_view value,
                                 StringMatcher::Type* type) {
  if (value == "*") {
    *type = StringMatcher::Type::kSafeRegex;
    // Presence match checks for non empty strings.
    return ".+";
  } else if (absl::StartsWith(value, "*")) {
    *type = StringMatcher::Type::kSuffix;
    return absl::StripPrefix(value, "*");
  } else if (absl::EndsWith(value, "*")) {
    *type = StringMatcher::Type::kPrefix;
    return absl::StripSuffix(value, "*");
  }
  *type = StringMatcher::Type::kExact;
  return value;
}

absl::StatusOr<StringMatcher> GetStringMatcher(absl::string_view value) {
  StringMatcher::Type type;
  absl::string_view matcher = GetMatcherType(value, &type);
  return StringMatcher::Create(type, matcher);
}

absl::StatusOr<HeaderMatcher> GetHeaderMatcher(absl::string_view name,
                                               absl::string_view value) {
  StringMatcher::Type type;
  absl::string_view matcher = GetMatcherType(value, &type);
  return HeaderMatcher::Create(name, static_cast<HeaderMatcher::Type>(type),
                               matcher);
}

bool IsUnsupportedHeader(absl::string_view header_name) {
  static const char* const kUnsupportedHeaders[] = {"host",
                                                    "connection",
                                                    "keep-alive",
                                                    "proxy-authenticate",
                                                    "proxy-authorization",
                                                    "te",
                                                    "trailer",
                                                    "transfer-encoding",
                                                    "upgrade"};
  for (size_t i = 0; i < GPR_ARRAY_SIZE(kUnsupportedHeaders); ++i) {
    if (absl::EqualsIgnoreCase(header_name, kUnsupportedHeaders[i])) {
      return true;
    }
  }
  return false;
}

absl::StatusOr<Rbac::Principal> ParsePrincipalsArray(const Json& json) {
  std::vector<std::unique_ptr<Rbac::Principal>> principal_names;
  for (size_t i = 0; i < json.array().size(); ++i) {
    const Json& child = json.array().at(i);
    if (child.type() != Json::Type::kString) {
      return absl::InvalidArgumentError(
          absl::StrCat("\"principals\" ", i, ": is not a string."));
    }
    auto matcher_or = GetStringMatcher(child.string());
    if (!matcher_or.ok()) {
      return absl::Status(matcher_or.status().code(),
                          absl::StrCat("\"principals\" ", i, ": ",
                                       matcher_or.status().message()));
    }
    principal_names.push_back(std::make_unique<Rbac::Principal>(
        Rbac::Principal::MakeAuthenticatedPrincipal(
            std::move(matcher_or.value()))));
  }
  return Rbac::Principal::MakeOrPrincipal(std::move(principal_names));
}

absl::StatusOr<Rbac::Principal> ParsePeer(const Json& json) {
  std::vector<std::unique_ptr<Rbac::Principal>> peer;
  for (const auto& object : json.object()) {
    if (object.first == "principals") {
      if (object.second.type() != Json::Type::kArray) {
        return absl::InvalidArgumentError("\"principals\" is not an array.");
      }
      auto principal_names_or = ParsePrincipalsArray(object.second);
      if (!principal_names_or.ok()) return principal_names_or.status();
      if (!principal_names_or.value().principals.empty()) {
        peer.push_back(std::make_unique<Rbac::Principal>(
            std::move(principal_names_or.value())));
      }
    } else {
      return absl::InvalidArgumentError(absl::StrFormat(
          "policy contains unknown field \"%s\" in \"source\".", object.first));
    }
  }
  if (peer.empty()) {
    return Rbac::Principal::MakeAnyPrincipal();
  }
  return Rbac::Principal::MakeAndPrincipal(std::move(peer));
}

absl::StatusOr<Rbac::Permission> ParseHeaderValues(
    const Json& json, absl::string_view header_name) {
  if (json.array().empty()) {
    return absl::InvalidArgumentError("\"values\" list is empty.");
  }
  std::vector<std::unique_ptr<Rbac::Permission>> values;
  for (size_t i = 0; i < json.array().size(); ++i) {
    const Json& child = json.array().at(i);
    if (child.type() != Json::Type::kString) {
      return absl::InvalidArgumentError(
          absl::StrCat("\"values\" ", i, ": is not a string."));
    }
    auto matcher_or = GetHeaderMatcher(header_name, child.string());
    if (!matcher_or.ok()) {
      return absl::Status(
          matcher_or.status().code(),
          absl::StrCat("\"values\" ", i, ": ", matcher_or.status().message()));
    }
    values.push_back(std::make_unique<Rbac::Permission>(
        Rbac::Permission::MakeHeaderPermission(std::move(matcher_or.value()))));
  }
  return Rbac::Permission::MakeOrPermission(std::move(values));
}

absl::StatusOr<Rbac::Permission> ParseHeaders(const Json& json) {
  absl::string_view key;
  const Json* values = nullptr;
  for (const auto& object : json.object()) {
    if (object.first == "key") {
      if (object.second.type() != Json::Type::kString) {
        return absl::InvalidArgumentError("\"key\" is not a string.");
      }
      key = object.second.string();
      if (absl::StartsWith(key, ":") || absl::StartsWith(key, "grpc-") ||
          IsUnsupportedHeader(key)) {
        return absl::InvalidArgumentError(
            absl::StrFormat("Unsupported \"key\" %s.", key));
      }
    } else if (object.first == "values") {
      if (object.second.type() != Json::Type::kArray) {
        return absl::InvalidArgumentError("\"values\" is not an array.");
      }
      values = &object.second;
    } else {
      return absl::InvalidArgumentError(absl::StrFormat(
          "policy contains unknown field \"%s\".", object.first));
    }
  }
  if (key.empty()) {
    return absl::InvalidArgumentError("\"key\" is not present.");
  }
  if (values == nullptr) {
    return absl::InvalidArgumentError("\"values\" is not present.");
  }
  return ParseHeaderValues(*values, key);
}

absl::StatusOr<Rbac::Permission> ParseHeadersArray(const Json& json) {
  std::vector<std::unique_ptr<Rbac::Permission>> headers;
  for (size_t i = 0; i < json.array().size(); ++i) {
    const Json& child = json.array().at(i);
    if (child.type() != Json::Type::kObject) {
      return absl::InvalidArgumentError(
          absl::StrCat("\"headers\" ", i, ": is not an object."));
    }
    auto headers_or = ParseHeaders(child);
    if (!headers_or.ok()) {
      return absl::Status(
          headers_or.status().code(),
          absl::StrCat("\"headers\" ", i, ": ", headers_or.status().message()));
    }
    headers.push_back(
        std::make_unique<Rbac::Permission>(std::move(headers_or.value())));
  }
  return Rbac::Permission::MakeAndPermission(std::move(headers));
}

absl::StatusOr<Rbac::Permission> ParsePathsArray(const Json& json) {
  std::vector<std::unique_ptr<Rbac::Permission>> paths;
  for (size_t i = 0; i < json.array().size(); ++i) {
    const Json& child = json.array().at(i);
    if (child.type() != Json::Type::kString) {
      return absl::InvalidArgumentError(
          absl::StrCat("\"paths\" ", i, ": is not a string."));
    }
    auto matcher_or = GetStringMatcher(child.string());
    if (!matcher_or.ok()) {
      return absl::Status(
          matcher_or.status().code(),
          absl::StrCat("\"paths\" ", i, ": ", matcher_or.status().message()));
    }
    paths.push_back(std::make_unique<Rbac::Permission>(
        Rbac::Permission::MakePathPermission(std::move(matcher_or.value()))));
  }
  return Rbac::Permission::MakeOrPermission(std::move(paths));
}

absl::StatusOr<Rbac::Permission> ParseRequest(const Json& json) {
  std::vector<std::unique_ptr<Rbac::Permission>> request;
  for (const auto& object : json.object()) {
    if (object.first == "paths") {
      if (object.second.type() != Json::Type::kArray) {
        return absl::InvalidArgumentError("\"paths\" is not an array.");
      }
      auto paths_or = ParsePathsArray(object.second);
      if (!paths_or.ok()) return paths_or.status();
      if (!paths_or.value().permissions.empty()) {
        request.push_back(
            std::make_unique<Rbac::Permission>(std::move(paths_or.value())));
      }
    } else if (object.first == "headers") {
      if (object.second.type() != Json::Type::kArray) {
        return absl::InvalidArgumentError("\"headers\" is not an array.");
      }
      auto headers_or = ParseHeadersArray(object.second);
      if (!headers_or.ok()) return headers_or.status();
      if (!headers_or.value().permissions.empty()) {
        request.push_back(
            std::make_unique<Rbac::Permission>(std::move(headers_or.value())));
      }
    } else {
      return absl::InvalidArgumentError(absl::StrFormat(
          "policy contains unknown field \"%s\" in \"request\".",
          object.first));
    }
  }
  if (request.empty()) {
    return Rbac::Permission::MakeAnyPermission();
  }
  return Rbac::Permission::MakeAndPermission(std::move(request));
}

absl::StatusOr<Rbac::Policy> ParseRule(const Json& json,
                                       std::string* policy_name) {
  absl::optional<Rbac::Principal> principals;
  absl::optional<Rbac::Permission> permissions;
  for (const auto& object : json.object()) {
    if (object.first == "name") {
      if (object.second.type() != Json::Type::kString) {
        return absl::InvalidArgumentError(
            absl::StrCat("\"name\" is not a string."));
      }
      *policy_name = object.second.string();
    } else if (object.first == "source") {
      if (object.second.type() != Json::Type::kObject) {
        return absl::InvalidArgumentError("\"source\" is not an object.");
      }
      auto peer_or = ParsePeer(object.second);
      if (!peer_or.ok()) return peer_or.status();
      principals = std::move(*peer_or);
    } else if (object.first == "request") {
      if (object.second.type() != Json::Type::kObject) {
        return absl::InvalidArgumentError("\"request\" is not an object.");
      }
      auto request_or = ParseRequest(object.second);
      if (!request_or.ok()) return request_or.status();
      permissions = std::move(*request_or);
    } else {
      return absl::InvalidArgumentError(absl::StrFormat(
          "policy contains unknown field \"%s\" in \"rule\".", object.first));
    }
  }
  if (policy_name->empty()) {
    return absl::InvalidArgumentError(absl::StrCat("\"name\" is not present."));
  }
  if (!principals.has_value()) {
    principals = Rbac::Principal::MakeAnyPrincipal();
  }
  if (!permissions.has_value()) {
    permissions = Rbac::Permission::MakeAnyPermission();
  }
  return Rbac::Policy(std::move(*permissions), std::move(*principals));
}

absl::StatusOr<std::map<std::string, Rbac::Policy>> ParseRulesArray(
    const Json& json, absl::string_view name) {
  if (json.array().empty()) {
    return absl::InvalidArgumentError("rules is empty.");
  }
  std::map<std::string, Rbac::Policy> policies;
  for (size_t i = 0; i < json.array().size(); ++i) {
    const Json& child = json.array().at(i);
    if (child.type() != Json::Type::kObject) {
      return absl::InvalidArgumentError(
          absl::StrCat("rules ", i, ": is not an object."));
    }
    std::string policy_name;
    auto policy_or = ParseRule(child, &policy_name);
    if (!policy_or.ok()) {
      return absl::Status(
          policy_or.status().code(),
          absl::StrCat("rules ", i, ": ", policy_or.status().message()));
    }
    policies[std::string(name) + "_" + policy_name] =
        std::move(policy_or.value());
  }
  return std::move(policies);
}

absl::StatusOr<Rbac> ParseDenyRulesArray(const Json& json,
                                         absl::string_view name) {
  auto policies_or = ParseRulesArray(json, name);
  if (!policies_or.ok()) return policies_or.status();
  return Rbac(Rbac::Action::kDeny, std::move(policies_or.value()));
}

absl::StatusOr<Rbac> ParseAllowRulesArray(const Json& json,
                                          absl::string_view name) {
  auto policies_or = ParseRulesArray(json, name);
  if (!policies_or.ok()) return policies_or.status();
  return Rbac(Rbac::Action::kAllow, std::move(policies_or.value()));
}

}  // namespace

absl::StatusOr<RbacPolicies> GenerateRbacPolicies(
    absl::string_view authz_policy) {
  auto json = Json::Parse(authz_policy);
  if (!json.ok()) {
    return absl::InvalidArgumentError(
        absl::StrCat("Failed to parse gRPC authorization policy. Error: ",
                     json.status().ToString()));
  }
  if (json->type() != Json::Type::kObject) {
    return absl::InvalidArgumentError(
        "SDK authorization policy is not an object.");
  }
  auto it = json->object().find("name");
  if (it == json->object().end()) {
    return absl::InvalidArgumentError("\"name\" field is not present.");
  }
  if (it->second.type() != Json::Type::kString) {
    return absl::InvalidArgumentError("\"name\" is not a string.");
  }
  absl::string_view name = it->second.string();
  RbacPolicies rbacs;
  bool has_allow_rbac = false;
  for (const auto& object : json->object()) {
    if (object.first == "name") {
      continue;
    } else if (object.first == "deny_rules") {
      if (object.second.type() != Json::Type::kArray) {
        return absl::InvalidArgumentError("\"deny_rules\" is not an array.");
      }
      auto deny_policy_or = ParseDenyRulesArray(object.second, name);
      if (!deny_policy_or.ok()) {
        return absl::Status(
            deny_policy_or.status().code(),
            absl::StrCat("deny_", deny_policy_or.status().message()));
      }
      rbacs.deny_policy = std::move(*deny_policy_or);
    } else if (object.first == "allow_rules") {
      if (object.second.type() != Json::Type::kArray) {
        return absl::InvalidArgumentError("\"allow_rules\" is not an array.");
      }
      auto allow_policy_or = ParseAllowRulesArray(object.second, name);
      if (!allow_policy_or.ok()) {
        return absl::Status(
            allow_policy_or.status().code(),
            absl::StrCat("allow_", allow_policy_or.status().message()));
      }
      rbacs.allow_policy = std::move(*allow_policy_or);
      has_allow_rbac = true;
    } else {
      return absl::InvalidArgumentError(absl::StrFormat(
          "policy contains unknown field \"%s\".", object.first));
    }
  }
  if (!has_allow_rbac) {
    return absl::InvalidArgumentError("\"allow_rules\" is not present.");
  }
  return std::move(rbacs);
}

}  // namespace grpc_core
