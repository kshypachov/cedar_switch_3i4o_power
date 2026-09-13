import { describe, expect, it } from 'vitest';

import type { AccessPoint, InterfaceStatus, NetworkConfigResponse } from '../../api/types';
import { networkConfig, networkRuntime, networkRuntimeRadioAbsent, scanResults } from '../../test/fixtures';
import { schemaErrors } from '../../test/schema';
import {
  credentialFor,
  fieldForPointer,
  formFromConfig,
  needsManualDnsHint,
  type NetworkForm,
  placeFieldErrors,
  prepare,
  securityFor,
  selectable,
  withAccessPoint,
  withEnabled,
  withSsidText,
  wifiUnavailable,
} from './form';

const stored = networkConfig.config.interfaces.wifi;
const fresh = (): NetworkForm => formFromConfig(networkConfig.config);
const ap = (ssid: string): AccessPoint => scanResults.items.find((i) => i.ssid === ssid)!;

function request(form: NetworkForm, base: NetworkConfigResponse = networkConfig) {
  const prepared = prepare(form, base);
  expect(prepared.problems && Object.fromEntries(prepared.problems)).toBeNull();
  expect(schemaErrors('NetworkTransactionRequest', prepared.request)).toEqual([]);
  return prepared.request!;
}

const problems = (form: NetworkForm) => Object.fromEntries(prepare(form, networkConfig).problems ?? []);

describe('the candidate built from the form', () => {
  it('sends the committed configuration back unchanged, at its revision, keeping the stored password', () => {
    const body = request(fresh());
    expect(body.base_revision).toBe(3);
    expect(body.config.interfaces.wifi).toEqual({
      enabled: true,
      ssid_base64: 'Q2VkYXItTGFi',
      security: 'wpa2_psk',
      hidden: false,
      ipv4: { mode: 'dhcp', address: null, prefix_length: null, gateway: null },
      credential: { action: 'keep' },
    });
    expect(body.config.dns).toEqual({ mode: 'automatic', servers: [] });
  });

  it('sends a static address with its prefix, and no gateway for an isolated LAN', () => {
    const form = fresh();
    form.ethernet.ipv4 = { mode: 'static', address: ' 10.1.2.3 ', prefix: '30', gateway: '' };
    expect(request(form).config.interfaces.ethernet.ipv4).toEqual({
      mode: 'static',
      address: '10.1.2.3',
      prefix_length: 30,
      gateway: null,
    });
  });

  it('sends nulls for DHCP whatever was typed before switching to it', () => {
    const form = fresh();
    form.ethernet.ipv4 = { mode: 'dhcp', address: '10.1.2.3', prefix: '24', gateway: '10.1.2.1' };
    expect(request(form).config.interfaces.ethernet.ipv4).toEqual({ mode: 'dhcp', address: null, prefix_length: null, gateway: null });
  });

  it('checks a static address, a prefix of 1-30 and a gateway format before asking the device', () => {
    const form = fresh();
    form.ethernet.ipv4 = { mode: 'static', address: '', prefix: '31', gateway: '10.0.0' };
    form.wifi.ipv4 = { mode: 'static', address: '300.1.1.1', prefix: '24.5', gateway: '' };
    expect(problems(form)).toEqual({
      'ethernet-address': 'field.required',
      'ethernet-prefix': 'network.prefix_range',
      'ethernet-gateway': 'field.invalid_format',
      'wifi-address': 'field.invalid_format',
      'wifi-prefix': 'network.prefix_range',
    });
    form.ethernet.ipv4 = { mode: 'static', address: '10.0.0.2', prefix: '0', gateway: '' };
    expect(problems(form)['ethernet-prefix']).toBe('network.prefix_range');
    form.ethernet.ipv4.prefix = '1';
    expect(problems(form)['ethernet-prefix']).toBeUndefined();
  });

  it('limits the SSID to 32 bytes, however few characters they are', () => {
    const form = fresh();
    form.wifi = { ...withSsidText(form.wifi, 'Кедр'.repeat(4)), password: 'a new password' };
    expect(request(form).config.interfaces.wifi.ssid_base64).toBe(btoa(String.fromCharCode(...new TextEncoder().encode('Кедр'.repeat(4)))));
    form.wifi = withSsidText(form.wifi, `${'Кедр'.repeat(4)}a`);
    expect(problems(form)).toEqual({ 'wifi-ssid': 'network.ssid_too_long' });
  });

  it('requires an SSID for enabled Wi-Fi only', () => {
    const form = fresh();
    form.wifi = { ...withSsidText(form.wifi, ''), password: 'a new password' };
    expect(problems(form)).toEqual({ 'wifi-ssid': 'field.required' });
    expect(request(withEnabled(form, 'wifi', false)).config.interfaces.wifi.enabled).toBe(false);
  });

  it('keeps the exact bytes of a picked network that are not UTF-8', () => {
    const picked = withAccessPoint(fresh(), ap('Ce\uFFFDd'), 'wpa2_psk');
    picked.wifi.password = 'a new password';
    expect(picked.wifi.ssid).toBe('Ce\uFFFDd');
    expect(request(picked).config.interfaces.wifi.ssid_base64).toBe('Q2X/ZA==');
  });

  it('takes 1-2 manual DNS servers, IPv4 or IPv6, and none when automatic', () => {
    const form = fresh();
    form.dns = { mode: 'manual', servers: ['', ''] };
    expect(problems(form)).toEqual({ 'dns-server-0': 'field.required' });
    form.dns.servers = ['192.168.88.1', 'not-an-address'];
    expect(problems(form)).toEqual({ 'dns-server-1': 'field.invalid_format' });
    form.dns.servers = [' 2001:db8::1 ', '9.9.9.9'];
    expect(request(form).config.dns).toEqual({ mode: 'manual', servers: ['2001:db8::1', '9.9.9.9'] });
    form.dns = { mode: 'automatic', servers: ['1.1.1.1', ''] };
    expect(request(form).config.dns).toEqual({ mode: 'automatic', servers: [] });
  });
});

