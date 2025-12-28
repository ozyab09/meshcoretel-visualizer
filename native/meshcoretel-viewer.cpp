#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <curl/curl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
#include <csignal>
#include <execinfo.h>
#include <exception>
#include <unordered_map>
#include <vector>

#include "json.hpp"

namespace {
using nlohmann::json;
std::mutex g_json_mutex;
constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 720;
constexpr int kTileSize = 256;
constexpr int kDefaultZoom = 10;
constexpr double kMoscowLat = 55.7558;
constexpr double kMoscowLon = 37.6176;
constexpr int kMaxPacketMessages = 5;
constexpr double kPi = 3.14159265358979323846;

struct Node {
  int id = 0;
  int node_hash = 0;
  double lat = 0.0;
  double lon = 0.0;
  bool has_position = false;
  bool is_room_server = false;
  bool is_repeater = false;
  bool is_chat_node = false;
  bool is_sensor = false;
  std::string name;
  std::string public_key_hex;
};

struct PacketMessage {
  std::string text;
  uint64_t timestamp_ms = 0;
};

struct MovingPulse {
  SDL_FPoint start;
  SDL_FPoint end;
  uint64_t start_time_ms = 0;
  float duration_ms = 1200.0f;
};

struct PathAnimation {
  std::vector<SDL_FPoint> points;
  uint64_t start_time_ms = 0;
  float duration_ms = 1500.0f;
  SDL_Color color{0, 255, 234, 255};
  float width = 2.0f;
};

struct AppState {
  std::vector<Node> nodes;
  std::unordered_map<int, size_t> node_hash_index;
  std::deque<PacketMessage> packet_messages;
  std::vector<MovingPulse> pulses;
  std::vector<PathAnimation> paths;
  std::string connection_status = "Initializing...";
  std::string last_update = "Never";
  int selected_node_index = -1;
  bool animations_enabled = true;
};

class LogSink {
 public:
  LogSink() {
    file_.open("native/client.log", std::ios::app);
    if (file_) {
      file_ << "---- client start ----\n";
    }
  }

  void Write(const std::string &line) {
    std::cerr << line << "\n";
    if (file_) {
      file_ << line << "\n";
      file_.flush();
    }
  }

 private:
  std::ofstream file_;
};

LogSink *g_log = nullptr;
volatile std::sig_atomic_t g_should_quit = 0;

void SignalHandler(int sig) {
  if (g_log) {
    g_log->Write(std::string("Fatal signal: ") + std::to_string(sig));
    void *frames[32];
    int count = backtrace(frames, 32);
    char **symbols = backtrace_symbols(frames, count);
    if (symbols) {
      for (int i = 0; i < count; i++) {
        g_log->Write(std::string("  ") + symbols[i]);
      }
      std::free(symbols);
    }
  }
  std::_Exit(1);
}

void QuitSignalHandler(int sig) {
  (void)sig;
  g_should_quit = 1;
}

struct TileKey {
  int z;
  int x;
  int y;
};

struct TileKeyHash {
  size_t operator()(const TileKey &key) const {
    return (static_cast<size_t>(key.z) << 48) ^
           (static_cast<size_t>(key.x) << 24) ^
           static_cast<size_t>(key.y);
  }
};

struct TileKeyEq {
  bool operator()(const TileKey &a, const TileKey &b) const {
    return a.z == b.z && a.x == b.x && a.y == b.y;
  }
};

struct TileTexture {
  SDL_Texture *texture = nullptr;
  int width = 0;
  int height = 0;
};

uint64_t NowMs() {
  return static_cast<uint64_t>(SDL_GetTicks64());
}

double DegToRad(double deg) {
  return deg * kPi / 180.0;
}

void LatLonToTile(double lat, double lon, int zoom, double *out_x, double *out_y) {
  const double n = std::pow(2.0, zoom);
  const double lat_rad = DegToRad(lat);
  *out_x = (lon + 180.0) / 360.0 * n;
  *out_y = (1.0 - std::log(std::tan(lat_rad) + 1.0 / std::cos(lat_rad)) / kPi) / 2.0 * n;
}

void LatLonToWorldPixel(double lat, double lon, int zoom, double *out_x, double *out_y) {
  double tile_x = 0.0;
  double tile_y = 0.0;
  LatLonToTile(lat, lon, zoom, &tile_x, &tile_y);
  *out_x = tile_x * kTileSize;
  *out_y = tile_y * kTileSize;
}

bool EnsureDir(const std::string &path) {
  std::string command = "mkdir -p \"" + path + "\"";
  return std::system(command.c_str()) == 0;
}

bool FileExists(const std::string &path) {
  std::ifstream file(path);
  return file.good();
}

size_t CurlWriteToString(void *contents, size_t size, size_t nmemb, void *userp) {
  size_t total = size * nmemb;
  std::string *out = static_cast<std::string *>(userp);
  out->append(static_cast<char *>(contents), total);
  return total;
}

std::string HttpGet(const std::string &url) {
  CURL *curl = curl_easy_init();
  if (!curl) {
    return {};
  }
  std::string response;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteToString);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "meshcoretel-native/1.0");
  CURLcode res = curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  if (res != CURLE_OK) {
    std::cerr << "HTTP GET failed: " << url << " (" << curl_easy_strerror(res) << ")\n";
    return {};
  }
  return response;
}

