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
#include <unordered_map>
#include <vector>

#include "json.hpp"

namespace {
using nlohmann::json;
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

int JsonSkipToken(const jsmntok_t *tokens, int index, int count) {
  if (index < 0 || index >= count) {
    return count;
  }
  int i = index;
  int to_process = 1;
  while (i < count && to_process > 0) {
    const jsmntok_t &tok = tokens[i];
    if (tok.size < 0 || tok.size > count) {
      std::cerr << "JsonSkipToken: invalid size " << tok.size << " at index " << i << "\n";
      return count;
    }
    to_process--;
    if (tok.type == JSMN_OBJECT) {
      if (tok.size > (count - i) / 2) {
        std::cerr << "JsonSkipToken: object size overflow at index " << i << "\n";
        return count;
      }
      to_process += tok.size * 2;
    } else if (tok.type == JSMN_ARRAY) {
      if (tok.size > (count - i)) {
        std::cerr << "JsonSkipToken: array size overflow at index " << i << "\n";
        return count;
      }
      to_process += tok.size;
    }
    i++;
  }
  return i;
}

bool JsonTokenEquals(const char *json, const jsmntok_t &token, const char *value) {
  if (token.type != JSMN_STRING) {
    return false;
  }
  int length = token.end - token.start;
  return std::strncmp(json + token.start, value, static_cast<size_t>(length)) == 0 &&
         value[length] == '\0';
}

int JsonFindKey(const char *json, const jsmntok_t *tokens, int obj_index, int count, const char *key) {
  if (obj_index < 0 || obj_index >= count) {
    return -1;
  }
  if (tokens[obj_index].type != JSMN_OBJECT) {
    return -1;
  }
  int idx = obj_index + 1;
  for (int i = 0; i < tokens[obj_index].size && idx < count; i++) {
    int key_idx = idx;
    int value_idx = idx + 1;
    if (value_idx < count && JsonTokenEquals(json, tokens[key_idx], key)) {
      return value_idx;
    }
    idx = JsonSkipToken(tokens, value_idx, count);
  }
  return -1;
}

std::string JsonGetString(const char *json, const jsmntok_t *tokens, int index) {
  if (index < 0) {
    return {};
  }
  if (tokens[index].type != JSMN_STRING) {
    return {};
  }
  int length = tokens[index].end - tokens[index].start;
  return std::string(json + tokens[index].start, static_cast<size_t>(length));
}

bool JsonGetBool(const char *json, const jsmntok_t *tokens, int index, bool *out) {
  if (index < 0) {
    return false;
  }
  if (tokens[index].type != JSMN_PRIMITIVE) {
    return false;
  }
  int length = tokens[index].end - tokens[index].start;
  std::string value(json + tokens[index].start, static_cast<size_t>(length));
  if (value == "true") {
    *out = true;
    return true;
  }
  if (value == "false") {
    *out = false;
    return true;
  }
  return false;
}

bool JsonGetNumber(const char *json, const jsmntok_t *tokens, int index, double *out) {
  if (index < 0) {
    return false;
  }
  if (tokens[index].type != JSMN_PRIMITIVE) {
    return false;
  }
  std::string value(json + tokens[index].start,
                    static_cast<size_t>(tokens[index].end - tokens[index].start));
  char *end = nullptr;
  double result = std::strtod(value.c_str(), &end);
  if (end == value.c_str()) {
    return false;
  }
  *out = result;
  return true;
}

std::string JsonUnescape(const std::string &input) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size(); i++) {
    char c = input[i];
    if (c == '\\' && i + 1 < input.size()) {
      char next = input[i + 1];
      switch (next) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          // Skip \uXXXX sequences; keep placeholder.
          out.push_back('?');
          i += 5;
          continue;
        }
        default:
          out.push_back(next);
          break;
      }
      i++;
    } else {
      out.push_back(c);
    }
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
  jsmn_parser parser;
  std::vector<jsmntok_t> tokens;
  int count = -1;
  size_t token_cap = 4096;
  while (token_cap <= 131072) {
    tokens.assign(token_cap, jsmntok_t{});
    jsmn_init(&parser);
    count = jsmn_parse(&parser, json.c_str(), json.size(), tokens.data(), tokens.size());
    if (count >= 1) {
      break;
    }
    token_cap *= 2;
  }
  if (count < 1 || tokens[0].type != JSMN_ARRAY) {
    std::cerr << "ParseNodesJson: unexpected JSON root\n";
    return nodes;
  }

  int idx = 1;
  for (int i = 0; i < tokens[0].size; i++) {
    if (idx >= count) {
      break;
    }
    int obj_index = idx;
    idx = JsonSkipToken(tokens.data(), idx, count);
    if (obj_index >= count) {
      break;
    }
    if (tokens[obj_index].type != JSMN_OBJECT) {
      continue;
    }

    Node node;
    int id_idx = JsonFindKey(json.c_str(), tokens.data(), obj_index, count, "id");
    double id_val = 0.0;
    if (JsonGetNumber(json.c_str(), tokens.data(), id_idx, &id_val)) {
      node.id = static_cast<int>(id_val);
    }

    int hash_idx = JsonFindKey(json.c_str(), tokens.data(), obj_index, count, "node_hash");
    double hash_val = 0.0;
    if (JsonGetNumber(json.c_str(), tokens.data(), hash_idx, &hash_val)) {
      node.node_hash = static_cast<int>(hash_val);
    }

    int lat_idx = JsonFindKey(json.c_str(), tokens.data(), obj_index, count, "lat");
    int lon_idx = JsonFindKey(json.c_str(), tokens.data(), obj_index, count, "lon");
    int lng_idx = JsonFindKey(json.c_str(), tokens.data(), obj_index, count, "lng");
    double lat = 0.0;
    double lon = 0.0;
    bool has_lat = JsonGetNumber(json.c_str(), tokens.data(), lat_idx, &lat);
    bool has_lon = JsonGetNumber(json.c_str(), tokens.data(), lon_idx, &lon);
    if (!has_lon) {
      has_lon = JsonGetNumber(json.c_str(), tokens.data(), lng_idx, &lon);
    }

    node.lat = lat;
    node.lon = lon;
    node.has_position = has_lat && has_lon && !(lat == 0.0 && lon == 0.0) &&
                        std::abs(lat) <= 90.0 && std::abs(lon) <= 180.0;

    node.name = JsonGetString(json.c_str(), tokens.data(),
                              JsonFindKey(json.c_str(), tokens.data(), obj_index, count, "name"));
    node.public_key_hex = JsonGetString(
        json.c_str(), tokens.data(),
        JsonFindKey(json.c_str(), tokens.data(), obj_index, count, "public_key_hex"));

    JsonGetBool(json.c_str(), tokens.data(),
                JsonFindKey(json.c_str(), tokens.data(), obj_index, count, "is_room_server"),
                &node.is_room_server);
    JsonGetBool(json.c_str(), tokens.data(),
                JsonFindKey(json.c_str(), tokens.data(), obj_index, count, "is_repeater"),
                &node.is_repeater);
    JsonGetBool(json.c_str(), tokens.data(),
                JsonFindKey(json.c_str(), tokens.data(), obj_index, count, "is_chat_node"),
                &node.is_chat_node);
    JsonGetBool(json.c_str(), tokens.data(),
                JsonFindKey(json.c_str(), tokens.data(), obj_index, count, "is_sensor"),
                &node.is_sensor);

    nodes.push_back(node);
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

void DrawFilledCircle(SDL_Renderer *renderer, int cx, int cy, int radius, SDL_Color color) {
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  for (int dy = -radius; dy <= radius; dy++) {
    int dx = static_cast<int>(std::sqrt(radius * radius - dy * dy));
    SDL_RenderDrawLine(renderer, cx - dx, cy + dy, cx + dx, cy + dy);
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
  jsmn_parser parser;
  jsmn_init(&parser);
  std::vector<jsmntok_t> tokens(2048);
  int count = jsmn_parse(&parser, payload.c_str(), payload.size(), tokens.data(), tokens.size());
  if (count < 1 || tokens[0].type != JSMN_OBJECT) {
    return;
  }

  auto getStr = [&](const char *key) -> std::string {
    int idx = JsonFindKey(payload.c_str(), tokens.data(), 0, count, key);
    return JsonGetString(payload.c_str(), tokens.data(), idx);
  };

  std::string direction = getStr("direction");
  std::string sender = getStr("sender_name");
  if (sender.empty()) {
    sender = getStr("group_sender_name");
  }
  if (sender.empty()) {
    sender = getStr("advert_name");
  }
  std::string origin = getStr("origin");

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

  int src_idx = JsonFindKey(payload.c_str(), tokens.data(), 0, count, "src_hash");
  int dst_idx = JsonFindKey(payload.c_str(), tokens.data(), 0, count, "dst_hash");
  double src_hash = 0.0;
  double dst_hash = 0.0;
  bool has_src = JsonGetNumber(payload.c_str(), tokens.data(), src_idx, &src_hash);
  bool has_dst = JsonGetNumber(payload.c_str(), tokens.data(), dst_idx, &dst_hash);
  if (has_src && has_dst) {
    auto src_it = state.node_hash_index.find(static_cast<int>(src_hash));
    auto dst_it = state.node_hash_index.find(static_cast<int>(dst_hash));
    if (src_it != state.node_hash_index.end() && dst_it != state.node_hash_index.end()) {
      const Node &src = state.nodes[src_it->second];
      const Node &dst = state.nodes[dst_it->second];
      if (src.has_position && dst.has_position) {
        MovingPulse pulse;
        double sx = 0.0;
        double sy = 0.0;
        double ex = 0.0;
        double ey = 0.0;
        LatLonToWorldPixel(src.lat, src.lon, kDefaultZoom, &sx, &sy);
        LatLonToWorldPixel(dst.lat, dst.lon, kDefaultZoom, &ex, &ey);
        pulse.start.x = static_cast<float>(sx);
        pulse.start.y = static_cast<float>(sy);
        pulse.end.x = static_cast<float>(ex);
        pulse.end.y = static_cast<float>(ey);
        pulse.start_time_ms = NowMs();
        state.pulses.push_back(pulse);
      }
    }
  }
}

void HandlePropagationMessage(AppState &state, const std::string &payload) {
  jsmn_parser parser;
  jsmn_init(&parser);
  std::vector<jsmntok_t> tokens(4096);
  int count = jsmn_parse(&parser, payload.c_str(), payload.size(), tokens.data(), tokens.size());
  if (count < 1 || tokens[0].type != JSMN_OBJECT) {
    return;
  }

  int type_idx = JsonFindKey(payload.c_str(), tokens.data(), 0, count, "type");
  std::string type = JsonGetString(payload.c_str(), tokens.data(), type_idx);
  if (type != "propagation.path") {
    return;
  }

  int path_idx = JsonFindKey(payload.c_str(), tokens.data(), 0, count, "path");
  if (path_idx < 0 || tokens[path_idx].type != JSMN_OBJECT) {
    return;
  }

  int nodes_idx = JsonFindKey(payload.c_str(), tokens.data(), path_idx, count, "nodes");
  if (nodes_idx < 0 || tokens[nodes_idx].type != JSMN_ARRAY) {
    return;
  }

  PathAnimation anim;
  anim.start_time_ms = NowMs();
  anim.duration_ms = std::max(800.0f, tokens[nodes_idx].size * 250.0f);

  int idx = nodes_idx + 1;
  for (int i = 0; i < tokens[nodes_idx].size; i++) {
    if (idx >= count) {
      break;
    }
    int node_token = idx;
    idx = JsonSkipToken(tokens.data(), idx, count);
    if (node_token >= count) {
      break;
    }
    double node_hash = 0.0;
    if (JsonGetNumber(payload.c_str(), tokens.data(), node_token, &node_hash)) {
      auto it = state.node_hash_index.find(static_cast<int>(node_hash));
      if (it != state.node_hash_index.end()) {
        const Node &node = state.nodes[it->second];
        if (node.has_position) {
          double px = 0.0;
          double py = 0.0;
          LatLonToWorldPixel(node.lat, node.lon, kDefaultZoom, &px, &py);
          SDL_FPoint pt{static_cast<float>(px), static_cast<float>(py)};
          anim.points.push_back(pt);
        }
      }
    }
  }

  if (anim.points.size() >= 2) {
    anim.color = SDL_Color{59, 130, 246, 255};
    anim.width = 2.0f;
    state.paths.push_back(anim);
  }
}

void HandleSseMessage(AppState &state, const std::string &json) {
  jsmn_parser parser;
  jsmn_init(&parser);
  std::vector<jsmntok_t> tokens(512);
  int count = jsmn_parse(&parser, json.c_str(), json.size(), tokens.data(), tokens.size());
  if (count < 1 || tokens[0].type != JSMN_OBJECT) {
    std::cerr << "SSE: invalid JSON payload\n";
    return;
  }

  int type_idx = JsonFindKey(json.c_str(), tokens.data(), 0, count, "type");
  std::string type = JsonGetString(json.c_str(), tokens.data(), type_idx);

  if (type == "statusUpdate" || type == "connected") {
    int status_idx = JsonFindKey(json.c_str(), tokens.data(), 0, count, "connectionStatus");
    std::string status = JsonGetString(json.c_str(), tokens.data(), status_idx);
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
    int data_idx = JsonFindKey(json.c_str(), tokens.data(), 0, count, "data");
    std::string data = JsonGetString(json.c_str(), tokens.data(), data_idx);
    if (data.empty()) {
      return;
    }
    std::string payload = JsonUnescape(data);
    if (type == "packet") {
      HandlePacketMessage(state, payload);
    } else {
      HandlePropagationMessage(state, payload);
    }
    state.last_update = FormatTimeNow();
    return;
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
      HandleSseMessage(*stream->state, payload);
    }
  }

  return total;
}

void RunSseThread(const std::string &base_url, AppState *state, std::mutex *mutex) {
  std::string url = base_url + "/sse";
  while (true) {
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
  }
}

void FetchNodesLoop(const std::string &base_url, AppState *state, std::mutex *mutex) {
  while (true) {
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
    SDL_Delay(30000);
  }
}

}  // namespace

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  LogSink log;
  g_log = &log;
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
        if (progress < 0.0f || progress > 1.5f) {
          continue;
        }
        SDL_SetRenderDrawColor(renderer, path.color.r, path.color.g, path.color.b,
                               static_cast<Uint8>(180 * (1.0f - std::min(progress, 1.0f))));
        for (size_t i = 1; i < path.points.size(); i++) {
          int x1 = static_cast<int>(path.points[i - 1].x - top_left_x);
          int y1 = static_cast<int>(path.points[i - 1].y - top_left_y);
          int x2 = static_cast<int>(path.points[i].x - top_left_x);
          int y2 = static_cast<int>(path.points[i].y - top_left_y);
          SDL_RenderDrawLine(renderer, x1, y1, x2, y2);
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
