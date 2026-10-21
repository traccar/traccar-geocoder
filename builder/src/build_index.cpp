#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <osmium/handler.hpp>
#include <osmium/io/pbf_input.hpp>
#include <osmium/visitor.hpp>
#include <osmium/handler/node_locations_for_ways.hpp>
#include <fcntl.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libkern/OSByteOrder.h>
#define htole16(x) OSSwapHostToLittleInt16(x)
#define le16toh(x) OSSwapLittleToHostInt16(x)
#else
#include <endian.h>
#endif
#include <osmium/index/map/sparse_file_array.hpp>
#include <osmium/area/assembler.hpp>
#include <osmium/area/multipolygon_manager.hpp>

#include <s2/s2cell_id.h>
#include <s2/s2latlng.h>
#include <s2/s2region_coverer.h>
#include <s2/s2polyline.h>
#include <s2/s2polygon.h>
#include <s2/s2loop.h>
#include <s2/s2builder.h>

#include "address_levels.hpp"

// --- Binary format structs ---

struct WayHeader {
    uint32_t node_offset;
    uint16_t node_count;
    uint32_t name_id;
};

struct AddrPoint {
    float lat;
    float lng;
    uint32_t housenumber_id;
    uint32_t street_id;
    uint32_t city_id;     // addr:city, 0 = none
    uint32_t suburb_id;   // addr:suburb, 0 = none
    uint32_t postcode_id; // addr:postcode, 0 = none
};

struct InterpWay {
    uint32_t node_offset;
    uint16_t node_count;
    uint32_t street_id;
    uint32_t start_number;
    uint32_t end_number;
    uint8_t interpolation;
};

struct AdminPolygon {
    uint32_t vertex_offset;
    uint16_t vertex_count;
    uint32_t name_id;
    uint8_t semantic;   // address_levels::Semantic enum
    float area;
    uint16_t country_code;
};

// Tag info kept in parallel during build, used to compute semantic level
// after pass 2 once we know each polygon's parent country.
// kind=0 means admin_level (numeric stored as level), kind=1 means place=X (value).
struct PolyTagInfo {
    uint8_t kind;       // 0 = admin, 1 = place
    uint8_t level;      // when kind==0
    std::string value;  // when kind==1
};

struct NodeCoord {
    float lat;
    float lng;
};

struct PlaceNode {
    float lat;
    float lng;
    uint32_t name_id;
    uint8_t semantic;     // address_levels::Semantic
    uint16_t country_code; // packed ISO 3166-1 alpha-2, 0 = unknown
};

// Tag info for place nodes (build-time only, parallel to place_nodes)
struct PlaceNodeTagInfo {
    std::string place_type; // e.g. "city", "town", "village"
};

static const uint32_t INTERIOR_FLAG = 0x80000000u;
static const uint32_t ID_MASK = 0x7FFFFFFFu;

// --- String interning ---

class StringPool {
public:
    uint32_t intern(const std::string& s) {
        auto it = index_.find(s);
        if (it != index_.end()) {
            return it->second;
        }
        uint32_t offset = static_cast<uint32_t>(data_.size());
        index_[s] = offset;
        data_.insert(data_.end(), s.begin(), s.end());
        data_.push_back('\0');
        return offset;
    }

    const std::vector<char>& data() const { return data_; }

private:
    std::unordered_map<std::string, uint32_t> index_;
    std::vector<char> data_;
};

// --- Collected data ---

static StringPool strings;

// Streets
static std::vector<WayHeader> ways;
static std::vector<NodeCoord> street_nodes;
static std::unordered_map<uint64_t, std::vector<uint32_t>> cell_to_ways;

// Addresses
static std::vector<AddrPoint> addr_points;
static std::unordered_map<uint64_t, std::vector<uint32_t>> cell_to_addrs;

// Interpolation
static std::vector<InterpWay> interp_ways;
static std::vector<NodeCoord> interp_nodes;
static std::unordered_map<uint64_t, std::vector<uint32_t>> cell_to_interps;

// Admin boundaries
static std::vector<AdminPolygon> admin_polygons;
static std::vector<PolyTagInfo> poly_tag_info; // parallel to admin_polygons (build-time only)
static std::vector<NodeCoord> admin_vertices;
static std::unordered_map<uint64_t, std::vector<uint32_t>> cell_to_admin;

// Place nodes (city/town/village/suburb points)
static std::vector<PlaceNode> place_nodes;
static std::vector<PlaceNodeTagInfo> place_node_tag_info; // parallel to place_nodes (build-time only)
static std::unordered_map<uint64_t, std::vector<uint32_t>> cell_to_places;

// --- S2 helpers ---

static int kStreetCellLevel = 17;
static bool kAllRoads = false;
static int kAdminCellLevel = 10;

static std::vector<S2CellId> cover_edge(double lat1, double lng1, double lat2, double lng2) {
    S2Point p1 = S2LatLng::FromDegrees(lat1, lng1).ToPoint();
    S2Point p2 = S2LatLng::FromDegrees(lat2, lng2).ToPoint();

    if (p1 == p2) {
        return {S2CellId(p1).parent(kStreetCellLevel)};
    }

    std::vector<S2Point> points = {p1, p2};
    S2Polyline polyline(points);

    S2RegionCoverer::Options options;
    options.set_fixed_level(kStreetCellLevel);

    S2RegionCoverer coverer(options);
    S2CellUnion covering = coverer.GetCovering(polyline);
    return covering.cell_ids();
}

