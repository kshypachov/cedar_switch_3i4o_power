import type { ErrorField } from '../../api/errors';
import type {
  AccessPoint,
  CredentialChange,
  InterfaceStatus,
  IPv4Config,
  NetworkConfigOutput,
  NetworkConfigResponse,
  NetworkTransactionRequest,
  WiFiConfigOutput,
} from '../../api/types';
import { codePoints } from '../../components/format';
import type { MessageKey } from '../../i18n';
import { decodeSsid, encodeSsid, SSID_MAX_BYTES, ssidByteLength } from './ssid';

// The form holds what the person is editing as text, and turns it into the
// contract's candidate only when it is staged. The server remains the
// authority on every rule; the checks here only save a round trip for a typo.

export type Iface = 'ethernet' | 'wifi';
export type WifiSecurity = WiFiConfigOutput['security'];

export interface Ipv4Form {
  mode: IPv4Config['mode'];
  address: string;
  prefix: string;
  gateway: string;
}

export interface WifiForm {
  enabled: boolean;
  /** What the input shows. */
  ssid: string;
  /** The exact bytes sent: from the configuration or a scan, or the typed text as UTF-8. */
  ssidBase64: string;
  security: WifiSecurity;
  hidden: boolean;
  /** Write-only: empty unless the person typed a new one. */
  password: string;
  ipv4: Ipv4Form;
}

export interface NetworkForm {
  preferred: Iface;
  ethernet: { enabled: boolean; ipv4: Ipv4Form };
  wifi: WifiForm;
  dns: { mode: 'automatic' | 'manual'; servers: [string, string] };
}

/** Input ids, which are also where a refusal's field is shown. */
export type FieldId =
  | `${Iface}-${'enabled' | 'mode' | 'address' | 'prefix' | 'gateway'}`
  | 'wifi-ssid'
  | 'wifi-security'
  | 'wifi-hidden'
  | 'wifi-password'
  | 'dns-mode'
  | 'dns-server-0'
  | 'dns-server-1'
  | 'preferred-interface';

/** The contract's credential value: 1-64 characters. */
export const PASSWORD_MAX = 64;
export const PREFIX_MIN = 1;
export const PREFIX_MAX = 30;

const ipv4Form = (c: IPv4Config): Ipv4Form => ({
  mode: c.mode,
  address: c.address ?? '',
  prefix: c.prefix_length === null ? '' : String(c.prefix_length),
  gateway: c.gateway ?? '',
});

export function formFromConfig(config: NetworkConfigOutput): NetworkForm {
  const { ethernet, wifi } = config.interfaces;
  const servers = config.dns.servers;
  return {
    preferred: config.preferred_interface,
    ethernet: { enabled: ethernet.enabled, ipv4: ipv4Form(ethernet.ipv4) },
    wifi: {
      enabled: wifi.enabled,
      ssid: decodeSsid(wifi.ssid_base64) ?? '',
      ssidBase64: wifi.ssid_base64,
      security: wifi.security,
      hidden: wifi.hidden,
      password: '',
      ipv4: ipv4Form(wifi.ipv4),
    },
    dns: { mode: config.dns.mode, servers: [servers[0] ?? '', servers[1] ?? ''] },
  };
}

/** A typed SSID: its bytes are the text's UTF-8. */
export function withSsidText(wifi: WifiForm, text: string): WifiForm {
  return { ...wifi, ssid: text, ssidBase64: encodeSsid(text) };
}

/** Switching an interface off moves the preference to the other one: the device refuses preferring a disabled interface. */
export function withEnabled(form: NetworkForm, iface: Iface, enabled: boolean): NetworkForm {
  const next = { ...form, [iface]: { ...form[iface], enabled } };
  if (!enabled && form.preferred === iface) next.preferred = iface === 'ethernet' ? 'wifi' : 'ethernet';
  return next;
}

/**
 * The configuration security for a scanned network, or null when this device
 * cannot join it. A transition network takes either; the stronger one is used
 * when the device offers it.
 */
export function securityFor(ap: AccessPoint['security'], modes: readonly WifiSecurity[]): WifiSecurity | null {
  const candidates: Record<AccessPoint['security'], WifiSecurity[]> = {
    open: ['open'],
    wpa2_psk: ['wpa2_psk'],
    wpa3_sae: ['wpa3_sae'],
    wpa2_wpa3_transition: ['wpa3_sae', 'wpa2_psk'],
    enterprise: [],
    unknown: [],
  };
  return candidates[ap].find((mode) => modes.includes(mode)) ?? null;
}

