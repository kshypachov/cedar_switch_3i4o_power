import { t, tMaybe } from '../../i18n';

/** A job's, a transaction's or an interface's error code as text. */
export function reasonText(code: string | null | undefined): string {
  return (code && tMaybe(`error.${code}`)) || t('error.unknown', { code: code ?? '?' });
}