static S2CellId point_to_cell(double lat, double lng) {
    return S2CellId(S2LatLng::FromDegrees(lat, lng)).parent(kStreetCellLevel);
}

// Returns pairs of (cell_id, is_interior)
static std::vector<std::pair<S2CellId, bool>> cover_polygon(const std::vector<std::pair<double,double>>& vertices) {
    std::vector<S2Point> points;
    points.reserve(vertices.size());
    for (const auto& [lat, lng] : vertices) {
        S2Point p = S2LatLng::FromDegrees(lat, lng).ToPoint();
        if (!points.empty() && points.back() == p) continue;
        points.push_back(p);
    }
    // Remove closing duplicate if first == last
    if (points.size() > 1 && points.front() == points.back()) {
        points.pop_back();
    }
    if (points.size() < 3) return {};

    // Build S2Loop (must be CCW), skip invalid polygons
    S2Error error;
    auto loop = std::make_unique<S2Loop>(points, S2Debug::DISABLE);
    loop->Normalize();
    if (loop->FindValidationError(&error)) return {};

    S2Polygon polygon(std::move(loop));

    S2RegionCoverer::Options options;
    options.set_max_level(kAdminCellLevel);
    options.set_max_cells(200);

    S2RegionCoverer coverer(options);
    S2CellUnion covering = coverer.GetCovering(polygon);
    S2CellUnion interior = coverer.GetInteriorCovering(polygon);

    // Build set of interior cell IDs for fast lookup
    std::unordered_set<uint64_t> interior_set;
    for (const auto& cell : interior.cell_ids()) {
        if (cell.level() <= kAdminCellLevel) {
            auto begin = cell.range_min().parent(kAdminCellLevel);
            auto end = cell.range_max().parent(kAdminCellLevel);
            for (auto c = begin; c != end; c = c.next()) {
                interior_set.insert(c.id());
            }
            interior_set.insert(end.id());
        } else {
            interior_set.insert(cell.parent(kAdminCellLevel).id());
        }
    }

    // Only mark a cell as interior if all its neighbors are also interior.
    // This erodes the interior by one cell from the border, avoiding
    // false positives from spherical/flat geometry mismatch near edges.
    std::unordered_set<uint64_t> safe_interior;
    for (uint64_t id : interior_set) {
        S2CellId cell(id);
        S2CellId neighbors[4];
        cell.GetEdgeNeighbors(neighbors);
        bool all_interior = true;
        for (const auto& n : neighbors) {
            if (interior_set.count(n.id()) == 0) {
                all_interior = false;
                break;
            }
        }
        if (all_interior) {
            safe_interior.insert(id);
        }
    }

    // Normalize all covering cells to kAdminCellLevel
    std::vector<std::pair<S2CellId, bool>> result;
    for (const auto& cell : covering.cell_ids()) {
        if (cell.level() <= kAdminCellLevel) {
            auto begin = cell.range_min().parent(kAdminCellLevel);
            auto end = cell.range_max().parent(kAdminCellLevel);
            for (auto c = begin; c != end; c = c.next()) {
                result.emplace_back(c, safe_interior.count(c.id()) > 0);
            }
            result.emplace_back(end, safe_interior.count(end.id()) > 0);
        } else {
            auto parent = cell.parent(kAdminCellLevel);
            result.emplace_back(parent, safe_interior.count(parent.id()) > 0);
        }
    }

    // Deduplicate by cell_id, keeping interior=true if any duplicate is interior
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    auto it = result.begin();
    for (auto curr = result.begin(); curr != result.end(); ) {
        auto next = curr + 1;
        bool is_interior = curr->second;
        while (next != result.end() && next->first == curr->first) {
            is_interior = is_interior || next->second;
            ++next;
        }
        *it = {curr->first, is_interior};
        ++it;
        curr = next;
    }
    result.erase(it, result.end());
    return result;
}

// Approximate polygon area in square degrees
static float polygon_area(const std::vector<std::pair<double,double>>& vertices) {
    double area = 0;
    size_t n = vertices.size();
    for (size_t i = 0; i < n; i++) {
        size_t j = (i + 1) % n;
        area += vertices[i].first * vertices[j].second;
        area -= vertices[j].first * vertices[i].second;
    }
    return static_cast<float>(std::fabs(area) / 2.0);
}

// Douglas-Peucker simplification
static void dp_simplify(const std::vector<std::pair<double,double>>& pts,
                        size_t start, size_t end, double epsilon,
                        std::vector<bool>& keep) {
    if (end <= start + 1) return;

    double max_dist = 0;
    size_t max_idx = start;

    double ax = pts[start].first, ay = pts[start].second;
    double bx = pts[end].first, by = pts[end].second;
    double dx = bx - ax, dy = by - ay;
    double len_sq = dx * dx + dy * dy;

    for (size_t i = start + 1; i < end; i++) {
        double px = pts[i].first - ax, py = pts[i].second - ay;
        double dist;
        if (len_sq == 0) {
            dist = std::sqrt(px * px + py * py);
        } else {
            double t = std::max(0.0, std::min(1.0, (px * dx + py * dy) / len_sq));
            double proj_x = t * dx - px, proj_y = t * dy - py;
            dist = std::sqrt(proj_x * proj_x + proj_y * proj_y);
        }
        if (dist > max_dist) {
            max_dist = dist;
            max_idx = i;
        }
    }

    if (max_dist > epsilon) {
        keep[max_idx] = true;
        dp_simplify(pts, start, max_idx, epsilon, keep);
        dp_simplify(pts, max_idx, end, epsilon, keep);
    }
}

