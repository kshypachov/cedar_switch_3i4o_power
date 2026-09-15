/**
 * SHA-256 (FIPS 180-4) in plain TypeScript.
 *
 * The page cannot use crypto.subtle: browsers expose it only in a secure
 * context, and the device is reached over plain HTTP by its address (plan
 * section 9). The digest is what createUpload declares and the device
 * recomputes, so it has to be exact; the tests check the NIST vectors and a
 * multi-megabyte input against Node's implementation.
 */

const K = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]);

const INITIAL = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19];

export class Sha256 {
  private readonly h = new Uint32Array(INITIAL);
  private readonly block = new Uint8Array(64);
  private readonly w = new Uint32Array(64);
  private used = 0;
  private length = 0; // bytes; a Number is exact far beyond any file this page hashes
  private finished = false;

  update(data: Uint8Array): this {
    if (this.finished) throw new Error('digest already taken');
    let i = 0;
    this.length += data.length;
    if (this.used > 0) {
      const take = Math.min(64 - this.used, data.length);
      this.block.set(data.subarray(0, take), this.used);
      this.used += take;
      i = take;
      if (this.used === 64) {
        this.compress(this.block, 0);
        this.used = 0;
      }
    }
    for (; i + 64 <= data.length; i += 64) this.compress(data, i);
    if (i < data.length) {
      this.block.set(data.subarray(i), 0);
      this.used = data.length - i;
    }
    return this;
  }

  digest(): Uint8Array {
    if (this.finished) throw new Error('digest already taken');
    const bits = this.length * 8;
    const pad = new Uint8Array((this.used < 56 ? 56 : 120) - this.used + 8);
    pad[0] = 0x80;
    const view = new DataView(pad.buffer);
    view.setUint32(pad.length - 8, Math.floor(bits / 0x100000000));
    view.setUint32(pad.length - 4, bits >>> 0);
    this.length -= pad.length; // padding is not message length
    this.update(pad);
    this.finished = true;
    const out = new Uint8Array(32);
    const outView = new DataView(out.buffer);
    for (let j = 0; j < 8; j++) outView.setUint32(j * 4, this.h[j]!);
    return out;
  }

  private compress(data: Uint8Array, at: number): void {
    const w = this.w;
    for (let t = 0; t < 16; t++) {
      const p = at + t * 4;
      w[t] = ((data[p]! << 24) | (data[p + 1]! << 16) | (data[p + 2]! << 8) | data[p + 3]!) >>> 0;
    }
    for (let t = 16; t < 64; t++) {
      const x = w[t - 15]!;
      const y = w[t - 2]!;
      const s0 = ((x >>> 7) | (x << 25)) ^ ((x >>> 18) | (x << 14)) ^ (x >>> 3);
      const s1 = ((y >>> 17) | (y << 15)) ^ ((y >>> 19) | (y << 13)) ^ (y >>> 10);
      w[t] = (s1 + w[t - 7]! + s0 + w[t - 16]!) >>> 0;
    }
    let a = this.h[0]!;
    let b = this.h[1]!;
    let c = this.h[2]!;
    let d = this.h[3]!;
    let e = this.h[4]!;
    let f = this.h[5]!;
    let g = this.h[6]!;
    let hh = this.h[7]!;
    for (let t = 0; t < 64; t++) {
      const ch = (e & f) ^ (~e & g);
      const maj = (a & b) ^ (a & c) ^ (b & c);
      const sigmaE = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
      const sigmaA = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
      const t1 = (hh + sigmaE + ch + K[t]! + w[t]!) >>> 0;
      const t2 = (sigmaA + maj) >>> 0;
      hh = g;
      g = f;
      f = e;
      e = (d + t1) >>> 0;
      d = c;
      c = b;
      b = a;
      a = (t1 + t2) >>> 0;
    }
    this.h[0] = (this.h[0]! + a) >>> 0;
    this.h[1] = (this.h[1]! + b) >>> 0;
    this.h[2] = (this.h[2]! + c) >>> 0;
    this.h[3] = (this.h[3]! + d) >>> 0;
    this.h[4] = (this.h[4]! + e) >>> 0;
    this.h[5] = (this.h[5]! + f) >>> 0;
    this.h[6] = (this.h[6]! + g) >>> 0;
    this.h[7] = (this.h[7]! + hh) >>> 0;
  }
}

export function toHex(bytes: Uint8Array): string {
  return Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
}

export function sha256Hex(data: Uint8Array): string {
  return toHex(new Sha256().update(data).digest());
}

/** Bytes hashed between yields: small enough that the page stays responsive. */
export const HASH_SLICE_BYTES = 256 * 1024;

/**
 * The SHA-256 of a file, read slice by slice, yielding to the event loop after
 * each so typing and polling go on while a 1.5 MB image is hashed.
 */
export async function hashBlob(
  blob: Blob,
  options: { onProgress?: (done: number, total: number) => void; signal?: AbortSignal; sliceBytes?: number } = {},
): Promise<string> {
  const slice = options.sliceBytes ?? HASH_SLICE_BYTES;
  const hash = new Sha256();
  for (let offset = 0; offset < blob.size; offset += slice) {
    options.signal?.throwIfAborted();
    const bytes = new Uint8Array(await blob.slice(offset, offset + slice).arrayBuffer());
    hash.update(bytes);
    options.onProgress?.(Math.min(offset + slice, blob.size), blob.size);
    await new Promise((resolve) => setTimeout(resolve, 0));
  }
  options.signal?.throwIfAborted();
  return toHex(hash.digest());
}
