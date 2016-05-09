/*******************************************************************************
The MIT License (MIT)

Copyright (c) 2016 Dmitry "Dima" Korolev <dmitry.korolev@gmail.com>

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

// Karl is the module responsible for collecting keepalives from Claire-s and reporting/visualizing them.
//
// Karl's storage model contains of the following pieces:
//
// 1) The Sherlock `Stream` of all keepalives received. Persisted on disk, not stored in memory.
//    Each "visualize production" request (be it JSON or SVG response) replays that stream over the desired
//    period of time. Most commonly it's the past five minutes.
//
// 2) The `Storage`, over a separate stream, to retain the information which may be required outside the
//    "visualized" time window. Includes Karl's launch history, and per-service codename -> build::Info.
//
// TODO(dkorolev) + TODO(mzhurovich): Stuff like nginx config for fresh start lives in the Storage part, right?
//                                    We'll need to have GenericKarl accept custom storage type then.
//
// The conventional wisdom is that Karl can start with both 1) and 2) missing. After one keepalive cycle,
// which is under half a minute, it would regain the state of the fleet, as long as all keepalives go to it.

// NOTE: Local `current_build.h` must be included before Karl/Claire headers.

#ifndef KARL_KARL_H
#define KARL_KARL_H

#include "../port.h"

#include "exceptions.h"
#include "schema_karl.h"
#include "schema_claire.h"
#include "locator.h"
#include "render.h"

#include "../Storage/storage.h"
#include "../Storage/persister/sherlock.h"

#include "../Bricks/net/http/impl/server.h"
#include "../Bricks/util/base64.h"

#include "../Blocks/HTTP/api.h"

#include "../Utils/Nginx/nginx.h"

#ifdef EXTRA_KARL_LOGGING
#include "../TypeSystem/Schema/schema.h"
#endif

namespace current {
namespace karl {

CURRENT_STRUCT_T(KarlPersistedKeepalive) {
  CURRENT_FIELD(location, ClaireServiceKey);
  CURRENT_FIELD(keepalive, T);
};

struct KarlNginxParameters {
  uint16_t port;
  std::string config_file;
  std::string route_prefix;
  KarlNginxParameters(uint16_t port, const std::string& config_file, const std::string& route_prefix = "/live")
      : port(port), config_file(config_file), route_prefix(route_prefix) {}
};

template <class STORAGE>
class KarlNginxManager {
 protected:
  explicit KarlNginxManager(STORAGE& storage, const KarlNginxParameters& nginx_parameters, uint16_t karl_port)
      : storage_(storage),
        has_nginx_config_file_(!nginx_parameters.config_file.empty()),
        nginx_parameters_(nginx_parameters),
        karl_port_(karl_port),
        last_reflected_state_stream_size_(0u) {
    if (has_nginx_config_file_) {
      if (!nginx::NginxInvoker().IsNginxAvailable()) {
        CURRENT_THROW(NginxRequestedButNotAvailableException());
      }
      if (nginx_parameters_.port == 0u) {
        CURRENT_THROW(NginxParametersInvalidPortException());
      }
      nginx_manager_ = std::make_unique<nginx::NginxManager>(nginx_parameters.config_file);
    }
  }

  void UpdateNginxIfNeeded() {
    // To spawn Nginx `server` at startup even if the storage is empty.
    static bool first_run = true;
    if (has_nginx_config_file_) {
      const uint64_t current_stream_size = storage_.InternalExposeStream().InternalExposePersister().Size();
      if (first_run || current_stream_size != last_reflected_state_stream_size_) {
        nginx::config::ServerDirective server(nginx_parameters_.port);
        server.CreateProxyPassLocation("/", Printf("http://localhost:%d/", karl_port_));
        storage_.ReadOnlyTransaction([this, &server](ImmutableFields<STORAGE> fields) -> void {
          for (const auto& claire : fields.claires) {
            if (claire.registered_state == ClaireRegisteredState::Active) {
              server.CreateProxyPassLocation(nginx_parameters_.route_prefix + '/' + claire.codename,
                                             claire.location.StatusPageURL());
            }
          }
        }).Go();
        nginx_manager_->UpdateConfig(std::move(server));
        last_reflected_state_stream_size_ = current_stream_size;
        first_run = false;
      }
    }
  }

  virtual ~KarlNginxManager() {}

 protected:
  STORAGE& storage_;
  const bool has_nginx_config_file_;
  const KarlNginxParameters nginx_parameters_;
  const uint16_t karl_port_;
  std::unique_ptr<nginx::NginxManager> nginx_manager_;

 private:
  uint64_t last_reflected_state_stream_size_;
};

template <typename... TS>
class GenericKarl final : private KarlNginxManager<ServiceStorage<SherlockStreamPersister>> {
 public:
  using runtime_status_variant_t = Variant<TS...>;
  using claire_status_t = ClaireServiceStatus<runtime_status_variant_t>;
  using karl_status_t = GenericKarlStatus<runtime_status_variant_t>;
  using persisted_keepalive_t = KarlPersistedKeepalive<claire_status_t>;
  using stream_t = sherlock::Stream<persisted_keepalive_t, current::persistence::File>;
  using storage_t = ServiceStorage<SherlockStreamPersister>;

  explicit GenericKarl(
      uint16_t port,
      const std::string& stream_persistence_file,
      const std::string& storage_persistence_file,
      const std::string& url = "/",
      const std::string& external_url = "http://localhost:{port}",
      const std::string& svg_name = "Karl",
      const std::string& github_repo_url = "",
      const KarlNginxParameters& nginx_parameters = KarlNginxParameters(0, ""),
      std::chrono::microseconds service_timeout_interval = std::chrono::microseconds(1000ll * 1000ll * 45))
      : KarlNginxManager(storage_, nginx_parameters, port),
        destructing_(false),
        svg_name_(svg_name),
        github_repo_url_(github_repo_url),
        external_url_(external_url == "http://localhost:{port}"
                          ? current::strings::Printf("http://localhost:%d", port)
                          : external_url),
        service_timeout_interval_(service_timeout_interval),
        keepalives_stream_(stream_persistence_file),
        storage_(storage_persistence_file),
        state_update_thread_([this]() { StateUpdateThread(); }),
        // TODO(mzhurovich): `/up` ?
        http_scope_(HTTP(port).Register(url,
                                        URLPathArgs::CountMask::None | URLPathArgs::CountMask::One,
                                        [this](Request r) { Serve(std::move(r)); }) +
                    HTTP(port).Register(url + "build",
                                        URLPathArgs::CountMask::One,
                                        [this](Request r) { ServeBuild(std::move(r)); }) +
                    HTTP(port).Register(url + "snapshot",
                                        URLPathArgs::CountMask::One,
                                        [this](Request r) { ServeSnapshot(std::move(r)); }) +
                    HTTP(port).Register(url + "favicon.png", http::CurrentFaviconHandler())) {
    // Report this Karl as up and running.
    // Oh, look, I'm doing work in constructor body. Sigh. -- D.K.
    storage_.ReadWriteTransaction([this](MutableFields<storage_t> fields) {
      const auto& stream_persister = keepalives_stream_.InternalExposePersister();
      KarlInfo self_info;
      if (!stream_persister.Empty()) {
        self_info.persisted_keepalives_info = stream_persister.LastPublishedIndexAndTimestamp();
      }
      fields.karl.Add(self_info);

      const auto now = current::time::Now();
      // Pre-populate services marked as `Active` due to abrupt shutdown into the cache.
      // This will help to eventually mark non-active services as `DisconnectedByTimeout`.
      for (const auto& claire : fields.claires) {
        if (claire.registered_state == ClaireRegisteredState::Active) {
          services_keepalive_time_cache_[claire.codename] = now;
        }
      }
    }).Wait();
  }

  ~GenericKarl() {
    destructing_ = true;
    storage_.ReadWriteTransaction([this](MutableFields<storage_t> fields) {
      KarlInfo self_info;
      self_info.up = false;
      fields.karl.Add(self_info);
    }).Wait();
    if (state_update_thread_.joinable()) {
      update_thread_condition_variable_.notify_one();
      state_update_thread_.join();
    }
  }

  size_t ActiveServicesCount() const {
    std::lock_guard<std::mutex> lock(services_keepalive_cache_mutex_);
    return services_keepalive_time_cache_.size();
  }

  storage_t& InternalExposeStorage() { return storage_; }

 private:
  void StateUpdateThread() {
    while (!destructing_) {
      const auto now = current::time::Now();
      std::unordered_set<std::string> timeouted_codenames;
      std::chrono::microseconds most_recent_keepalive_time(0);
      {
        std::lock_guard<std::mutex> lock(services_keepalive_cache_mutex_);
        for (auto it = services_keepalive_time_cache_.cbegin(); it != services_keepalive_time_cache_.cend();) {
          if ((now - it->second) > service_timeout_interval_) {
            timeouted_codenames.insert(it->first);
            services_keepalive_time_cache_.erase(it++);
          } else {
            most_recent_keepalive_time = std::max(it->second, most_recent_keepalive_time);
            ++it;
          }
        }
      }
      if (!timeouted_codenames.empty()) {
        storage_.ReadWriteTransaction([&timeouted_codenames](MutableFields<storage_t> fields) -> void {
          for (const auto& codename : timeouted_codenames) {
            const auto& current_claire_info = fields.claires[codename];
            ClaireInfo claire;
            if (Exists(current_claire_info)) {
              claire = Value(current_claire_info);
            } else {
              claire.codename = codename;
            }
            claire.registered_state = ClaireRegisteredState::DisconnectedByTimeout;
            fields.claires.Add(claire);
          }
        }).Wait();
      }
      UpdateNginxIfNeeded();
#ifdef CURRENT_MOCK_TIME
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
#else
      std::unique_lock<std::mutex> lock(services_keepalive_cache_mutex_);
      if (most_recent_keepalive_time.count() != 0) {
        const auto wait_interval =
            service_timeout_interval_ - (current::time::Now() - most_recent_keepalive_time);
        if (wait_interval.count() > 0) {
          update_thread_condition_variable_.wait_for(lock, wait_interval + std::chrono::microseconds(1));
        }
      } else {
        update_thread_condition_variable_.wait(lock);
      }
#endif
    }
  }

  void Serve(Request r) {
    if (r.method != "GET" && r.method != "POST" && r.method != "DELETE") {
      r(current::net::DefaultMethodNotAllowedMessage(),
        HTTPResponseCode.MethodNotAllowed,
        net::constants::kDefaultHTMLContentType);
      return;
    }

    const auto& qs = r.url.query;

    if (r.method == "DELETE") {
      const std::string ip = r.connection.RemoteIPAndPort().ip;
      if (qs.has("codename")) {
        const std::string codename = qs["codename"];
        storage_.ReadWriteTransaction([codename](MutableFields<storage_t> fields) -> Response {
          ClaireInfo claire;
          const auto& current_claire_info = fields.claires[codename];
          if (Exists(current_claire_info)) {
            claire = Value(current_claire_info);
          } else {
            claire.codename = codename;
          }
          claire.registered_state = ClaireRegisteredState::Deregistered;
          fields.claires.Add(claire);
          return Response("OK\n");
        }, std::move(r)).Detach();
        {
          // Delete this `codename` from cache.
          std::lock_guard<std::mutex> lock(services_keepalive_cache_mutex_);
          services_keepalive_time_cache_.erase(codename);
        }
        update_thread_condition_variable_.notify_one();
      } else {
        // Respond with "200 OK" in any case.
        r("NOP\n");
      }
    } else if (r.method == "POST") {
      try {
        const std::string ip = r.connection.RemoteIPAndPort().ip;
        const std::string url = "http://" + ip + ':' + qs["port"] + "/.current";

        // If `&confirm` is set, along with `codename` and `port`, Karl calls the service back
        // via the URL from the inbound request and the port the service has provided,
        // to confirm two-way communication.
        const std::string json = [&]() -> std::string {
          if (qs.has("confirm") && qs.has("port")) {
            // Send a GET request, with a random component in the URL to prevent caching.
            return HTTP(GET(url + "?all&rnd" + current::ToString(current::random::CSRandomUInt(1e9, 2e9))))
                .body;
          } else {
            return r.body;
          }
        }();

        const auto body = ParseJSON<ClaireStatus>(json);
        if ((!qs.has("codename") || body.codename == qs["codename"]) &&
            (!qs.has("port") || body.local_port == current::FromString<uint16_t>(qs["port"]))) {
          ClaireServiceKey location;
          location.ip = ip;
          location.port = body.local_port;
          location.prefix = "/";  // TODO(dkorolev) + TODO(mzhurovich): Add support for `qs["prefix"]`.

          const auto dependencies = body.dependencies;

          // If the received status can be parsed in detail, including the "runtime" variant, persist it.
          // If no, no big deal, keep the top-level one regardless.
          const auto status = [&]() -> claire_status_t {
            try {
              return ParseJSON<claire_status_t>(json);
            } catch (const TypeSystemParseJSONException&) {

#ifdef EXTRA_KARL_LOGGING
              std::cerr << "Could not parse: " << json << '\n';
              reflection::StructSchema struct_schema;
              struct_schema.AddType<claire_status_t>();
              std::cerr << "As:\n" << struct_schema.GetSchemaInfo().Describe<reflection::Language::Current>()
                        << '\n';
#endif

              claire_status_t status;
              // Initialize `ClaireStatus` from `ClaireServiceStatus`, keep the `Variant<...> runtime` empty.
              static_cast<ClaireStatus&>(status) = body;
              return status;
            }
          }();

          const auto now = current::time::Now();
          const std::string service = body.service;
          const std::string codename = body.codename;

          {
            persisted_keepalive_t record;
            record.location = location;
            record.keepalive = status;
            {
              std::lock_guard<std::mutex> lock(latest_keepalive_index_mutex_);
              latest_keepalive_index_plus_one_[codename] = keepalives_stream_.Publish(std::move(record)).index;
            }
          }

          Optional<current::build::Info> optional_build = body.build;
          Optional<std::chrono::microseconds> optional_behind_this_by;
          if (Exists(body.last_successful_ping_epoch_microseconds)) {
            optional_behind_this_by = now - body.now - Value(body.last_successful_ping_epoch_microseconds) / 2;
          }

          storage_.ReadWriteTransaction(
                       [this,
                        now,
                        codename,
                        service,
                        location,
                        dependencies,
                        optional_build,
                        optional_behind_this_by](MutableFields<storage_t> fields) -> Response {
                         // Update per-server time skew.
                         if (Exists(optional_behind_this_by)) {
                           const std::chrono::microseconds behind_this_by = Value(optional_behind_this_by);
                           ServerInfo server;
                           server.ip = location.ip;
                           // Just in case load old value if we decide to add fields to `ServerInfo`.
                           const ImmutableOptional<ServerInfo> current_server_info =
                               fields.servers[location.ip];
                           bool need_to_update = true;
                           if (Exists(current_server_info)) {
                             server = Value(current_server_info);
                             const auto time_skew_difference = server.behind_this_by - behind_this_by;
                             if (static_cast<uint64_t>(std::abs(time_skew_difference.count())) <
                                 kUpdateServerInfoThresholdByTimeSkewDifference) {
                               need_to_update = false;
                             }
                           }
                           if (need_to_update) {
                             server.behind_this_by = behind_this_by;
                             fields.servers.Add(server);
                           }
                         }

                         // Update the `DB` if the build information was not stored there yet.
                         const ImmutableOptional<ClaireBuildInfo> current_claire_build_info =
                             fields.builds[codename];
                         if (Exists(optional_build) &&
                             (!Exists(current_claire_build_info) ||
                              Value(current_claire_build_info).build != Value(optional_build))) {
                           ClaireBuildInfo build;
                           build.codename = codename;
                           build.build = Value(optional_build);
                           fields.builds.Add(build);
                         }
                         // Update the `DB` if "codename", "location", or "dependencies" differ.
                         const ImmutableOptional<ClaireInfo> current_claire_info = fields.claires[codename];
                         if ([&]() {
                               if (!Exists(current_claire_info)) {
                                 return true;
                               } else if (Value(current_claire_info).location != location) {
                                 return true;
                               } else if (Value(current_claire_info).registered_state !=
                                          ClaireRegisteredState::Active) {
                                 return true;
                               } else {
                                 return false;
                               }
                             }()) {
                           ClaireInfo claire;
                           if (Exists(current_claire_info)) {
                             // Do not overwrite `build` with `null`.
                             claire = Value(current_claire_info);
                           }

                           claire.codename = codename;
                           claire.service = service;
                           claire.location = location;
                           claire.reported_timestamp = now;
                           claire.url_status_page_direct = location.StatusPageURL();
                           claire.registered_state = ClaireRegisteredState::Active;

                           fields.claires.Add(claire);
                         }
                         return Response("OK\n");
                       },
                       std::move(r)).Wait();
          {
            std::lock_guard<std::mutex> lock(services_keepalive_cache_mutex_);
            auto& placeholder = services_keepalive_time_cache_[codename];
            if (placeholder.count() == 0) {
              placeholder = now;
              // Notify the thread only if the new codename has appeared in the cache.
              update_thread_condition_variable_.notify_one();
            } else {
              placeholder = now;
            }
          }
        } else {
          r("Inconsistent URL/body parameters.\n", HTTPResponseCode.BadRequest);
        }
      } catch (const net::NetworkException&) {
        r("Callback error.\n", HTTPResponseCode.BadRequest);
      } catch (const TypeSystemParseJSONException&) {
        r("JSON parse error.\n", HTTPResponseCode.BadRequest);
      } catch (const Exception&) {
        r("Karl registration error.\n", HTTPResponseCode.InternalServerError);
      }
    } else {
      BuildStatusAndRespondWithIt(std::move(r));
    }
  }

  void ServeBuild(Request r) {
    const auto codename = r.url_path_args[0];
    storage_.ReadOnlyTransaction([this, codename](ImmutableFields<storage_t> fields) -> Response {
      const auto result = fields.builds[codename];
      if (Exists(result)) {
        return Value(result);
      } else {
        return Response(current_service_state::Error("Codename '" + codename + "' not found."),
                        HTTPResponseCode.NotFound);
      }
    }, std::move(r)).Detach();
  }

  void ServeSnapshot(Request r) {
    const auto codename = r.url_path_args[0];

    uint64_t& index_placeholder = [&]() {
      std::lock_guard<std::mutex> lock(latest_keepalive_index_mutex_);
      return std::ref(latest_keepalive_index_plus_one_[codename]);
    }();

    uint64_t index = index_placeholder;
    if (!index) {
      // If no latest keepalive index in cache, go through the whole log.
      for (const auto& e : keepalives_stream_.InternalExposePersister().Iterate()) {
        if (e.entry.keepalive.codename == codename) {
          index = e.idx_ts.index + 1;
        }
      }
      if (index) {
        std::lock_guard<std::mutex> lock(latest_keepalive_index_mutex_);
        index_placeholder = std::max(index_placeholder, index);
      }
    }

    if (index) {
      const auto e = (*keepalives_stream_.InternalExposePersister().Iterate(index - 1).begin());
      if (!r.url.query.has("nobuild")) {
        r(JSON<JSONFormat::Minimalistic>(SnapshotOfKeepalive<runtime_status_variant_t>(
              e.idx_ts.us - current::time::Now(), e.entry.keepalive)),
          HTTPResponseCode.OK,
          current::net::constants::kDefaultJSONContentType);
      } else {
        auto tmp = e.entry.keepalive;
        tmp.build = nullptr;
        r(JSON<JSONFormat::Minimalistic>(
              SnapshotOfKeepalive<runtime_status_variant_t>(e.idx_ts.us - current::time::Now(), tmp)),
          HTTPResponseCode.OK,
          current::net::constants::kDefaultJSONContentType);
      }
    } else {
      r(current_service_state::Error("No keepalives from '" + codename + "' have been received."),
        HTTPResponseCode.NotFound);
    }
  }

  void BuildStatusAndRespondWithIt(Request r) {
    // For a GET response, compile the status page and return it.
    const auto now = current::time::Now();
    const auto from = [&]() -> std::chrono::microseconds {
      if (r.url.query.has("from")) {
        return current::FromString<std::chrono::microseconds>(r.url.query["from"]);
      }
      if (r.url.query.has("m")) {  // `m` stands for minutes.
        return now - std::chrono::microseconds(
                         static_cast<int64_t>(current::FromString<double>(r.url.query["m"]) * 1e6 * 60));
      }
      if (r.url.query.has("h")) {  // `h` stands for hours.
        return now - std::chrono::microseconds(
                         static_cast<int64_t>(current::FromString<double>(r.url.query["h"]) * 1e6 * 60 * 60));
      }
      if (r.url.query.has("d")) {  // `d` stands for days.
        return now - std::chrono::microseconds(static_cast<int64_t>(
                         current::FromString<double>(r.url.query["d"]) * 1e6 * 60 * 60 * 24));
      }
      // Five minutes by default.
      return now - std::chrono::microseconds(static_cast<int64_t>(1e6 * 60 * 5));
    }();
    const auto to = [&]() -> std::chrono::microseconds {
      if (r.url.query.has("to")) {
        return current::FromString<std::chrono::microseconds>(r.url.query["to"]);
      }
      if (r.url.query.has("interval_us")) {
        return from + current::FromString<std::chrono::microseconds>(r.url.query["interval_us"]);
      }
      // By the present moment by default.
      return now;
    }();

    // Codenames to resolve to `ClaireServiceKey`-s later, in a `ReadOnlyTransaction`.
    std::unordered_set<std::string> codenames_to_resolve;

    // The builder for the response.
    struct ProtoReport {
      current_service_state::state_variant_t currently;
      std::vector<ClaireServiceKey> dependencies;
      Optional<runtime_status_variant_t> runtime;  // Must be `Optional<>`, as it is in `ClaireServiceStatus`.
    };
    std::map<std::string, ProtoReport> report_for_codename;
    std::map<std::string, std::set<std::string>> codenames_per_service;
    std::map<ClaireServiceKey, std::string> service_key_into_codename;

    for (const auto& e : keepalives_stream_.InternalExposePersister().Iterate()) {
      if (e.idx_ts.us >= from && e.idx_ts.us < to) {
        const claire_status_t& keepalive = e.entry.keepalive;

        codenames_to_resolve.insert(keepalive.codename);
        service_key_into_codename[e.entry.location] = keepalive.codename;

        codenames_per_service[keepalive.service].insert(keepalive.codename);
        // DIMA: More per-codename reporting fields go here; tailored to specific type, `.Call(populator)`, etc.
        ProtoReport report;
        const std::string last_keepalive =
            current::strings::TimeIntervalAsHumanReadableString(now - e.idx_ts.us) + " ago";
        if ((now - e.idx_ts.us) < service_timeout_interval_) {
          // Service is up.
          const auto projected_uptime_us = keepalive.uptime_epoch_microseconds + (now - e.idx_ts.us);
          report.currently = current_service_state::up(
              keepalive.start_time_epoch_microseconds,
              last_keepalive,
              e.idx_ts.us,
              current::strings::TimeIntervalAsHumanReadableString(projected_uptime_us));
        } else {
          // Service is down.
          // TODO(dkorolev): Graceful shutdown case for `done`.
          report.currently = current_service_state::down(
              keepalive.start_time_epoch_microseconds, last_keepalive, e.idx_ts.us, keepalive.uptime);
        }
        report.dependencies = keepalive.dependencies;
        report.runtime = keepalive.runtime;
        report_for_codename[keepalive.codename] = report;
      }
    }

    // To list only the services that are currently in `Active` state.
    const bool active_only = r.url.query.has("active_only");

    enum class ResponseType { JSONFull, JSONMinimalistic, DOT, HTML };
    const auto response_type = [&r]() -> ResponseType {
      if (r.url.query.has("full")) {
        return ResponseType::JSONFull;
      }
      if (r.url.query.has("json")) {
        return ResponseType::JSONMinimalistic;
      }
      if (r.url.query.has("dot")) {
        return ResponseType::DOT;
      }
      const char* kAcceptHeader = "Accept";
      if (r.headers.Has(kAcceptHeader)) {
        for (const auto& h : strings::Split(r.headers[kAcceptHeader].value, ',')) {
          if (strings::Split(h, ';').front() == "text/html") {  // Allow "text/html; charset=...", etc.
            return ResponseType::HTML;
          }
        }
      }
      return ResponseType::JSONMinimalistic;
    }();

    const std::string external_url = external_url_;
    storage_.ReadOnlyTransaction(
                 [this,
                  now,
                  from,
                  to,
                  active_only,
                  response_type,
                  external_url,
                  codenames_to_resolve,
                  report_for_codename,
                  codenames_per_service,
                  service_key_into_codename](ImmutableFields<storage_t> fields) -> Response {
                   std::unordered_map<std::string, ClaireServiceKey> resolved_codenames;
                   karl_status_t result;
                   result.now = now;
                   result.from = from;
                   result.to = to;
                   for (const auto& codename : codenames_to_resolve) {
                     resolved_codenames[codename] = [&]() -> ClaireServiceKey {
                       const ImmutableOptional<ClaireInfo> resolved = fields.claires[codename];
                       if (Exists(resolved)) {
                         return Value(resolved).location;
                       } else {
                         ClaireServiceKey key;
                         key.ip = "zombie/" + codename;
                         key.port = 0;
                         return key;
                       }
                     }();
                   }
                   for (const auto& iterating_over_services : codenames_per_service) {
                     const std::string& service = iterating_over_services.first;
                     for (const auto& codename : iterating_over_services.second) {
                       ServiceToReport<runtime_status_variant_t> blob;
                       const auto& rhs = report_for_codename.at(codename);
                       if (active_only) {
                         const auto& persisted_claire = fields.claires[codename];
                         if (Exists(persisted_claire) &&
                             Value(persisted_claire).registered_state != ClaireRegisteredState::Active) {
                           continue;
                         }
                       }
                       blob.currently = rhs.currently;
                       blob.service = service;
                       blob.codename = codename;
                       blob.location = resolved_codenames[codename];
                       for (const auto& dep : rhs.dependencies) {
                         const auto cit = service_key_into_codename.find(dep);
                         if (cit != service_key_into_codename.end()) {
                           blob.dependencies.push_back(cit->second);
                         } else {
                           blob.unresolved_dependencies.push_back(dep.StatusPageURL());
                         }
                       }

                       {
                         const auto optional_build = fields.builds[codename];
                         if (Exists(optional_build)) {
                           const auto& info = Value(optional_build).build;
                           blob.build_time = info.build_time;
                           blob.build_time_epoch_microseconds = info.build_time_epoch_microseconds;
                           blob.git_commit = info.git_commit_hash;
                           blob.git_branch = info.git_branch;
                           blob.git_dirty = !info.git_dirty_files.empty();
                         }
                       }

                       if (has_nginx_config_file_) {
                         blob.url_status_page_proxied =
                             external_url_ + nginx_parameters_.route_prefix + '/' + codename;
                       }
                       blob.url_status_page_direct = blob.location.StatusPageURL();
                       blob.location = resolved_codenames[codename];
                       blob.runtime = rhs.runtime;
                       result.machines[blob.location.ip].services[codename] = std::move(blob);
                     }
                   }
                   // Update per-server time skew information.
                   for (auto& iterating_over_reported_servers : result.machines) {
                     const std::string& ip = iterating_over_reported_servers.first;
                     auto& server = iterating_over_reported_servers.second;
                     const auto persisted_server_info = fields.servers[ip];
                     if (Exists(persisted_server_info)) {
                       const int64_t behind_this_by_us = Value(persisted_server_info).behind_this_by.count();
                       if (std::abs(behind_this_by_us) < 100000) {
                         server.time_skew = "NTP OK";
                       } else if (behind_this_by_us > 0) {
                         server.time_skew =
                             current::strings::Printf("behind by %.1lfs", 1e-6 * behind_this_by_us);
                       } else {
                         server.time_skew =
                             current::strings::Printf("ahead by %.1lfs", 1e-6 * behind_this_by_us);
                       }
                     }
                   }
                   result.generation_time = current::time::Now() - now;
                   if (response_type == ResponseType::JSONMinimalistic) {
                     return Response(JSON<JSONFormat::Minimalistic>(result),
                                     HTTPResponseCode.OK,
                                     current::net::constants::kDefaultJSONContentType);
                   } else if (response_type == ResponseType::HTML) {
                     // clang-format off
                     return Response(
                         "<!doctype html>"
                         "<head><link rel='icon' href='./favicon.png'></head>"
                         "<body>" + Render(result, svg_name_, github_repo_url_).AsSVG() + "</body>",
                         HTTPResponseCode.OK,
                         net::constants::kDefaultHTMLContentType);
                     // clang-format on
                   } else if (response_type == ResponseType::DOT) {
                     return Response(Render(result, svg_name_, github_repo_url_).AsDOT());
                   } else {
                     return result;
                   }
                 },
                 std::move(r)).Detach();
  }

  std::atomic_bool destructing_;
  std::unordered_map<std::string, std::chrono::microseconds> services_keepalive_time_cache_;
  mutable std::mutex services_keepalive_cache_mutex_;
  std::condition_variable update_thread_condition_variable_;

  // codename -> stream index of the most recent keepalive from this codename.
  std::mutex latest_keepalive_index_mutex_;
  // Plus one to have `0` == "no keepalives", and avoid the corner case of record at index 0 being the one.
  std::unordered_map<std::string, uint64_t> latest_keepalive_index_plus_one_;

  const std::string svg_name_ = "Karl";
  const std::string github_repo_url_ = "";

  const std::string external_url_;
  const std::chrono::microseconds service_timeout_interval_;
  stream_t keepalives_stream_;
  storage_t storage_;
  std::thread state_update_thread_;
  const HTTPRoutesScope http_scope_;
};

using Karl = GenericKarl<default_user_status::status>;

}  // namespace current::karl
}  // namespace current

#endif  // KARL_KARL_H