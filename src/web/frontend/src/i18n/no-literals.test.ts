import { readdirSync, readFileSync, statSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import ts from 'typescript';
import { describe, expect, it } from 'vitest';

import { ru } from './ru';

// Plan section 4: strings live in the dictionary from the first screen. This
// walks every component with the TypeScript parser and fails on text written
// into markup or into the attributes a person reads.

const src = join(dirname(fileURLToPath(import.meta.url)), '..') + '/';
const READ_ATTRIBUTES = new Set(['placeholder', 'title', 'aria-label', 'alt', 'label']);
const LETTERS = /[A-Za-zА-Яа-яЁё]/;

function tsxFiles(dir: string): string[] {
  return readdirSync(dir).flatMap((name) => {
    const path = join(dir, name);
    if (statSync(path).isDirectory()) return name === 'test' ? [] : tsxFiles(path);
    return path.endsWith('.tsx') && !path.endsWith('.test.tsx') ? [path] : [];
  });
}

function literals(path: string): string[] {
  const source = ts.createSourceFile(path, readFileSync(path, 'utf8'), ts.ScriptTarget.Latest, true, ts.ScriptKind.TSX);
  const found: string[] = [];
  const visit = (node: ts.Node) => {
    if (ts.isJsxText(node) && LETTERS.test(node.text)) {
      found.push(node.text.trim());
    }
    // A dictionary key handed to a component that translates it is not text.
    if (ts.isJsxAttribute(node) && READ_ATTRIBUTES.has(node.name.getText()) && node.initializer && ts.isStringLiteral(node.initializer) && LETTERS.test(node.initializer.text) && !(node.initializer.text in ru)) {
      found.push(`${node.name.getText()}="${node.initializer.text}"`);
    }
    ts.forEachChild(node, visit);
  };
  visit(source);
  return found;
}

describe('components hold no text of their own', () => {
  const files = tsxFiles(src);

  it('found the components', () => {
    expect(files.length).toBeGreaterThan(5);
  });

  it.each(files.map((f) => [f.slice(src.length), f]))('%s', (_, file) => {
    expect(literals(file)).toEqual([]);
  });

  it('would catch one', () => {
    const probe = join(src, 'test', 'probe.tsx');
    const source = ts.createSourceFile(probe, 'export const X = () => <p title="Привет">Hello</p>;', ts.ScriptTarget.Latest, true, ts.ScriptKind.TSX);
    let hits = 0;
    const visit = (node: ts.Node) => {
      if (ts.isJsxText(node) && LETTERS.test(node.text)) hits++;
      if (ts.isJsxAttribute(node) && node.initializer && ts.isStringLiteral(node.initializer) && LETTERS.test(node.initializer.text)) hits++;
      ts.forEachChild(node, visit);
    };
    visit(source);
    expect(hits).toBe(2);
  });
});
