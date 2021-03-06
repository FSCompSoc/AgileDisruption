#pragma once
#include "agiledisruption/internal/common.hpp"

#include <unordered_map>
#include <functional>
#include <string>
#include <optional>
#include <shared_mutex>
#include <memory>

#define AGILEDISRUPTION_DEFINE_INTERFACE(...) \
  std::shared_ptr<agiledisruption::api> interface = std::make_shared<agiledisruption::api>( \
    std::initializer_list<std::pair<const std::string, agiledisruption::api::handler>>{__VA_ARGS__} \
  )

#define AGILEDISRUPTION_EXPOSE_INTERFACE extern std::shared_ptr<agiledisruption::api> interface

namespace agiledisruption {
  class api {
  public:
    using handler = std::function<json(const json&)>;
  private:
    std::unordered_map<std::string, handler> _api_calls = {};
    mutable std::shared_mutex _api_calls_mutex = {};

  public:
    inline void add(std::string name, handler handler) {
      std::unique_lock lock(_api_calls_mutex);
      _api_calls.insert_or_assign(name, handler);
    }
    inline void remove(std::string name) {
      std::unique_lock lock(_api_calls_mutex);
      _api_calls.erase(name);
    }

    inline std::optional<handler> get(std::string name) const {
      std::shared_lock lock(_api_calls_mutex);

      auto iter = _api_calls.find(name);

      if (iter == _api_calls.end()) return std::nullopt;
      else return { iter->second };
    }

  public:
    api() = default;
    api(const api&) = delete;
    api(api&&) = delete;

    inline api(std::initializer_list<decltype(_api_calls)::value_type> il) : _api_calls(il) {}
  };

  class channel_server {
  public:
    virtual void bind(std::shared_ptr<const api>) = 0;
    virtual void unbind() = 0;

  public:
    inline channel_server& operator=(std::shared_ptr<const api> a) {
      bind(std::forward<decltype(a)>(a));
      return *this;
    }

  public:
    // binds to loopback
    static std::unique_ptr<channel_server> tcp_ip(uint16_t port);
    //static std::unique_ptr<channel_server> fifo(const std::string& path);

  public:
    virtual ~channel_server() = default;
  };
}
