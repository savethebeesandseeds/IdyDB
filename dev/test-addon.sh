#!/bin/sh
# Run inside idydb-dev. All npm dependencies and build outputs stay in /build.
set -eu
workspace=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p /build/node-source/addons /build/node-source/idydb/tests
cp "$workspace/addons/addon.cpp" "$workspace/addons/binding.gyp" "$workspace/addons/package.json" \
   "$workspace/addons/package-lock.json" "$workspace/addons/test.js" \
   "$workspace/addons/test-failure.js" /build/node-source/addons/
cp -R "$workspace/idydb/include" "$workspace/idydb/impl" /build/node-source/idydb/
cp "$workspace/idydb/tests/failure_hooks.h" /build/node-source/idydb/tests/
cd /build/node-source/addons
npm ci --ignore-scripts --cache /build/npm-cache
npm run build -- --nodedir=/usr
npm test
npm run build -- --nodedir=/usr --idydb_testing=1
npm run test:failure
# Leave the usable build in its normal, non-instrumented configuration.
npm run build -- --nodedir=/usr --idydb_testing=0
node -e 'const a = require("./build/Release/idydb.node"); if (a.__testFailNextSync !== undefined || a.__testSyncFailures !== undefined) throw Error("test hooks present in production addon");'