static std::vector<std::pair<double,double>> simplify_polygon(
    const std::vector<std::pair<double,double>>& pts, size_t max_vertices) {
    if (pts.size() <= max_vertices) return pts;

    // Binary search for epsilon that gives ~max_vertices
    double lo = 0, hi = 1.0;
    std::vector<std::pair<double,double>> result;

    for (int iter = 0; iter < 20; iter++) {
        double epsilon = (lo + hi) / 2;
        std::vector<bool> keep(pts.size(), false);
        keep[0] = true;
        keep[pts.size() - 1] = true;
        dp_simplify(pts, 0, pts.size() - 1, epsilon, keep);

        size_t count = 0;
        for (bool k : keep) if (k) count++;

        if (count > max_vertices) {
            lo = epsilon;
        } else {
            hi = epsilon;
        }
    }

    std::vector<bool> keep(pts.size(), false);
    keep[0] = true;
    keep[pts.size() - 1] = true;
    dp_simplify(pts, 0, pts.size() - 1, hi, keep);

    result.clear();
    for (size_t i = 0; i < pts.size(); i++) {
        if (keep[i]) result.push_back(pts[i]);
    }
    return result;
}

// --- Highway filter ---

static const std::vector<std::string> kExcludedHighways = {
    "footway", "path", "steps", "cycleway",
    "bridleway", "construction"
};

static bool is_included_highway(const char* value) {
    for (const auto& excluded : kExcludedHighways) {
        if (excluded == value) return false;
    }
    return true;
}

// --- Parse house number (leading digits) ---

static uint32_t parse_house_number(const char* s) {
    if (!s) return 0;
    uint32_t n = 0;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        s++;
    }
    return n;
}

// --- Add an address point ---

static uint64_t addr_count_total = 0;

// Packs an ISO 3166-1 alpha-2 code, 0 = unknown. Always uppercase, so codes
// from country polygon tags and from the (lowercase) country lookup for place
// nodes compare equal in the server.
static uint16_t pack_country_code(const char* code) {
    if (!code || !code[0] || !code[1]) return 0;
    return static_cast<uint16_t>(
        (std::toupper(static_cast<unsigned char>(code[0])) << 8) |
        std::toupper(static_cast<unsigned char>(code[1])));
}

// String ID 0 is reserved for "not present" (interned as empty string in main).
static inline uint32_t intern_or_zero(const char* s) {
    return (s && *s) ? strings.intern(s) : 0;
}

static void add_addr_point(double lat, double lng,
                            const char* housenumber, const char* street,
                            const char* city, const char* suburb, const char* postcode) {
    uint32_t addr_id = static_cast<uint32_t>(addr_points.size());
    addr_points.push_back({
        static_cast<float>(lat),
        static_cast<float>(lng),
        strings.intern(housenumber),
        strings.intern(street),
        intern_or_zero(city),
        intern_or_zero(suburb),
        intern_or_zero(postcode),
    });

    S2CellId cell = point_to_cell(lat, lng);
    cell_to_addrs[cell.id()].push_back(addr_id);

    addr_count_total++;
    if (addr_count_total % 1000000 == 0) {
        std::cerr << "Collected " << addr_count_total / 1000000 << "M addresses..." << std::endl;
    }
}

// --- Add an admin polygon ---
//
// `semantic` may be Semantic::None to defer; the post-pass will compute it
// from `tag` once the parent country is known. Pass a fixed value
// (e.g. Country for ISO-tagged level 2, Postcode for boundary=postal_code)
// to skip the lookup.
static void add_admin_polygon(const std::vector<std::pair<double,double>>& vertices,
                               const char* name,
                               address_levels::Semantic semantic,
                               const PolyTagInfo& tag,
                               const char* country_code) {
    // Simplify large polygons (proportional to preserve border accuracy)
    size_t max_vertices = std::clamp(vertices.size() / 5, size_t(500), size_t(65000));
    auto simplified = simplify_polygon(vertices, max_vertices);
    if (simplified.size() < 3) return;

    uint32_t poly_id = static_cast<uint32_t>(admin_polygons.size());
    uint32_t vertex_offset = static_cast<uint32_t>(admin_vertices.size());

    for (const auto& [lat, lng] : simplified) {
        admin_vertices.push_back({static_cast<float>(lat), static_cast<float>(lng)});
    }

    AdminPolygon poly{};
    poly.vertex_offset = vertex_offset;
    poly.vertex_count = static_cast<uint16_t>(std::min(simplified.size(), size_t(65535)));
    poly.name_id = strings.intern(name);
    poly.semantic = static_cast<uint8_t>(semantic);
    poly.area = polygon_area(simplified);
    poly.country_code = pack_country_code(country_code);
    admin_polygons.push_back(poly);
    poly_tag_info.push_back(tag);

    // S2 cell coverage (high bit marks interior cells)
    auto cell_ids = cover_polygon(simplified);
    for (const auto& [cell_id, is_interior] : cell_ids) {
        uint32_t entry = is_interior ? (poly_id | INTERIOR_FLAG) : poly_id;
        cell_to_admin[cell_id.id()].push_back(entry);
    }
}