export function selectable(ap: AccessPoint, modes: readonly WifiSecurity[]): boolean {
  return ap.connect_supported && securityFor(ap.security, modes) !== null;
}

/** A network picked from a scan, with its exact bytes. An empty SSID is a hidden network: its name is typed. */
export function withAccessPoint(form: NetworkForm, ap: AccessPoint, security: WifiSecurity): NetworkForm {
  return {
    ...form,
    wifi: {
      ...form.wifi,
      enabled: true,
      ssid: decodeSsid(ap.ssid_base64) ?? ap.ssid,
      ssidBase64: ap.ssid_base64,
      security,
      hidden: ap.ssid_base64 === '',
      password: '',
    },
  };
}

/**
 * What happens to the stored Wi-Fi password (contract, network section). The
 * page never has the stored password, so:
 * - an open network clears it;
 * - a typed password replaces it;
 * - nothing typed keeps it, but only for the same SSID and security, because
 *   the device refuses `keep` across a profile change;
 * - a disabled interface with nothing to keep clears it.
 * Null: an enabled protected network needs a password typed.
 */
export function credentialFor(wifi: WifiForm, stored: WiFiConfigOutput): CredentialChange | null {
  if (wifi.security === 'open') return { action: 'clear' };
  if (wifi.password) return { action: 'replace', value: wifi.password };
  const sameProfile = wifi.ssidBase64 === stored.ssid_base64 && wifi.security === stored.security;
  if (stored.password_set && sameProfile) return { action: 'keep' };
  if (!wifi.enabled) return { action: 'clear' };
  return null;
}

/** Whether the stored password would be kept if nothing is typed. */
export function keepsPassword(wifi: WifiForm, stored: WiFiConfigOutput): boolean {
  return wifi.security !== 'open' && credentialFor({ ...wifi, password: '' }, stored)?.action === 'keep';
}

const IPV4 = /^(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)(\.(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)){3}$/;
export const isIpv4 = (text: string) => IPV4.test(text);
// Loose on purpose: the device checks the format; this only catches a typo.
const isIpv6 = (text: string) => text.includes(':') && /^[0-9A-Fa-f:.]+$/.test(text);

function ipv4Input(f: Ipv4Form): IPv4Config {
  if (f.mode === 'dhcp') return { mode: 'dhcp', address: null, prefix_length: null, gateway: null };
  return {
    mode: 'static',
    address: f.address.trim() || null,
    prefix_length: f.prefix.trim() === '' ? null : Number(f.prefix.trim()),
    gateway: f.gateway.trim() || null,
  };
}

function ipv4Problems(iface: Iface, f: Ipv4Form, problems: Map<FieldId, MessageKey>): void {
  if (f.mode === 'dhcp') return;
  const address = f.address.trim();
  const prefix = f.prefix.trim();
  const gateway = f.gateway.trim();
  if (!address) problems.set(`${iface}-address`, 'field.required');
  else if (!isIpv4(address)) problems.set(`${iface}-address`, 'field.invalid_format');
  if (!prefix) problems.set(`${iface}-prefix`, 'field.required');
  else if (!/^\d+$/.test(prefix) || Number(prefix) < PREFIX_MIN || Number(prefix) > PREFIX_MAX) {
    problems.set(`${iface}-prefix`, 'network.prefix_range');
  }
  if (gateway && !isIpv4(gateway)) problems.set(`${iface}-gateway`, 'field.invalid_format');
}

export type Prepared =
  | { request: NetworkTransactionRequest; problems: null }
  | { request: null; problems: Map<FieldId, MessageKey> };

