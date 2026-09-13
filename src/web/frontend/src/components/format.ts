import { t } from '../i18n';

/** "2 д 3 ч", "5 мин 7 с": the two largest units of a duration. */
export function formatDuration(totalSeconds: number): string {
  const s = Math.max(0, Math.floor(totalSeconds));
  const units: [number, 'duration.days' | 'duration.hours' | 'duration.minutes' | 'duration.seconds'][] = [
    [Math.floor(s / 86400), 'duration.days'],
    [Math.floor((s % 86400) / 3600), 'duration.hours'],
    [Math.floor((s % 3600) / 60), 'duration.minutes'],
    [s % 60, 'duration.seconds'],
  ];
  const first = units.findIndex(([n]) => n > 0);
  if (first < 0) return t('duration.seconds', { n: 0 });
  return units
    .slice(first, first + 2)
    .filter(([n], i) => i === 0 || n > 0)
    .map(([n, key]) => t(key, { n }))
    .join(' ');
}

/** uptime_ms arrives as a decimal string so it never loses precision. */
export function uptimeSeconds(uptimeMs: string): number {
  return Number(BigInt(uptimeMs) / 1000n);
}

/** Code points, as the schemas count minLength and maxLength. */
export function codePoints(text: string): number {
  return [...text].length;
}
