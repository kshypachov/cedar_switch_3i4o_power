import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { describe, expect, it } from 'vitest';

import { t } from '.';
import { ru } from './ru';

// Not new URL(..., import.meta.url): Vite rewrites that form as an asset import.
const here = dirname(fileURLToPath(import.meta.url));
const repo = (path: string) => join(here, '../../../../..', path);

// The device's error table and field codes, read from the mock's transcription
// of api_validation.c (which its own suite checks against the C source).
const errorsPy = readFileSync(repo('tools/api-contract/cedar_contract/errors.py'), 'utf8');
const errorCodes = [...errorsPy.matchAll(/^\s+"([a-z_]+)": \(\d{3}, (?:True|False)\),/gm)].map((m) => m[1]!);
const fieldStart = errorsPy.indexOf('FIELD_CODES: tuple');
const fieldBlock = errorsPy.slice(fieldStart, errorsPy.indexOf(')', fieldStart));
const fieldCodes = [...fieldBlock.matchAll(/"([a-z_]+)"/g)].map((m) => m[1]!);

const document = JSON.parse(readFileSync(repo('docs/device-development/openapi.json'), 'utf8'));
const schemas = document.components.schemas;
const enumOf = (schema: string, property: string): string[] => schemas[schema].properties[property].enum;

describe('the dictionary covers what the device can say', () => {
  it('found the tables it checks against', () => {
    expect(errorCodes.length).toBeGreaterThanOrEqual(30);
    expect(fieldCodes).toContain('unknown_field');
  });

  it.each(errorCodes)('error code %s has a message', (code) => {
    expect(ru).toHaveProperty(`error.${code}`);
  });

  it.each(fieldCodes)('field code %s has a message', (code) => {
    expect(ru).toHaveProperty(`field.${code}`);
  });

  it.each([
    ['Job', 'kind', 'job.kind'],
    ['Job', 'state', 'job.state'],
    ['InterfaceStatus', 'state', 'interface'],
    ['MatterStatus', 'state', 'matter'],
    ['CoprocessorStatus', 'state', 'coprocessor'],
    ['Address', 'source', 'address.source'],
    ['AccessPoint', 'security', 'wifi.security'],
    ['NetworkTransaction', 'state', 'network.tx'],
    ['IPv4Config', 'mode', 'network.ipv4_mode'],
    ['DNSConfig', 'mode', 'network.dns_mode'],
    ['LogSource', 'id', 'logs.source'],
    ['LogRecord', 'kind', 'logs.kind'],
    ['CoprocessorStatus', 'uart_mode', 'logs.uart_mode'],
    ['Upload', 'state', 'update.upload_state'],
    ['UpdateSummary', 'state', 'update.summary'],
    ['FirmwareImage', 'format', 'update.format'],
  ])('%s.%s values all have text', (schema, property, prefix) => {
    for (const value of enumOf(schema, property)) {
      expect(ru, `${prefix}.${value}`).toHaveProperty(`${prefix}.${value}`);
    }
  });

  // Nullable enums are declared as anyOf [enum, null].
  it.each([
    ['CommissioningWindow', 'mode', 'matter.mode'],
    ['CommissioningWindow', 'source', 'matter.source'],
    ['OnboardingCodes', 'reason', 'matter.codes'],
    ['LogRecord', 'level', 'logs.level'],
  ])('%s.%s values all have text', (schema, property, prefix) => {
    const values: string[] = schemas[schema].properties[property].anyOf.find((s: { enum?: string[] }) => s.enum).enum;
    expect(values.length).toBeGreaterThan(0);
    for (const value of values) {
      expect(ru, `${prefix}.${value}`).toHaveProperty(`${prefix}.${value}`);
    }
  });

  it('fills placeholders and leaves unknown ones visible', () => {
    expect(t('password.too_short', { min: 12 })).toBe('Не короче 12 символов.');
    expect(t('error.request_id', {})).toContain('{id}');
  });
});
