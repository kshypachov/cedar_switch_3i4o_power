// Response bodies for component tests, typed by the generated schema types so
// a shape the device cannot send does not compile, and checked against
// openapi.json itself in fixtures.test.ts for what types cannot say
// (patterns, formats, bounds).
import type {
  AuthState,
  Capabilities,
  CommissioningWindow,
  CoprocessorStatus,
  Fabrics,
  Job,
  JobAccepted,
  MatterStatus,
  NetworkConfigOutput,
  NetworkConfigResponse,
  NetworkStatus,
  NetworkTransaction,
  OnboardingCodes,
  ScanResults,
  Session,
  SystemStatus,
} from '../api/types';

export const authStateFresh: AuthState = {
  setup_required: true,
  setup_allowed: true,
  setup_token: 'Qm9vdFRva2VuX2Zvcl90ZXN0c18wMQ',
};

export const authStateConfigured: AuthState = {
  setup_required: false,
  setup_allowed: false,
  setup_token: null,
};

export const session: Session = {
  username: 'admin',
  csrf_token: 'Y3NyZi10b2tlbi1mb3ItdGVzdHMtMDEyMzQ1Njc4OWFi',
  idle_timeout_seconds: 1800,
  absolute_remaining_seconds: 28730,
};

export const systemStatus: SystemStatus = {
  device_id: 'cedar-0011aabbccdd',
  model: 'cedar_switch_3in4out_power',
  firmware_version: 'd30ae91-dirty',
  frontend_version: '0.1.0+d30ae91',
  boot_id: 'boot_0123456789abcdef',
  uptime_ms: '93784000',
  wall_time: null,
  active_job_ids: [],
};

export const networkStatus: NetworkStatus = {
  interfaces: [
    {
      id: 'ethernet',
      enabled: true,
      link_up: true,
      state: 'ready',
      mac_address: '80:34:28:10:12:73',
      addresses: [{ family: 'ipv4', address: '192.168.88.14', prefix_length: 24, source: 'dhcp' }],
      ssid: null,
      rssi_dbm: null,
      error: null,
    },
    {
      id: 'wifi',
      enabled: true,
      link_up: true,
      state: 'ready',
      mac_address: '80:34:28:10:12:74',
      addresses: [{ family: 'ipv4', address: '192.168.88.10', prefix_length: 24, source: 'dhcp' }],
      ssid: '<b>k2</b>',
      rssi_dbm: -57,
      error: null,
    },
  ],
  default_interface: 'ethernet',
  dns_servers: ['192.168.88.1'],
};

export const matterStatus: MatterStatus = {
  state: 'ready',
  commissioned: true,
  fabric_count: 2,
  error: null,
};

export const coprocessorStatus: CoprocessorStatus = {
  state: 'ready',
  chip: 'esp32c6',
  firmware_version: '2.3.1',
  host_protocol: '1',
  partition_layout_id: 'c6-4mb-ota2',
  transport_ready: true,
  uart_mode: 'console',
  generation: 1,
  ota: { available: false, reason: 'not_implemented' },
  uart_update: { available: false, reason: 'not_implemented' },
  last_update: null,
};

export const passwordAccepted: JobAccepted = {
  job_id: 'job_00000001',
  job_url: '/api/v1/jobs/job_00000001',
  resource_url: '/api/v1/auth/session',
};

export const passwordJobRunning: Job = {
  id: 'job_00000001',
  boot_id: 'boot_0123456789abcdef',
  kind: 'password_change',
  state: 'running',
  phase: 'hashing',
  progress: null,
  cancellable: false,
  created_uptime_ms: '100000',
  updated_uptime_ms: '100200',
  resource_url: '/api/v1/auth/session',
  error: null,
};


export const capabilities: Capabilities = {
  api_version: '1',
  features: {
    matter: { available: true, reason: null },
    esp32_logs: { available: false, reason: 'not_implemented' },
    esp32_ota: { available: false, reason: 'not_implemented' },
    esp32_uart: { available: false, reason: 'not_implemented' },
  },
  limits: {
    json_body_bytes: 8192,
    upload_chunk_bytes: 16384,
    upload_max_bytes: 2097152,
    log_page_records: 100,
    scan_records: 64,
    commissioning_min_seconds: 180,
    commissioning_max_seconds: 900,
    network_confirm_min_seconds: 60,
    network_confirm_max_seconds: 300,
  },
  wifi_security_modes: ['open', 'wpa2_psk', 'wpa3_sae'],
  firmware_formats: ['raw_app'],
  update_requires_ethernet: true,
};

