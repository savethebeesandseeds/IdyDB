'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { setImmediate: nextTurn } = require('node:timers/promises');
const {
  IdyDb, IDYDB_CREATE, __testFailNextSync: failNextSync,
  __testSyncFailures: syncFailures,
} = require('./build/Release/idydb.node');

assert.equal(typeof failNextSync, 'function', 'build the addon with --idydb_testing=1');
assert.equal(typeof syncFailures, 'function');
assert.equal(typeof global.gc, 'function', 'run tests using node --expose-gc');

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'idydb-addon-failure-'));
const handles = [];
function handle() {
  const db = new IdyDb();
  handles.push(db);
  return db;
}

function failClose(db, competitor, filename) {
  const before = syncFailures();
  failNextSync();
  assert.throws(() => db.close(), /close .*IdyDB error/);
  assert.equal(syncFailures(), before + 1, 'the close must hit the injected failure');
  assert.equal(db.columnNextRow(1), 2, 'failed close must retain a usable native handle');
  assert.throws(() => competitor.open(filename, 0), /IdyDB error/,
    'failed close must retain the lock until retry or discard');
}

async function run() {
  const db = handle();
  const competitor = handle();
  const retryPath = path.join(directory, 'retry.db');
  db.open(retryPath, IDYDB_CREATE);
  db.insertConstChar(1, 1, 'committed before close');
  failClose(db, competitor, retryPath);
  db.close();
  competitor.open(retryPath, 0);
  assert.equal(competitor.columnNextRow(1), 2, 'retry must persist and release the lock');
  competitor.close();

  const discardPath = path.join(directory, 'discard.db');
  db.open(discardPath, IDYDB_CREATE);
  db.insertConstChar(1, 1, 'committed before discard');
  failClose(db, competitor, discardPath);
  db.discard();
  db.discard();
  assert.throws(() => db.columnNextRow(1), /not open/);
  competitor.open(discardPath, 0);
  assert.equal(competitor.columnNextRow(1), 2, 'discard must retain committed plaintext writes');
  competitor.close();

  // Prove the native finalizer reaches the failed-close fallback, then releases
  // the lock. A finalizer that only calls close leaks the native handle here.
  const orphanPath = path.join(directory, 'orphan.db');
  const before = syncFailures();
  (() => {
    const orphan = new IdyDb();
    orphan.open(orphanPath, IDYDB_CREATE);
    orphan.insertConstChar(1, 1, 'committed before finalizer');
    failNextSync();
  })();
  for (let i = 0; i < 100 && syncFailures() === before; ++i) {
    global.gc();
    await nextTurn();
  }
  assert.equal(syncFailures(), before + 1, 'GC finalizer must encounter the injected close failure');
  assert.doesNotThrow(() => competitor.open(orphanPath, 0),
    'finalizer fallback must release the native lock after failed close');
  assert.equal(competitor.columnNextRow(1), 2, 'finalizer fallback must release the native lock');
  competitor.close();
}

run().then(() => {
  console.log('IdyDB addon persistence failure tests passed');
}).catch(error => {
  console.error(error);
  process.exitCode = 1;
}).finally(() => {
  for (const db of handles) db.discard();
  fs.rmSync(directory, { recursive: true, force: true });
});
