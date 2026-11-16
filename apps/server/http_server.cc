#include "http_server.h"

#include <httplib.h>

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "game_registry.h"

namespace scrabble::server {
namespace {

// Requests carry at most a board, a rack and a move, or one GCG record
// (a few kilobytes; games.cc caps it at 64 KiB); anything larger is a
// mistake or abuse.
constexpr std::size_t kMaxRequestBytes = 128 * 1024;

constexpr const char* kJsonType = "application/json";

constexpr const char* kMissingFrontendPage = R"(<!doctype html>
<html><head><meta charset="utf-8"><title>Scrabble server</title></head>
<body style="font-family: system-ui, sans-serif; max-width: 40rem;
             margin: 3rem auto">
<h1>Frontend not built</h1>
<p>The API is running, but no production build was found. Either build it:</p>
<pre>cd web &amp;&amp; npm ci &amp;&amp; npm run build</pre>
<p>and restart the server, or run the Vite dev server (<code>npm run dev</code>)
and open <a href="http://localhost:5173">http://localhost:5173</a>.</p>
</body></html>
)";

void Send(httplib::Response& res, const ApiResponse& response) {
  res.status = response.status;
  res.set_content(response.body.dump(), kJsonType);
}

// Wraps a POST handler that consumes the request body.
template <typename Method>
httplib::Server::Handler Post(const Api& api, Method method) {
  return [&api, method](const httplib::Request& req, httplib::Response& res) {
    Send(res, (api.*method)(req.body));
  };
}

// How long one call of the stream's content provider waits for an event.
// Bounded so the provider regularly returns to cpp-httplib, which is how a
// closed connection is noticed.
constexpr std::chrono::milliseconds kStreamPoll{1000};
// A comment line is sent after this many empty polls, so proxies do not
// time the idle stream out and a vanished client is detected by the
// failing write.
constexpr int kKeepAlivePolls = 15;

// The index of the first event a client still needs: zero for a new
// stream, or one past the Last-Event-ID an EventSource sends when it
// reconnects.
std::size_t ResumeIndex(const httplib::Request& req) {
  const std::string last = req.get_header_value("Last-Event-ID");
  if (last.empty()) return 0;
  std::size_t index = 0;
  const auto [end, error] =
      std::from_chars(last.data(), last.data() + last.size(), index);
  if (error != std::errc() || end != last.data() + last.size()) return 0;
  return index + 1;
}

// Serves GET /api/games/{id}/events as text/event-stream.
//
// cpp-httplib calls the chunked content provider repeatedly on this
// connection's worker thread until it calls sink.done() or returns false.
// Each call waits at most kStreamPoll for new events, writes whatever has
// arrived, and returns, so the thread is never parked indefinitely and the
// game's producer is never blocked by a slow reader.
void StreamEvents(GamesApi& games, const httplib::Request& req,
                  httplib::Response& res) {
  const std::string id = req.matches[1].str();
  std::shared_ptr<GameSession> session = games.Find(id);
  if (session == nullptr) {
    Send(res, ErrorResponse(404, "unknown_game",
                            "no game " + id + " (it may have expired)"));
    return;
  }
  res.set_header("Cache-Control", "no-cache");
  // Disables response buffering in nginx-style reverse proxies.
  res.set_header("X-Accel-Buffering", "no");

  struct Cursor {
    std::size_t next = 0;
    int idle_polls = 0;
  };
  auto cursor = std::make_shared<Cursor>();
  cursor->next = ResumeIndex(req);
  session->AddReader();
  res.set_chunked_content_provider(
      "text/event-stream",
      [session, cursor](std::size_t /*offset*/, httplib::DataSink& sink) {
        std::vector<std::string> frames;
        switch (session->Read(cursor->next, kStreamPoll, frames)) {
          case GameSession::ReadResult::kEnd:
            sink.done();
            return true;
          case GameSession::ReadResult::kTimeout: {
            if (++cursor->idle_polls < kKeepAlivePolls) {
              return sink.is_writable();
            }
            cursor->idle_polls = 0;
            static constexpr std::string_view kKeepAlive = ": keep-alive\n\n";
            return sink.write(kKeepAlive.data(), kKeepAlive.size());
          }
          case GameSession::ReadResult::kData:
            for (const std::string& frame : frames) {
              if (!sink.write(frame.data(), frame.size())) return false;
            }
            cursor->next += frames.size();
            cursor->idle_polls = 0;
            return true;
        }
        return false;
      },
      [session](bool /*success*/) { session->RemoveReader(); });
}

}  // namespace