export const windowClosed: CommissioningWindow = {
  open: false,
  mode: null,
  source: null,
  remaining_seconds: 0,
  codes_available: false,
};

export const windowOpenFromWeb: CommissioningWindow = {
  open: true,
  mode: 'basic',
  source: 'web',
  remaining_seconds: 297,
  codes_available: true,
};

export const windowOpenByController: CommissioningWindow = {
  open: true,
  mode: 'enhanced',
  source: 'controller',
  remaining_seconds: 0,
  codes_available: false,
};

/** Leading zeros on purpose: the page must keep them (plan section 6). */
export const onboardingCodes: OnboardingCodes = {
  available: true,
  reason: null,
  qr_payload: 'MT:Y.K9042C00KA0648G00',
  manual_pairing_code: '01234567890',
  setup_passcode: '00012345',
};

export const fabrics: Fabrics = {
  items: [
    {
      id: '9B02575873B12C99:0000000000000001',
      fabric_index: 1,
      fabric_id: '0000000000000001',
      node_id: '0000000000000001',
      vendor_id: 65521,
      label: '<b>lab</b>',
    },
    {
      id: '3700EC0D6DAB4145:0000000000000002',
      fabric_index: 2,
      fabric_id: '0000000000000002',
      node_id: '00000000000000A7',
      vendor_id: 4937,
      label: '',
    },
  ],
  count: 2,
};

export const matterOpenAccepted: JobAccepted = {
  job_id: 'job_00000002',
  job_url: '/api/v1/jobs/job_00000002',
  resource_url: '/api/v1/matter/commissioning',
};

export const matterOpenSucceeded: Job = {
  id: 'job_00000002',
  boot_id: 'boot_0123456789abcdef',
  kind: 'matter_open',
  state: 'succeeded',
  phase: 'opening',
  progress: null,
  cancellable: false,
  created_uptime_ms: '100000',
  updated_uptime_ms: '100300',
  resource_url: '/api/v1/matter/commissioning',
  error: null,
};

/** Both families and every address source the network screen labels. */
export const networkRuntime: NetworkStatus = {
  interfaces: [
    {
      id: 'ethernet',
      enabled: true,
      link_up: true,
      state: 'ready',
      mac_address: '80:34:28:10:12:73',
      addresses: [
        { family: 'ipv4', address: '192.168.88.14', prefix_length: 24, source: 'dhcp' },
        { family: 'ipv6', address: 'fe80::8234:28ff:fe10:1273', prefix_length: 64, source: 'link_local' },
        { family: 'ipv6', address: '2001:db8::14', prefix_length: 64, source: 'slaac' },
      ],
      ssid: null,
      rssi_dbm: null,
      error: null,
    },
    {
      id: 'wifi',
      enabled: true,
      link_up: true,
      state: 'ready',
      mac_address: '80:34:28:10:12:74',
      addresses: [{ family: 'ipv4', address: '192.168.88.10', prefix_length: 24, source: 'dhcp' }],
      ssid: '<b>k2</b>',
      rssi_dbm: -57,
      error: null,
    },
  ],
  default_interface: 'ethernet',
  dns_servers: ['192.168.88.1', '2001:db8::1'],
};

/** The coprocessor is not ready: Wi-Fi carries the reason even while it is disabled. */
export const networkRuntimeRadioAbsent: NetworkStatus = {
  ...networkRuntime,
  interfaces: [
    networkRuntime.interfaces[0]!,
    {
      id: 'wifi',
      enabled: false,
      link_up: false,
      state: 'disabled',
      mac_address: '80:34:28:10:12:74',
      addresses: [],
      ssid: null,
      rssi_dbm: null,
      error: {
        code: 'capability_unavailable',
        message: 'The Wi-Fi coprocessor is offline',
        request_id: 'req_00000009',
        retryable: false,
      },
    },
  ],
  dns_servers: ['192.168.88.1'],
};

const dhcp = { mode: 'dhcp', address: null, prefix_length: null, gateway: null } as const;

