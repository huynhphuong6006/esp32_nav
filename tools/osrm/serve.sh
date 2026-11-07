#!/bin/sh
# Đổi sang dữ liệu mới (build.new -> current) và chạy osrm-routed ở 127.0.0.1:5000
set -e
cd "$(dirname "$0")"
IMG=ghcr.io/project-osrm/osrm-backend:latest
if [ -d build.new ]; then rm -rf current.old; [ -d current ] && mv current current.old; mv build.new current; fi
docker rm -f osrm >/dev/null 2>&1 || true
docker run -d --name osrm --restart=always -p 127.0.0.1:5000:5000 -v "$PWD/current:/data" $IMG \
  osrm-routed --algorithm mld --max-table-size 100 /data/vietnam.osrm
rm -rf current.old
