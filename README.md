# Traccar Geocoder

A fast, self-hosted reverse geocoding service built from OpenStreetMap data. Given latitude and longitude coordinates, it returns the nearest street address including house number, street name, city, state, county, postcode, and country.

Part of the [Traccar](https://www.traccar.org) open source GPS tracking platform. Also available as a [hosted service](https://www.traccar.org/product/geocoder/).

## Features

- Street-level reverse geocoding from OSM data
- Address point, street name, and address interpolation lookup
- Administrative boundary resolution (country, state, county, city, postcode)
- Sub-millisecond query latency with memory-mapped index files
- Automatic HTTPS with Let's Encrypt
- Docker support with automatic PBF download and indexing

## Quick Start

### Docker Compose

```yaml
services:
  geocoder:
    image: traccar/traccar-geocoder
    environment:
      - PBF_URLS=https://download.geofabrik.de/europe/monaco-latest.osm.pbf
    ports:
      - "3000:3000"
    volumes:
      - geocoder-data:/data

volumes:
  geocoder-data:
```

```bash
docker compose up
```

### Docker

```bash
# All-in-one: download, build index, and serve
docker run -e PBF_URLS="https://download.geofabrik.de/europe-latest.osm.pbf" \
  -v geocoder-data:/data -p 3000:3000 traccar/traccar-geocoder

# Build index only
docker run -e PBF_URLS="https://download.geofabrik.de/europe-latest.osm.pbf" \
  -v geocoder-data:/data traccar/traccar-geocoder build

# Serve only (from pre-built index)
docker run -v geocoder-data:/data -p 3000:3000 traccar/traccar-geocoder serve

# Multiple PBF files
docker run -e PBF_URLS="https://download.geofabrik.de/europe/france-latest.osm.pbf https://download.geofabrik.de/europe/germany-latest.osm.pbf" \
  -v geocoder-data:/data -p 3000:3000 traccar/traccar-geocoder

# With automatic HTTPS
docker run -e PBF_URLS="https://planet.openstreetmap.org/pbf/planet-latest.osm.pbf" \
  -e DOMAIN=geocoder.example.com \
  -v geocoder-data:/data -p 443:443 traccar/traccar-geocoder
```

PBF files can be downloaded from [Geofabrik](https://download.geofabrik.de/).

We also provide a [ready-to-use index](https://traccar.nyc3.digitaloceanspaces.com/download/index.tar.gz) so you can skip building the index yourself.

The full earth index will take up around 21 GB of disk space, so for high performance you want a machine with at least 16gb ram, or fast NVME storage.

## API

### GET /reverse

Query parameters:
- `lat` - latitude (required)
- `lon` - longitude (required)
- `key` - API key (required)
- `distance` - max search distance in meters (optional, default 75)

Example request:

```
GET /reverse?lat=43.7384&lon=7.4246&key=YOUR_API_KEY
```

Response follows [Nominatim](https://nominatim.org/release-docs/latest/api/Reverse/) format:

```json
{
  "display_name": "Avenue de la Costa 42, 98000 Monaco, Monaco",
  "address": {
    "house_number": "42",
    "road": "Avenue de la Costa",
    "city": "Monaco",
    "state": "Monaco",
    "county": "Monaco",
    "postcode": "98000",
    "country": "Monaco",
    "country_code": "MC"
  }
}
```

Fields are omitted when not available.

Status codes:
- `200` - success
- `401` - missing or invalid API key
- `429` - rate limit exceeded

### GET /snap

Snaps a coordinate to the closest point on a street.

Query parameters:
- `lat` - latitude (required)
- `lon` - longitude (required)
- `key` - API key (required)
- `distance` - max search distance in meters (optional, default 75)

Example request:

```
GET /snap?lat=43.7384&lon=7.4246&key=YOUR_API_KEY
```

Response:

```json
{
  "lat": 43.7383,
  "lon": 7.4247,
  "distance": 4.2
}
```

`distance` is meters from the input to the snapped point.

Status codes:
- `200` - success
- `401` - missing or invalid API key
- `404` - no street within `distance`
- `429` - rate limit exceeded

### Authentication

The server includes a web dashboard for managing API keys. On first launch, navigate to the server URL in a browser to create an admin account. Once logged in, you can generate API keys and create additional users with configurable rate limits.

## Architecture

The project consists of two components:

- **Builder** (C++) - Parses OSM PBF files and creates a compact binary index using S2 geometry cells for spatial lookup.
- **Server** (Rust) - Memory-maps the index files and serves queries via HTTP/HTTPS with sub-millisecond latency.

### Index Structure

The builder produces 14 binary files:

| File | Description |
|------|-------------|
| `geo_cells.bin` | Merged S2 cell index for streets, addresses, and interpolations |
| `street_entries.bin` | Street way IDs per cell |
| `street_ways.bin` | Street way headers (node offset, name) |
| `street_nodes.bin` | Street node coordinates |
| `addr_entries.bin` | Address point IDs per cell |
| `addr_points.bin` | Address point data (coordinates, house number, street) |
| `interp_entries.bin` | Interpolation way IDs per cell |
| `interp_ways.bin` | Interpolation way headers |
| `interp_nodes.bin` | Interpolation node coordinates |
| `admin_cells.bin` | S2 cell index for admin boundaries |
| `admin_entries.bin` | Admin polygon IDs per cell |
| `admin_polygons.bin` | Admin polygon metadata |
| `admin_vertices.bin` | Admin polygon vertices |
| `strings.bin` | Deduplicated string pool |

## Building from Source

### Prerequisites

**Builder (C++):**
- CMake 3.22+
- C++17 compiler
- libosmium, protozero, zlib, bzip2, expat

**Server (Rust):**
- Rust toolchain

### Build

```bash
# Build the indexer
mkdir build && cd build && cmake ../builder && make

# Build the server
cargo build --release --manifest-path server/Cargo.toml
```

### Run

```bash
# Create index from PBF file
./build/build-index data-dir/index input.osm.pbf [input2.osm.pbf ...]

# Also index unnamed roads for road snapping
./build/build-index data-dir/index input.osm.pbf --all-roads

# Start the server
./server/target/release/query-server data-dir [bind-address]

# Start with automatic HTTPS
./server/target/release/query-server data-dir --domain geocoder.example.com
```

## Environment Variables (Docker)

| Variable | Description | Default |
|----------|-------------|---------|
| `PBF_URLS` | Space-separated list of PBF download URLs | (required for auto/build) |
| `DOMAIN` | Domain name for automatic HTTPS via Let's Encrypt | (disabled) |
| `BIND_ADDR` | HTTP bind address | `0.0.0.0:3000` |
| `DATA_DIR` | Data directory for PBF files and index | `/data` |
| `CACHE_DIR` | ACME certificate cache directory | `acme-cache` |
| `INDEX_ALL_ROADS` | Set to `true` to also index unnamed roads, so `/snap` works on every road (reverse geocoding still names the nearest named road) | `false` |

## License

    Apache License, Version 2.0

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