describe('the stored Wi-Fi password', () => {
  const wifi = fresh().wifi;

  it('is kept when nothing is typed and the SSID and security are unchanged', () => {
    expect(credentialFor(wifi, stored)).toEqual({ action: 'keep' });
  });

  it('is replaced by a typed one', () => {
    expect(credentialFor({ ...wifi, password: 'typed' }, stored)).toEqual({ action: 'replace', value: 'typed' });
  });

  it('is cleared for an open network, typed or not', () => {
    expect(credentialFor({ ...wifi, security: 'open', password: 'typed' }, stored)).toEqual({ action: 'clear' });
  });

  it('cannot be kept across an SSID or security change, so a password is required', () => {
    expect(credentialFor(withSsidText(wifi, 'other'), stored)).toBeNull();
    expect(credentialFor({ ...wifi, security: 'wpa3_sae' }, stored)).toBeNull();
    expect(problems({ ...fresh(), wifi: { ...wifi, security: 'wpa3_sae' } })).toEqual({ 'wifi-password': 'network.password_required' });
  });

  it('cannot be kept when none is stored', () => {
    expect(credentialFor(wifi, { ...stored, password_set: false })).toBeNull();
  });

  it('is cleared, not required, for disabled Wi-Fi with nothing to keep', () => {
    expect(credentialFor({ ...withSsidText(wifi, 'other'), enabled: false }, stored)).toEqual({ action: 'clear' });
    expect(credentialFor({ ...wifi, enabled: false }, stored)).toEqual({ action: 'keep' });
  });
});

