import type { ReactNode } from 'react';

import type { ErrorDetail } from '../../api/errors';
import type { CoprocessorStatus, FeatureAvailability } from '../../api/types';
import { Card, Facts } from '../../components/ui';
import { type MessageKey, t, tMaybe } from '../../i18n';

/** A reason string from the device, in words when it is one this page knows. */
export function reasonText(reason: string | null): string {
  if (!reason) return t('value.unknown');
  return tMaybe(`update.reason.${reason}`) ?? reason;
}

/** An error the device recorded: its code in words, and the device's own message. */
export function DetailText({ detail }: { detail: ErrorDetail }) {
  return (
    <>
      {tMaybe(`error.${detail.code}`) ?? t('error.unknown', { code: detail.code })}{' '}
      <span className="muted small">{t('update.device_message', { message: detail.message })}</span>
    </>
  );
}

function availability(feature: FeatureAvailability): string {
  return feature.available ? t('update.available') : t('update.unavailable', { reason: reasonText(feature.reason) });
}

export function StatusCard({ status }: { status: CoprocessorStatus }) {
  const last = status.last_update;
  const lastRows: [MessageKey, ReactNode][] = last
    ? [
        ['update.last_state', <span key="state" data-testid="last-update-state">{t(`update.summary.${last.state}`)}</span>],
        ['update.last_version', last.version ?? t('value.none')],
        ['update.last_job', <code key="job">{last.job_id}</code>],
      ]
    : [];
  if (last?.error) lastRows.push(['update.last_error', <DetailText key="error" detail={last.error} />]);
  return (
    <>
      <Card title="update.status">
        <Facts
          rows={[
            ['update.state', <span key="state" data-testid="coprocessor-state">{t(`coprocessor.${status.state}`)}</span>],
            ['update.firmware_version', <code key="version" data-testid="coprocessor-version">{status.firmware_version ?? t('value.unknown')}</code>],
            ['update.transport', status.transport_ready ? t('value.yes') : t('value.no')],
            ['update.uart_mode', <span key="uart" data-testid="coprocessor-uart">{t(`logs.uart_mode.${status.uart_mode}`)}</span>],
            ['update.generation', String(status.generation)],
            ['update.host_protocol', status.host_protocol ?? t('value.none')],
            ['update.layout', status.partition_layout_id ?? t('value.none')],
          ]}
        />
        <h3>{t('update.methods')}</h3>
        {/* OTA is a capability the device reports as missing, with its reason - not a switch that does nothing. */}
        <Facts
          rows={[
            ['update.method_uart', <span key="uart-update" data-testid="method-uart">{availability(status.uart_update)}</span>],
            ['update.method_ota', <span key="ota" data-testid="method-ota">{availability(status.ota)}</span>],
          ]}
        />
      </Card>
      <Card title="update.last">
        {last === null ? (
          <p className="muted">{t('update.last_none')}</p>
        ) : (
          <div data-testid="last-update">
            <Facts rows={lastRows} />
            {last.recovery_required ? (
              <p className="notice notice-warning" role="note" data-testid="recovery-required">
                {t('update.recovery_required')}
              </p>
            ) : null}
          </div>
        )}
      </Card>
    </>
  );
}
