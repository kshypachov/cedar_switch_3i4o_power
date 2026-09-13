import type { components } from '../../api/schema.gen';
import { codePoints } from '../../components/format';
import { t } from '../../i18n';

// The bounds the device enforces, checked before a request so a typo does not
// cost a round trip. The server remains the authority; its 422 is shown too.
export const PASSWORD_MIN = 12;
export const PASSWORD_MAX = 128;

// Compile-time guard that the schema still has the field these bounds belong to.
type _NewPassword = components['schemas']['SetupRequest']['password'];

export function newPasswordProblem(password: string, confirmation: string): string | null {
  const n = codePoints(password);
  if (n === 0) return t('password.required');
  if (n < PASSWORD_MIN) return t('password.too_short', { min: PASSWORD_MIN });
  if (n > PASSWORD_MAX) return t('password.too_long', { max: PASSWORD_MAX });
  if (password !== confirmation) return t('password.mismatch');
  return null;
}

export type { _NewPassword };
