// Stamp dist/ with the version the firmware reports as frontend_version, and
// print what the device will actually store. tools/web-assets/gen_web_assets.py
// reads the stamp; its DEPENDS on this file is what makes the firmware build
// pick up a new frontend.
import { execFileSync } from 'node:child_process';
import { readFileSync, readdirSync, statSync, writeFileSync } from 'node:fs';
import { join, relative } from 'node:path';
import { gzipSync } from 'node:zlib';

const root = new URL('..', import.meta.url).pathname;
const dist = join(root, 'dist');
const pkg = JSON.parse(readFileSync(join(root, 'package.json'), 'utf8'));

let revision = 'unknown';
try {
  revision = execFileSync('git', ['describe', '--always', '--dirty', '--abbrev=12'], {
    cwd: root,
    encoding: 'utf8',
  }).trim();
} catch {
  // Not a checkout: the version alone still identifies the build.
}

function walk(dir) {
  return readdirSync(dir).flatMap((name) => {
    const path = join(dir, name);
    if (name.startsWith('.')) return [];
    return statSync(path).isDirectory() ? walk(path) : [path];
  });
}

let raw = 0;
let gzipped = 0;
for (const file of walk(dist)) {
  const bytes = readFileSync(file);
  raw += bytes.length;
  gzipped += gzipSync(bytes, { level: 9 }).length;
  console.log(`  ${relative(dist, file).padEnd(40)} ${bytes.length}`);
}

const version = `${pkg.version}+${revision}`;
writeFileSync(join(dist, '.cedar-build.json'), `${JSON.stringify({ version }, null, 2)}\n`);
console.log(`dist: ${raw} bytes, ${gzipped} bytes gzipped (budget 524288), version ${version}`);
if (gzipped > 512 * 1024) {
  console.error('over the 512 KiB gzip budget of plan section 4');
  process.exit(1);
}
