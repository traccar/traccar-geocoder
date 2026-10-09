mod auth;

use axum::extract::Query;
use axum::http::StatusCode;
use axum::response::{IntoResponse, Response};
use axum::routing::get;
use axum::Router;
use memmap2::Mmap;
use s2::cellid::CellID;
use s2::latlng::LatLng;
use serde::{Deserialize, Serialize};
use std::borrow::Cow;
use std::fs::File;
use std::sync::{Arc, RwLock};

// --- S2 helpers ---

const DEFAULT_STREET_CELL_LEVEL: u64 = 17;
const DEFAULT_ADMIN_CELL_LEVEL: u64 = 10;
const DEFAULT_SEARCH_DISTANCE: f64 = 75.0;

fn cell_id_at_level(lat: f64, lng: f64, level: u64) -> u64 {
    let ll = LatLng::from_degrees(lat, lng);
    CellID::from(ll).parent(level).0
}

fn cell_neighbors_at_level(cell_id: u64, level: u64) -> Vec<u64> {
    let cell = CellID(cell_id);
    cell.all_neighbors(level).into_iter().map(|c| c.0).collect()
}

// --- Binary format structs (must match C++ build pipeline) ---

#[repr(C)]
#[derive(Clone, Copy)]
struct WayHeader {
    node_offset: u32,
    node_count: u8,
    name_id: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct AddrPoint {
    lat: f32,
    lng: f32,
    housenumber_id: u32,
    street_id: u32,
    city_id: u32,     // 0 = not present
    suburb_id: u32,   // 0 = not present
    postcode_id: u32, // 0 = not present
}

#[repr(C)]
#[derive(Clone, Copy)]
struct InterpWay {
    node_offset: u32,
    node_count: u8,
    street_id: u32,
    start_number: u32,
    end_number: u32,
    interpolation: u8,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct AdminPolygon {
    vertex_offset: u32,
    vertex_count: u16,
    name_id: u32,
    semantic: u8,
    area: f32,
    country_code: u16,
}

const SEM_NONE: u8 = 0;
const SEM_COUNTRY: u8 = 1;
const SEM_STATE: u8 = 2;
const SEM_COUNTY: u8 = 3;
const SEM_CITY: u8 = 4;
const SEM_SUBURB: u8 = 5;
const SEM_POSTCODE: u8 = 6;

// Max distance from query to a place node when falling back (degrees, squared).
// 20 km / 111320 m-per-degree.
const PLACE_FALLBACK_MAX_DIST: f32 = (20000.0 / 111320.0) * (20000.0 / 111320.0);
const SEM_COUNT: usize = 7;

#[repr(C)]
#[derive(Clone, Copy)]
struct NodeCoord {
    lat: f32,
    lng: f32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct PlaceNode {
    lat: f32,
    lng: f32,
    name_id: u32,
    semantic: u8,
    country_code: u16, // packed ISO 3166-1 alpha-2, 0 = unknown
}

// --- Index data ---

struct Index {
    geo_cells: Mmap,
    street_entries: Mmap,
    street_ways: Mmap,
    street_nodes: Mmap,
    addr_entries: Mmap,
    addr_points: Mmap,
    interp_entries: Mmap,
    interp_ways: Mmap,
    interp_nodes: Mmap,
    admin_cells: Mmap,
    admin_entries: Mmap,
    admin_polygons: Mmap,
    admin_vertices: Mmap,
    place_cells: Mmap,
    place_entries: Mmap,
    place_nodes: Mmap,
    strings: Mmap,
    street_cell_level: u64,
    admin_cell_level: u64,
}

const NO_DATA: u32 = 0xFFFFFFFF;

struct GeoCellOffsets {
    street: u32,
    addr: u32,
    interp: u32,
}

fn mmap_file(path: &str) -> Result<Mmap, String> {
    let file = File::open(path).map_err(|e| format!("Failed to open {}: {}", path, e))?;
    unsafe { Mmap::map(&file).map_err(|e| format!("Failed to mmap {}: {}", path, e)) }
}

impl Index {
    fn load(dir: &str, street_cell_level: u64, admin_cell_level: u64) -> Result<Self, String> {
        Ok(Index {
            geo_cells: mmap_file(&format!("{}/geo_cells.bin", dir))?,
            street_entries: mmap_file(&format!("{}/street_entries.bin", dir))?,
            street_ways: mmap_file(&format!("{}/street_ways.bin", dir))?,
            street_nodes: mmap_file(&format!("{}/street_nodes.bin", dir))?,
            addr_entries: mmap_file(&format!("{}/addr_entries.bin", dir))?,
            addr_points: mmap_file(&format!("{}/addr_points.bin", dir))?,
            interp_entries: mmap_file(&format!("{}/interp_entries.bin", dir))?,
            interp_ways: mmap_file(&format!("{}/interp_ways.bin", dir))?,
            interp_nodes: mmap_file(&format!("{}/interp_nodes.bin", dir))?,
            admin_cells: mmap_file(&format!("{}/admin_cells.bin", dir))?,
            admin_entries: mmap_file(&format!("{}/admin_entries.bin", dir))?,
            admin_polygons: mmap_file(&format!("{}/admin_polygons.bin", dir))?,
            admin_vertices: mmap_file(&format!("{}/admin_vertices.bin", dir))?,
            place_cells: mmap_file(&format!("{}/place_cells.bin", dir))?,
            place_entries: mmap_file(&format!("{}/place_entries.bin", dir))?,
            place_nodes: mmap_file(&format!("{}/place_nodes.bin", dir))?,
            strings: mmap_file(&format!("{}/strings.bin", dir))?,
            street_cell_level,
            admin_cell_level,
        })
    }

