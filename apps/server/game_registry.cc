#include "game_registry.h"

#include <cstddef>
#include <cstdio>
#include <exception>
#include <utility>

namespace scrabble::server {
namespace {

using Json = nlohmann::json;

constexpr int kIdHexDigits = 12;

}  // namespace

std::string SseFrame(std::size_t id, std::string_view type, const Json& data) {
  std::string frame = "id: " + std::to_string(id) + "\nevent: ";
  frame += type;
  frame += "\ndata: ";
  frame += data.dump();
  frame += "\n\n";
  return frame;
}

GameSession::GameSession(std::string id, std::string mode)
    : id_(std::move(id)), mode_(std::move(mode)) {}

void GameSession::Publish(std::string_view type, Json data) {
  data["type"] = type;
  {
    const std::lock_guard lock(mutex_);
    if (closed_) return;
    frames_.push_back(SseFrame(frames_.size(), type, data));
  }
  changed_.notify_all();
}

void GameSession::Close() {
  {
    const std::lock_guard lock(mutex_);
    closed_ = true;
  }
  changed_.notify_all();
}

bool GameSession::closed() const {
  const std::lock_guard lock(mutex_);
  return closed_;
}

GameSession::ReadResult GameSession::Read(
    std::size_t from, std::chrono::milliseconds timeout,
    std::vector<std::string>& out) const {
  std::unique_lock lock(mutex_);
  changed_.wait_for(lock, timeout,
                    [&] { return closed_ || from < frames_.size(); });
  if (from < frames_.size()) {
    out.insert(out.end(),
               frames_.begin() + static_cast<std::ptrdiff_t>(from),
               frames_.end());
    return ReadResult::kData;
  }
  return closed_ ? ReadResult::kEnd : ReadResult::kTimeout;
}

std::size_t GameSession::size() const {
  const std::lock_guard lock(mutex_);
  return frames_.size();
}

void GameSession::set_gcg(std::string gcg) {
  const std::lock_guard lock(mutex_);
  gcg_ = std::move(gcg);
}

std::string GameSession::gcg() const {
  const std::lock_guard lock(mutex_);
  return gcg_;
}

Json GameSession::Summary() const {
  const std::lock_guard lock(mutex_);
  const char* status = !closed_                ? "running"
                       : cancel_requested_.load() ? "cancelled"
                                                  : "finished";
  return {{"id", id_},
          {"mode", mode_},
          {"status", status},
          {"events", frames_.size()},
          {"hasGcg", !gcg_.empty()}};
}

GameRegistry::GameRegistry(RegistryOptions options,
                           std::function<Clock::time_point()> now)
    : options_(options),
      now_(std::move(now)),
      id_source_(std::random_device{}()) {}

GameRegistry::~GameRegistry() {
  Map games;
  {
    const std::lock_guard lock(mutex_);
    games.swap(games_);
  }
  for (auto& [id, entry] : games) entry.session->RequestCancel();
  for (auto& [id, entry] : games) {
    if (entry.worker.joinable()) entry.worker.join();
  }
}

std::shared_ptr<GameSession> GameRegistry::StartBackground(std::string mode,
                                                           Task task) {
  std::vector<std::thread> finished;
  std::shared_ptr<GameSession> session;
  {
    const std::lock_guard lock(mutex_);
    SweepLocked(finished);
    if (RunningLocked() < options_.max_running && MakeRoomLocked(finished)) {
      Entry& entry = InsertLocked(std::move(mode))->second;
      session = entry.session;
      entry.worker = std::thread([session, task = std::move(task)] {
        try {
          task(*session);
        } catch (const std::exception& e) {
          // Tasks report their own failures as events; this is the last
          // line of defence, since an exception escaping a thread would
          // terminate the server.
          std::fprintf(stderr, "game %s failed: %s\n", session->id().c_str(),
                       e.what());
        } catch (...) {
          std::fprintf(stderr, "game %s failed\n", session->id().c_str());
        }
        session->Close();
      });
    }
  }
  Join(finished);
  return session;
}

std::shared_ptr<GameSession> GameRegistry::AddCompleted(std::string mode,
                                                        const Task& task) {
  std::vector<std::thread> finished;
  std::shared_ptr<GameSession> session;
  {
    const std::lock_guard lock(mutex_);
    SweepLocked(finished);
    if (MakeRoomLocked(finished)) {
      session = InsertLocked(std::move(mode))->second.session;
    }
  }
  Join(finished);
  if (session != nullptr) {
    // Outside the lock, so a slow task does not stall other requests.
    // Clients learn the id only from the return value, so none can be
    // reading the session before it is complete.
    task(*session);
    session->Close();
  }
  return session;
}

std::shared_ptr<GameSession> GameRegistry::Find(std::string_view id) {
  std::vector<std::thread> finished;
  std::shared_ptr<GameSession> session;
  {
    const std::lock_guard lock(mutex_);
    SweepLocked(finished);
    const auto it = games_.find(id);
    if (it != games_.end()) {
      it->second.last_watched = now_();
      session = it->second.session;
    }
  }
  Join(finished);
  return session;
}

bool GameRegistry::Cancel(std::string_view id) {
  std::vector<std::thread> finished;
  bool found = false;
  {
    const std::lock_guard lock(mutex_);
    SweepLocked(finished);
    const auto it = games_.find(id);
    if (it != games_.end()) {
      // A finished game has nothing to stop; leave its status alone.
      if (!it->second.session->closed()) it->second.session->RequestCancel();
      found = true;
    }
  }
  Join(finished);
  return found;
}

void GameRegistry::Sweep() {
  std::vector<std::thread> finished;
  {
    const std::lock_guard lock(mutex_);
    SweepLocked(finished);
  }
  Join(finished);
}

std::size_t GameRegistry::size() const {
  const std::lock_guard lock(mutex_);
  return games_.size();
}

std::size_t GameRegistry::running() const {
  const std::lock_guard lock(mutex_);
  return RunningLocked();
}

void GameRegistry::SweepLocked(std::vector<std::thread>& finished) {
  const Clock::time_point now = now_();
  for (auto it = games_.begin(); it != games_.end();) {
    Entry& entry = it->second;
    GameSession& session = *entry.session;
    if (session.closed()) {
      if (!entry.finished_at) entry.finished_at = now;
      if (now - *entry.finished_at >= options_.finished_ttl) {
        if (entry.worker.joinable()) {
          finished.push_back(std::move(entry.worker));
        }
        it = games_.erase(it);
        continue;
      }
    } else if (session.readers() > 0) {
      entry.last_watched = now;
    } else if (now - entry.last_watched >= options_.idle_ttl) {
      session.RequestCancel();
    }
    ++it;
  }
}

bool GameRegistry::MakeRoomLocked(std::vector<std::thread>& finished) {
  while (games_.size() >= options_.max_games) {
    auto oldest = games_.end();
    for (auto it = games_.begin(); it != games_.end(); ++it) {
      if (!it->second.session->closed()) continue;
      if (oldest == games_.end() ||
          it->second.sequence < oldest->second.sequence) {
        oldest = it;
      }
    }
    if (oldest == games_.end()) return false;  // all running
    if (oldest->second.worker.joinable()) {
      finished.push_back(std::move(oldest->second.worker));
    }
    games_.erase(oldest);
  }
  return true;
}

std::size_t GameRegistry::RunningLocked() const {
  std::size_t running = 0;
  for (const auto& [id, entry] : games_) {
    if (!entry.session->closed()) ++running;
  }
  return running;
}

std::string GameRegistry::NewIdLocked() {
  static constexpr char kHex[] = "0123456789abcdef";
  while (true) {
    std::uint64_t bits = id_source_();
    std::string id(kIdHexDigits, '0');
    for (char& c : id) {
      c = kHex[bits & 0xF];
      bits >>= 4;
    }
    if (!games_.contains(id)) return id;
  }
}

GameRegistry::Map::iterator GameRegistry::InsertLocked(std::string mode) {
  std::string id = NewIdLocked();
  Entry entry;
  entry.session = std::make_shared<GameSession>(id, std::move(mode));
  entry.sequence = sequence_++;
  entry.last_watched = now_();
  return games_.emplace(std::move(id), std::move(entry)).first;
}

void GameRegistry::Join(std::vector<std::thread>& threads) {
  for (std::thread& thread : threads) thread.join();
  threads.clear();
}

}  // namespace scrabble::server
