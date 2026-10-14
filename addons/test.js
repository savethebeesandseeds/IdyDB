'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { setImmediate: nextTurn } = require('node:timers/promises');
const {
  IdyDb, IDYDB_CREATE, IDYDB_READONLY, IDYDB_SIM_COSINE, IDYDB_SIM_L2,
} = require('./build/Release/idydb.node');

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'idydb-addon-'));
const filename = path.join(directory, 'test.db');
const query = new Float32Array([1, 0]);
const handles = [];
function handle() {
  const db = new IdyDb();
  handles.push(db);
  return db;
}

async function run() {
  const db = handle();
  assert.throws(() => db.columnNextRow(1), /not open/);
  assert.throws(() => db.flush(), /not open/);
  assert.throws(() => db.flush('extra'), TypeError);
  assert.throws(() => db.discard('extra'), TypeError);
  db.discard();
  db.discard();
  assert.throws(() => db.open(), TypeError);
  assert.throws(() => db.open(filename, 1), RangeError);
  assert.throws(() => db.open(filename + '\0ignored', IDYDB_CREATE), RangeError);
  assert.throws(() => db.open(path.join(directory, 'missing.db'), 0), /IdyDB error/);
  assert.throws(() => db.insertInt(1, 1, 4), /not open/);
  db.open(filename, IDYDB_CREATE);
  assert.throws(() => db.open(path.join(directory, 'other.db'), IDYDB_CREATE), /already open/);
  assert.equal(fs.existsSync(path.join(directory, 'other.db')), false);
  assert.equal(db.columnNextRow(1), 1);

  db.ragUpsertText(1, 2, 1, 'first text', query);
  db.ragUpsertText(1, 2, 2, 'second text', new Float32Array([0, 1]));
  db.ragUpsertText(1, 2, 3, 'third text', new Float32Array([-1, 0]));
  db.setRowsIncluded(3, [1, 2], true);
  db.insertBool(3, 3, false);
  db.insertConstChar(4, 1, 'alpha');
  db.insertConstChar(4, 2, 'beta');
  db.insertInt(5, 1, 42);
  db.insertConstChar(6, 1, 'metadata');
  db.insertBool(7, 1, true);
  assert.equal(db.columnNextRow(1), 4);

  const hits = () => db.ragQueryHitsIncludedOnly(1, 2, 3, 4, query, 3, IDYDB_SIM_COSINE, [2, 5, 6, 7, 8]);
  const first = hits();
  assert.deepEqual(first.map(hit => hit.row), [1, 2]);
  assert.equal(first[0].text, 'first text');
  assert.equal(first[0].score, 1);
  assert.equal(first[0].meta['5'], 42);
  assert.equal(first[0].meta['6'], 'metadata');
  assert.equal(first[0].meta['7'], true);
  assert.equal(first[0].meta['8'], null);
  assert.ok(first[0].meta['2'] instanceof Float32Array);
  assert.deepEqual([...first[0].meta['2']], [1, 0]);
  first[0].meta['2'][0] = 99;
  assert.deepEqual([...hits()[0].meta['2']], [1, 0], 'returned vectors must own their storage');

  const relation = db.ragQueryHitsIncludedOnly(1, 2, 3, 4, query, 3, IDYDB_SIM_L2, [], 'beta');
  assert.deepEqual(relation.map(hit => hit.row), [2]);
  assert.ok(Math.abs(relation[0].score + Math.SQRT2) < 1e-6);
  assert.equal(db.ragQueryContextIncludedOnly(1, 2, 3, query, 3, IDYDB_SIM_COSINE, 0), 'first text\n---\nsecond text');
  assert.equal(db.ragQueryContext(1, 2, query, 1, IDYDB_SIM_COSINE, 5), 'first');
  assert.equal(db.ragQueryContext(1, 12, query, 1, IDYDB_SIM_COSINE, 0), '');
  assert.deepEqual(db.ragQueryHitsIncludedOnly(1, 2, 3, 4, query, 3, IDYDB_SIM_COSINE, [], 'missing'), []);

  // Previously these values were silently truncated/wrapped before native calls.
  for (const invalid of [0, -1, 1.5, NaN, Infinity, 65537, Number.MAX_SAFE_INTEGER]) {
    assert.throws(() => db.insertInt(1, invalid, 7), RangeError);
    assert.throws(() => db.columnNextRow(invalid), RangeError);
  }
  assert.throws(() => db.insertInt('1', 1, 7), TypeError);
  assert.throws(() => db.insertInt(1, 1, 2147483648), RangeError);
  assert.throws(() => db.insertBool(1, 1, 1), TypeError);
  assert.throws(() => db.insertConstChar(1, 1, 'abc\0def'), RangeError);
  assert.throws(() => db.insertConstChar(1, 1, 'x'.repeat(65533)), RangeError);
  assert.throws(() => db.insertConstChar(1, 1, 'é'.repeat(32767)), RangeError);
  assert.throws(() => db.ragUpsertText(1, 1, 1, 'changed', query), RangeError);
  for (const invalid of [[], new Float64Array([1, 0]), {}, null]) {
    assert.throws(() => db.ragUpsertText(1, 2, 1, 'changed', invalid), TypeError);
  }
  for (const invalid of [new Float32Array(0), new Float32Array(16384), new Float32Array(65537), new Float32Array([NaN]), new Float32Array([Infinity])]) {
    assert.throws(() => db.ragUpsertText(1, 2, 1, 'changed', invalid), RangeError);
  }
  for (const invalid of [0, -1, 1.5, 65536, 65537, Infinity]) {
    assert.throws(() => db.ragQueryContext(1, 2, query, invalid, IDYDB_SIM_COSINE, 0), RangeError);
  }
  assert.throws(() => db.ragQueryContext(1, 2, query, 1, 3, 0), RangeError);
  assert.throws(() => db.ragQueryContext(1, 2, query, 1, IDYDB_SIM_COSINE, -1), RangeError);
  assert.throws(() => db.ragQueryContext(1, 2, query, 1, IDYDB_SIM_COSINE, 0.5), RangeError);
  assert.throws(() => db.ragQueryHitsIncludedOnly(1, 2, 3, 4, query, 3, IDYDB_SIM_COSINE, [5, 'invalid']), TypeError);
  assert.throws(() => db.ragQueryHitsIncludedOnly(1, 2, 3, 4, query, 3, IDYDB_SIM_COSINE, [], 7), TypeError);
  assert.throws(() => db.setRowsIncluded(3, [1, 0], false), RangeError);
  assert.throws(() => db.setRowsIncluded(3, [1, 'invalid'], false), TypeError);
  const getterRows = [1];
  Object.defineProperty(getterRows, '1', { get() { throw new Error('getter failure'); } });
  assert.throws(() => db.setRowsIncluded(3, getterRows, false), /getter failure/);
  assert.equal(hits()[0].text, 'first text', 'invalid inputs must not replace text');
  assert.deepEqual(hits().map(hit => hit.row), [1, 2], 'invalid batches must not partially change inclusion');

  db.ragUpsertText(1, 2, 65536, 'last row', query);
  db.insertBool(3, 65536, true);
  db.insertConstChar(4, 65536, 'last');
  assert.throws(() => db.columnNextRow(1), /columnNextRow/);
  assert.deepEqual(db.ragQueryHitsIncludedOnly(1, 2, 3, 4, query, 3, IDYDB_SIM_COSINE, [], 'last').map(hit => hit.row), [65536]);
  db.deleteCell(1, 65536);
  db.deleteCell(2, 65536);
  assert.equal(db.columnNextRow(1), 4);

  const competitor = handle();
  db.flush();
  assert.equal(db.columnNextRow(1), 4, 'flush must leave the database open');
  assert.throws(() => competitor.open(filename, 0), /IdyDB error/);
  assert.throws(() => competitor.columnNextRow(1), /not open/);
  db.close();
  db.close();
  competitor.open(filename, IDYDB_READONLY);
  competitor.flush();
  assert.equal(competitor.ragQueryContext(1, 2, query, 1, IDYDB_SIM_COSINE, 0), 'first text');
  assert.throws(() => competitor.insertInt(5, 1, 99), /IdyDB error/);
  competitor.close();

  db.open(filename, 0);
  db.deleteCell(1, 3);
  db.deleteCell(2, 3);
  assert.equal(db.columnNextRow(1), 3);
  db.discard();
  db.discard();
  assert.throws(() => db.columnNextRow(1), /not open/);
  assert.throws(() => db.flush(), /not open/);
  competitor.open(filename, 0);
  assert.equal(competitor.columnNextRow(1), 3, 'discard must retain completed plaintext commits');
  assert.equal(competitor.ragQueryContext(1, 2, query, 1, IDYDB_SIM_COSINE, 0), 'first text');
  competitor.close();
  db.open(filename, 0);
  assert.equal(db.columnNextRow(1), 3, 'a discarded wrapper can reopen');
  db.close();

  // GC must release the native lock even if a caller forgets explicit close.
  assert.equal(typeof global.gc, 'function', 'run tests using node --expose-gc');
  const orphanPath = path.join(directory, 'orphan.db');
  (() => {
    const orphan = new IdyDb();
    orphan.open(orphanPath, IDYDB_CREATE);
    orphan.insertConstChar(1, 1, 'persisted');
  })();
  const reopened = handle();
  let collected = false;
  for (let i = 0; i < 100; ++i) {
    global.gc();
    await nextTurn();
    try {
      reopened.open(orphanPath, 0);
      collected = true;
      break;
    } catch (error) {
      assert.match(error.message, /IdyDB error/);
    }
  }
  assert.ok(collected, 'native handle must close when its JS wrapper is collected');
  assert.equal(reopened.columnNextRow(1), 2);
  reopened.close();
}

run().then(() => {
  console.log('IdyDB addon regression tests passed');
}).catch(error => {
  console.error(error);
  process.exitCode = 1;
}).finally(() => {
  for (const db of handles) db.discard();
  fs.rmSync(directory, { recursive: true, force: true });
});