// --- OSM handler (pass 2) ---

class BuildHandler : public osmium::handler::Handler {
public:
    void node(const osmium::Node& node) {
        if (!node.location().valid()) return;
        double lat = node.location().lat();
        double lng = node.location().lon();

        // Address point
        const char* housenumber = node.tags()["addr:housenumber"];
        const char* street = node.tags()["addr:street"];
        if (housenumber && street) {
            add_addr_point(lat, lng, housenumber, street,
                           node.tags()["addr:city"],
                           node.tags()["addr:suburb"],
                           node.tags()["addr:postcode"]);
        }

        // Place node (city/town/village/suburb/hamlet/...)
        const char* place = node.tags()["place"];
        const char* name = node.tags()["name"];
        if (place && name) {
            uint32_t place_id = static_cast<uint32_t>(place_nodes.size());
            place_nodes.push_back({
                static_cast<float>(lat), static_cast<float>(lng),
                strings.intern(name), 0 /* semantic filled in post-pass */
            });
            place_node_tag_info.push_back({place});
            S2CellId cell = S2CellId(S2LatLng::FromDegrees(lat, lng)).parent(kAdminCellLevel);
            cell_to_places[cell.id()].push_back(place_id);
        }
    }

    void way(const osmium::Way& way) {
        // Address interpolation
        const char* interpolation = way.tags()["addr:interpolation"];
        if (interpolation) {
            process_interpolation_way(way, interpolation);
            return;
        }

        // Building addresses
        const char* housenumber = way.tags()["addr:housenumber"];
        if (housenumber) {
            const char* street = way.tags()["addr:street"];
            if (street) {
                process_building_address(way, housenumber, street,
                                         way.tags()["addr:city"],
                                         way.tags()["addr:suburb"],
                                         way.tags()["addr:postcode"]);
            }
        }

        // Highway ways
        const char* highway = way.tags()["highway"];
        if (highway && is_included_highway(highway)) {
            const char* name = way.tags()["name"];
            if (name || kAllRoads) {
                process_highway(way, name);
            }
        }
    }

    void area(const osmium::Area& area) {
        const char* boundary = area.tags()["boundary"];
        if (!boundary) return;

        bool is_admin  = (std::strcmp(boundary, "administrative") == 0);
        bool is_place  = (std::strcmp(boundary, "place") == 0);
        bool is_postal = (std::strcmp(boundary, "postal_code") == 0);
        if (!is_admin && !is_place && !is_postal) return;

        const char* name = area.tags()["name"];

        // Decide tag info + immediate semantic where possible.
        PolyTagInfo tag{};
        address_levels::Semantic semantic = address_levels::Semantic::None;
        const char* country_code = nullptr;
        std::string name_str;

        if (is_admin) {
            const char* level_str = area.tags()["admin_level"];
            if (!level_str || !name) return;
            int admin_level = std::atoi(level_str);
            if (admin_level < 2 || admin_level > 12) return;

            tag.kind = 0;
            tag.level = static_cast<uint8_t>(admin_level);
            name_str = name;

            if (admin_level == 2) {
                country_code = area.tags()["ISO3166-1:alpha2"];
                if (!country_code) return;
                semantic = address_levels::Semantic::Country;
            }
        } else if (is_place) {
            const char* place = area.tags()["place"];
            if (!place || !name) return;
            tag.kind = 1;
            tag.value = place;
            name_str = name;
        } else { // is_postal
            const char* postal_code = area.tags()["postal_code"];
            if (!postal_code) postal_code = name;
            if (!postal_code) return;
            // No tag info needed - semantic is already final
            name_str = postal_code;
            semantic = address_levels::Semantic::Postcode;
        }

        // Admin/place boundaries may also carry a postal_code tag
        // (e.g. suburbs in Australia like Lyndhurst).
        const char* extra_postal_code = (is_admin || is_place) ? area.tags()["postal_code"] : nullptr;

        for (const auto& outer_ring : area.outer_rings()) {
            std::vector<std::pair<double,double>> vertices;
            for (const auto& node_ref : outer_ring) {
                if (node_ref.location().valid()) {
                    vertices.emplace_back(node_ref.location().lat(), node_ref.location().lon());
                }
            }
            if (vertices.size() < 3) continue;

            add_admin_polygon(vertices, name_str.c_str(), semantic, tag, country_code);
            if (extra_postal_code) {
                PolyTagInfo none{};
                add_admin_polygon(vertices, extra_postal_code,
                                  address_levels::Semantic::Postcode, none, nullptr);
            }
        }

        admin_count_++;
        if (admin_count_ % 10000 == 0) {
            std::cerr << "Processed " << admin_count_ / 1000 << "K admin/place boundaries..." << std::endl;
        }
    }

    uint64_t way_count() const { return way_count_; }
    uint64_t building_addr_count() const { return building_addr_count_; }
    uint64_t interp_count() const { return interp_count_; }
    uint64_t admin_count() const { return admin_count_; }

private:
    uint64_t way_count_ = 0;
    uint64_t building_addr_count_ = 0;
    uint64_t interp_count_ = 0;
    uint64_t admin_count_ = 0;