bool Serve(const Api& api, GamesApi& games, const HttpOptions& options) {
  httplib::Server server;
  server.set_payload_max_length(kMaxRequestBytes);

  server.Get("/api/health",
             [&api](const httplib::Request&, httplib::Response& res) {
               Send(res, api.Health());
             });
  server.Get("/api/rules",
             [&api](const httplib::Request&, httplib::Response& res) {
               Send(res, api.Rules());
             });
  server.Post("/api/position", Post(api, &Api::PositionSummary));
  server.Post("/api/moves", Post(api, &Api::Moves));
  server.Post("/api/validate", Post(api, &Api::Validate));
  server.Post("/api/play", Post(api, &Api::Play));

  // Game viewer (see games.h). These handlers only touch the registry and
  // event logs; the engine runs on the registry's worker threads.
  server.Post("/api/games",
              [&games](const httplib::Request& req, httplib::Response& res) {
                Send(res, games.Start(req.body));
              });
  server.Get(R"(/api/games/([0-9a-f]+))",
             [&games](const httplib::Request& req, httplib::Response& res) {
               Send(res, games.Status(req.matches[1].str()));
             });
  server.Delete(R"(/api/games/([0-9a-f]+))",
                [&games](const httplib::Request& req, httplib::Response& res) {
                  Send(res, games.Cancel(req.matches[1].str()));
                });
  server.Get(R"(/api/games/([0-9a-f]+)/gcg)",
             [&games](const httplib::Request& req, httplib::Response& res) {
               const std::string id = req.matches[1].str();
               const std::shared_ptr<GameSession> session = games.Find(id);
               const std::string gcg = session ? session->gcg() : "";
               if (gcg.empty()) {
                 Send(res, ErrorResponse(404, "no_gcg",
                                         "no record for game " + id +
                                             " (unknown, or still running)"));
                 return;
               }
               res.set_header("Content-Disposition",
                              "attachment; filename=\"game-" + id + ".gcg\"");
               res.set_content(gcg, "text/plain; charset=utf-8");
             });
  // Human-vs-bot games (human_game.h).
  server.Post(R"(/api/games/([0-9a-f]+)/moves)",
              [&games](const httplib::Request& req, httplib::Response& res) {
                Send(res, games.HumanMove(req.matches[1].str(), req.body));
              });
  server.Get(R"(/api/games/([0-9a-f]+)/hint)",
             [&games](const httplib::Request& req, httplib::Response& res) {
               Send(res, games.Hint(req.matches[1].str()));
             });
  server.Get(R"(/api/games/([0-9a-f]+)/events)",
             [&games](const httplib::Request& req, httplib::Response& res) {
               StreamEvents(games, req, res);
             });

  // Unknown API paths get a JSON 404 rather than falling through to the
  // static files, so client bugs surface as clear errors.
  const auto not_found = [](const httplib::Request& req,
                            httplib::Response& res) {
    Send(res, Api::NotFound(req.path));
  };
  server.Get(R"(/api/.*)", not_found);
  server.Post(R"(/api/.*)", not_found);
  server.Delete(R"(/api/.*)", not_found);

  std::error_code ec;
  if (std::filesystem::exists(options.static_dir / "index.html", ec)) {
    server.set_mount_point("/", options.static_dir.string());
  } else {
    server.Get("/", [](const httplib::Request&, httplib::Response& res) {
      res.set_content(kMissingFrontendPage, "text/html");
    });
  }

  server.set_exception_handler([](const httplib::Request&,
                                  httplib::Response& res,
                                  const std::exception_ptr& error) {
    std::string message = "unknown error";
    try {
      std::rethrow_exception(error);
    } catch (const std::exception& e) {
      message = e.what();
    } catch (...) {
    }
    Send(res, ApiResponse{500, {{"error", "internal_error"},
                                {"message", message}}});
  });

  server.set_logger([](const httplib::Request& req,
                       const httplib::Response& res) {
    std::fprintf(stderr, "%s %s -> %d\n", req.method.c_str(),
                 req.path.c_str(), res.status);
  });

  if (!server.bind_to_port(options.host, options.port)) return false;
  std::fprintf(stderr, "Listening on http://%s:%d\n", options.host.c_str(),
               options.port);
  return server.listen_after_bind();
}

}  // namespace scrabble::server
