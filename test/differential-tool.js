/*
Checks for the differential runner, which needs the same JavaScript reference checkout and Node.js 26 as `make differential`.
*/
import {test} from 'node:test';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

const runner = path.join(import.meta.dirname, 'differential.js');

for (const count of ['0', '-1', '0.5', 'NaN', 'Infinity', '9007199254740992']) {
	test(`rejects the invalid input count ${count}`, () => {
		const result = spawnSync(process.execPath, [runner, count], {encoding: 'utf8', timeout: 2000});
		assert.equal(result.error, undefined, count);
		assert.notEqual(result.status, 0, count);
		assert.match(result.stderr, /The input count must be a positive safe integer/, count);
	});
}

test('temporary inputs are removed if writing fails', () => {
	const preload = `
		import fs from 'node:fs';
		fs.writeFileSync = file => {
			console.error('input-file:' + file);
			throw new Error('Injected input write failure');
		};
	`;
	const result = spawnSync(process.execPath, ['--import', `data:text/javascript,${encodeURIComponent(preload)}`, runner, '1'], {encoding: 'utf8'});
	assert.notEqual(result.status, 0);
	assert.match(result.stderr, /Injected input write failure/);
	const file = /^input-file:(.+)$/m.exec(result.stderr)?.[1];
	assert.ok(file);
	assert.equal(fs.existsSync(path.dirname(file)), false, file);
});