    void process_building_address(const osmium::Way& way, const char* housenumber, const char* street,
                                   const char* city, const char* suburb, const char* postcode) {
        const auto& wnodes = way.nodes();
        if (wnodes.empty()) return;

        double sum_lat = 0, sum_lng = 0;
        int valid = 0;
        for (const auto& nr : wnodes) {
            if (!nr.location().valid()) continue;
            sum_lat += nr.location().lat();
            sum_lng += nr.location().lon();
            valid++;
        }
        if (valid == 0) return;

        add_addr_point(sum_lat / valid, sum_lng / valid, housenumber, street, city, suburb, postcode);
        building_addr_count_++;
    }

    void process_interpolation_way(const osmium::Way& way, const char* interpolation) {
        const auto& wnodes = way.nodes();
        if (wnodes.size() < 2) return;

        for (const auto& nr : wnodes) {
            if (!nr.location().valid()) return;
        }

        const char* street = way.tags()["addr:street"];
        if (!street) return;

        uint8_t interp_type = 0;
        if (std::strcmp(interpolation, "even") == 0) interp_type = 1;
        else if (std::strcmp(interpolation, "odd") == 0) interp_type = 2;

        uint32_t interp_id = static_cast<uint32_t>(interp_ways.size());
        uint32_t node_offset = static_cast<uint32_t>(interp_nodes.size());

        for (const auto& nr : wnodes) {
            interp_nodes.push_back({
                static_cast<float>(nr.location().lat()),
                static_cast<float>(nr.location().lon())
            });
        }

        InterpWay iw{};
        iw.node_offset = node_offset;
        iw.node_count = htole16(static_cast<uint16_t>(std::min(wnodes.size(), size_t(UINT16_MAX))));
        iw.street_id = strings.intern(street);
        iw.start_number = 0;
        iw.end_number = 0;
        iw.interpolation = interp_type;
        interp_ways.push_back(iw);

        std::unordered_set<uint64_t> interp_cells;
        for (size_t i = 0; i + 1 < wnodes.size(); i++) {
            double lat1 = wnodes[i].location().lat();
            double lng1 = wnodes[i].location().lon();
            double lat2 = wnodes[i + 1].location().lat();
            double lng2 = wnodes[i + 1].location().lon();

            auto cell_ids = cover_edge(lat1, lng1, lat2, lng2);
            for (const auto& cell_id : cell_ids) {
                interp_cells.insert(cell_id.id());
            }
        }
        for (uint64_t cell_id : interp_cells) {
            cell_to_interps[cell_id].push_back(interp_id);
        }

        interp_count_++;
    }

    void process_highway(const osmium::Way& way, const char* name) {
        const auto& wnodes = way.nodes();
        if (wnodes.size() < 2) return;

        for (const auto& nr : wnodes) {
            if (!nr.location().valid()) return;
        }

        uint32_t way_id = static_cast<uint32_t>(ways.size());
        uint32_t node_offset = static_cast<uint32_t>(street_nodes.size());

        for (const auto& nr : wnodes) {
            street_nodes.push_back({
                static_cast<float>(nr.location().lat()),
                static_cast<float>(nr.location().lon())
            });
        }

        WayHeader header{};
        header.node_offset = node_offset;
        header.node_count = htole16(static_cast<uint16_t>(std::min(wnodes.size(), size_t(UINT16_MAX))));
        header.name_id = intern_or_zero(name);
        ways.push_back(header);

        std::unordered_set<uint64_t> way_cells;
        for (size_t i = 0; i + 1 < wnodes.size(); i++) {
            double lat1 = wnodes[i].location().lat();
            double lng1 = wnodes[i].location().lon();
            double lat2 = wnodes[i + 1].location().lat();
            double lng2 = wnodes[i + 1].location().lon();

            auto cell_ids = cover_edge(lat1, lng1, lat2, lng2);
            for (const auto& cell_id : cell_ids) {
                way_cells.insert(cell_id.id());
            }
        }
        for (uint64_t cell_id : way_cells) {
            cell_to_ways[cell_id].push_back(way_id);
        }

        way_count_++;
        if (way_count_ % 1000000 == 0) {
            std::cerr << "Processed " << way_count_ / 1000000 << "M street ways..." << std::endl;
        }
    }
};

// --- Resolve interpolation way endpoint house numbers ---

static void resolve_interpolation_endpoints() {
    struct CoordKey {
        int32_t lat;
        int32_t lng;
        bool operator==(const CoordKey& o) const { return lat == o.lat && lng == o.lng; }
    };
    struct CoordHash {
        size_t operator()(const CoordKey& k) const {
            return std::hash<int64_t>()(((int64_t)k.lat << 32) | (uint32_t)k.lng);
        }
    };

    std::unordered_map<CoordKey, uint32_t, CoordHash> addr_by_coord;
    for (uint32_t i = 0; i < addr_points.size(); i++) {
        CoordKey key{
            static_cast<int32_t>(addr_points[i].lat * 100000),
            static_cast<int32_t>(addr_points[i].lng * 100000)
        };
        addr_by_coord[key] = i;
    }

    uint32_t resolved = 0;
    for (auto& iw : interp_ways) {
        if (le16toh(iw.node_count) < 2) continue;

        const auto& start = interp_nodes[iw.node_offset];
        CoordKey start_key{
            static_cast<int32_t>(start.lat * 100000),
            static_cast<int32_t>(start.lng * 100000)
        };
        auto it_start = addr_by_coord.find(start_key);

        const auto& end = interp_nodes[iw.node_offset + le16toh(iw.node_count) - 1];
        CoordKey end_key{
            static_cast<int32_t>(end.lat * 100000),
            static_cast<int32_t>(end.lng * 100000)
        };
        auto it_end = addr_by_coord.find(end_key);

        if (it_start != addr_by_coord.end()) {
            const char* hn = strings.data().data() + addr_points[it_start->second].housenumber_id;
            iw.start_number = parse_house_number(hn);
        }
        if (it_end != addr_by_coord.end()) {
            const char* hn = strings.data().data() + addr_points[it_end->second].housenumber_id;
            iw.end_number = parse_house_number(hn);
        }

        if (iw.start_number > 0 && iw.end_number > 0) resolved++;
    }

    std::cerr << "Resolved " << resolved << "/" << interp_ways.size()
              << " interpolation ways" << std::endl;
}