    fn get_string(&self, offset: u32) -> &str {
        let bytes = &self.strings[offset as usize..];
        let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
        std::str::from_utf8(&bytes[..end]).unwrap_or("")
    }

    fn read_u16(data: &[u8], offset: usize) -> u16 {
        u16::from_le_bytes([data[offset], data[offset + 1]])
    }

    fn read_u32(data: &[u8], offset: usize) -> u32 {
        u32::from_le_bytes(data[offset..offset + 4].try_into().unwrap())
    }

    fn read_u64(data: &[u8], offset: usize) -> u64 {
        u64::from_le_bytes(data[offset..offset + 8].try_into().unwrap())
    }

    // Iterate entry IDs inline from entries file at given offset
    fn for_each_entry(entries: &[u8], offset: u32, mut f: impl FnMut(u32)) {
        if offset == NO_DATA { return; }
        let offset = offset as usize;
        if offset + 2 > entries.len() { return; }

        let id_count = Self::read_u16(entries, offset) as usize;
        let data_start = offset + 2;
        if data_start + id_count * 4 > entries.len() { return; }

        for i in 0..id_count {
            f(Self::read_u32(entries, data_start + i * 4));
        }
    }

    // Binary search geo cell index: 20 bytes per entry (u64 cell_id + u32 street + u32 addr + u32 interp)
    fn lookup_geo_cell(cells: &[u8], cell_id: u64) -> GeoCellOffsets {
        let entry_size: usize = 20;
        let count = cells.len() / entry_size;
        let empty = GeoCellOffsets { street: NO_DATA, addr: NO_DATA, interp: NO_DATA };
        if count == 0 { return empty; }

        let mut lo = 0usize;
        let mut hi = count;
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            let mid_id = Self::read_u64(cells, mid * entry_size);
            if mid_id == cell_id {
                return GeoCellOffsets {
                    street: Self::read_u32(cells, mid * entry_size + 8),
                    addr: Self::read_u32(cells, mid * entry_size + 12),
                    interp: Self::read_u32(cells, mid * entry_size + 16),
                };
            } else if mid_id < cell_id {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        empty
    }

    // Binary search admin cell index: 12 bytes per entry (u64 cell_id + u32 offset)
    fn lookup_admin_cell(cells: &[u8], cell_id: u64) -> u32 {
        let entry_size: usize = 12;
        let count = cells.len() / entry_size;
        if count == 0 { return NO_DATA; }

        let mut lo = 0usize;
        let mut hi = count;
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            let mid_id = Self::read_u64(cells, mid * entry_size);
            if mid_id == cell_id {
                return Self::read_u32(cells, mid * entry_size + 8);
            } else if mid_id < cell_id {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        NO_DATA
    }

    // --- Geo lookup (streets, addresses, interpolation from merged index) ---

    fn query_geo(&self, lat: f64, lng: f64) -> (Option<(f64, &AddrPoint)>, Option<(f64, &InterpWay, u32)>, Option<(f64, &WayHeader)>) {
        let cell = cell_id_at_level(lat, lng, self.street_cell_level);
        let neighbors = cell_neighbors_at_level(cell, self.street_cell_level);

        let all_points: &[AddrPoint] = unsafe {
            std::slice::from_raw_parts(
                self.addr_points.as_ptr() as *const AddrPoint,
                self.addr_points.len() / std::mem::size_of::<AddrPoint>(),
            )
        };
        let all_ways: &[WayHeader] = unsafe {
            std::slice::from_raw_parts(
                self.street_ways.as_ptr() as *const WayHeader,
                self.street_ways.len() / std::mem::size_of::<WayHeader>(),
            )
        };
        let all_street_nodes: &[NodeCoord] = unsafe {
            std::slice::from_raw_parts(
                self.street_nodes.as_ptr() as *const NodeCoord,
                self.street_nodes.len() / std::mem::size_of::<NodeCoord>(),
            )
        };
        let all_interps: &[InterpWay] = unsafe {
            std::slice::from_raw_parts(
                self.interp_ways.as_ptr() as *const InterpWay,
                self.interp_ways.len() / std::mem::size_of::<InterpWay>(),
            )
        };
        let all_interp_nodes: &[NodeCoord] = unsafe {
            std::slice::from_raw_parts(
                self.interp_nodes.as_ptr() as *const NodeCoord,
                self.interp_nodes.len() / std::mem::size_of::<NodeCoord>(),
            )
        };

        let cos_lat = lat.to_radians().cos();

        let mut best_addr_dist = f64::MAX;
        let mut best_addr: Option<&AddrPoint> = None;
        let mut best_street_dist = f64::MAX;
        let mut best_street: Option<&WayHeader> = None;
        let mut best_interp_dist = f64::MAX;
        let mut best_interp: Option<&InterpWay> = None;
        let mut best_interp_t: f64 = 0.0;

        // Fixed-size hash set on stack to skip duplicate street IDs across cells
        let mut seen_streets: [u32; 64] = [u32::MAX; 64];

        for c in std::iter::once(cell).chain(neighbors.into_iter()) {
            let offsets = Self::lookup_geo_cell(&self.geo_cells, c);

            // Addresses
            Self::for_each_entry(&self.addr_entries, offsets.addr, |id| {
                let point = &all_points[id as usize];
                let dlat = (point.lat as f64 - lat).to_radians();
                let dlng = (point.lng as f64 - lng).to_radians();
                let dist = dist_sq(dlat, dlng, cos_lat);
                if dist < best_addr_dist {
                    best_addr_dist = dist;
                    best_addr = Some(point);
                }
            });

            // Streets
            Self::for_each_entry(&self.street_entries, offsets.street, |id| {
                let slot = (id as usize) & 0x3F;
                if seen_streets[slot] == id { return; }
                seen_streets[slot] = id;

                let way = &all_ways[id as usize];
                let offset = way.node_offset as usize;
                let count = way.node_count as usize;
                let nodes = &all_street_nodes[offset..offset + count];

                for i in 0..nodes.len() - 1 {
                    let dist = point_to_segment_distance(
                        lat, lng,
                        nodes[i].lat as f64, nodes[i].lng as f64,
                        nodes[i + 1].lat as f64, nodes[i + 1].lng as f64,
                        cos_lat,
                    );
                    if dist < best_street_dist {
                        best_street_dist = dist;
                        best_street = Some(way);
                    }
                }
            });

            // Interpolation
            Self::for_each_entry(&self.interp_entries, offsets.interp, |id| {
                let iw = &all_interps[id as usize];
                if iw.start_number == 0 || iw.end_number == 0 { return; }

                let offset = iw.node_offset as usize;
                let count = iw.node_count as usize;
                let nodes = &all_interp_nodes[offset..offset + count];

                let mut total_len: f64 = 0.0;
                for i in 0..nodes.len() - 1 {
                    let dlat = (nodes[i + 1].lat as f64 - nodes[i].lat as f64).to_radians();
                    let dlng = (nodes[i + 1].lng as f64 - nodes[i].lng as f64).to_radians();
                    total_len += dist_sq(dlat, dlng, cos_lat);
                }
                if total_len == 0.0 { return; }

                let mut best_seg_dist = f64::MAX;
                let mut best_seg_t: f64 = 0.0;
                let mut prev_accumulated: f64 = 0.0;

                for i in 0..nodes.len() - 1 {
                    let dlat = (nodes[i + 1].lat as f64 - nodes[i].lat as f64).to_radians();
                    let dlng = (nodes[i + 1].lng as f64 - nodes[i].lng as f64).to_radians();
                    let seg_len = dist_sq(dlat, dlng, cos_lat);
                    let (dist, seg_t) = point_to_segment_with_t(
                        lat, lng,
                        nodes[i].lat as f64, nodes[i].lng as f64,
                        nodes[i + 1].lat as f64, nodes[i + 1].lng as f64,
                        cos_lat,
                    );
                    if dist < best_seg_dist {
                        best_seg_dist = dist;
                        best_seg_t = (prev_accumulated + seg_t * seg_len) / total_len;
                    }
                    prev_accumulated += seg_len;
                }

                if best_seg_dist < best_interp_dist {
                    best_interp_dist = best_seg_dist;
                    best_interp = Some(iw);
                    best_interp_t = best_seg_t;
                }
            });
        }

        let addr_result = best_addr.map(|p| (best_addr_dist, p));
        let street_result = best_street.map(|w| (best_street_dist, w));
        let interp_result = best_interp.map(|iw| {
            let start = iw.start_number as f64;
            let end = iw.end_number as f64;
            let raw = start + best_interp_t * (end - start);

            let step: u32 = match iw.interpolation {
                1 | 2 => 2,
                _ => 1,
            };

            let number = if step == 2 {
                let base = iw.start_number;
                let offset = ((raw - base as f64) / step as f64).round() as u32 * step;
                base + offset
            } else {
                raw.round() as u32
            };

            (best_interp_dist, iw, number)
        });

        (addr_result, interp_result, street_result)
    }

    // --- Admin boundary lookup (point-in-polygon) ---

    fn find_admin(&self, lat: f64, lng: f64) -> AdminResult<'_> {
        let cell = cell_id_at_level(lat, lng, self.admin_cell_level);
        let neighbors = cell_neighbors_at_level(cell, self.admin_cell_level);

        let all_polygons: &[AdminPolygon] = unsafe {
            std::slice::from_raw_parts(
                self.admin_polygons.as_ptr() as *const AdminPolygon,
                self.admin_polygons.len() / std::mem::size_of::<AdminPolygon>(),
            )
        };
        let all_vertices: &[NodeCoord] = unsafe {
            std::slice::from_raw_parts(
                self.admin_vertices.as_ptr() as *const NodeCoord,
                self.admin_vertices.len() / std::mem::size_of::<NodeCoord>(),
            )
        };

        // For each semantic level, find the smallest-area polygon containing the point
        let mut best_by_semantic: [Option<(f32, &AdminPolygon)>; SEM_COUNT] = [None; SEM_COUNT];

        const INTERIOR_FLAG: u32 = 0x80000000;
        const ID_MASK: u32 = 0x7FFFFFFF;

        for c in std::iter::once(cell).chain(neighbors.iter().copied()) {
            Self::for_each_entry(&self.admin_entries, Self::lookup_admin_cell(&self.admin_cells, c), |id| {
                let is_interior = (id & INTERIOR_FLAG) != 0;
                let poly_id = (id & ID_MASK) as usize;
                let poly = &all_polygons[poly_id];
                let semantic = poly.semantic as usize;
                if semantic == SEM_NONE as usize || semantic >= SEM_COUNT { return; }

                if let Some((best_area, _)) = best_by_semantic[semantic] {
                    if poly.area >= best_area { return; }
                }

                if is_interior || point_in_polygon(lat as f32, lng as f32, {
                    let offset = poly.vertex_offset as usize;
                    let count = poly.vertex_count as usize;
                    &all_vertices[offset..offset + count]
                }) {
                    best_by_semantic[semantic] = Some((poly.area, poly));
                }
            });
        }

        // Fallback: nearest place node by semantic. Only consider nodes whose
        // country matches the resolved country, to avoid cross-border leaks
        // (e.g. a Luxembourg suburb showing up for a French point near the border).
        let mut best_node_by_semantic: [Option<(f32, &PlaceNode)>; SEM_COUNT] = [None; SEM_COUNT];
        let resolved_country = best_by_semantic[SEM_COUNTRY as usize].and_then(|(_, p)| {
            if p.country_code != 0 { Some(p.country_code) } else { None }
        });
        if let Some(country) = resolved_country {
            let all_places: &[PlaceNode] = unsafe {
                std::slice::from_raw_parts(
                    self.place_nodes.as_ptr() as *const PlaceNode,
                    self.place_nodes.len() / std::mem::size_of::<PlaceNode>(),
                )
            };
            let qlat = lat as f32;
            let qlng = lng as f32;
            for c in std::iter::once(cell).chain(neighbors.into_iter()) {
                Self::for_each_entry(&self.place_entries, Self::lookup_admin_cell(&self.place_cells, c), |id| {
                    let node = &all_places[id as usize];
                    let semantic = node.semantic as usize;
                    if semantic == SEM_NONE as usize || semantic >= SEM_COUNT { return; }
                    if best_by_semantic[semantic].is_some() { return; }
                    if node.country_code != 0 && node.country_code != country { return; }
                    let dx = node.lat - qlat;
                    let dy = node.lng - qlng;
                    let dist = dx * dx + dy * dy;
                    if dist > PLACE_FALLBACK_MAX_DIST { return; }
                    if let Some((best_d, _)) = best_node_by_semantic[semantic] {
                        if dist >= best_d { return; }
                    }
                    best_node_by_semantic[semantic] = Some((dist, node));
                });
            }
        }

        let mut result = AdminResult::default();
        if let Some((_, poly)) = best_by_semantic[SEM_COUNTRY as usize] {
            result.country = Some(self.get_string(poly.name_id));
            if poly.country_code != 0 {
                result.country_code = Some([
                    (poly.country_code >> 8) as u8,
                    (poly.country_code & 0xFF) as u8,
                ]);
            }
        }

        for (sem, slot) in [
            (SEM_STATE,  &mut result.state),
            (SEM_COUNTY, &mut result.county),
            (SEM_CITY,   &mut result.city),
            (SEM_SUBURB, &mut result.suburb),
        ] {
            if let Some((_, poly)) = best_by_semantic[sem as usize] {
                *slot = Some(self.get_string(poly.name_id));
            } else if let Some((_, node)) = best_node_by_semantic[sem as usize] {
                *slot = Some(self.get_string(node.name_id));
            }
        }

        if let Some((_, poly)) = best_by_semantic[SEM_POSTCODE as usize] {
            result.postcode = Some(self.get_string(poly.name_id));
        }
        result
    }

    // --- Combined query ---

    fn query(&self, lat: f64, lng: f64, search_distance: f64) -> Address<'_> {
        let meters_to_rad = search_distance / 111_320.0;
        let max_dist = meters_to_rad * meters_to_rad;

        let admin = self.find_admin(lat, lng);
        let (addr, interp, street) = self.query_geo(lat, lng);

        // Pick whichever is closer: the nearest house (address point or
        // interpolation) or the nearest street centerline. If the centerline
        // wins, attach a house number only when it lies on that same road.
        let mut house_number: Option<Cow<'_, str>> = None;
        let mut road: Option<&str> = None;

        let addr = addr.filter(|(d, _)| *d < max_dist);
        let interp = interp.filter(|(d, _, _)| *d < max_dist);
        let street = street.filter(|(d, _)| *d < max_dist);

        // Only use explicit address tags when that address supplies the result.
        let mut matched_addr = None;

        let house = [
            addr.map(|(d, p)| (d, p.street_id, Cow::Borrowed(self.get_string(p.housenumber_id)), Some(p))),
            interp.map(|(d, iw, n)| (d, iw.street_id, Cow::Owned(n.to_string()), None)),
        ]
        .into_iter()
        .flatten()
        .min_by(|a, b| a.0.partial_cmp(&b.0).unwrap());

        if let Some((hd, hs, hn, point)) = house {
            if let Some((_, way)) = street.filter(|(sd, _)| *sd < hd) {
                road = Some(self.get_string(way.name_id));
                if hs == way.name_id {
                    house_number = Some(hn);
                    matched_addr = point;
                }
            } else {
                road = Some(self.get_string(hs));
                house_number = Some(hn);
                matched_addr = point;
            }
        } else if let Some((_, way)) = street {
            road = Some(self.get_string(way.name_id));
        }

        if road.is_none() && admin.country.is_none() && admin.city.is_none() {
            return Address::default();
        }

        // Explicit addr:* tags take priority; inferred fields fill any gaps.
        let mut city = admin.city;
        let mut suburb = admin.suburb;
        let mut postcode = admin.postcode;
        if let Some(p) = matched_addr {
            for (slot, id) in [
                (&mut city,     p.city_id),
                (&mut suburb,   p.suburb_id),
                (&mut postcode, p.postcode_id),
            ] {
                if id != 0 {
                    *slot = Some(self.get_string(id));
                }
            }
        }

        let address = AddressDetails {
            house_number,
            road,
            suburb,
            city,
            state: admin.state,
            county: admin.county,
            postcode,
            country: admin.country,
            country_code: admin.country_code.map(|c| String::from_utf8_lossy(&c).into_owned()),
        };
        let display_name = format_address(&address);
        Address { display_name, address }
    }