bool WriteFile(const std::string &path, const std::string &data) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  return true;
}

SDL_Texture *LoadTextureFromFile(SDL_Renderer *renderer, const std::string &path, int *w, int *h) {
  SDL_Surface *surface = IMG_Load(path.c_str());
  if (!surface) {
    return nullptr;
  }
  SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
  if (w) {
    *w = surface->w;
  }
  if (h) {
    *h = surface->h;
  }
  SDL_FreeSurface(surface);
  return texture;
}

class TileCache {
 public:
  TileCache(SDL_Renderer *renderer, const std::string &cache_root)
      : renderer_(renderer), cache_root_(cache_root) {}

  TileTexture GetTile(int zoom, int x, int y) {
    TileKey key{zoom, x, y};
    auto it = tiles_.find(key);
    if (it != tiles_.end()) {
      return it->second;
    }
    TileTexture tex = LoadTile(zoom, x, y);
    tiles_.emplace(key, tex);
    return tex;
  }

  void Clear() {
    for (auto &entry : tiles_) {
      if (entry.second.texture) {
        SDL_DestroyTexture(entry.second.texture);
      }
    }
    tiles_.clear();
  }

 private:
  TileTexture LoadTile(int zoom, int x, int y) {
    TileTexture tex;
    std::ostringstream dir;
    dir << cache_root_ << "/" << zoom << "/" << x;
    EnsureDir(dir.str());

    std::ostringstream path;
    path << dir.str() << "/" << y << ".png";

    if (!FileExists(path.str())) {
      std::ostringstream url;
      url << "https://a.tile.openstreetmap.org/" << zoom << "/" << x << "/" << y << ".png";
      std::string data = HttpGet(url.str());
      if (!data.empty()) {
        WriteFile(path.str(), data);
      }
    }

    tex.texture = LoadTextureFromFile(renderer_, path.str(), &tex.width, &tex.height);
    return tex;
  }

  SDL_Renderer *renderer_ = nullptr;
  std::string cache_root_;
  std::unordered_map<TileKey, TileTexture, TileKeyHash, TileKeyEq> tiles_;
};

json ParseJson(const std::string &input) {
  std::lock_guard<std::mutex> lock(g_json_mutex);
  return json::parse(input, nullptr, false);
}

std::string JsonGetString(const json &obj, const char *key) {
  auto it = obj.find(key);
  if (it != obj.end() && it->is_string()) {
    return it->get<std::string>();
  }
  return {};
}

bool JsonGetBool(const json &obj, const char *key, bool fallback) {
  auto it = obj.find(key);
  if (it != obj.end() && it->is_boolean()) {
    return it->get<bool>();
  }
  return fallback;
}

bool LooksLikeJsonObject(const std::string &input) {
  for (char c : input) {
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      continue;
    }
    return c == '{';
  }
  return false;
}

std::string ExtractJsonStringField(const std::string &input, const char *key) {
  std::string pattern = "\"";
  pattern += key;
  pattern += "\"";
  size_t pos = input.find(pattern);
  if (pos == std::string::npos) {
    return {};
  }
  pos = input.find(':', pos + pattern.size());
  if (pos == std::string::npos) {
    return {};
  }
  pos++;
  while (pos < input.size() && (input[pos] == ' ' || input[pos] == '\t' || input[pos] == '\r' || input[pos] == '\n')) {
    pos++;
  }
  if (pos >= input.size() || input[pos] != '"') {
    return {};
  }
  pos++;
  std::string out;
  out.reserve(128);
  bool escape = false;
  for (; pos < input.size(); pos++) {
    char c = input[pos];
    if (escape) {
      out.push_back(c);
      escape = false;
      continue;
    }
    if (c == '\\') {
      escape = true;
      continue;
    }
    if (c == '"') {
      break;
    }
    out.push_back(c);
  }
  return out;
}

std::string UnescapeJsonString(const std::string &input) {
  std::string out;
  out.reserve(input.size());
  bool escape = false;
  for (size_t i = 0; i < input.size(); i++) {
    char c = input[i];
    if (escape) {
      switch (c) {
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case '\\': out.push_back('\\'); break;
        case '"': out.push_back('"'); break;
        default: out.push_back(c); break;
      }
      escape = false;
      continue;
    }
    if (c == '\\') {
      escape = true;
      continue;
    }
    out.push_back(c);
  }
  return out;
}

std::string FormatTimeNow() {
  std::time_t now = std::time(nullptr);
  std::tm tm = *std::localtime(&now);
  char buf[16];
  std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
  return buf;
}