// --- Deduplicate IDs per cell ---

template<typename Map>
static void deduplicate(Map& cell_map) {
    for (auto& [cell_id, ids] : cell_map) {
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    }
}

// --- Write cell index ---

static const uint32_t NO_DATA = 0xFFFFFFFFu;

// Write entries file and return offset map
static std::unordered_map<uint64_t, uint32_t> write_entries(
    const std::string& path,
    const std::vector<uint64_t>& sorted_cells,
    const std::unordered_map<uint64_t, std::vector<uint32_t>>& cell_map
) {
    std::unordered_map<uint64_t, uint32_t> offsets;
    std::ofstream f(path, std::ios::binary);
    uint32_t current = 0;
    for (uint64_t cell_id : sorted_cells) {
        auto it = cell_map.find(cell_id);
        if (it == cell_map.end()) continue;
        offsets[cell_id] = current;
        uint16_t count = static_cast<uint16_t>(std::min(it->second.size(), size_t(65535)));
        f.write(reinterpret_cast<const char*>(&count), sizeof(count));
        f.write(reinterpret_cast<const char*>(it->second.data()), it->second.size() * sizeof(uint32_t));
        current += sizeof(uint16_t) + it->second.size() * sizeof(uint32_t);
    }
    return offsets;
}

static void write_cell_index(
    const std::string& cells_path,
    const std::string& entries_path,
    const std::unordered_map<uint64_t, std::vector<uint32_t>>& cell_map
) {
    std::vector<std::pair<uint64_t, std::vector<uint32_t>>> sorted(
        cell_map.begin(), cell_map.end());
    std::sort(sorted.begin(), sorted.end());

    {
        std::ofstream f(cells_path, std::ios::binary);
        uint32_t current_offset = 0;
        for (const auto& [cell_id, ids] : sorted) {
            f.write(reinterpret_cast<const char*>(&cell_id), sizeof(cell_id));
            f.write(reinterpret_cast<const char*>(&current_offset), sizeof(current_offset));
            current_offset += sizeof(uint16_t) + ids.size() * sizeof(uint32_t);
        }
    }

    {
        std::ofstream f(entries_path, std::ios::binary);
        for (const auto& [cell_id, ids] : sorted) {
            uint16_t count = static_cast<uint16_t>(std::min(ids.size(), size_t(65535)));
            f.write(reinterpret_cast<const char*>(&count), sizeof(count));
            f.write(reinterpret_cast<const char*>(ids.data()), ids.size() * sizeof(uint32_t));
        }
    }
}

// --- Resolve semantic level for each polygon ---
//
// For each polygon whose semantic is still None, look up its parent country
// (by point-in-polygon against country polygons on the centroid), then map
// (country, tag) -> rank -> semantic via the Nominatim ruleset.
static bool point_in_polygon_f64(double lat, double lng, const NodeCoord* verts, size_t n) {
    bool inside = false;
    size_t j = n - 1;
    for (size_t i = 0; i < n; i++) {
        double vi_lat = verts[i].lat, vi_lng = verts[i].lng;
        double vj_lat = verts[j].lat, vj_lng = verts[j].lng;
        if ((vi_lng > lng) != (vj_lng > lng) &&
            lat < (vj_lat - vi_lat) * (lng - vi_lng) / (vj_lng - vi_lng) + vi_lat) {
            inside = !inside;
        }
        j = i;
    }
    return inside;
}

