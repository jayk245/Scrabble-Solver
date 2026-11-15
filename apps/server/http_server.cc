#include "http_server.h"

#include <httplib.h>

#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>

namespace scrabble::server {
namespace {

// Requests carry at most a board, a rack and a move; anything larger is a
// mistake or abuse.
constexpr std::size_t kMaxRequestBytes = 64 * 1024;

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

}  // namespace

bool Serve(const Api& api, const HttpOptions& options) {
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

  // Reserved for game streaming (see api.h); registered now so the routes
  // and their status are discoverable.
  server.Post("/api/games",
              [](const httplib::Request&, httplib::Response& res) {
                Send(res, Api::NotImplemented("starting games"));
              });
  server.Get(R"(/api/games/([^/]+)/events)",
             [](const httplib::Request&, httplib::Response& res) {
               Send(res, Api::NotImplemented("game event streams"));
             });

  // Unknown API paths get a JSON 404 rather than falling through to the
  // static files, so client bugs surface as clear errors.
  const auto not_found = [](const httplib::Request& req,
                            httplib::Response& res) {
    Send(res, Api::NotFound(req.path));
  };
  server.Get(R"(/api/.*)", not_found);
  server.Post(R"(/api/.*)", not_found);

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
