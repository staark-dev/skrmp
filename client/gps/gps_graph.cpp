#include "gps_graph.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <initializer_list>
#include <cstdio>
#include <cstring>
#include <memory>
#include <queue>
#include <thread>

namespace skr::gps::graph {

uint32_t RoadGraph::Global(int area, int id) const {
    if (area < 0 || area >= kAreas || id < 0) return kNoNode;
    if (static_cast<uint32_t>(id) >= AreaNodeCount(area)) return kNoNode;
    return areaOffset[area] + static_cast<uint32_t>(id);
}

// ── Parsing ─────────────────────────────────────────────────────────

namespace {

constexpr size_t kHeader   = 20;
constexpr size_t kNodeSize = 28;
constexpr size_t kNaviSize = 14;
constexpr size_t kFiller   = 768;
constexpr float  kAreaSize = 750.0f;
constexpr float  kWorldMin = -3000.0f;
constexpr float  kAreaSlack = 2.0f;   // compressed coordinates may sit on the boundary

template <typename T> T Rd(const uint8_t* p) { T v; memcpy(&v, p, sizeof(T)); return v; }

} // namespace

bool ParseArea(int area, const uint8_t* data, size_t size, AreaFile& out, std::string& err) {
    char buf[160];
    out = AreaFile{};
    if (size < kHeader) { err = "file shorter than header"; return false; }
    const uint32_t numNodes = Rd<uint32_t>(data);
    const uint32_t numVeh   = Rd<uint32_t>(data + 4);
    const uint32_t numPed   = Rd<uint32_t>(data + 8);
    const uint32_t numNavi  = Rd<uint32_t>(data + 12);
    const uint32_t numLinks = Rd<uint32_t>(data + 16);
    if (numNodes > 20000 || numLinks > 200000 || numNavi > 50000 || numVeh + numPed != numNodes) {
        snprintf(buf, sizeof(buf), "implausible header nodes=%u veh=%u ped=%u links=%u",
                 numNodes, numVeh, numPed, numLinks);
        err = buf;
        return false;
    }
    const size_t linksOff = kHeader + numNodes * kNodeSize + numNavi * kNaviSize;
    const size_t need = linksOff + numLinks * 4 + kFiller + numLinks * 4;
    if (size < need) {
        snprintf(buf, sizeof(buf), "file %zu bytes, layout needs %zu", size, need);
        err = buf;
        return false;
    }

    const float ax0 = kWorldMin + (area % 8) * kAreaSize, ay0 = kWorldMin + (area / 8) * kAreaSize;
    out.nodes.resize(numNodes);
    for (uint32_t i = 0; i < numNodes; ++i) {
        const uint8_t* p = data + kHeader + i * kNodeSize;
        Node& n = out.nodes[i];
        n.x = Rd<int16_t>(p + 8) / 8.0f;
        n.y = Rd<int16_t>(p + 10) / 8.0f;
        n.z = Rd<int16_t>(p + 12) / 8.0f;
        const int16_t base = Rd<int16_t>(p + 16);
        n.area      = Rd<int16_t>(p + 18);
        n.id        = Rd<int16_t>(p + 20);
        n.floodFill = p[23];
        n.linkCount = p[24] & 0x0F;
        n.water     = (p[24] & 0x80) != 0;
        n.vehicle   = i < numVeh;
        if (n.area != area || n.id != static_cast<int16_t>(i)) {
            snprintf(buf, sizeof(buf), "node %u carries area %d id %d", i, n.area, n.id);
            err = buf;
            return false;
        }
        if (base < 0 || static_cast<uint32_t>(base) + n.linkCount > numLinks) {
            snprintf(buf, sizeof(buf), "node %u links [%d,+%u) outside %u", i, base, n.linkCount, numLinks);
            err = buf;
            return false;
        }
        if (n.x < ax0 - kAreaSlack || n.x > ax0 + kAreaSize + kAreaSlack ||
            n.y < ay0 - kAreaSlack || n.y > ay0 + kAreaSize + kAreaSlack) {
            snprintf(buf, sizeof(buf), "node %u at %.1f,%.1f outside area bounds", i, n.x, n.y);
            err = buf;
            return false;
        }
        n.firstLink = static_cast<uint32_t>(base);
    }
    out.links.resize(numLinks);
    for (uint32_t i = 0; i < numLinks; ++i) {
        const uint8_t* p = data + linksOff + i * 4;
        out.links[i] = { Rd<uint16_t>(p), Rd<uint16_t>(p + 2) };
    }
    return true;
}

bool BuildGraph(std::vector<AreaFile>& areas, RoadGraph& out, std::string& err) {
    out = RoadGraph{};
    if (areas.size() != kAreas) { err = "expected 64 areas"; return false; }
    size_t totalLinks = 0;
    for (int a = 0; a < kAreas; ++a) {
        out.areaOffset[a + 1] = out.areaOffset[a] + static_cast<uint32_t>(areas[a].nodes.size());
        totalLinks += areas[a].links.size();
    }
    out.nodes.reserve(out.areaOffset[kAreas]);
    out.links.reserve(totalLinks);
    for (int a = 0; a < kAreas; ++a) {
        const uint32_t linkBase = static_cast<uint32_t>(out.links.size());
        for (Node n : areas[a].nodes) {
            n.firstLink += linkBase;
            out.vehicleNodes += n.vehicle ? 1u : 0u;
            out.nodes.push_back(n);
        }
        for (const AreaFile::RawLink& l : areas[a].links)
            out.links.push_back(out.Global(l.area, l.node));
        std::vector<Node>().swap(areas[a].nodes);
        std::vector<AreaFile::RawLink>().swap(areas[a].links);
    }
    if (out.vehicleNodes == 0) { err = "no vehicle nodes"; return false; }
    return true;
}

bool LoadFromImg(const std::wstring& imgPath, RoadGraph& out, std::string& err) {
    HANDLE f = CreateFileW(imgPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { err = "gta3.img could not be opened"; return false; }
    auto readAt = [&](uint64_t off, void* dst, DWORD bytes) {
        LARGE_INTEGER pos; pos.QuadPart = static_cast<LONGLONG>(off);
        DWORD got = 0;
        return SetFilePointerEx(f, pos, nullptr, FILE_BEGIN) &&
               ReadFile(f, dst, bytes, &got, nullptr) && got == bytes;
    };
    struct Closer { HANDLE h; ~Closer() { CloseHandle(h); } } closer{f};

    uint8_t header[8];
    if (!readAt(0, header, 8) || memcmp(header, "VER2", 4) != 0) { err = "gta3.img is not IMG v2"; return false; }
    const uint32_t count = Rd<uint32_t>(header + 4);
    if (count == 0 || count > 200000) { err = "gta3.img directory invalid"; return false; }
    std::vector<uint8_t> dir(static_cast<size_t>(count) * 32);
    if (!readAt(8, dir.data(), static_cast<DWORD>(dir.size()))) { err = "gta3.img directory read failed"; return false; }

    uint32_t offset[kAreas] = {}, sectors[kAreas] = {};
    bool present[kAreas] = {};
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = dir.data() + static_cast<size_t>(i) * 32;
        char name[25] = {};
        memcpy(name, e + 8, 24);
        int idx = -1;
        char tail[8] = {};
        if (_strnicmp(name, "nodes", 5) != 0 || sscanf_s(name + 5, "%d%7s", &idx, tail, 8) != 2) continue;
        if (_stricmp(tail, ".dat") != 0 || idx < 0 || idx >= kAreas) continue;
        const uint16_t streaming = Rd<uint16_t>(e + 4), archive = Rd<uint16_t>(e + 6);
        offset[idx]  = Rd<uint32_t>(e);
        sectors[idx] = archive ? archive : streaming;
        present[idx] = sectors[idx] > 0;
    }
    std::vector<AreaFile> areas(kAreas);
    std::vector<uint8_t> buf;
    for (int a = 0; a < kAreas; ++a) {
        if (!present[a]) { err = "nodes" + std::to_string(a) + ".dat missing from gta3.img"; return false; }
        buf.resize(static_cast<size_t>(sectors[a]) * 2048);
        if (!readAt(static_cast<uint64_t>(offset[a]) * 2048, buf.data(), static_cast<DWORD>(buf.size()))) {
            err = "nodes" + std::to_string(a) + ".dat read failed";
            return false;
        }
        std::string why;
        if (!ParseArea(a, buf.data(), buf.size(), areas[a], why)) {
            err = "nodes" + std::to_string(a) + ".dat: " + why;
            return false;
        }
    }
    return BuildGraph(areas, out, err);
}

// ── Search ──────────────────────────────────────────────────────────

const char* ToString(SearchStatus s) {
    switch (s) {
        case SearchStatus::Found:                 return "found";
        case SearchStatus::NoRoadNearOrigin:      return "no road near origin";
        case SearchStatus::NoRoadNearDestination: return "no road near destination";
        case SearchStatus::Disconnected:          return "endpoints not connected";
        case SearchStatus::Exhausted:             return "no path";
    }
    return "?";
}

namespace {
float Dist3(const Node& a, const Node& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
}

uint32_t NearestNode(const RoadGraph& g, const Vec3& p, bool use3D, float maxDist, float* outDist) {
    uint32_t best = kNoNode;
    float bestD2 = maxDist * maxDist;
    for (uint32_t i = 0; i < g.nodes.size(); ++i) {
        if (!g.Routable(i)) continue;
        const Node& n = g.nodes[i];
        const float dx = n.x - p.x, dy = n.y - p.y, dz = use3D ? n.z - p.z : 0.0f;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 <= bestD2) { bestD2 = d2; best = i; }
    }
    if (outDist) *outDist = best == kNoNode ? 0.0f : std::sqrt(bestD2);
    return best;
}

namespace {
float DistP(const Vec3& p, const Node& n) {
    const float dx = p.x - n.x, dy = p.y - n.y, dz = p.z - n.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
}

RoadPoint NearestRoadPoint(const RoadGraph& g, const Vec3& p, float zWeight, float maxDist, int floodFill) {
    RoadPoint best;
    float bestD2 = maxDist * maxDist;
    for (uint32_t a = 0; a < g.nodes.size(); ++a) {
        if (!g.Routable(a)) continue;
        const Node& na = g.nodes[a];
        if (floodFill >= 0 && na.floodFill != floodFill) continue;
        for (uint32_t k = 0; k < na.linkCount; ++k) {
            const uint32_t b = g.links[na.firstLink + k];
            if (b <= a || !g.Routable(b)) continue;    // each undirected link once
            const Node& nb = g.nodes[b];
            const float ex = nb.x - na.x, ey = nb.y - na.y;
            const float len2 = ex * ex + ey * ey;
            float t = len2 > 1e-4f ? ((p.x - na.x) * ex + (p.y - na.y) * ey) / len2 : 0.0f;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            const float qx = na.x + t * ex, qy = na.y + t * ey, qz = na.z + t * (nb.z - na.z);
            const float dx = p.x - qx, dy = p.y - qy, dz = (p.z - qz) * zWeight;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < bestD2) {
                bestD2 = d2;
                best.a = a; best.b = b; best.t = t;
                best.point = {qx, qy, qz};
            }
        }
    }
    if (best.Valid()) best.dist = std::sqrt(bestD2);
    return best;
}

SearchResult FindRoute(const RoadGraph& g, const Vec3& origin, const Vec3& destination,
                       const SearchLimits& limits) {
    SearchResult r;
    // Origin: height matters (bridges, overpasses). Destination: map waypoints have z = 0.
    const RoadPoint from = NearestRoadPoint(g, origin, 2.0f, limits.originSnap);
    if (!from.Valid()) { r.status = SearchStatus::NoRoadNearOrigin; return r; }
    r.originSnapDist = from.dist;
    const int component = g.nodes[from.a].floodFill;
    RoadPoint to = NearestRoadPoint(g, destination, 0.0f, limits.destinationSnap);
    if (!to.Valid()) { r.status = SearchStatus::NoRoadNearDestination; return r; }
    if (g.nodes[to.a].floodFill != component) {
        // Nearest road belongs to another network: use the nearest reachable one.
        const RoadPoint reachable = NearestRoadPoint(g, destination, 0.0f, limits.destinationSnap, component);
        if (!reachable.Valid()) { r.destSnapDist = to.dist; r.status = SearchStatus::Disconnected; return r; }
        to = reachable;
        r.destReSnapped = true;
    }
    r.destSnapDist = to.dist;
    r.start = from.point;
    r.end = to.point;

    // Both points on the same link: the route is that piece of road.
    if ((from.a == to.a && from.b == to.b) || (from.a == to.b && from.b == to.a)) {
        const float dx = from.point.x - to.point.x, dy = from.point.y - to.point.y, dz = from.point.z - to.point.z;
        r.length = std::sqrt(dx * dx + dy * dy + dz * dz);
        r.status = SearchStatus::Found;
        return r;
    }

    const size_t n = g.nodes.size();
    std::vector<float> cost(n, INFINITY);
    std::vector<uint32_t> parent(n, kNoNode);
    std::vector<uint8_t> closed(n, 0);
    using Entry = std::pair<float, uint32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    auto heuristic = [&](uint32_t v) { return DistP(to.point, g.nodes[v]); };
    for (uint32_t s : {from.a, from.b}) {
        const float c = DistP(from.point, g.nodes[s]);
        if (c < cost[s]) { cost[s] = c; open.push({c + heuristic(s), s}); }
    }
    // Goals: both ends of the destination link, finishing along the link to `to.point`.
    const float finishA = DistP(to.point, g.nodes[to.a]);
    const float finishB = DistP(to.point, g.nodes[to.b]);
    float best = INFINITY;
    uint32_t bestGoal = kNoNode;

    while (!open.empty()) {
        const auto [f, cur] = open.top();
        if (f >= best) break;                    // admissible heuristic: nothing better remains
        open.pop();
        if (closed[cur]) continue;
        closed[cur] = 1;
        ++r.expanded;
        if (cur == to.a || cur == to.b) {
            const float total = cost[cur] + (cur == to.a ? finishA : finishB);
            if (total < best) { best = total; bestGoal = cur; }
        }
        const Node& cn = g.nodes[cur];
        for (uint32_t k = 0; k < cn.linkCount; ++k) {
            const uint32_t nb = g.links[cn.firstLink + k];
            if (!g.Routable(nb) || closed[nb]) continue;
            const float c = cost[cur] + Dist3(cn, g.nodes[nb]);
            if (c < cost[nb]) {
                cost[nb] = c;
                parent[nb] = cur;
                open.push({c + heuristic(nb), nb});
            }
        }
    }
    if (bestGoal == kNoNode) { r.status = SearchStatus::Exhausted; return r; }

    for (uint32_t v = bestGoal; v != kNoNode; v = parent[v]) r.path.push_back(v);
    std::reverse(r.path.begin(), r.path.end());
    r.length = best;
    r.status = SearchStatus::Found;
    return r;
}

// ── Background loader ───────────────────────────────────────────────

namespace {
constexpr int     kPublishing = 100;     // internal: loader owns g_graph / g_error
std::atomic<int>  g_state{static_cast<int>(LoadState::NotStarted)};
const RoadGraph*  g_graph = nullptr;     // published before g_state = Ready (release)
std::string       g_error;               // written before g_state = Failed (release)
std::unique_ptr<RoadGraph> g_owned;

std::wstring GameImgPath() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = (n && n < MAX_PATH) ? wcsrchr(path, L'\\') : nullptr;
    if (!slash) return {};
    *(slash + 1) = L'\0';
    return std::wstring(path) + L"models\\gta3.img";
}
}