static void resolve_semantic_levels() {
    // Collect country polygons with bounding boxes for fast PIP prefilter.
    struct Country {
        uint32_t poly_id;
        char code[3]; // null-terminated lowercase
        float min_lat, max_lat;
        float min_lng, max_lng;
    };
    std::vector<Country> countries;
    for (uint32_t i = 0; i < admin_polygons.size(); i++) {
        const auto& p = admin_polygons[i];
        if (p.semantic != (uint8_t)address_levels::Semantic::Country || p.country_code == 0) continue;
        Country c{};
        c.poly_id = i;
        c.code[0] = std::tolower((p.country_code >> 8) & 0xFF);
        c.code[1] = std::tolower(p.country_code & 0xFF);
        c.code[2] = 0;
        const NodeCoord* verts = admin_vertices.data() + p.vertex_offset;
        c.min_lat = c.max_lat = verts[0].lat;
        c.min_lng = c.max_lng = verts[0].lng;
        for (uint32_t v = 1; v < p.vertex_count; v++) {
            c.min_lat = std::min(c.min_lat, verts[v].lat);
            c.max_lat = std::max(c.max_lat, verts[v].lat);
            c.min_lng = std::min(c.min_lng, verts[v].lng);
            c.max_lng = std::max(c.max_lng, verts[v].lng);
        }
        countries.push_back(c);
    }

    auto find_country = [&](double lat, double lng) -> const char* {
        const char* code = nullptr;
        float best_area = std::numeric_limits<float>::infinity();
        for (const auto& c : countries) {
            const auto& cp = admin_polygons[c.poly_id];
            if (cp.area >= best_area) continue;
            if (lat < c.min_lat || lat > c.max_lat ||
                lng < c.min_lng || lng > c.max_lng) continue;
            const NodeCoord* cverts = admin_vertices.data() + cp.vertex_offset;
            if (point_in_polygon_f64(lat, lng, cverts, cp.vertex_count)) {
                code = c.code;
                best_area = cp.area;
            }
        }
        return code;
    };

    uint32_t resolved_polys = 0;
    for (uint32_t i = 0; i < admin_polygons.size(); i++) {
        auto& p = admin_polygons[i];
        if (p.semantic != (uint8_t)address_levels::Semantic::None) continue;

        const NodeCoord* verts = admin_vertices.data() + p.vertex_offset;
        double cx = 0, cy = 0;
        for (uint32_t j = 0; j < p.vertex_count; j++) {
            cx += verts[j].lat;
            cy += verts[j].lng;
        }
        cx /= p.vertex_count;
        cy /= p.vertex_count;

        const char* country_code = find_country(cx, cy);
        address_levels::Semantic sem = address_levels::Semantic::None;
        const auto& tag = poly_tag_info[i];
        if (tag.kind == 0) {
            sem = address_levels::lookup_admin(country_code, tag.level);
        } else {
            sem = address_levels::lookup_place(tag.value.c_str(), true /* polygon */);
        }
        p.semantic = static_cast<uint8_t>(sem);
        if (sem != address_levels::Semantic::None) resolved_polys++;
    }

    uint32_t resolved_places = 0;
    for (uint32_t i = 0; i < place_nodes.size(); i++) {
        auto& n = place_nodes[i];
        const char* country_code = find_country(n.lat, n.lng);
        address_levels::Semantic sem = address_levels::lookup_place(
            place_node_tag_info[i].place_type.c_str(), false /* node */);
        n.semantic = static_cast<uint8_t>(sem);
        n.country_code = pack_country_code(country_code);
        if (sem != address_levels::Semantic::None) resolved_places++;
    }

    std::cerr << "Resolved semantic level for " << resolved_polys << "/"
              << admin_polygons.size() << " polygons, "
              << resolved_places << "/" << place_nodes.size() << " place nodes ("
              << countries.size() << " country polygons)" << std::endl;
}

// --- Write all index files ---

static void write_index(const std::string& output_dir) {
    // Merged geo cell index for streets, addresses, and interpolation
    std::set<uint64_t> all_geo_cells;
    for (const auto& [id, _] : cell_to_ways) all_geo_cells.insert(id);
    for (const auto& [id, _] : cell_to_addrs) all_geo_cells.insert(id);
    for (const auto& [id, _] : cell_to_interps) all_geo_cells.insert(id);
    std::vector<uint64_t> sorted_geo_cells(all_geo_cells.begin(), all_geo_cells.end());

    auto street_offsets = write_entries(output_dir + "/street_entries.bin", sorted_geo_cells, cell_to_ways);
    auto addr_offsets = write_entries(output_dir + "/addr_entries.bin", sorted_geo_cells, cell_to_addrs);
    auto interp_offsets = write_entries(output_dir + "/interp_entries.bin", sorted_geo_cells, cell_to_interps);

    {
        std::ofstream f(output_dir + "/geo_cells.bin", std::ios::binary);
        for (uint64_t cell_id : sorted_geo_cells) {
            f.write(reinterpret_cast<const char*>(&cell_id), sizeof(cell_id));
            auto write_offset = [&](const std::unordered_map<uint64_t, uint32_t>& offsets) {
                auto it = offsets.find(cell_id);
                uint32_t offset = (it != offsets.end()) ? it->second : NO_DATA;
                f.write(reinterpret_cast<const char*>(&offset), sizeof(offset));
            };
            write_offset(street_offsets);
            write_offset(addr_offsets);
            write_offset(interp_offsets);
        }
    }

    std::cerr << "geo index: " << sorted_geo_cells.size() << " cells ("
              << ways.size() << " ways, "
              << addr_points.size() << " addrs, "
              << interp_ways.size() << " interps)" << std::endl;

    write_cell_index(output_dir + "/admin_cells.bin", output_dir + "/admin_entries.bin", cell_to_admin);
    std::cerr << "admin index: " << cell_to_admin.size() << " cells, " << admin_polygons.size() << " polygons" << std::endl;

    write_cell_index(output_dir + "/place_cells.bin", output_dir + "/place_entries.bin", cell_to_places);
    std::cerr << "place index: " << cell_to_places.size() << " cells, " << place_nodes.size() << " nodes" << std::endl;

    // Street ways
    {
        std::ofstream f(output_dir + "/street_ways.bin", std::ios::binary);
        f.write(reinterpret_cast<const char*>(ways.data()), ways.size() * sizeof(WayHeader));
    }

    // Street nodes
    {
        std::ofstream f(output_dir + "/street_nodes.bin", std::ios::binary);
        f.write(reinterpret_cast<const char*>(street_nodes.data()), street_nodes.size() * sizeof(NodeCoord));
    }

    // Address points
    {
        std::ofstream f(output_dir + "/addr_points.bin", std::ios::binary);
        f.write(reinterpret_cast<const char*>(addr_points.data()), addr_points.size() * sizeof(AddrPoint));
    }

    // Interpolation ways
    {
        std::ofstream f(output_dir + "/interp_ways.bin", std::ios::binary);
        f.write(reinterpret_cast<const char*>(interp_ways.data()), interp_ways.size() * sizeof(InterpWay));
    }

    // Interpolation nodes
    {
        std::ofstream f(output_dir + "/interp_nodes.bin", std::ios::binary);
        f.write(reinterpret_cast<const char*>(interp_nodes.data()), interp_nodes.size() * sizeof(NodeCoord));
    }

    // Admin polygons
    {
        std::ofstream f(output_dir + "/admin_polygons.bin", std::ios::binary);
        f.write(reinterpret_cast<const char*>(admin_polygons.data()), admin_polygons.size() * sizeof(AdminPolygon));
    }

    // Admin vertices
    {
        std::ofstream f(output_dir + "/admin_vertices.bin", std::ios::binary);
        f.write(reinterpret_cast<const char*>(admin_vertices.data()), admin_vertices.size() * sizeof(NodeCoord));
    }

    // Place nodes
    {
        std::ofstream f(output_dir + "/place_nodes.bin", std::ios::binary);
        f.write(reinterpret_cast<const char*>(place_nodes.data()), place_nodes.size() * sizeof(PlaceNode));
    }

    // Strings
    {
        std::ofstream f(output_dir + "/strings.bin", std::ios::binary);
        f.write(strings.data().data(), strings.data().size());
        std::cerr << "strings.bin: " << strings.data().size() << " bytes" << std::endl;
    }


}