export const networkConfig: NetworkConfigResponse = {
  revision: 3,
  config: {
    preferred_interface: 'ethernet',
    dns: { mode: 'automatic', servers: [] },
    interfaces: {
      ethernet: { enabled: true, ipv4: dhcp },
      wifi: {
        enabled: true,
        ssid_base64: 'Q2VkYXItTGFi',
        security: 'wpa2_psk',
        hidden: false,
        ipv4: dhcp,
        password_set: true,
      },
    },
  },
  pending_transaction_id: null,
};

const staticCandidate: NetworkConfigOutput = {
  ...networkConfig.config,
  interfaces: {
    ...networkConfig.config.interfaces,
    ethernet: {
      enabled: true,
      ipv4: { mode: 'static', address: '192.168.88.50', prefix_length: 24, gateway: '192.168.88.1' },
    },
  },
};

export const txStaged: NetworkTransaction = {
  id: 'nettx_0001',
  boot_id: 'boot_0123456789abcdef',
  base_revision: 3,
  state: 'staged',
  candidate: staticCandidate,
  remaining_seconds: 287,
  reconnect_urls: [],
  job_id: null,
  error: null,
};

export const txAwaiting: NetworkTransaction = {
  ...txStaged,
  state: 'awaiting_confirmation',
  remaining_seconds: 37,
  reconnect_urls: ['http://192.168.88.50/'],
  job_id: 'job_00000010',
};

/** A DHCP change: the device cannot know the address, so it names none. */
export const txAwaitingDhcp: NetworkTransaction = {
  ...txAwaiting,
  candidate: networkConfig.config,
  reconnect_urls: [],
};

export const txCommitted: NetworkTransaction = {
  ...txAwaiting,
  state: 'committed',
  remaining_seconds: null,
  reconnect_urls: [],
};

export const txRolledBack: NetworkTransaction = { ...txCommitted, state: 'rolled_back' };

export const txTimedOut: NetworkTransaction = {
  ...txRolledBack,
  error: {
    code: 'resource_expired',
    message: 'Confirmation timed out; the previous configuration was restored',
    request_id: 'req_0000000a',
    retryable: false,
  },
};

export const networkApplyAccepted: JobAccepted = {
  job_id: 'job_00000010',
  job_url: '/api/v1/jobs/job_00000010',
  resource_url: '/api/v1/network/config',
};

const networkJob: Job = {
  id: 'job_00000010',
  boot_id: 'boot_0123456789abcdef',
  kind: 'network_apply',
  state: 'succeeded',
  phase: 'committing',
  progress: { completed: 2, total: 2, unit: 'steps' },
  cancellable: false,
  created_uptime_ms: '100000',
  updated_uptime_ms: '140000',
  resource_url: '/api/v1/network/config',
  error: null,
};

export const networkCommitSucceeded: Job = networkJob;
export const networkRollbackSucceeded: Job = { ...networkJob, phase: 'rolling_back' };

export const networkDiscardAccepted: JobAccepted = {
  job_id: 'job_00000011',
  job_url: '/api/v1/jobs/job_00000011',
  resource_url: '/api/v1/network/config',
};

export const networkDiscardSucceeded: Job = {
  ...networkJob,
  id: 'job_00000011',
  kind: 'network_discard',
  phase: 'discarding',
  progress: null,
};

export const scanAccepted: JobAccepted = {
  job_id: 'job_00000020',
  job_url: '/api/v1/jobs/job_00000020',
  resource_url: '/api/v1/network/wifi/scans/job_00000020',
};

export const scanSucceeded: Job = {
  ...networkJob,
  id: 'job_00000020',
  kind: 'wifi_scan',
  phase: 'scanning',
  progress: { completed: 11, total: 11, unit: 'steps' },
  resource_url: '/api/v1/network/wifi/scans/job_00000020',
};

