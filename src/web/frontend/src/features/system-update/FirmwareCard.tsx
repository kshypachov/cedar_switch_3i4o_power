import { type ReactNode, useEffect, useState } from 'react';

import type { FeatureAvailability, SystemFirmware } from '../../api/types';
import { formatDuration } from '../../components/format';
import { Card, Facts } from '../../components/ui';
import { type MessageKey, t, tMaybe } from '../../i18n';
import { DetailText } from '../coprocessor-update/StatusCard';
import { remainingSeconds } from './install';

/** A reason string from the device, in words when it is one this page knows. */
export function systemReasonText(reason: string | null): string {
  if (!reason) return t('sysupd.reason.unknown');
  return tMaybe(`sysupd.reason.${reason}`) ?? tMaybe(`update.reason.${reason}`) ?? reason;
}

/** Why the update cannot be offered now, from the firmware and the capabilities; null when it can. */
export function unavailableReason(firmware: SystemFirmware | null, feature: FeatureAvailability | undefined): string | null {
  if (feature && !feature.available) return systemReasonText(feature.reason);
  if (firmware && !firmware.update.available) return systemReasonText(firmware.update.reason);
  return null;
}

/** The device's countdown, ticking down locally between polls. */
function useCountdown(reported: number | null, reportedAt: number): number | null {
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    if (reported === null) return;
    const timer = setInterval(() => setNow(Date.now()), 1_000);
    return () => clearInterval(timer);
  }, [reported]);
  return reported === null ? null : remainingSeconds(reported, reportedAt, Math.max(now, reportedAt));
}

export function FirmwareCard({ firmware }: { firmware: SystemFirmware }) {
  const [reportedAt, setReportedAt] = useState(() => Date.now());
  useEffect(() => setReportedAt(Date.now()), [firmware]);
  const remaining = useCountdown(firmware.running.confirmed ? null : firmware.confirm_remaining_seconds, reportedAt);

  const confirmation = firmware.running.confirmed
    ? t('sysupd.confirmed')
    : remaining === null
      ? t('sysupd.awaiting')
      : remaining > 0
        ? `${t('sysupd.awaiting')}, ${t('sysupd.confirm_in', { time: formatDuration(remaining) })}`
        : `${t('sysupd.awaiting')}, ${t('sysupd.confirm_now')}`;

  return (
    <Card title="sysupd.running">
      <Facts
        rows={[
          ['sysupd.version', <code key="version" data-testid="running-version">{firmware.running.version}</code>],
          ['sysupd.confirmation', <span key="confirmation" data-testid="running-confirmation">{confirmation}</span>],
          ['sysupd.image_hash', firmware.running.image_hash ? <code key="hash" className="small">{firmware.running.image_hash}</code> : t('value.unknown')],
        ]}
      />
      {firmware.running.confirmed ? null : (
        <p className="notice notice-warning" role="note" data-testid="awaiting-warning">
          {t('sysupd.awaiting_warning')}
        </p>
      )}
      {firmware.swap_pending ? (
        <p className="notice notice-warning" role="note" data-testid="swap-pending">
          {t('sysupd.swap_pending')}
        </p>
      ) : null}
    </Card>
  );
}

export function LastUpdateCard({ firmware }: { firmware: SystemFirmware }) {
  const last = firmware.last_update;
  if (!last) {
    return (
      <Card title="sysupd.last">
        <p className="muted">{t('sysupd.last_none')}</p>
      </Card>
    );
  }
  const rows: [MessageKey, ReactNode][] = [
    ['sysupd.last_state', <span key="state" data-testid="system-last-state">{t(`sysupd.summary.${last.state}`)}</span>],
    ['sysupd.last_from', last.from_version ?? t('value.none')],
    ['sysupd.last_to', last.version ?? t('value.none')],
    ['sysupd.last_job', <code key="job">{last.job_id}</code>],
  ];
  if (last.error) rows.push(['sysupd.last_error', <DetailText key="error" detail={last.error} />]);
  return (
    <Card title="sysupd.last">
      <div data-testid="system-last-update">
        <Facts rows={rows} />
      </div>
    </Card>
  );
}
