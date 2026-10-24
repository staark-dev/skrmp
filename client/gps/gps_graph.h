#pragma once
// ═══════════════════════════════════════════════════════════════════════
// GPS road graph — GTA SA vehicle path network for long-distance routing
// ═══════════════════════════════════════════════════════════════════════
// CPathFind::DoPathSearch (0x4515D0) cannot route between cities: it stops
// after touching 4950 nodes (0x972CD0 array) and only sees path areas that
// UpdateStreaming keeps loaded (≈350 units around the player). This module
// reads the same data GTA loads — models\gta3.img nodes0.dat … nodes63.dat —
// once, on a background thread (file I/O only, no game memory), and runs A*
// over the complete vehicle network.
//
// nodesN.dat layout (in-memory CPathNode layout, plugin-sdk CPathNode.h):
//   header: u32 numNodes, numVehicleNodes, numPedNodes, numNaviNodes, numLinks
//   numNodes   x 28  path nodes (pos int16/8 @+8, baseLink @+16, area @+18,
//                                node @+20, floodFill @+23, flags @+24:
//                                bits0-3 link count, bit7 water)
//   numNavi    x 14  navi nodes
//   numLinks   x 4   links {u16 area, u16 node}
//   768              filler
//   numLinks   x 2   navi links, numLinks x 1 link lengths, numLinks x 1 flags
// Vehicle nodes are indices [0, numVehicleNodes) of each area.

#include "gps_types.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace skr::gps::graph {

constexpr int      kAreas    = 64;
constexpr uint32_t kNoNode   = 0xFFFFFFFFu;

struct Node {
    float    x = 0, y = 0, z = 0;
    uint32_t firstLink = 0;
    uint8_t  linkCount = 0;
    uint8_t  floodFill = 0;
    bool     vehicle   = false;   // index < numVehicleNodes of its area
    bool     water     = false;
    int16_t  area = -1, id = -1;
};

struct RoadGraph {
    std::vector<Node>     nodes;    // all areas, global index = areaOffset[area] + id
    std::vector<uint32_t> links;    // global target index or kNoNode
    uint32_t areaOffset[kAreas + 1] = {};
    uint32_t vehicleNodes = 0;
    uint32_t AreaNodeCount(int area) const { return areaOffset[area + 1] - areaOffset[area]; }
    uint32_t Global(int area, int id) const;   // kNoNode if out of range
    bool Routable(uint32_t n) const {
        return n < nodes.size() && nodes[n].vehicle && !nodes[n].water && nodes[n].linkCount > 0;
    }
};

// Raw contents of one nodesN.dat before link resolution.
struct AreaFile {
    struct RawLink { uint16_t area, node; };
    std::vector<Node>    nodes;
    std::vector<RawLink> links;
};

// Parses and structurally validates one area file. `err` explains a failure.
bool ParseArea(int area, const uint8_t* data, size_t size, AreaFile& out, std::string& err);

// Joins 64 parsed areas, resolving links to global indices.
bool BuildGraph(std::vector<AreaFile>& areas, RoadGraph& out, std::string& err);

// Reads every nodesN.dat from an IMG v2 archive.
bool LoadFromImg(const std::wstring& imgPath, RoadGraph& out, std::string& err);

// ── Search ────────────────────────────────────────────────────────────
enum class SearchStatus : uint8_t {
    Found,
    NoRoadNearOrigin,
    NoRoadNearDestination,
    Disconnected,      // endpoints in different flood-fill components
    Exhausted,         // A* explored every reachable node without the target
};
const char* ToString(SearchStatus s);

struct SearchLimits {
    float originSnap      = 300.0f;   // max distance origin → nearest road point (z-weighted)
    float destinationSnap = 300.0f;   // max distance waypoint → nearest road point (2D; waypoint z = 0)
};

// A point on a road link a–b (both routable): point = a + t·(b − a).
struct RoadPoint {
    uint32_t a = kNoNode, b = kNoNode;
    float    t = 0.0f;
    Vec3     point = {};
    float    dist = 0.0f;          // snap distance from the query position
    bool     Valid() const { return a != kNoNode; }
};

struct SearchResult {
    SearchStatus          status = SearchStatus::Exhausted;
    std::vector<uint32_t> path;           // graph nodes origin → destination, consecutive nodes are linked
    Vec3                  start = {};     // on-road start point (on the link into path.front())
    Vec3                  end   = {};     // on-road end point (on the link out of path.back())
    float                 length = 0.0f;  // start → path → end, 3D
    float                 originSnapDist = 0.0f;
    float                 destSnapDist   = 0.0f;
    bool                  destReSnapped  = false;   // nearest road unreachable; reachable one chosen
    uint32_t              expanded = 0;
};

// Nearest routable node. use3D=false ignores z (map waypoints have z = 0).
uint32_t NearestNode(const RoadGraph& g, const Vec3& p, bool use3D, float maxDist, float* outDist);

// Nearest point on any routable link. zWeight scales height difference
// (0 = 2D). floodFill >= 0 restricts to that connected component.
RoadPoint NearestRoadPoint(const RoadGraph& g, const Vec3& p, float zWeight, float maxDist, int floodFill = -1);

SearchResult FindRoute(const RoadGraph& g, const Vec3& origin, const Vec3& destination,
                       const SearchLimits& limits = {});

// ── Background loader (call from the GTA thread) ─────────────────────
enum class LoadState : uint8_t { NotStarted, Loading, Ready, Failed };

void             RequestLoad();     // starts the one-time load of <gta>\models\gta3.img
LoadState        GetLoadState();
const RoadGraph* Get();             // non-null only when Ready
const char*      LoadError();       // valid when Failed

// Tests: install a graph (state Ready) or nullptr (state NotStarted).
void SetForTesting(const RoadGraph* g);

} // namespace skr::gps::graph