/** Every awkward case at once: one SSID on two BSSIDs, hidden, enterprise, unknown, markup, bytes that are not UTF-8. */
export const scanResults: ScanResults = {
  job_id: 'job_00000020',
  state: 'succeeded',
  items: [
    { ssid: 'Cedar-Lab', ssid_base64: 'Q2VkYXItTGFi', bssid: 'a4:2b:b0:11:22:33', channel: 6, rssi_dbm: -41, security: 'wpa2_psk', connect_supported: true },
    { ssid: 'Cedar-Lab', ssid_base64: 'Q2VkYXItTGFi', bssid: 'a4:2b:b0:99:88:77', channel: 11, rssi_dbm: -72, security: 'wpa2_psk', connect_supported: true },
    { ssid: 'guest', ssid_base64: 'Z3Vlc3Q=', bssid: 'de:ad:be:ef:00:01', channel: 1, rssi_dbm: -67, security: 'open', connect_supported: true },
    { ssid: 'office-wpa23', ssid_base64: 'b2ZmaWNlLXdwYTIz', bssid: 'de:ad:be:ef:00:02', channel: 3, rssi_dbm: -70, security: 'wpa2_wpa3_transition', connect_supported: true },
    { ssid: 'corp-eap', ssid_base64: 'Y29ycC1lYXA=', bssid: 'de:ad:be:ef:00:03', channel: 9, rssi_dbm: -63, security: 'enterprise', connect_supported: false },
    { ssid: '', ssid_base64: '', bssid: 'de:ad:be:ef:00:04', channel: 13, rssi_dbm: -78, security: 'wpa2_psk', connect_supported: true },
    { ssid: 'Кедр', ssid_base64: '0JrQtdC00YA=', bssid: 'de:ad:be:ef:00:05', channel: 7, rssi_dbm: -59, security: 'wpa3_sae', connect_supported: true },
    { ssid: 'legacy-wep', ssid_base64: 'bGVnYWN5LXdlcA==', bssid: 'de:ad:be:ef:00:06', channel: 2, rssi_dbm: -81, security: 'unknown', connect_supported: false },
    { ssid: 'Ce\uFFFDd', ssid_base64: 'Q2X/ZA==', bssid: 'de:ad:be:ef:00:07', channel: 4, rssi_dbm: -66, security: 'wpa2_psk', connect_supported: true },
    { ssid: '<i>x</i>', ssid_base64: 'PGk+eDwvaT4=', bssid: 'de:ad:be:ef:00:08', channel: 5, rssi_dbm: -75, security: 'open', connect_supported: true },
  ],
  truncated: false,
  error: null,
};

export const scanResultsTruncated: ScanResults = { ...scanResults, truncated: true };

export const all: Record<string, [string, unknown]> = {
  authStateFresh: ['AuthState', authStateFresh],
  authStateConfigured: ['AuthState', authStateConfigured],
  session: ['Session', session],
  systemStatus: ['SystemStatus', systemStatus],
  networkStatus: ['NetworkStatus', networkStatus],
  matterStatus: ['MatterStatus', matterStatus],
  coprocessorStatus: ['CoprocessorStatus', coprocessorStatus],
  passwordAccepted: ['JobAccepted', passwordAccepted],
  passwordJobRunning: ['Job', passwordJobRunning],
  capabilities: ['Capabilities', capabilities],
  windowClosed: ['CommissioningWindow', windowClosed],
  windowOpenFromWeb: ['CommissioningWindow', windowOpenFromWeb],
  windowOpenByController: ['CommissioningWindow', windowOpenByController],
  onboardingCodes: ['OnboardingCodes', onboardingCodes],
  fabrics: ['Fabrics', fabrics],
  matterOpenAccepted: ['JobAccepted', matterOpenAccepted],
  matterOpenSucceeded: ['Job', matterOpenSucceeded],
  networkRuntime: ['NetworkStatus', networkRuntime],
  networkRuntimeRadioAbsent: ['NetworkStatus', networkRuntimeRadioAbsent],
  networkConfig: ['NetworkConfigResponse', networkConfig],
  txStaged: ['NetworkTransaction', txStaged],
  txAwaiting: ['NetworkTransaction', txAwaiting],
  txAwaitingDhcp: ['NetworkTransaction', txAwaitingDhcp],
  txCommitted: ['NetworkTransaction', txCommitted],
  txRolledBack: ['NetworkTransaction', txRolledBack],
  txTimedOut: ['NetworkTransaction', txTimedOut],
  networkApplyAccepted: ['JobAccepted', networkApplyAccepted],
  networkCommitSucceeded: ['Job', networkCommitSucceeded],
  networkRollbackSucceeded: ['Job', networkRollbackSucceeded],
  networkDiscardAccepted: ['JobAccepted', networkDiscardAccepted],
  networkDiscardSucceeded: ['Job', networkDiscardSucceeded],
  scanAccepted: ['JobAccepted', scanAccepted],
  scanSucceeded: ['Job', scanSucceeded],
  scanResults: ['ScanResults', scanResults],
  scanResultsTruncated: ['ScanResults', scanResultsTruncated],
};
