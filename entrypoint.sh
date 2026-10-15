#!/bin/sh
set -e

DATA_DIR="${DATA_DIR:-/data}"

download_pbf() {
    mkdir -p "$DATA_DIR/pbf"
    for url in $PBF_URLS; do
        filename=$(basename "$url")
        if [ ! -f "$DATA_DIR/pbf/$filename" ]; then
            echo "Downloading $url..."
            curl -fSL -o "$DATA_DIR/pbf/$filename" "$url"
        else
            echo "Already downloaded: $filename"
        fi
    done
}

build_index() {
    files=""
    for f in "$DATA_DIR"/pbf/*.osm.pbf; do
        [ -f "$f" ] && files="$files $f"
    done
    if [ -z "$files" ]; then
        echo "Error: no PBF files found in $DATA_DIR/pbf/"
        exit 1
    fi
    if [ -f "$DATA_DIR/index/geo_cells.bin" ]; then
        echo "Index already exists, skipping build"
        return
    fi
    mkdir -p "$DATA_DIR/index"
    level_args=""
    [ -n "$STREET_LEVEL" ] && level_args="$level_args --street-level $STREET_LEVEL"
    [ -n "$ADMIN_LEVEL" ] && level_args="$level_args --admin-level $ADMIN_LEVEL"
    [ "$INDEX_ALL_ROADS" = "true" ] && level_args="$level_args --all-roads"
    echo "Building index..."
    build-index "$DATA_DIR/index" $files $level_args
    echo "Index built."
}

serve() {
    if [ ! -f "$DATA_DIR/geocoder.json" ] && [ -f "$DATA_DIR/index/geocoder.json" ]; then
        echo "Migrating geocoder.json out of index folder..."
        mv "$DATA_DIR/index/geocoder.json" "$DATA_DIR/geocoder.json"
    fi
    args="$DATA_DIR"
    if [ -n "$DOMAIN" ]; then
        args="$args --domain $DOMAIN"
        if [ -n "$CACHE_DIR" ]; then
            args="$args --cache $CACHE_DIR"
        fi
    else
        args="$args ${BIND_ADDR:-0.0.0.0:3000}"
    fi
    [ -n "$STREET_LEVEL" ] && args="$args --street-level $STREET_LEVEL"
    [ -n "$ADMIN_LEVEL" ] && args="$args --admin-level $ADMIN_LEVEL"
    [ -n "$SEARCH_DISTANCE" ] && args="$args --search-distance $SEARCH_DISTANCE"
    echo "Starting server..."
    exec query-server $args
}

case "${1:-auto}" in
    build)
        download_pbf
        build_index
        ;;
    serve)
        serve
        ;;
    auto)
        download_pbf
        build_index
        serve
        ;;
    *)
        exec "$@"
        ;;
esac
