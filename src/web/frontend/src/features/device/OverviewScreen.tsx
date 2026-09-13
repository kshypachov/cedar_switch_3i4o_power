import {
  getCoprocessorStatus,
  getJob,
  getMatterStatus,
  getNetworkStatus,
  getSystemStatus,
} from '../../api/device';
import type { InterfaceStatus } from '../../api/types';
import { formatDuration, uptimeSeconds } from '../../components/format';
import { Body } from '../../components/Polled';
import { Card, Facts } from '../../components/ui';
import { type MessageKey, t } from '../../i18n';
import { usePolling } from '../../state/usePolling';
import { notServed, useSessionGuard } from '../../state/useSessionGuard';

const STATUS_MS = 5_000;
const JOB_MS = 1_000;

const yesNo = (value: boolean) => t(value ? 'value.yes' : 'value.no');

function InterfaceFacts({ iface }: { iface: InterfaceStatus }) {
  const rows: [MessageKey, React.ReactNode][] = [
    ['overview.link', `${yesNo(iface.link_up)}, ${t(`interface.${iface.state}`)}`],
    [
      'overview.addresses',
      iface.addresses.length ? (
        <ul className="plain">
          {iface.addresses.map((a) => (
            <li key={a.address}>
              <code>
                {a.address}/{a.prefix_length}
              </code>
            </li>
          ))}
        </ul>
      ) : (
        t('value.none')
      ),
    ],
  ];
  if (iface.id === 'wifi') {
    // SSIDs are shown as text, never as markup (plan section 4).
    rows.push(['overview.ssid', iface.ssid ?? t('value.none')]);
    rows.push(['overview.rssi', iface.rssi_dbm === null ? t('value.none') : t('value.dbm', { value: iface.rssi_dbm })]);
  }
  return (
    <div className="subcard">
      <h3>{t(iface.id === 'ethernet' ? 'overview.ethernet' : 'overview.wifi')}</h3>
      <Facts rows={rows} />
    </div>
  );
}

function JobLine({ jobId }: { jobId: string }) {
  const job = usePolling((signal) => getJob(jobId, signal), JOB_MS, { stopOn: notServed });
  if (!job.data) return <li className="muted">{jobId}</li>;
  return (
    <li>
      {t(`job.kind.${job.data.kind}`)} — {t(`job.state.${job.data.state}`)}
    </li>
  );
}

export function OverviewScreen() {
  const status = usePolling(getSystemStatus, STATUS_MS);
  const network = usePolling(getNetworkStatus, STATUS_MS, { stopOn: notServed });
  const matter = usePolling(getMatterStatus, STATUS_MS, { stopOn: notServed });
  const coprocessor = usePolling(getCoprocessorStatus, STATUS_MS * 2, { stopOn: notServed });
  useSessionGuard(status, network, matter, coprocessor);

  return (
    <main>
      <h1>{t('overview.title')}</h1>
      <div className="grid">
        <Card title="overview.device">
          <Body
            polled={status}
            render={(s) => (
              <Facts
                rows={[
                  ['overview.model', s.model],
                  ['overview.device_id', <code key="id">{s.device_id}</code>],
                  ['overview.firmware', <code key="fw">{s.firmware_version}</code>],
                  ['overview.frontend', <code key="ui">{s.frontend_version}</code>],
                  ['overview.uptime', formatDuration(uptimeSeconds(s.uptime_ms))],
                  ['overview.access_address', <code key="addr">{window.location.host}</code>],
                ]}
              />
            )}
          />
        </Card>
        <Card title="overview.operations">
          <Body
            polled={status}
            render={(s) =>
              s.active_job_ids.length ? (
                <ul className="plain">
                  {s.active_job_ids.map((id) => (
                    <JobLine key={id} jobId={id} />
                  ))}
                </ul>
              ) : (
                <p className="muted">{t('overview.no_operations')}</p>
              )
            }
          />
        </Card>
        <Card title="overview.network">
          <Body
            polled={network}
            render={(n) => (
              <>
                {n.interfaces.map((iface) => (
                  <InterfaceFacts key={iface.id} iface={iface} />
                ))}
              </>
            )}
          />
        </Card>
        <Card title="overview.matter">
          <Body
            polled={matter}
            render={(m) => (
              <Facts
                rows={[
                  ['overview.matter_state', t(`matter.${m.state}`)],
                  ['overview.fabrics', String(m.fabric_count)],
                ]}
              />
            )}
          />
        </Card>
        <Card title="overview.coprocessor">
          <Body
            polled={coprocessor}
            render={(c) => (
              <Facts
                rows={[
                  ['overview.matter_state', t(`coprocessor.${c.state}`)],
                  ['overview.coprocessor_version', c.firmware_version ?? t('value.unknown')],
                  ['overview.transport', yesNo(c.transport_ready)],
                ]}
              />
            )}
          />
        </Card>
      </div>
    </main>
  );
}
