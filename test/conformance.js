/*
Runs the language-neutral conformance suite in `conformance/` through `build/soml-tagged`. The suite is a copy of the one in the `soml` spec repository, updated with `../soml/sync-conformance.sh test/conformance`.

- `valid/**\/name.soml` must parse to the tagged value in `name.json`, and serialize with `soml_write_canonical` to exactly `name.canonical.soml`.
- `valid/**\/name.formatted.soml` is the expected output of a formatter. soml.h has no formatter, so it is not checked, and like `name.canonical.soml` it is not a case.
- `invalid/**\/name.soml` must be rejected, at the line and column the reference implementation reports (`conformance/invalid-reasons.json`). The specification does not fix positions, but they match, and this keeps them matching.
- `edit/name.json` is a case for a document editor. soml.h has no editor, so these are not run.

Run it with `make test`, which builds the tool first.
*/
import {test, suite} from 'node:test';
import assert from 'node:assert/strict';
import {execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

const root = path.join(import.meta.dirname, 'conformance');
const tool = process.env.SOML_TAGGED ?? path.join(import.meta.dirname, '..', 'build', 'soml-tagged');

function listCases(directory) {
	return fs.readdirSync(path.join(root, directory), {recursive: true, encoding: 'utf8'})
		.filter(file => file.endsWith('.soml') && !file.endsWith('.canonical.soml') && !file.endsWith('.formatted.soml'))
		.map(file => file.slice(0, -'.soml'.length).replaceAll(path.sep, '/'))
		.toSorted();
}

function runTool(files) {
	const output = execFileSync(tool, ['-'], {input: files.join('\n'), maxBuffer: 1024 * 1024 * 1024, encoding: 'utf8'});
	const results = new Map();

	for (const line of output.split('\n')) {
		if (line !== '') {
			const result = JSON.parse(line);
			results.set(result.file, result);
		}
	}

	return results;
}

/*
Object members compare without order, as the specification says, by sorting the keys of every object.
*/
function normalize(value) {
	if (Array.isArray(value)) {
		return value.map(item => normalize(item));
	}

	if (value !== null && typeof value === 'object') {
		return Object.fromEntries(Object.keys(value).toSorted().map(key => [key, normalize(value[key])]));
	}

	return value;
}

const validCases = listCases('valid');
const invalidCases = listCases('invalid');
const validResults = runTool(validCases.map(name => path.join(root, 'valid', `${name}.soml`)));
const invalidResults = runTool(invalidCases.map(name => path.join(root, 'invalid', `${name}.soml`)));
const reasons = JSON.parse(fs.readFileSync(path.join(root, 'invalid-reasons.json'), 'utf8'));

test('the suite is there', () => {
	assert.ok(validCases.length > 300);
	assert.ok(invalidCases.length > 300);
});

suite('valid', () => {
	for (const name of validCases) {
		test(name, () => {
			const result = validResults.get(path.join(root, 'valid', `${name}.soml`));
			assert.ok(result.ok, `Rejected: ${result.message} at ${result.line}:${result.column}`);
			const expected = JSON.parse(fs.readFileSync(path.join(root, 'valid', `${name}.json`), 'utf8'));
			assert.deepEqual(normalize(result.value), normalize(expected));
			assert.equal(result.canonical, fs.readFileSync(path.join(root, 'valid', `${name}.canonical.soml`), 'utf8'));
		});
	}
});

suite('invalid', () => {
	for (const name of invalidCases) {
		test(name, () => {
			const result = invalidResults.get(path.join(root, 'invalid', `${name}.soml`));
			assert.equal(result.ok, false, `Accepted: ${JSON.stringify(result.value)}`);
			assert.ok(result.code > 0);
		});
	}
});

test('error positions match the reference implementation', () => {
	const differences = [];

	for (const name of invalidCases) {
		const result = invalidResults.get(path.join(root, 'invalid', `${name}.soml`));
		const reason = reasons[name];

		if (reason === undefined) {
			differences.push(`${name}: no entry in conformance/invalid-reasons.json`);
		} else if (reason.line !== result.line || reason.column !== result.column) {
			differences.push(`${name}: ${result.line}:${result.column}, reference ${reason.line}:${reason.column}`);
		}
	}

	assert.deepEqual(differences, []);
});
