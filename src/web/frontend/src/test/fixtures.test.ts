import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import Ajv2020 from 'ajv/dist/2020';
import addFormats from 'ajv-formats';
import { describe, expect, it } from 'vitest';

import { all } from './fixtures';

const documentPath = join(dirname(fileURLToPath(import.meta.url)), '../../../../../docs/device-development/openapi.json');
const document = JSON.parse(readFileSync(documentPath, 'utf8'));

const ajv = new Ajv2020({ strict: false, allErrors: true });
addFormats(ajv);
ajv.addSchema({ $id: 'openapi', components: document.components });

describe('fixtures are bodies the device can send', () => {
  for (const [name, [schema, value]] of Object.entries(all)) {
    it(`${name} validates as ${schema}`, () => {
      const validate = ajv.getSchema(`openapi#/components/schemas/${schema}`);
      expect(validate, schema).toBeDefined();
      const ok = validate!(value);
      expect(validate!.errors ?? []).toEqual([]);
      expect(ok).toBe(true);
    });
  }
});
