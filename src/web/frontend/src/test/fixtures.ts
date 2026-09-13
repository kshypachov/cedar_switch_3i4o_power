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
  NetworkStatus,
  OnboardingCodes,
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
    network_confirm_min_seconds: 30,
    network_confirm_max_seconds: 900,
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
};