    fn snap(&self, lat: f64, lng: f64, search_distance: f64) -> Option<Snap> {
        let meters_to_rad = search_distance / 111_320.0;
        let max_dist = meters_to_rad * meters_to_rad;

        let cell = cell_id_at_level(lat, lng, self.street_cell_level);
        let neighbors = cell_neighbors_at_level(cell, self.street_cell_level);

        let all_ways: &[WayHeader] = unsafe {
            std::slice::from_raw_parts(
                self.street_ways.as_ptr() as *const WayHeader,
                self.street_ways.len() / std::mem::size_of::<WayHeader>(),
            )
        };
        let all_street_nodes: &[NodeCoord] = unsafe {
            std::slice::from_raw_parts(
                self.street_nodes.as_ptr() as *const NodeCoord,
                self.street_nodes.len() / std::mem::size_of::<NodeCoord>(),
            )
        };

        let cos_lat = lat.to_radians().cos();

        let mut best_dist = f64::MAX;
        let mut best_lat = 0.0_f64;
        let mut best_lng = 0.0_f64;

        let mut seen_streets: [u32; 64] = [u32::MAX; 64];

        for c in std::iter::once(cell).chain(neighbors.into_iter()) {
            let offsets = Self::lookup_geo_cell(&self.geo_cells, c);

            Self::for_each_entry(&self.street_entries, offsets.street, |id| {
                let slot = (id as usize) & 0x3F;
                if seen_streets[slot] == id { return; }
                seen_streets[slot] = id;

                let way = &all_ways[id as usize];
                let offset = way.node_offset as usize;
                let count = way.node_count as usize;
                let nodes = &all_street_nodes[offset..offset + count];

                for i in 0..nodes.len() - 1 {
                    let ax = nodes[i].lat as f64;
                    let ay = nodes[i].lng as f64;
                    let bx = nodes[i + 1].lat as f64;
                    let by = nodes[i + 1].lng as f64;
                    let (dist, t) = point_to_segment_with_t(lat, lng, ax, ay, bx, by, cos_lat);
                    if dist < best_dist {
                        best_dist = dist;
                        best_lat = ax + t * (bx - ax);
                        best_lng = ay + t * (by - ay);
                    }
                }
            });
        }

        if best_dist >= max_dist { return None; }

        Some(Snap {
            lat: best_lat,
            lng: best_lng,
            distance: best_dist.sqrt() * 111_320.0,
        })
    }
}

// --- Geometry helpers ---

fn dist_sq(dlat: f64, dlng: f64, cos_lat: f64) -> f64 {
    dlat * dlat + dlng * dlng * cos_lat * cos_lat
}

fn point_to_segment_with_t(
    px: f64, py: f64,
    ax: f64, ay: f64,
    bx: f64, by: f64,
    cos_lat: f64,
) -> (f64, f64) {
    let dx = bx - ax;
    let dy = by - ay;
    let len_sq = dx * dx + dy * dy;

    let t = if len_sq == 0.0 {
        0.0
    } else {
        (((px - ax) * dx + (py - ay) * dy) / len_sq).clamp(0.0, 1.0)
    };

    let proj_x = ax + t * dx;
    let proj_y = ay + t * dy;
    let dlat = (px - proj_x).to_radians();
    let dlng = (py - proj_y).to_radians();
    (dist_sq(dlat, dlng, cos_lat), t)
}

fn point_to_segment_distance(
    px: f64, py: f64,
    ax: f64, ay: f64,
    bx: f64, by: f64,
    cos_lat: f64,
) -> f64 {
    point_to_segment_with_t(px, py, ax, ay, bx, by, cos_lat).0
}

// Ray casting point-in-polygon test
fn point_in_polygon(lat: f32, lng: f32, vertices: &[NodeCoord]) -> bool {
    let mut inside = false;
    let n = vertices.len();
    let mut j = n - 1;

    for i in 0..n {
        let vi = &vertices[i];
        let vj = &vertices[j];

        if ((vi.lng > lng) != (vj.lng > lng))
            && (lat < (vj.lat - vi.lat) * (lng - vi.lng) / (vj.lng - vi.lng) + vi.lat)
        {
            inside = !inside;
        }
        j = i;
    }

    inside
}

// --- API types ---

#[derive(Default)]
struct AdminResult<'a> {
    country: Option<&'a str>,
    country_code: Option<[u8; 2]>,
    state: Option<&'a str>,
    county: Option<&'a str>,
    city: Option<&'a str>,
    suburb: Option<&'a str>,
    postcode: Option<&'a str>,
}

#[derive(Serialize, Default)]
struct AddressDetails<'a> {
    #[serde(skip_serializing_if = "Option::is_none")]
    house_number: Option<Cow<'a, str>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    road: Option<&'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    suburb: Option<&'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    city: Option<&'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    state: Option<&'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    county: Option<&'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    postcode: Option<&'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    country: Option<&'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    country_code: Option<String>,
}

#[derive(Serialize, Default)]
struct Address<'a> {
    #[serde(skip_serializing_if = "Option::is_none")]
    display_name: Option<String>,
    address: AddressDetails<'a>,
}

