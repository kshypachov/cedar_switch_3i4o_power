/**
 * What an update screen needs to pick up after a reload: the upload it was
 * sending and the install job it was following. Ids only - no secrets; the
 * device stays the authority, and a remembered id it no longer knows is dropped.
 *
 * Storage can be unavailable (private windows, blocked site data): every access
 * is guarded and the screen works without it, only without resuming.
 *
 * Each screen keeps its own entry (memoryAt): the ESP32 and STM32 screens can
 * each have an upload in mind without one forgetting the other's.
 */
export interface Remembered {
  uploadId: string | null;
  installJobId: string | null;
}

export interface Memory {
  recall(): Remembered;
  remember(change: Partial<Remembered>): void;
}

export const STORAGE_KEY = 'cedar.coprocessor-update';

const EMPTY: Remembered = { uploadId: null, installJobId: null };
const ID = /^[A-Za-z0-9_-]{1,64}$/;

export function memoryAt(storageKey: string): Memory {
  const recall = (): Remembered => {
    try {
      const raw = window.localStorage.getItem(storageKey);
      if (!raw) return EMPTY;
      const value = JSON.parse(raw) as Partial<Remembered>;
      return {
        uploadId: typeof value.uploadId === 'string' && ID.test(value.uploadId) ? value.uploadId : null,
        installJobId: typeof value.installJobId === 'string' && ID.test(value.installJobId) ? value.installJobId : null,
      };
    } catch {
      return EMPTY;
    }
  };
  const remember = (change: Partial<Remembered>): void => {
    try {
      const next = { ...recall(), ...change };
      if (!next.uploadId && !next.installJobId) {
        window.localStorage.removeItem(storageKey);
      } else {
        window.localStorage.setItem(storageKey, JSON.stringify(next));
      }
    } catch {
      // Not remembered: a reload starts over, nothing else breaks.
    }
  };
  return { recall, remember };
}

const coprocessor = memoryAt(STORAGE_KEY);

/** The ESP32 screen's entry. */
export const recall = coprocessor.recall;
export const remember = coprocessor.remember;