void RequestLoad() {
    int expected = static_cast<int>(LoadState::NotStarted);
    if (!g_state.compare_exchange_strong(expected, static_cast<int>(LoadState::Loading))) return;
    std::wstring img = GameImgPath();
    std::thread([img]() {
        auto graph = std::make_unique<RoadGraph>();
        std::string err;
        if (img.empty()) err = "game directory not found";
        const bool ok = err.empty() && LoadFromImg(img, *graph, err);
        // Publish only if nobody replaced the state meanwhile (SetForTesting).
        int loading = static_cast<int>(LoadState::Loading);
        if (!g_state.compare_exchange_strong(loading, kPublishing)) return;
        if (ok) {
            g_owned = std::move(graph);
            g_graph = g_owned.get();
        } else {
            g_error = err;
        }
        g_state.store(static_cast<int>(ok ? LoadState::Ready : LoadState::Failed), std::memory_order_release);
    }).detach();
}

LoadState GetLoadState() {
    const int s = g_state.load(std::memory_order_acquire);
    return s == kPublishing ? LoadState::Loading : static_cast<LoadState>(s);
}

const RoadGraph* Get() {
    return GetLoadState() == LoadState::Ready ? g_graph : nullptr;
}

const char* LoadError() {
    return GetLoadState() == LoadState::Failed ? g_error.c_str() : "";
}

void SetForTesting(const RoadGraph* g) {
    g_graph = g;
    g_state.store(static_cast<int>(g ? LoadState::Ready : LoadState::NotStarted), std::memory_order_release);
}

} // namespace skr::gps::graph
