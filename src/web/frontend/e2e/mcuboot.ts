import { createHash } from 'node:crypto';

/**
 * A small image MCUboot (and the mock's check) takes as zephyr.signed.bin for
 * this board: a 0x400 header, a body that starts with a vector table pointing
 * into SRAM and the application window, and the TLV area with the SHA-256 of
 * header and body. Nothing here is signed - the board runs without signatures.
 */
export interface Version {
  major: number;
  minor: number;
  revision: number;
  build: number;
}

const HEADER_SIZE = 0x400;
const IMAGE_MAGIC = 0x96f3b83d;
const TLV_INFO_MAGIC = 0x6907;
const TLV_SHA256 = 0x10;

export function mcubootImage(version: Version, bodySize = 60_000): Buffer {
  const header = Buffer.alloc(HEADER_SIZE, 0xff);
  header.writeUInt32LE(IMAGE_MAGIC, 0);
  header.writeUInt32LE(0, 4); // ih_load_addr
  header.writeUInt16LE(HEADER_SIZE, 8); // ih_hdr_size
  header.writeUInt16LE(0, 10); // ih_protect_tlv_size
  header.writeUInt32LE(bodySize, 12); // ih_img_size
  header.writeUInt32LE(0, 16); // ih_flags
  header.writeUInt8(version.major, 20);
  header.writeUInt8(version.minor, 21);
  header.writeUInt16LE(version.revision, 22);
  header.writeUInt32LE(version.build, 24);
  header.writeUInt32LE(0, 28);

  const body = Buffer.alloc(bodySize);
  for (let i = 0; i < bodySize; i++) body[i] = (i * 29 + version.minor) & 0xff;
  body.writeUInt32LE(0x2000_1000, 0); // initial stack pointer: SRAM
  body.writeUInt32LE(0x0200_0401, 4); // reset vector: application window, Thumb

  const digest = createHash('sha256').update(header).update(body).digest();
  const tlv = Buffer.alloc(4 + 4 + digest.length);
  tlv.writeUInt16LE(TLV_INFO_MAGIC, 0);
  tlv.writeUInt16LE(tlv.length, 2);
  tlv.writeUInt16LE(TLV_SHA256, 4);
  tlv.writeUInt16LE(digest.length, 6);
  digest.copy(tlv, 8);
  return Buffer.concat([header, body, tlv]);
}

export const versionText = (v: Version) => `${v.major}.${v.minor}.${v.revision}+${v.build}`;
