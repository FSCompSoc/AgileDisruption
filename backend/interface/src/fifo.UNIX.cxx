#include "agiledisruption/server.hpp"
#include "agiledisruption/client.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <thread>
#include <shared_mutex>
#include <memory>
#include <atomic>

namespace agiledisruption {
  class unix_fifo_server : channel_server {
  public:
    std::shared_ptr<const api> base = nullptr;
    std::shared_mutex base_mutex = {};

    std::ifstream request_fifo = {};

    std::thread worker = {};
    std::atomic<bool> keep_working = false;

  public:
    void stop_working() {
      keep_working = false;
      if (worker.joinable())
        worker.join();
    }

    void start_working() {
      keep_working = true;
      worker = std::thread{&unix_fifo_server::worker_body, this};
    }

    void bind(std::shared_ptr<const api> a) override {
      stop_working();

      std::unique_lock lock{base_mutex};
      base = a;

      start_working();
    }

    void unbind() override {
      std::unique_lock lock{base_mutex};
      base.reset();
    }

    // Not resiliant to unterminated strings or slow writes
    void worker_body() {
      std::shared_mutex wait_for_me;

      while (keep_working) {
        while (request_fifo.peek() == EOF) std::this_thread::yield();

        std::string raw_json;
        std::getline(request_fifo, raw_json, '\0');

        // Now we have our data, we can process it elsewhere
        // and continue reading from the fifo
        std::shared_lock lock{wait_for_me};
        std::thread([&](){
          try {
            std::shared_lock our_lock = std::move(lock);

            auto js = json::parse(raw_json);

            auto response_path = js.find("response_path");
            auto payload = js.find("payload");
            auto id = js.find("id");
            auto op = js.find("id");

            // Check we have a valid message
            // We will just drop it if it's invalid
            if (
              response_path == js.end() ||
              payload == js.end() ||
              id == js.end() ||
              op == js.end()
            ) return;

            json response;

            {
              std::shared_lock base_lock{base_mutex};
              if (auto handler = base->get(*op))
                response["payload"] = (*handler)(*payload);
            }

            response["id"] = *id;

            std::ofstream response_fifo{static_cast<std::string>(*response_path)};

            response_fifo << response << '\0';
          }
          catch (...) {}
        }).detach();
      }

      wait_for_me.lock();
    }

  public:
    unix_fifo_server(const std::string& path) {
      // We don't care if this fails
      ::unlink(path.c_str());

      if (::mkfifo(path.c_str(), 0600))
        throw std::invalid_argument{"Could not open fifo"};

      request_fifo = std::ifstream{path};
    }
  };

  class unix_fifo_client : channel_client {
  public:
    std::ifstream response_fifo = {};
    std::ofstream request_fifo;
    std::string response_path = "/tmp/AgileDisruption.Client.XXXXXX";

    std::unordered_map<uint64_t, std::promise<std::optional<json>>> waiting_for = {};
    std::shared_mutex waiting_for_mutex = {};
    // If there are > 2^64 simultaneous requests, we will have bigger problems...
    // This would take > 3500 years if this counter was just incremented without waiting
    std::atomic<uint64_t> next_id = 0;

    std::atomic<bool> keep_working = true;
    std::thread worker = {};

  public:
    void worker_body() {
      while (keep_working) {
        while (response_fifo.peek() == EOF) std::this_thread::yield();

        std::string raw_json;
        std::getline(response_fifo, raw_json, '\0');
        auto js = json::parse(raw_json);

        auto id = js.find("id");
        if (id != js.end()) {
          auto maybe_payload = js.find("payload");

          std::unique_lock lock{waiting_for_mutex};
          auto iter = waiting_for.find(*id);
          if (iter != waiting_for.end()) {
            if (maybe_payload == js.end())
              iter->second.set_value(std::nullopt);
            else
              iter->second.set_value(*maybe_payload);
          }
        }
      }
    }

    std::future<std::optional<json>> request(std::string op, const json& js) override {
      json to_send;

      auto id = next_id++;
      auto promise = std::promise<std::optional<json>>{};

      auto ret = promise.get_future();

      to_send["response_path"] = response_path;
      to_send["id"] = id;
      to_send["op"] = op;
      to_send["payload"] = js;

      request_fifo << to_send << '\0';

      waiting_for.insert_or_assign(id, std::move(promise));

      return ret;
    }

  public:
    unix_fifo_client(const std::string& path) : request_fifo(path) {
      char* fifo = &response_path[0];

      // Generate temporary paths until we find one we can use
      // Note: a collusion is unlikely, so this should only run once
      do ::mktemp(fifo);
      while (::mkfifo(fifo, 0600));

      worker = std::thread{&unix_fifo_client::worker_body, this};
    }

    ~unix_fifo_client() override {
      ::unlink(response_path.c_str());
    }
  };
}