/** The candidate to stage, or what to fix first. */
export function prepare(form: NetworkForm, base: NetworkConfigResponse): Prepared {
  const problems = new Map<FieldId, MessageKey>();
  const stored = base.config.interfaces.wifi;
  ipv4Problems('ethernet', form.ethernet.ipv4, problems);
  ipv4Problems('wifi', form.wifi.ipv4, problems);

  const { wifi } = form;
  if (ssidByteLength(wifi.ssidBase64) > SSID_MAX_BYTES) problems.set('wifi-ssid', 'network.ssid_too_long');
  else if (wifi.enabled && wifi.ssidBase64 === '') problems.set('wifi-ssid', 'field.required');
  const credential = credentialFor(wifi, stored);
  if (!credential) problems.set('wifi-password', 'network.password_required');
  else if (credential.action === 'replace' && codePoints(credential.value) > PASSWORD_MAX) {
    problems.set('wifi-password', 'field.too_long');
  }

  const servers = form.dns.servers.map((s) => s.trim());
  if (form.dns.mode === 'manual') {
    if (!servers.some(Boolean)) problems.set('dns-server-0', 'field.required');
    servers.forEach((server, i) => {
      if (server && !isIpv4(server) && !isIpv6(server)) problems.set(`dns-server-${i as 0 | 1}`, 'field.invalid_format');
    });
  }

  if (problems.size || !credential) return { request: null, problems };
  return {
    request: {
      base_revision: base.revision,
      config: {
        preferred_interface: form.preferred,
        dns: { mode: form.dns.mode, servers: form.dns.mode === 'manual' ? servers.filter(Boolean) : [] },
        interfaces: {
          ethernet: { enabled: form.ethernet.enabled, ipv4: ipv4Input(form.ethernet.ipv4) },
          wifi: {
            enabled: wifi.enabled,
            ssid_base64: wifi.ssidBase64,
            security: wifi.security,
            hidden: wifi.hidden,
            ipv4: ipv4Input(wifi.ipv4),
            credential,
          },
        },
      },
    },
    problems: null,
  };
}

// JSON Pointers of a 422 to the input they belong to. More specific pointers
// come first; a pointer also covers everything under it (`/credential/value`).
const POINTERS: [string, FieldId][] = [
  ['/config/preferred_interface', 'preferred-interface'],
  ['/config/dns/mode', 'dns-mode'],
  ['/config/dns/servers/1', 'dns-server-1'],
  ['/config/dns/servers', 'dns-server-0'],
  ['/config/interfaces/wifi/ssid_base64', 'wifi-ssid'],
  ['/config/interfaces/wifi/security', 'wifi-security'],
  ['/config/interfaces/wifi/hidden', 'wifi-hidden'],
  ['/config/interfaces/wifi/credential', 'wifi-password'],
  ...(['ethernet', 'wifi'] as const).flatMap((iface): [string, FieldId][] => [
    [`/config/interfaces/${iface}/enabled`, `${iface}-enabled`],
    [`/config/interfaces/${iface}/ipv4/mode`, `${iface}-mode`],
    [`/config/interfaces/${iface}/ipv4/address`, `${iface}-address`],
    [`/config/interfaces/${iface}/ipv4/prefix_length`, `${iface}-prefix`],
    [`/config/interfaces/${iface}/ipv4/gateway`, `${iface}-gateway`],
  ]),
];

export function fieldForPointer(path: string): FieldId | null {
  const hit = POINTERS.find(([pointer]) => path === pointer || path.startsWith(`${pointer}/`));
  return hit ? hit[1] : null;
}

/** A refusal's fields by input; the ones no input owns are returned, never dropped. */
export function placeFieldErrors(fields: readonly ErrorField[]): { placed: Map<FieldId, string>; unplaced: ErrorField[] } {
  const placed = new Map<FieldId, string>();
  const unplaced: ErrorField[] = [];
  for (const field of fields) {
    const id = fieldForPointer(field.path);
    if (!id) unplaced.push(field);
    else if (!placed.has(id)) placed.set(id, field.code);
  }
  return { placed, unplaced };
}

/**
 * Static addresses everywhere and automatic DNS: nothing will hand the device
 * a resolver. Allowed - a LAN reached by IP needs none - so a hint, not an error.
 */
export function needsManualDnsHint(form: NetworkForm): boolean {
  const enabled = [form.ethernet, form.wifi].filter((i) => i.enabled);
  return form.dns.mode === 'automatic' && enabled.length > 0 && enabled.every((i) => i.ipv4.mode === 'static');
}

/** The radio is the coprocessor: when it is not ready the device says so on the Wi-Fi interface, enabled or not. */
export function wifiUnavailable(iface: InterfaceStatus | undefined): boolean {
  return iface?.error?.code === 'capability_unavailable';
}
