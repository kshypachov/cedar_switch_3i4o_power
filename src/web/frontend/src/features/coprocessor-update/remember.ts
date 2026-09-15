/**
 * What the ESP32 screen needs to pick up after a reload: the upload it was
 * sending and the install job it was following. Ids only - no secrets; the
 * device stays the authority, and a remembered id it no longer knows is dropped.
 *
 * Storage can be unavailable (private windows, blocked site data): every access
 * is guarded and the screen works without it, only without resuming.
 */
export interface Remembered {
  uploadId: string | null;
  installJobId: string | null;
}

export const STORAGE_KEY = 'cedar.coprocessor-update';

const EMPTY: Remembered = { uploadId: null, installJobId: null };
const ID = /^[A-Za-z0-9_-]{1,64}$/;

export function recall(): Remembered {
  try {
    const raw = window.localStorage.getItem(STORAGE_KEY);
    if (!raw) return EMPTY;
    const value = JSON.parse(raw) as Partial<Remembered>;
    return {
      uploadId: typeof value.uploadId === 'string' && ID.test(value.uploadId) ? value.uploadId : null,
      installJobId: typeof value.installJobId === 'string' && ID.test(value.installJobId) ? value.installJobId : null,
    };
  } catch {
    return EMPTY;
  }
}

export function remember(change: Partial<Remembered>): void {
  try {
    const next = { ...recall(), ...change };
    if (!next.uploadId && !next.installJobId) {
      window.localStorage.removeItem(STORAGE_KEY);
    } else {
      window.localStorage.setItem(STORAGE_KEY, JSON.stringify(next));
    }
  } catch {
    // Not remembered: a reload starts over, nothing else breaks.
  }
}
