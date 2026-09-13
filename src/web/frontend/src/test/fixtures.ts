// Response bodies for component tests, typed by the generated schema types so
// a shape the device cannot send does not compile, and checked against
// openapi.json itself in fixtures.test.ts for what types cannot say
// (patterns, formats, bounds).
import type {
  AuthState,
  CoprocessorStatus,
  Job,
  JobAccepted,
  MatterStatus,
  NetworkStatus,
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
};
