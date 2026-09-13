/** Timeouts the form offers; filtered to the device's published limits. */
export const TIMEOUT_CHOICES = [180, 300, 600, 900] as const;
export const DEFAULT_TIMEOUT = 300;

export function timeoutChoices(min: number, max: number): number[] {
  const choices = TIMEOUT_CHOICES.filter((s) => s >= min && s <= max);
  return choices.length ? choices : [min];
}

/**
 * Seconds left, counted down locally between polls from the last answer:
 * @p remaining as the device said it at @p receivedAt (ms), seen at @p now.
 */
export function remainingNow(remaining: number, receivedAt: number, now: number): number {
  return Math.max(0, remaining - Math.floor(Math.max(0, now - receivedAt) / 1000));
}
