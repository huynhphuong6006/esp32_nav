#!/bin/sh
# Xử lý dữ liệu OSRM (MLD) cho xe máy. Dùng: ./build.sh [file.osm.pbf]
set -e
cd "$(dirname "$0")"
IMG=ghcr.io/project-osrm/osrm-backend:latest
mkdir -p data
PBF=${1:-data/vietnam-latest.osm.pbf}
[ -f "$PBF" ] || curl -L -o "$PBF" https://download.geofabrik.de/asia/vietnam-latest.osm.pbf
rm -rf build.new && mkdir build.new
cp "$PBF" build.new/vietnam.osm.pbf
cp motorcycle.lua build.new/
docker run --rm -v "$PWD/build.new:/data" $IMG osrm-extract -p /data/motorcycle.lua /data/vietnam.osm.pbf
docker run --rm -v "$PWD/build.new:/data" $IMG osrm-partition /data/vietnam.osrm
docker run --rm -v "$PWD/build.new:/data" $IMG osrm-customize /data/vietnam.osrm
rm -f build.new/vietnam.osm.pbf
echo "Xong: build.new (chay ./serve.sh)"
