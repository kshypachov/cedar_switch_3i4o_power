import { describe, expect, it } from 'vitest';

import { networkConfig, networkRuntime, txStaged } from '../../test/fixtures';
import {
  confirmTimeoutChoices,
  currentIpv4,
  futureAddress,
  reconnectLinks,
  transactionFromSearch,
} from './transaction';

describe('the transaction across an address change', () => {
  it('links to the network screen at each address the device named, carrying the transaction', () => {
    expect(reconnectLinks(['http://192.168.88.50/', 'http://10.0.0.7/'], 'nettx_0001')).toEqual([
      'http://192.168.88.50/network?txn=nettx_0001',
      'http://10.0.0.7/network?txn=nettx_0001',
    ]);
  });

  it('offers no link for a DHCP change, and never one that is not a web address', () => {
    expect(reconnectLinks([], 'nettx_0001')).toEqual([]);
    expect(reconnectLinks(['javascript:alert(1)', 'not a url', 'file:///etc/passwd'], 'nettx_0001')).toEqual([]);
  });

  it('reads the transaction from the parameter only when it is a transaction id', () => {
    expect(transactionFromSearch('?txn=nettx_0001')).toBe('nettx_0001');
    expect(transactionFromSearch('?other=1&txn=nettx_0001')).toBe('nettx_0001');
    expect(transactionFromSearch('?txn=%3Cb%3E')).toBeNull();
    expect(transactionFromSearch('?txn=')).toBeNull();
    expect(transactionFromSearch('')).toBeNull();
  });
});

describe('the apply form', () => {
  it('offers confirmation timeouts within both the schema and the device limits', () => {
    expect(confirmTimeoutChoices(30, 900)).toEqual([60, 120, 180, 300]);
    expect(confirmTimeoutChoices(90, 200)).toEqual([120, 180]);
    expect(confirmTimeoutChoices(200, 250)).toEqual([200]);
  });

  it('says what each address will be: static as configured, DHCP unknown, or off', () => {
    expect(futureAddress(txStaged.candidate, 'ethernet')).toEqual({ kind: 'static', address: '192.168.88.50', prefix: 24 });
    expect(futureAddress(networkConfig.config, 'ethernet')).toEqual({ kind: 'dhcp' });
    const off = structuredClone(networkConfig.config);
    off.interfaces.wifi.enabled = false;
    expect(futureAddress(off, 'wifi')).toEqual({ kind: 'disabled' });
  });

  it('takes the current IPv4 addresses from the runtime state, not IPv6', () => {
    expect(currentIpv4(networkRuntime.interfaces[0])).toEqual(['192.168.88.14/24']);
    expect(currentIpv4(undefined)).toEqual([]);
  });
});