void UpdateNodeIndex(AppState &state) {
  state.node_hash_index.clear();
  for (size_t i = 0; i < state.nodes.size(); i++) {
    if (state.nodes[i].node_hash != 0) {
      state.node_hash_index[state.nodes[i].node_hash] = i;
    }
  }
}

std::vector<Node> ParseNodesJson(const std::string &json) {
  std::vector<Node> nodes;
  try {
    auto root = ParseJson(json);
    if (root.is_discarded() || !root.is_array()) {
      std::cerr << "ParseNodesJson: unexpected JSON root\n";
      return nodes;
    }

    for (const auto &entry : root) {
      if (!entry.is_object()) {
        continue;
      }
      Node node;
      auto id_it = entry.find("id");
      if (id_it != entry.end() && id_it->is_number()) {
        node.id = id_it->get<int>();
      }
      auto hash_it = entry.find("node_hash");
      if (hash_it != entry.end() && hash_it->is_number()) {
        node.node_hash = hash_it->get<int>();
      }
      node.name = JsonGetString(entry, "name");
      node.public_key_hex = JsonGetString(entry, "public_key_hex");
      node.is_room_server = JsonGetBool(entry, "is_room_server", false);
      node.is_repeater = JsonGetBool(entry, "is_repeater", false);
      node.is_chat_node = JsonGetBool(entry, "is_chat_node", false);
      node.is_sensor = JsonGetBool(entry, "is_sensor", false);

      auto lat_it = entry.find("lat");
      auto lon_it = entry.find("lon");
      auto lng_it = entry.find("lng");
      bool has_lat = lat_it != entry.end() && lat_it->is_number();
      bool has_lon = lon_it != entry.end() && lon_it->is_number();
      bool has_lng = lng_it != entry.end() && lng_it->is_number();

      if (has_lat) {
        node.lat = lat_it->get<double>();
      }
      if (has_lon) {
        node.lon = lon_it->get<double>();
      } else if (has_lng) {
        node.lon = lng_it->get<double>();
        has_lon = true;
      }

      node.has_position = has_lat && has_lon && !(node.lat == 0.0 && node.lon == 0.0) &&
                          std::abs(node.lat) <= 90.0 && std::abs(node.lon) <= 180.0;

      nodes.push_back(node);
    }
  } catch (const std::exception &e) {
    std::cerr << "ParseNodesJson error: " << e.what() << "\n";
  }

  return nodes;
}

SDL_Color ColorForNode(const Node &node) {
  if (node.is_room_server) {
    return SDL_Color{250, 204, 21, 255};
  }
  if (node.is_repeater) {
    return SDL_Color{59, 130, 246, 255};
  }
  if (node.is_chat_node) {
    return SDL_Color{16, 185, 129, 255};
  }
  if (node.is_sensor) {
    return SDL_Color{239, 68, 68, 255};
  }
  return SDL_Color{0, 255, 234, 255};
}

const Node *FindNodeByPublicKeyPrefix(const std::vector<Node> &nodes, const std::string &prefix) {
  if (prefix.empty()) {
    return nullptr;
  }
  std::string needle = prefix;
  std::transform(needle.begin(), needle.end(), needle.begin(), ::toupper);
  for (const auto &node : nodes) {
    if (node.public_key_hex.empty()) {
      continue;
    }
    std::string hex = node.public_key_hex;
    std::transform(hex.begin(), hex.end(), hex.begin(), ::toupper);
    if (hex.rfind(needle, 0) == 0) {
      return &node;
    }
  }
  return nullptr;
}

const Node *FindNodeByPropagationToken(const std::vector<Node> &nodes, const std::string &token) {
  if (token.empty()) {
    return nullptr;
  }
  std::string needle = token;
  std::transform(needle.begin(), needle.end(), needle.begin(), ::toupper);
  for (const auto &node : nodes) {
    if (!node.public_key_hex.empty()) {
      std::string hex = node.public_key_hex;
      std::transform(hex.begin(), hex.end(), hex.begin(), ::toupper);
      if (hex.rfind(needle, 0) == 0) {
        return &node;
      }
    }
    if (node.node_hash != 0) {
      std::ostringstream oss;
      oss << std::uppercase << std::hex << node.node_hash;
      std::string hash_hex = oss.str();
      if (hash_hex.rfind(needle, 0) == 0) {
        return &node;
      }
    }
  }
  return nullptr;
}

void DrawFilledCircle(SDL_Renderer *renderer, int cx, int cy, int radius, SDL_Color color) {
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  for (int dy = -radius; dy <= radius; dy++) {
    int dx = static_cast<int>(std::sqrt(radius * radius - dy * dy));
    SDL_RenderDrawLine(renderer, cx - dx, cy + dy, cx + dx, cy + dy);
  }
}