#[derive(Serialize)]
struct Snap {
    lat: f64,
    #[serde(rename = "lon")]
    lng: f64,
    distance: f64,
}

// Address formatting patterns by country code
// Returns (number_after_street, postcode_before_city, include_state)
fn format_rules(country_code: Option<&str>) -> (bool, bool, bool) {
    match country_code {
        // Number before street, postcode after city, include state
        Some("US") | Some("CA") | Some("AU") | Some("NZ")
        | Some("GB") | Some("IE") | Some("ZA") | Some("IN")
        | Some("NG") | Some("KE") | Some("GH") | Some("PK")
        | Some("PH") | Some("TH") | Some("MY") => (false, false, true),

        // Number before street, postcode before city, include state
        Some("JP") | Some("KR") | Some("CN") | Some("TW") => (false, true, true),

        // Number after street, postcode before city, no state (most of Europe, etc.)
        _ => (true, true, false),
    }
}

fn format_address(addr: &AddressDetails<'_>) -> Option<String> {
    if addr.road.is_none() && addr.city.is_none() && addr.country.is_none() {
        return None;
    }

    let (number_after, postcode_before_city, include_state) = format_rules(addr.country_code.as_deref());
    let mut parts: Vec<String> = Vec::new();

    // Street + house number
    if let Some(road) = addr.road {
        if let Some(ref hn) = addr.house_number {
            if number_after {
                parts.push(format!("{} {}", road, hn));
            } else {
                parts.push(format!("{} {}", hn, road));
            }
        } else {
            parts.push(road.to_string());
        }
    }

    // City + postcode + state
    if postcode_before_city {
        let mut city_part = String::new();
        if let Some(pc) = addr.postcode {
            city_part.push_str(pc);
            city_part.push(' ');
        }
        if let Some(city) = addr.city {
            city_part.push_str(city);
        }
        if include_state {
            if let Some(state) = addr.state {
                if !city_part.is_empty() { city_part.push_str(", "); }
                city_part.push_str(state);
            }
        }
        if !city_part.is_empty() {
            parts.push(city_part.trim().to_string());
        }
    } else {
        let mut city_part = String::new();
        if let Some(city) = addr.city {
            city_part.push_str(city);
        }
        if include_state {
            if let Some(state) = addr.state {
                if !city_part.is_empty() { city_part.push_str(", "); }
                city_part.push_str(state);
            }
        }
        if let Some(pc) = addr.postcode {
            if !city_part.is_empty() { city_part.push(' '); }
            city_part.push_str(pc);
        }
        if !city_part.is_empty() {
            parts.push(city_part);
        }
    }

    // Country
    if let Some(country) = addr.country {
        parts.push(country.to_string());
    }

    if parts.is_empty() { None } else { Some(parts.join(", ")) }
}