// --- Main ---

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: build-index <output-dir> <input.osm.pbf> [input2.osm.pbf ...] [--street-level N] [--admin-level N] [--all-roads]" << std::endl;
        return 1;
    }

    std::string output_dir = argv[1];
    std::vector<std::string> input_files;
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--street-level" && i + 1 < argc) {
            kStreetCellLevel = std::atoi(argv[++i]);
        } else if (arg == "--admin-level" && i + 1 < argc) {
            kAdminCellLevel = std::atoi(argv[++i]);
        } else if (arg == "--all-roads") {
            kAllRoads = true;
        } else {
            input_files.push_back(arg);
        }
    }

    // Reserve string offset 0 for the empty string (used as a "not present" marker).
    strings.intern("");

    BuildHandler handler;

    for (const auto& input_file : input_files) {
        std::cerr << "Processing " << input_file << "..." << std::endl;

        // --- Pass 1: collect relation members for multipolygon assembly ---
        std::cerr << "  Pass 1: scanning relations..." << std::endl;

        osmium::area::Assembler::config_type assembler_config;
        osmium::area::MultipolygonManager<osmium::area::Assembler> mp_manager{assembler_config};

        {
            osmium::io::Reader reader1{input_file, osmium::osm_entity_bits::relation};
            osmium::apply(reader1, mp_manager);
            reader1.close();
            mp_manager.prepare_for_lookup();
        }

        // --- Pass 2: process all data ---
        std::cerr << "  Pass 2: processing nodes, ways, and areas..." << std::endl;

        using index_type = osmium::index::map::SparseFileArray<
            osmium::unsigned_object_id_type, osmium::Location>;
        using location_handler_type = osmium::handler::NodeLocationsForWays<index_type>;

        std::string tmp_path = output_dir + "/node_locations.tmp";
        int fd = open(tmp_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        index_type index{fd};
        location_handler_type location_handler{index};

        osmium::io::Reader reader2{input_file};

        osmium::apply(reader2, location_handler, handler, mp_manager.handler([&handler](osmium::memory::Buffer&& buffer) {
            osmium::apply(buffer, handler);
        }));
        reader2.close();
        close(fd);
        std::remove(tmp_path.c_str());
    }

    std::cerr << "Done reading:" << std::endl;
    std::cerr << "  " << handler.way_count() << " street ways" << std::endl;
    std::cerr << "  " << addr_count_total << " address points ("
              << handler.building_addr_count() << " from buildings)" << std::endl;
    std::cerr << "  " << handler.interp_count() << " interpolation ways" << std::endl;
    std::cerr << "  " << handler.admin_count() << " admin/postcode boundaries ("
              << admin_polygons.size() << " polygon rings)" << std::endl;

    std::cerr << "Resolving interpolation endpoints..." << std::endl;
    resolve_interpolation_endpoints();

    std::cerr << "Resolving semantic levels..." << std::endl;
    resolve_semantic_levels();
    poly_tag_info.clear();
    poly_tag_info.shrink_to_fit();
    place_node_tag_info.clear();
    place_node_tag_info.shrink_to_fit();

    std::cerr << "Deduplicating..." << std::endl;
    deduplicate(cell_to_ways);
    deduplicate(cell_to_addrs);
    deduplicate(cell_to_interps);
    deduplicate(cell_to_admin);
    deduplicate(cell_to_places);

    std::cerr << "Writing index files to " << output_dir << "..." << std::endl;
    write_index(output_dir);

    std::cerr << "Done." << std::endl;
    return 0;
}
