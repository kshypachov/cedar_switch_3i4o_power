import { describe, expect, it } from 'vitest';

import { remainingNow, timeoutChoices } from './window';

describe('the commissioning window form and timer', () => {
  it('offers only the timeouts the device allows', () => {
    expect(timeoutChoices(180, 900)).toEqual([180, 300, 600, 900]);
    expect(timeoutChoices(300, 600)).toEqual([300, 600]);
    expect(timeoutChoices(200, 250)).toEqual([200]);
  });

  it('counts down whole seconds from the last answer and stops at zero', () => {
    expect(remainingNow(300, 1_000, 1_000)).toBe(300);
    expect(remainingNow(300, 1_000, 1_999)).toBe(300);
    expect(remainingNow(300, 1_000, 2_000)).toBe(299);
    expect(remainingNow(10, 0, 60_000)).toBe(0);
    expect(remainingNow(10, 5_000, 0)).toBe(10);
  });
});
