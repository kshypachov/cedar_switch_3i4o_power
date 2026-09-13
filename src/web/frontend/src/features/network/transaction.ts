import type { InterfaceStatus, NetworkConfigOutput, NetworkTransaction } from '../../api/types';
import type { Iface } from './form';

export type TransactionState = NetworkTransaction['state'];

/** States in which the transaction still holds the device's one candidate. */
export const PENDING_STATES: ReadonlySet<TransactionState> = new Set([
  'staged',
  'applying',
  'awaiting_confirmation',
  'rolling_back',
]);

/** Timeouts the apply form offers, within NetworkApplyRequest's 60-300 and the device's limits. */
export const CONFIRM_TIMEOUT_CHOICES = [60, 120, 180, 300] as const;
export const DEFAULT_CONFIRM_TIMEOUT = 120;
const SCHEMA_MIN = 60;
const SCHEMA_MAX = 300;

export function confirmTimeoutChoices(min: number, max: number): number[] {
  const low = Math.max(min, SCHEMA_MIN);
  const high = Math.min(max, SCHEMA_MAX);
  const choices = CONFIRM_TIMEOUT_CHOICES.filter((s) => s >= low && s <= high);
  return choices.length ? choices : [Math.min(Math.max(DEFAULT_CONFIRM_TIMEOUT, low), high)];
}

/**
 * The query parameter that carries a transaction to another origin. Cookies and
 * the CSRF token stay with the old address, so after an address change the
 * person signs in again there, and the network screen reopens the transaction
 * from this parameter to be confirmed before the deadline.
 */
export const TXN_PARAM = 'txn';
const TRANSACTION_ID = /^[A-Za-z0-9_-]{1,64}$/;

export function transactionFromSearch(search: string): string | null {
  const id = new URLSearchParams(search).get(TXN_PARAM);
  return id && TRANSACTION_ID.test(id) ? id : null;
}

/**
 * Links to this screen at the addresses the device named. Only http(s): a link
 * built from a device string must never become a script URL.
 */
export function reconnectLinks(urls: readonly string[], transactionId: string): string[] {
  const links: string[] = [];
  for (const raw of urls) {
    let url: URL;
    try {
      url = new URL(raw);
    } catch {
      continue;
    }
    if (url.protocol !== 'http:' && url.protocol !== 'https:') continue;
    const link = new URL('/network', url.origin);
    link.searchParams.set(TXN_PARAM, transactionId);
    links.push(link.href);
  }
  return links;
}

export type FutureAddress =
  | { kind: 'disabled' }
  | { kind: 'dhcp' }
  | { kind: 'static'; address: string; prefix: number | null };

/** What an interface's IPv4 address will be under @p config. A DHCP address is not known in advance. */
export function futureAddress(config: NetworkConfigOutput, iface: Iface): FutureAddress {
  const { enabled, ipv4 } = config.interfaces[iface];
  if (!enabled) return { kind: 'disabled' };
  if (ipv4.mode === 'dhcp' || !ipv4.address) return { kind: 'dhcp' };
  return { kind: 'static', address: ipv4.address, prefix: ipv4.prefix_length };
}

/** The IPv4 addresses an interface has now, as `address/prefix`. */
export function currentIpv4(iface: InterfaceStatus | undefined): string[] {
  return (iface?.addresses ?? []).filter((a) => a.family === 'ipv4').map((a) => `${a.address}/${a.prefix_length}`);
}
