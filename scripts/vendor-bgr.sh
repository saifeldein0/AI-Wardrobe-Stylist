#!/usr/bin/env bash
# Vendors @imgly/background-removal into public/bgr/ so background removal
# runs fully offline (no CDN). Run once before deploying or using local mode.
set -e
cd "$(dirname "$0")/.."
mkdir -p public/bgr
npm init -y >/dev/null 2>&1 || true
npm install @imgly/background-removal@1.4.5 esbuild
npx esbuild node_modules/@imgly/background-removal/dist/index.mjs \
  --bundle --format=esm --outfile=public/bgr/bg.mjs
# copy the model + wasm resources (content-addressed chunks)
node -e '
const fs = require("fs"), path = require("path");
const dist = "node_modules/@imgly/background-removal-data/dist";
const res = JSON.parse(fs.readFileSync(path.join(dist, "resources.json")));
fs.copyFileSync(path.join(dist, "resources.json"), "public/bgr/resources.json");
for (const entry of Object.values(res)) {
  for (const chunk of Object.values(entry.chunks || {})) {
    for (const h of chunk.hash ? [chunk.hash] : Object.keys(chunk)) {
      const f = path.join(dist, h);
      if (fs.existsSync(f)) fs.copyFileSync(f, path.join("public/bgr", h));
    }
  }
}
console.log("bgr vendored");
'