describe('refusals placed at their inputs', () => {
  it.each([
    ['/config/interfaces/ethernet/ipv4/gateway', 'ethernet-gateway'],
    ['/config/interfaces/wifi/ipv4/prefix_length', 'wifi-prefix'],
    ['/config/interfaces/wifi/enabled', 'wifi-enabled'],
    ['/config/interfaces/wifi/ssid_base64', 'wifi-ssid'],
    ['/config/interfaces/wifi/credential', 'wifi-password'],
    ['/config/interfaces/wifi/credential/action', 'wifi-password'],
    ['/config/dns/servers', 'dns-server-0'],
    ['/config/dns/servers/0', 'dns-server-0'],
    ['/config/dns/servers/1', 'dns-server-1'],
    ['/config/preferred_interface', 'preferred-interface'],
  ])('%s -> %s', (path, field) => {
    expect(fieldForPointer(path)).toBe(field);
  });

  it('owns no pointer it does not know, and keeps those for display', () => {
    expect(fieldForPointer('/base_revision')).toBeNull();
    expect(fieldForPointer('/config/interfaces/ethernet/ipv4/addresses')).toBeNull();
    const { placed, unplaced } = placeFieldErrors([
      { path: '/config/interfaces/ethernet/ipv4/gateway', code: 'out_of_range' },
      { path: '/config/interfaces/ethernet/ipv4/gateway', code: 'conflicting' },
      { path: '/base_revision', code: 'required' },
    ]);
    expect(Object.fromEntries(placed)).toEqual({ 'ethernet-gateway': 'out_of_range' });
    expect(unplaced).toEqual([{ path: '/base_revision', code: 'required' }]);
  });
});

describe('scanned networks', () => {
  const modes = ['open', 'wpa2_psk', 'wpa3_sae'] as const;

  it('are joinable only when supported and their security is offered', () => {
    expect(selectable(ap('Cedar-Lab'), modes)).toBe(true);
    expect(selectable(ap('corp-eap'), modes)).toBe(false);
    expect(selectable(ap('legacy-wep'), modes)).toBe(false);
    expect(selectable(ap('Кедр'), ['open', 'wpa2_psk'])).toBe(false);
    expect(selectable({ ...ap('Cedar-Lab'), connect_supported: false }, modes)).toBe(false);
  });

  it('map a transition network to WPA3 when offered, else WPA2', () => {
    expect(securityFor('wpa2_wpa3_transition', modes)).toBe('wpa3_sae');
    expect(securityFor('wpa2_wpa3_transition', ['open', 'wpa2_psk'])).toBe('wpa2_psk');
    expect(securityFor('enterprise', modes)).toBeNull();
  });

  it('fill the form; a hidden one leaves the name to be typed', () => {
    const picked = withAccessPoint(withEnabled(fresh(), 'wifi', false), ap('Кедр'), 'wpa3_sae');
    expect(picked.wifi).toMatchObject({ enabled: true, ssid: 'Кедр', ssidBase64: '0JrQtdC00YA=', security: 'wpa3_sae', hidden: false });
    expect(withAccessPoint(fresh(), ap(''), 'wpa2_psk').wifi).toMatchObject({ ssid: '', ssidBase64: '', hidden: true });
  });
});

describe('around the form', () => {
  it('moves the preference off an interface being switched off', () => {
    expect(withEnabled(fresh(), 'ethernet', false).preferred).toBe('wifi');
    expect(withEnabled(fresh(), 'wifi', false).preferred).toBe('ethernet');
  });

  it('hints at manual DNS when every enabled interface is static and DNS is automatic', () => {
    const form = fresh();
    form.ethernet.ipv4.mode = 'static';
    expect(needsManualDnsHint(form)).toBe(false);
    form.wifi.ipv4.mode = 'static';
    expect(needsManualDnsHint(form)).toBe(true);
    expect(needsManualDnsHint({ ...form, dns: { mode: 'manual', servers: ['1.1.1.1', ''] } })).toBe(false);
    expect(needsManualDnsHint(withEnabled({ ...form, wifi: { ...form.wifi, ipv4: { ...form.wifi.ipv4, mode: 'dhcp' } } }, 'wifi', false))).toBe(true);
  });

  it('knows Wi-Fi is unavailable from the interface the device reports', () => {
    expect(wifiUnavailable(networkRuntimeRadioAbsent.interfaces[1])).toBe(true);
    expect(wifiUnavailable(networkRuntime.interfaces[1])).toBe(false);
    const failedOtherwise: InterfaceStatus = {
      ...networkRuntime.interfaces[1]!,
      state: 'failed',
      error: { code: 'invalid_credentials', message: 'x', request_id: 'req_1', retryable: false },
    };
    expect(wifiUnavailable(failedOtherwise)).toBe(false);
  });
});