void DrawThickLine(SDL_Renderer *renderer, int x1, int y1, int x2, int y2,
                   float width, SDL_Color color, SDL_BlendMode blend_mode) {
  SDL_SetRenderDrawBlendMode(renderer, blend_mode);
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  float dx = static_cast<float>(x2 - x1);
  float dy = static_cast<float>(y2 - y1);
  float len = std::sqrt(dx * dx + dy * dy);
  if (len < 1.0f) {
    SDL_RenderDrawLine(renderer, x1, y1, x2, y2);
    return;
  }
  float nx = -dy / len;
  float ny = dx / len;
  int half = static_cast<int>(std::max(1.0f, width) / 2.0f);
  for (int i = -half; i <= half; i++) {
    int ox = static_cast<int>(nx * i);
    int oy = static_cast<int>(ny * i);
    SDL_RenderDrawLine(renderer, x1 + ox, y1 + oy, x2 + ox, y2 + oy);
  }
}

void DrawText(SDL_Renderer *renderer, TTF_Font *font, const std::string &text,
              SDL_Color color, int x, int y) {
  SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text.c_str(), color);
  if (!surface) {
    return;
  }
  SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
  SDL_Rect dst{ x, y, surface->w, surface->h };
  SDL_FreeSurface(surface);
  if (!texture) {
    return;
  }
  SDL_RenderCopy(renderer, texture, nullptr, &dst);
  SDL_DestroyTexture(texture);
}

struct SseStreamState {
  std::string buffer;
  std::mutex *mutex = nullptr;
  AppState *state = nullptr;
};

void HandlePacketMessage(AppState &state, const std::string &payload) {
  try {
    if (payload.size() > 1024 * 1024 || !LooksLikeJsonObject(payload)) {
      return;
    }
    auto root = ParseJson(payload);
    if (root.is_discarded() || !root.is_object()) {
      return;
    }

    std::string direction = JsonGetString(root, "direction");
    std::string sender = JsonGetString(root, "sender_name");
    if (sender.empty()) {
      sender = JsonGetString(root, "group_sender_name");
    }
    if (sender.empty()) {
      sender = JsonGetString(root, "advert_name");
    }
    std::string origin = JsonGetString(root, "origin");
    if (sender.empty()) {
      sender = "unknown";
    }
    if (origin.empty()) {
      origin = "unknown";
    }

    std::string time_prefix = FormatTimeNow();

    std::ostringstream message;
    if (!time_prefix.empty()) {
      message << time_prefix << " ";
    }
    if (!direction.empty()) {
      std::transform(direction.begin(), direction.end(), direction.begin(), ::toupper);
      message << direction << ": ";
    }
    message << sender << " -> " << origin;

    state.packet_messages.push_front(PacketMessage{message.str(), NowMs()});
    while (state.packet_messages.size() > kMaxPacketMessages) {
      state.packet_messages.pop_back();
    }

    const Node *src_node = nullptr;
    const Node *dst_node = nullptr;
    auto src_hash_it = root.find("src_hash");
    auto dst_hash_it = root.find("dst_hash");
    if (src_hash_it != root.end() && dst_hash_it != root.end()) {
      if (src_hash_it->is_number() && dst_hash_it->is_number()) {
        int src_hash = src_hash_it->get<int>();
        int dst_hash = dst_hash_it->get<int>();
        auto src_it = state.node_hash_index.find(src_hash);
        auto dst_it = state.node_hash_index.find(dst_hash);
        if (src_it != state.node_hash_index.end()) {
          src_node = &state.nodes[src_it->second];
        }
        if (dst_it != state.node_hash_index.end()) {
          dst_node = &state.nodes[dst_it->second];
        }
      } else if (src_hash_it->is_string() && dst_hash_it->is_string()) {
        std::string src_prefix = src_hash_it->get<std::string>();
        std::string dst_prefix = dst_hash_it->get<std::string>();
        src_node = FindNodeByPublicKeyPrefix(state.nodes, src_prefix);
        dst_node = FindNodeByPublicKeyPrefix(state.nodes, dst_prefix);
      }
    }

    if (src_node && dst_node && src_node->has_position && dst_node->has_position) {
      MovingPulse pulse;
      double sx = 0.0;
      double sy = 0.0;
      double ex = 0.0;
      double ey = 0.0;
      LatLonToWorldPixel(src_node->lat, src_node->lon, kDefaultZoom, &sx, &sy);
      LatLonToWorldPixel(dst_node->lat, dst_node->lon, kDefaultZoom, &ex, &ey);
      pulse.start.x = static_cast<float>(sx);
      pulse.start.y = static_cast<float>(sy);
      pulse.end.x = static_cast<float>(ex);
      pulse.end.y = static_cast<float>(ey);
      pulse.start_time_ms = NowMs();
      state.pulses.push_back(pulse);
    }
  } catch (const std::exception &e) {
    std::cerr << "Packet parse error: " << e.what() << "\n";
  }
}