#[derive(Deserialize)]
struct QueryParams {
    lat: f64,
    lon: f64,
    key: Option<String>,
    distance: Option<f64>,
}

fn authorize(
    key: &Option<String>,
    db: &RwLock<auth::Db>,
    limiter: &auth::RateLimiter,
    addr: std::net::SocketAddr,
) -> Result<(), Response> {
    let key = key.as_deref()
        .ok_or_else(|| (StatusCode::UNAUTHORIZED, "Missing API key").into_response())?;

    let (login, rps, rpd, by_ip) = db.read().unwrap().validate_token(key)
        .ok_or_else(|| (StatusCode::UNAUTHORIZED, "Invalid API key").into_response())?;

    let rate_key = if by_ip { format!("{}:{}", login, addr.ip()) } else { login };

    auth::check_rate(limiter, &rate_key, rps, rpd)
        .map_err(|msg| (StatusCode::TOO_MANY_REQUESTS, msg).into_response())
}

fn json_response<T: Serialize>(value: &T) -> Response {
    let json = serde_json::to_string(value).unwrap_or_default();
    ([(axum::http::header::CONTENT_TYPE, "application/json")], json).into_response()
}

async fn reverse_geocode(
    Query(params): Query<QueryParams>,
    state: axum::extract::State<Arc<RwLock<auth::Db>>>,
    index: axum::extract::Extension<Arc<Index>>,
    limiter: axum::extract::Extension<Arc<auth::RateLimiter>>,
    connect_info: axum::extract::ConnectInfo<std::net::SocketAddr>,
) -> Response {
    if let Err(r) = authorize(&params.key, &state, &limiter, connect_info.0) { return r; }

    let search_distance = params.distance.unwrap_or(DEFAULT_SEARCH_DISTANCE);
    json_response(&index.query(params.lat, params.lon, search_distance))
}

