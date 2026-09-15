/**
 * MCUboot image versions as the device writes them: `major.minor.revision+build`
 * (FirmwareImage.version, SystemFirmware.running.version). The build number is
 * part of the order, as MCUboot compares it when it is configured to.
 */
export interface ImageVersion {
  major: number;
  minor: number;
  revision: number;
  build: number;
}

const PATTERN = /^(\d{1,3})\.(\d{1,3})\.(\d{1,5})(?:\+(\d{1,10}))?$/;

export function parseVersion(text: string): ImageVersion | null {
  const m = PATTERN.exec(text.trim());
  if (!m) return null;
  const [major, minor, revision, build] = [m[1], m[2], m[3], m[4] ?? '0'].map(Number) as [number, number, number, number];
  if (major > 255 || minor > 255 || revision > 65535 || build > 0xffffffff) return null;
  return { major, minor, revision, build };
}

export type Comparison = 'newer' | 'same' | 'older' | 'unknown';

/** How the image @p candidate stands against the @p running firmware. */
export function compareVersions(candidate: string, running: string): Comparison {
  const a = parseVersion(candidate);
  const b = parseVersion(running);
  if (!a || !b) return 'unknown';
  for (const key of ['major', 'minor', 'revision', 'build'] as const) {
    if (a[key] !== b[key]) return a[key] > b[key] ? 'newer' : 'older';
  }
  return 'same';
}