void HandlePropagationMessage(AppState &state, const std::string &payload) {
  static int propagation_seen = 0;
  try {
    if (payload.size() > 1024 * 1024 || !LooksLikeJsonObject(payload)) {
      return;
    }
    auto root = ParseJson(payload);
    if (root.is_discarded() || !root.is_object()) {
      return;
    }
    std::string type = JsonGetString(root, "type");
    if (type != "propagation.path") {
      return;
    }
    propagation_seen++;
    if (propagation_seen <= 5 || propagation_seen % 50 == 0) {
      std::cerr << "Propagation event received (" << propagation_seen << ")\n";
    }
    if (propagation_seen <= 2) {
      std::string preview = payload.substr(0, 400);
      std::cerr << "Propagation payload preview: " << preview << "\n";
    }

    auto path_it = root.find("path");
    if (path_it == root.end() || !path_it->is_object()) {
      return;
    }
    auto nodes_it = path_it->find("nodes");
    if (nodes_it == path_it->end() || !nodes_it->is_array()) {
      return;
    }
    if (nodes_it->size() < 2) {
      return;
    }

    PathAnimation anim;
    anim.start_time_ms = NowMs();
    anim.duration_ms = std::max(800.0f, static_cast<float>(nodes_it->size()) * 250.0f);

    for (const auto &node_value : *nodes_it) {
      const Node *node = nullptr;
      if (node_value.is_number()) {
        int node_hash = node_value.get<int>();
        auto it = state.node_hash_index.find(node_hash);
        if (it != state.node_hash_index.end()) {
          node = &state.nodes[it->second];
        }
      } else if (node_value.is_string()) {
        node = FindNodeByPropagationToken(state.nodes, node_value.get<std::string>());
      }
      if (node && node->has_position) {
        double px = 0.0;
        double py = 0.0;
        LatLonToWorldPixel(node->lat, node->lon, kDefaultZoom, &px, &py);
        SDL_FPoint pt{static_cast<float>(px), static_cast<float>(py)};
        anim.points.push_back(pt);
      }
    }

    if (anim.points.size() >= 2) {
      static const SDL_Color colors[] = {
        {59, 130, 246, 255},   // blue
        {250, 204, 21, 255},   // yellow
        {16, 185, 129, 255},   // green
        {239, 68, 68, 255},    // red
        {139, 92, 246, 255},   // purple
        {6, 182, 212, 255},    // cyan
        {249, 115, 22, 255}    // orange
      };
      static uint32_t seed = 0;
      if (seed == 0) {
        seed = static_cast<uint32_t>(NowMs());
      }
      seed = seed * 1664525u + 1013904223u;
      anim.color = colors[seed % (sizeof(colors) / sizeof(colors[0]))];
      anim.width = 3.5f;
      state.paths.push_back(anim);
      if (propagation_seen <= 5 || propagation_seen % 50 == 0) {
        std::cerr << "Propagation path points: " << anim.points.size() << "\n";
      }
    } else if (propagation_seen <= 5 || propagation_seen % 50 == 0) {
      std::cerr << "Propagation path dropped (matched points: " << anim.points.size() << ")\n";
    }
  } catch (const std::exception &e) {
    std::cerr << "Propagation parse error: " << e.what() << "\n";
  }
}

void HandleSseMessage(AppState &state, const std::string &json) {
  try {
    if (json.size() > 1024 * 1024 || !LooksLikeJsonObject(json)) {
      return;
    }
    std::string type = ExtractJsonStringField(json, "type");

    if (type == "statusUpdate" || type == "connected") {
      std::string status = UnescapeJsonString(ExtractJsonStringField(json, "connectionStatus"));
      if (!status.empty()) {
        state.connection_status = status;
      }
      return;
    }

    if (type == "ping") {
      state.last_update = FormatTimeNow();
      return;
    }

    if (type == "packet" || type == "propagation") {
      std::string data = UnescapeJsonString(ExtractJsonStringField(json, "data"));
      if (data.empty()) {
        return;
      }
      std::string payload = data;
      if (type == "packet") {
        HandlePacketMessage(state, payload);
      } else {
        HandlePropagationMessage(state, payload);
      }
      state.last_update = FormatTimeNow();
      return;
    }
  } catch (const std::exception &e) {
    std::cerr << "SSE parse error: " << e.what() << "\n";
  }
}

size_t CurlWriteSse(void *contents, size_t size, size_t nmemb, void *userp) {
  size_t total = size * nmemb;
  SseStreamState *stream = static_cast<SseStreamState *>(userp);
  stream->buffer.append(static_cast<char *>(contents), total);

  size_t pos = 0;
  while ((pos = stream->buffer.find('\n')) != std::string::npos) {
    std::string line = stream->buffer.substr(0, pos);
    stream->buffer.erase(0, pos + 1);
    if (line.rfind("data: ", 0) == 0) {
      std::string payload = line.substr(6);
      std::lock_guard<std::mutex> lock(*stream->mutex);
      try {
        HandleSseMessage(*stream->state, payload);
      } catch (const std::exception &e) {
        std::cerr << "SSE handler error: " << e.what() << "\n";
      } catch (...) {
        std::cerr << "SSE handler error: unknown exception\n";
      }
    }
  }

  return total;
}