async fn snap_to_road(
    Query(params): Query<QueryParams>,
    state: axum::extract::State<Arc<RwLock<auth::Db>>>,
    index: axum::extract::Extension<Arc<Index>>,
    limiter: axum::extract::Extension<Arc<auth::RateLimiter>>,
    connect_info: axum::extract::ConnectInfo<std::net::SocketAddr>,
) -> Response {
    if let Err(r) = authorize(&params.key, &state, &limiter, connect_info.0) { return r; }

    let search_distance = params.distance.unwrap_or(DEFAULT_SEARCH_DISTANCE);
    match index.snap(params.lat, params.lon, search_distance) {
        Some(s) => json_response(&s),
        None => StatusCode::NOT_FOUND.into_response(),
    }
}

#[tokio::main]
async fn main() {
    let args: Vec<String> = std::env::args().collect();
    let data_dir = args.get(1).map(|s| s.as_str()).unwrap_or(".");

    let arg_value = |flag: &str| -> Option<&String> {
        args.iter().position(|a| a == flag).and_then(|p| args.get(p + 1))
    };
    let street_cell_level = arg_value("--street-level").and_then(|v| v.parse().ok()).unwrap_or(DEFAULT_STREET_CELL_LEVEL);
    let admin_cell_level = arg_value("--admin-level").and_then(|v| v.parse().ok()).unwrap_or(DEFAULT_ADMIN_CELL_LEVEL);

    let db_path = format!("{}/geocoder.json", data_dir);
    let db = auth::Db::load(&db_path);

    let index_dir = format!("{}/index", data_dir);
    eprintln!("Loading index from {}...", index_dir);
    let index = match Index::load(&index_dir, street_cell_level, admin_cell_level) {
        Ok(idx) => Arc::new(idx),
        Err(e) => {
            eprintln!("Error: {}", e);
            std::process::exit(1);
        }
    };

    let db = Arc::new(RwLock::new(db));
    let limiter = Arc::new(auth::RateLimiter::default()); // RwLock<HashMap> with atomic counters

    let app = Router::new()
        .route("/reverse", get(reverse_geocode))
        .route("/snap", get(snap_to_road))
        .merge(auth::router())
        .layer(axum::Extension(index))
        .layer(axum::Extension(limiter))
        .with_state(db);

    // ACME mode: --domain <domain> [--cache <dir>]
    let domain_pos = args.iter().position(|a| a == "--domain");
    if let Some(pos) = domain_pos {
        let domain = args.get(pos + 1).expect("--domain requires a value").clone();
        let cache_dir = args.iter().position(|a| a == "--cache")
            .and_then(|p| args.get(p + 1).cloned())
            .unwrap_or_else(|| "acme-cache".to_string());

        use rustls_acme::caches::DirCache;
        use rustls_acme::AcmeConfig;
        use tokio_stream::StreamExt;

        let mut state = AcmeConfig::new([domain.clone()])
            .cache(DirCache::new(cache_dir.clone()))
            .directory_lets_encrypt(true)
            .state();
        let acceptor = state.axum_acceptor(state.default_rustls_config());

        tokio::spawn(async move {
            loop {
                match state.next().await {
                    Some(Ok(ok)) => eprintln!("ACME event: {:?}", ok),
                    Some(Err(err)) => eprintln!("ACME error: {:?}", err),
                    None => break,
                }
            }
        });

        let addr = std::net::SocketAddr::from(([0, 0, 0, 0], 443));
        eprintln!("Starting HTTPS server on :443 for {}...", domain);
        axum_server::bind(addr)
            .acceptor(acceptor)
            .serve(app.into_make_service_with_connect_info::<std::net::SocketAddr>())
            .await
            .unwrap();
    } else {
        let bind_addr = args.get(2).map(|s| s.as_str()).unwrap_or("0.0.0.0:3000");
        eprintln!("Starting HTTP server on {}...", bind_addr);
        let listener = tokio::net::TcpListener::bind(bind_addr).await.unwrap();
        axum::serve(listener, app.into_make_service_with_connect_info::<std::net::SocketAddr>()).await.unwrap();
    }
}
