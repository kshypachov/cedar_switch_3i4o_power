import { describe, expect, it } from 'vitest';

import { t } from '../../i18n';
import { newPasswordProblem, PASSWORD_MAX, PASSWORD_MIN } from './passwordPolicy';

const chars = (n: number, unit = 'a') => unit.repeat(n);

describe('newPasswordProblem', () => {
  it('matches the schema bounds exactly', () => {
    expect(PASSWORD_MIN).toBe(12);
    expect(PASSWORD_MAX).toBe(128);
    expect(newPasswordProblem(chars(11), chars(11))).toBe(t('password.too_short', { min: 12 }));
    expect(newPasswordProblem(chars(12), chars(12))).toBeNull();
    expect(newPasswordProblem(chars(128), chars(128))).toBeNull();
    expect(newPasswordProblem(chars(129), chars(129))).toBe(t('password.too_long', { max: 128 }));
  });

  it('counts code points, as the schema does', () => {
    // 11 four-byte characters are 22 UTF-16 units but 11 code points.
    expect(newPasswordProblem(chars(11, '🔑'), chars(11, '🔑'))).toBe(t('password.too_short', { min: 12 }));
    expect(newPasswordProblem(chars(12, '🔑'), chars(12, '🔑'))).toBeNull();
    expect(newPasswordProblem(chars(128, 'я'), chars(128, 'я'))).toBeNull();
  });

  it('requires a password and a matching confirmation', () => {
    expect(newPasswordProblem('', '')).toBe(t('password.required'));
    expect(newPasswordProblem(chars(12), chars(12, 'b'))).toBe(t('password.mismatch'));
  });
});