void RunSseThread(const std::string &base_url, AppState *state, std::mutex *mutex) {
  std::string url = base_url + "/sse";
  while (true) {
    try {
      CURL *curl = curl_easy_init();
      if (!curl) {
        std::cerr << "SSE init failed, retrying...\n";
        std::this_thread::sleep_for(std::chrono::seconds(5));
        continue;
      }
      std::cerr << "SSE connect: " << url << "\n";
      SseStreamState stream;
      stream.mutex = mutex;
      stream.state = state;

      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteSse);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &stream);
      curl_easy_setopt(curl, CURLOPT_USERAGENT, "meshcoretel-native/1.0");
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
      curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
      CURLcode res = curl_easy_perform(curl);
      if (res != CURLE_OK) {
        std::cerr << "SSE error: " << curl_easy_strerror(res) << "\n";
      }
      curl_easy_cleanup(curl);
      std::cerr << "SSE disconnected, retrying...\n";
      std::this_thread::sleep_for(std::chrono::seconds(5));
    } catch (const std::exception &e) {
      std::cerr << "SSE thread error: " << e.what() << "\n";
      std::this_thread::sleep_for(std::chrono::seconds(5));
    } catch (...) {
      std::cerr << "SSE thread error: unknown exception\n";
      std::this_thread::sleep_for(std::chrono::seconds(5));
    }
  }
}

