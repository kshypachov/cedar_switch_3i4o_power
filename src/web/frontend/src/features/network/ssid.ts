/**
 * An SSID is 0-32 bytes that need not be UTF-8, which is why the contract
 * carries it as `ssid_base64`. The page keeps those bytes exactly: a network
 * picked from a scan or read from the configuration is sent back byte for byte,
 * and only an SSID the user typed is encoded, as UTF-8.
 */
export const SSID_MAX_BYTES = 32;

export function utf8Bytes(text: string): Uint8Array {
  return new TextEncoder().encode(text);
}

export function bytesToBase64(bytes: Uint8Array): string {
  let binary = '';
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary);
}

/** The bytes, or null when @p value is not base64. */
export function base64ToBytes(value: string): Uint8Array | null {
  try {
    return Uint8Array.from(atob(value), (c) => c.charCodeAt(0));
  } catch {
    return null;
  }
}

/** A typed SSID as the device takes it. */
export function encodeSsid(text: string): string {
  return bytesToBase64(utf8Bytes(text));
}

/**
 * Display text for exact SSID bytes: bytes that are not UTF-8 become U+FFFD, so
 * a name is always shown, never dropped and never guessed. Null when the base64
 * itself is broken.
 */
export function decodeSsid(base64: string): string | null {
  const bytes = base64ToBytes(base64);
  if (!bytes) return null;
  return new TextDecoder('utf-8', { fatal: false, ignoreBOM: true }).decode(bytes);
}

/** Decoded length in bytes, which is what the 32 limit counts. */
export function ssidByteLength(base64: string): number {
  return base64ToBytes(base64)?.length ?? 0;
}
