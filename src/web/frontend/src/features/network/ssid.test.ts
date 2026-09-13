import { describe, expect, it } from 'vitest';

import { base64ToBytes, bytesToBase64, decodeSsid, encodeSsid, SSID_MAX_BYTES, ssidByteLength } from './ssid';

describe('SSID bytes', () => {
  it('encodes a typed SSID as UTF-8 and counts its bytes, not its characters', () => {
    expect(encodeSsid('Cedar-Lab')).toBe('Q2VkYXItTGFi');
    expect(encodeSsid('Кедр')).toBe('0JrQtdC00YA=');
    expect(ssidByteLength(encodeSsid('Кедр'))).toBe(8);
    expect(ssidByteLength(encodeSsid('Кедр'.repeat(4)))).toBe(SSID_MAX_BYTES);
    expect(ssidByteLength(encodeSsid(`${'Кедр'.repeat(4)}a`))).toBe(33);
    expect(ssidByteLength('')).toBe(0);
  });

  it('shows bytes that are not UTF-8 as replacement characters, and keeps the bytes exact', () => {
    expect(decodeSsid('Q2X/ZA==')).toBe('Ce\uFFFDd');
    expect(bytesToBase64(base64ToBytes('Q2X/ZA==')!)).toBe('Q2X/ZA==');
    // Re-encoding the display text would not give the device its SSID back.
    expect(encodeSsid(decodeSsid('Q2X/ZA==')!)).not.toBe('Q2X/ZA==');
  });

  it('decodes an empty SSID to empty text and broken base64 to null', () => {
    expect(decodeSsid('')).toBe('');
    expect(decodeSsid('%%%')).toBeNull();
    expect(base64ToBytes('%%%')).toBeNull();
  });

  it('keeps a leading byte order mark as part of the name', () => {
    expect(decodeSsid(bytesToBase64(new Uint8Array([0xef, 0xbb, 0xbf, 0x41])))).toBe('\uFEFFA');
  });
});
