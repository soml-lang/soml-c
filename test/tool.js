/*
Native command-line tool I/O checks. Run with `make tool-test`. WebAssembly runs the language conformance suite instead, because its host may hide stdout errors from the guest.
*/
import {test} from 'node:test';
import assert from 'node:assert/strict';
import {execFileSync} from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const root = path.join(import.meta.dirname, 'conformance');
const tool = process.env.SOML_TAGGED ?? path.join(import.meta.dirname, '..', 'build', 'soml-tagged');

test('file read failures are not syntax errors', () => {
	const files = [root, path.join(root, 'does-not-exist.soml')];
	const output = execFileSync(tool, files, {encoding: 'utf8'});
	const results = output.trim().split('\n').map(line => JSON.parse(line));

	assert.deepEqual(results.map(result => result.file), files);

	for (const result of results) {
		assert.equal(result.ok, false);
		assert.equal(result.code, -1);
	}
});

test('the tool reports output failures', () => {
	const file = path.join(root, 'valid', 'array', 'empty-nested.soml');
	const descriptor = fs.openSync(file, 'r');

	try {
		assert.throws(() => execFileSync(tool, [file], {stdio: ['pipe', descriptor, 'pipe']}), error => error.status > 0);
	} finally {
		fs.closeSync(descriptor);
	}
});

test('the tool reports input failures', {skip: process.platform === 'win32'}, () => {
	// On POSIX, opening a directory succeeds, but reading it as a file fails.
	const descriptor = fs.openSync(root, 'r');

	try {
		assert.throws(() => execFileSync(tool, ['-'], {stdio: [descriptor, 'pipe', 'pipe']}), error => error.status > 0);
	} finally {
		fs.closeSync(descriptor);
	}
});

test('long path records stay whole and the last record needs no newline', () => {
	const file = path.join(root, 'valid', 'array', 'empty-nested.soml');
	const longPath = 'x'.repeat(5000);
	const output = execFileSync(tool, ['-'], {input: `\n${longPath}\n\n${file}`, encoding: 'utf8'});
	const results = output.trim().split('\n').map(line => JSON.parse(line));
	assert.deepEqual(results.map(result => result.file), [longPath, file]);
	assert.equal(results[0].code, -1);
	assert.equal(results[1].ok, true);
});

test('an embedded NUL cannot select the path before it', () => {
	const file = path.join(root, 'valid', 'array', 'empty-nested.soml');
	assert.throws(() => execFileSync(tool, ['-'], {input: `${file}\0ignored\n`, encoding: 'utf8'}), error => error.status > 0 && error.stdout === '');
});

test('tagging integers preserves their entire signed range', () => {
	using directory = fs.mkdtempDisposableSync(path.join(os.tmpdir(), 'tagged-integers-'));
	const file = path.join(directory.path, 'integers.soml');
	const values = ['-9223372036854775808', '-1', '0', '1', '9223372036854775807'];
	fs.writeFileSync(file, `[${values.join(',')}]`);
	const result = JSON.parse(execFileSync(tool, [file], {encoding: 'utf8'}));
	assert.equal(result.ok, true);
	assert.deepEqual(result.value, values.map(value => ({type: 'int', value})));
	assert.equal(result.canonical, `[\n${values.map(value => `\t${value}\n`).join('')}]\n`);
});
