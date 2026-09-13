import { describe, expect, it } from 'vitest';

import { codePoints, formatDuration, uptimeSeconds } from './format';

describe('formatDuration', () => {
  it.each([
    [0, '0 с'],
    [59, '59 с'],
    [60, '1 мин'],
    [61, '1 мин 1 с'],
    [3600, '1 ч'],
    [3661, '1 ч 1 мин'],
    [90061, '1 д 1 ч'],
    [28800, '8 ч'],
  ])('%i s is "%s"', (seconds, text) => {
    expect(formatDuration(seconds)).toBe(text);
  });
});

describe('uptimeSeconds', () => {
  it('reads the decimal string without losing precision', () => {
    expect(uptimeSeconds('93784999')).toBe(93784);
    expect(uptimeSeconds('18446744073709551615')).toBe(18446744073709551);
  });
});

describe('codePoints', () => {
  it('counts what the schema counts', () => {
    expect(codePoints('пароль')).toBe(6);
    expect(codePoints('🔑🔑')).toBe(2);
    expect(codePoints('abc')).toBe(3);
  });
});