void FetchNodesLoop(const std::string &base_url, AppState *state, std::mutex *mutex) {
  while (true) {
    try {
      std::string response = HttpGet(base_url + "/api/adverts");
      if (!response.empty()) {
        std::vector<Node> nodes = ParseNodesJson(response);
        if (!nodes.empty()) {
          std::lock_guard<std::mutex> lock(*mutex);
          state->nodes = std::move(nodes);
          UpdateNodeIndex(*state);
          state->last_update = FormatTimeNow();
          std::cerr << "Nodes updated: " << state->nodes.size() << "\n";
        }
      } else {
        std::cerr << "Nodes fetch returned empty response\n";
      }
    } catch (const std::exception &e) {
      std::cerr << "FetchNodesLoop error: " << e.what() << "\n";
    } catch (...) {
      std::cerr << "FetchNodesLoop error: unknown exception\n";
    }
    SDL_Delay(30000);
  }
}

}  // namespace

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  LogSink log;
  g_log = &log;
  std::set_terminate([]() {
    if (g_log) {
      g_log->Write("std::terminate called");
      auto eptr = std::current_exception();
      if (eptr) {
        try {
          std::rethrow_exception(eptr);
        } catch (const std::exception &e) {
          g_log->Write(std::string("Unhandled exception: ") + e.what());
        } catch (...) {
          g_log->Write("Unhandled exception: unknown");
        }
      }
    }
    std::_Exit(1);
  });
  std::signal(SIGSEGV, SignalHandler);
  std::signal(SIGABRT, SignalHandler);
  std::signal(SIGFPE, SignalHandler);
  std::signal(SIGILL, SignalHandler);
  std::signal(SIGBUS, SignalHandler);
  std::signal(SIGINT, QuitSignalHandler);
  std::signal(SIGTERM, QuitSignalHandler);
  log.Write("Client booting");

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
    log.Write(std::string("SDL init failed: ") + SDL_GetError());
    return 1;
  }
  if (IMG_Init(IMG_INIT_PNG) == 0) {
    log.Write(std::string("SDL_image init failed: ") + IMG_GetError());
    SDL_Quit();
    return 1;
  }
  if (TTF_Init() != 0) {
    log.Write(std::string("SDL_ttf init failed: ") + TTF_GetError());
    IMG_Quit();
    SDL_Quit();
    return 1;
  }
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
    log.Write("curl init failed");
  }

  SDL_Window *window = SDL_CreateWindow(
      "MeshCoreTel Visualizer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      kDefaultWidth, kDefaultHeight, SDL_WINDOW_RESIZABLE);
  if (!window) {
    log.Write(std::string("SDL window create failed: ") + SDL_GetError());
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    return 1;
  }
  log.Write("SDL window created");

  SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
  if (!renderer) {
    log.Write(std::string("SDL renderer create failed: ") + SDL_GetError());
    SDL_DestroyWindow(window);
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    return 1;
  }
  log.Write("SDL renderer created");

  const char *font_path_env = std::getenv("MESHCORETEL_FONT_PATH");
  std::string font_path = font_path_env ? font_path_env :
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
  TTF_Font *font = TTF_OpenFont(font_path.c_str(), 16);
  if (!font) {
    log.Write("Failed to load font: " + font_path);
  }

  std::string base_url = "http://localhost:3000";
  if (const char *env = std::getenv("MESHCORETEL_SERVER_URL")) {
    base_url = env;
  }

  AppState state;
  std::mutex state_mutex;

  std::thread sse_thread(RunSseThread, base_url, &state, &state_mutex);
  std::thread nodes_thread(FetchNodesLoop, base_url, &state, &state_mutex);

  TileCache tile_cache(renderer, "native/cache");

  bool running = true;
  int window_width = kDefaultWidth;
  int window_height = kDefaultHeight;
  double center_lat = kMoscowLat;
  double center_lon = kMoscowLon;
  int zoom = kDefaultZoom;

  uint64_t start_ms = NowMs();
  while (running) {
    if (g_should_quit) {
      log.Write("Shutdown requested");
      running = false;
      break;
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) {
        if (NowMs() - start_ms < 1000) {
          log.Write("Ignoring SDL_QUIT during startup");
        } else {
          log.Write("SDL_QUIT received");
          running = false;
        }
      } else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
        window_width = event.window.data1;
        window_height = event.window.data2;
      } else if (event.type == SDL_KEYDOWN) {
        if (event.key.keysym.sym == SDLK_r) {
          center_lat = kMoscowLat;
          center_lon = kMoscowLon;
        } else if (event.key.keysym.sym == SDLK_a) {
          std::lock_guard<std::mutex> lock(state_mutex);
          state.animations_enabled = !state.animations_enabled;
        }
      } else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
        int mx = event.button.x;
        int my = event.button.y;
        std::lock_guard<std::mutex> lock(state_mutex);
        state.selected_node_index = -1;
        double center_x = 0.0;
        double center_y = 0.0;
        LatLonToWorldPixel(center_lat, center_lon, zoom, &center_x, &center_y);
        double top_left_x = center_x - window_width / 2.0;
        double top_left_y = center_y - window_height / 2.0;
        for (size_t i = 0; i < state.nodes.size(); i++) {
          const Node &node = state.nodes[i];
          if (!node.has_position) {
            continue;
          }
          double px = 0.0;
          double py = 0.0;
          LatLonToWorldPixel(node.lat, node.lon, zoom, &px, &py);
          int sx = static_cast<int>(px - top_left_x);
          int sy = static_cast<int>(py - top_left_y);
          int dx = sx - mx;
          int dy = sy - my;
          if (dx * dx + dy * dy <= 100) {
            state.selected_node_index = static_cast<int>(i);
            break;
          }
        }
      }
    }

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    double center_x = 0.0;
    double center_y = 0.0;
    LatLonToWorldPixel(center_lat, center_lon, zoom, &center_x, &center_y);
    double top_left_x = center_x - window_width / 2.0;
    double top_left_y = center_y - window_height / 2.0;

    int start_tile_x = static_cast<int>(std::floor(top_left_x / kTileSize));
    int start_tile_y = static_cast<int>(std::floor(top_left_y / kTileSize));
    int end_tile_x = static_cast<int>(std::floor((top_left_x + window_width) / kTileSize)) + 1;
    int end_tile_y = static_cast<int>(std::floor((top_left_y + window_height) / kTileSize)) + 1;

    for (int tx = start_tile_x; tx <= end_tile_x; tx++) {
      for (int ty = start_tile_y; ty <= end_tile_y; ty++) {
        if (tx < 0 || ty < 0) {
          continue;
        }
        TileTexture tile = tile_cache.GetTile(zoom, tx, ty);
        if (!tile.texture) {
          continue;
        }
        int screen_x = static_cast<int>(tx * kTileSize - top_left_x);
        int screen_y = static_cast<int>(ty * kTileSize - top_left_y);
        SDL_Rect dst{screen_x, screen_y, kTileSize, kTileSize};
        SDL_RenderCopy(renderer, tile.texture, nullptr, &dst);
      }
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 120);
    SDL_Rect shade{0, 0, window_width, window_height};
    SDL_RenderFillRect(renderer, &shade);

    AppState snapshot;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      uint64_t now = NowMs();
      state.pulses.erase(std::remove_if(state.pulses.begin(), state.pulses.end(),
                                        [now](const MovingPulse &pulse) {
                                          return now - pulse.start_time_ms >
                                                 static_cast<uint64_t>(pulse.duration_ms + 500.0f);
                                        }),
                         state.pulses.end());
      state.paths.erase(std::remove_if(state.paths.begin(), state.paths.end(),
                                       [now](const PathAnimation &path) {
                                         return now - path.start_time_ms >
                                                static_cast<uint64_t>(path.duration_ms + 1500.0f);
                                       }),
                        state.paths.end());
      snapshot = state;
    }

    for (const Node &node : snapshot.nodes) {
      if (!node.has_position) {
        continue;
      }
      double px = 0.0;
      double py = 0.0;
      LatLonToWorldPixel(node.lat, node.lon, zoom, &px, &py);
      int sx = static_cast<int>(px - top_left_x);
      int sy = static_cast<int>(py - top_left_y);
      DrawFilledCircle(renderer, sx, sy, 6, ColorForNode(node));
    }

    if (snapshot.animations_enabled) {
      uint64_t now = NowMs();
      for (const auto &pulse : snapshot.pulses) {
        float progress = static_cast<float>(now - pulse.start_time_ms) / pulse.duration_ms;
        if (progress < 0.0f || progress > 1.0f) {
          continue;
        }
        float x = pulse.start.x + (pulse.end.x - pulse.start.x) * progress;
        float y = pulse.start.y + (pulse.end.y - pulse.start.y) * progress;
        int sx = static_cast<int>(x - top_left_x);
        int sy = static_cast<int>(y - top_left_y);
        DrawFilledCircle(renderer, sx, sy, 4, SDL_Color{0, 255, 234, 200});
      }

      for (const auto &path : snapshot.paths) {
        float progress = static_cast<float>(now - path.start_time_ms) / path.duration_ms;
        if (progress < 0.0f || progress > 2.5f) {
          continue;
        }
        float alpha_scale = progress <= 1.0f ? 1.0f : std::max(0.0f, 1.0f - (progress - 1.0f));
        SDL_Color core_color = path.color;
        core_color.a = static_cast<Uint8>(220 * alpha_scale);
        SDL_Color glow_color = path.color;
        glow_color.a = static_cast<Uint8>(90 * alpha_scale);
        SDL_Color outer_color = path.color;
        outer_color.a = static_cast<Uint8>(40 * alpha_scale);
        for (size_t i = 1; i < path.points.size(); i++) {
          int x1 = static_cast<int>(path.points[i - 1].x - top_left_x);
          int y1 = static_cast<int>(path.points[i - 1].y - top_left_y);
          int x2 = static_cast<int>(path.points[i].x - top_left_x);
          int y2 = static_cast<int>(path.points[i].y - top_left_y);
          DrawThickLine(renderer, x1, y1, x2, y2, path.width + 4.0f, outer_color, SDL_BLENDMODE_ADD);
          DrawThickLine(renderer, x1, y1, x2, y2, path.width + 2.0f, glow_color, SDL_BLENDMODE_ADD);
          DrawThickLine(renderer, x1, y1, x2, y2, path.width, core_color, SDL_BLENDMODE_BLEND);
        }
      }
    }

    if (font) {
      SDL_Color white{255, 255, 255, 255};
      SDL_Color muted{148, 163, 184, 255};
      SDL_SetRenderDrawColor(renderer, 0, 0, 0, 170);
      SDL_Rect overlay{20, 20, 260, 80};
      SDL_RenderFillRect(renderer, &overlay);
      DrawText(renderer, font, "MeshCoreTel Network", white, 30, 28);
      DrawText(renderer, font, "Nodes: " + std::to_string(snapshot.nodes.size()), muted, 30, 52);

      SDL_Rect node_box{20, window_height - 140, 320, 110};
      SDL_RenderFillRect(renderer, &node_box);
      DrawText(renderer, font, "Node Information", white, 30, window_height - 130);
      if (snapshot.selected_node_index >= 0 &&
          snapshot.selected_node_index < static_cast<int>(snapshot.nodes.size())) {
        const Node &node = snapshot.nodes[snapshot.selected_node_index];
        DrawText(renderer, font, node.name.empty() ? "Unnamed" : node.name, muted,
                 30, window_height - 105);
        std::ostringstream detail;
        detail << "ID: " << node.id << "  Lat: " << node.lat << "  Lon: " << node.lon;
        DrawText(renderer, font, detail.str(), muted, 30, window_height - 80);
      } else {
        DrawText(renderer, font, "Select a node for details", muted, 30, window_height - 105);
      }

      SDL_Rect packet_box{window_width - 340, 20, 320, 140};
      SDL_RenderFillRect(renderer, &packet_box);
      DrawText(renderer, font, "Packet Info", white, window_width - 330, 28);
      int msg_y = 52;
      if (snapshot.packet_messages.empty()) {
        DrawText(renderer, font, "No packets yet...", muted, window_width - 330, msg_y);
      } else {
        for (const auto &msg : snapshot.packet_messages) {
          DrawText(renderer, font, msg.text, muted, window_width - 330, msg_y);
          msg_y += 18;
        }
      }

      SDL_Rect status_box{window_width - 340, window_height - 100, 320, 80};
      SDL_RenderFillRect(renderer, &status_box);
      DrawText(renderer, font, "Status", white, window_width - 330, window_height - 90);
      DrawText(renderer, font, snapshot.connection_status, muted, window_width - 330,
               window_height - 70);
      DrawText(renderer, font, "Last update: " + snapshot.last_update, muted,
               window_width - 330, window_height - 50);
    }

    SDL_RenderPresent(renderer);
    SDL_Delay(16);
  }

  log.Write("Client shutting down");
  tile_cache.Clear();
  if (font) {
    TTF_CloseFont(font);
  }
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  curl_global_cleanup();
  TTF_Quit();
  IMG_Quit();
  SDL_Quit();

  if (sse_thread.joinable()) {
    sse_thread.detach();
  }
  if (nodes_thread.joinable()) {
    nodes_thread.detach();
  }

  return 0;
}
