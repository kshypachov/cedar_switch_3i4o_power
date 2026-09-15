import { createHash } from 'node:crypto';

import { describe, expect, it } from 'vitest';

import { HASH_SLICE_BYTES, Sha256, hashBlob, sha256Hex, toHex } from './sha256';

const ascii = (s: string) => new TextEncoder().encode(s);
const node = (data: Uint8Array) => createHash('sha256').update(data).digest('hex');

/** Deterministic bytes with every value, so padding and word order are exercised. */
function pattern(length: number, seed = 1): Uint8Array<ArrayBuffer> {
  const out = new Uint8Array(length);
  let x = seed;
  for (let i = 0; i < length; i++) {
    x = (x * 1103515245 + 12345) >>> 0;
    out[i] = x >>> 24;
  }
  return out;
}

describe('SHA-256', () => {
  // FIPS 180-4 examples (NIST CSRC "SHA256.pdf") and the empty message.
  it.each([
    ['', 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'],
    ['abc', 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'],
    [
      'abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq',
      '248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1',
    ],
    [
      'abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu',
      'cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1',
    ],
  ])('NIST vector %j', (message, digest) => {
    expect(sha256Hex(ascii(message))).toBe(digest);
  });

  it('a million "a"', () => {
    const data = new Uint8Array(1_000_000).fill(0x61);
    expect(sha256Hex(data)).toBe('cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0');
    expect(sha256Hex(data)).toBe(node(data));
  });

  it.each([0, 1, 55, 56, 57, 63, 64, 65, 119, 120, 127, 128, 1000])('agrees with Node for %i bytes', (n) => {
    const data = pattern(n, n + 7);
    expect(sha256Hex(data)).toBe(node(data));
  });

  it('gives the same digest however the input is split', () => {
    const data = pattern(4096, 3);
    const whole = sha256Hex(data);
    for (const step of [1, 7, 63, 64, 65, 1000]) {
      const h = new Sha256();
      for (let i = 0; i < data.length; i += step) h.update(data.subarray(i, i + step));
      expect(toHex(h.digest()), `pieces of ${step}`).toBe(whole);
    }
  });

  it('takes the digest once', () => {
    const h = new Sha256().update(ascii('abc'));
    h.digest();
    expect(() => h.digest()).toThrow();
    expect(() => h.update(ascii('x'))).toThrow();
  });

  it('hashes a 1.4 MB image from a Blob in slices, reporting progress', async () => {
    const data = pattern(1_468_176, 11); // the merged Wi-Fi CP image's size
    const progress: number[] = [];
    const digest = await hashBlob(new Blob([data]), { onProgress: (done) => progress.push(done) });
    expect(digest).toBe(node(data));
    expect(progress).toHaveLength(Math.ceil(data.length / HASH_SLICE_BYTES));
    expect(progress.at(-1)).toBe(data.length);
    expect(progress).toEqual([...progress].sort((a, b) => a - b));
  });

  it('stops at the next slice when asked, without reading the rest', async () => {
    const controller = new AbortController();
    let reports = 0;
    const pending = hashBlob(new Blob([pattern(3 * HASH_SLICE_BYTES)]), {
      signal: controller.signal,
      onProgress: () => {
        reports++;
        controller.abort();
      },
    });
    await expect(pending).rejects.toBeDefined();
    expect(reports).toBe(1);
  });

  it('lets the page run between slices', async () => {
    let ticks = 0;
    const timer = setInterval(() => ticks++, 0);
    let ticksAtFirstSlice = -1;
    await hashBlob(new Blob([pattern(4 * HASH_SLICE_BYTES)]), {
      onProgress: (done) => {
        if (done === HASH_SLICE_BYTES) ticksAtFirstSlice = ticks;
      },
    });
    const ticksAtEnd = ticks;
    clearInterval(timer);
    expect(ticksAtEnd).toBeGreaterThan(ticksAtFirstSlice);
  });
});
