import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import Ajv2020 from 'ajv/dist/2020';
import addFormats from 'ajv-formats';

// openapi.json itself, for checking what the page sends: a request body built
// by the page must be one the device's schema accepts, formats and bounds
// included, which the generated types alone cannot say.
const documentPath = join(dirname(fileURLToPath(import.meta.url)), '../../../../../docs/device-development/openapi.json');
const document = JSON.parse(readFileSync(documentPath, 'utf8'));

const ajv = new Ajv2020({ strict: false, allErrors: true });
addFormats(ajv);
ajv.addSchema({ $id: 'openapi', components: document.components });

/** The schema's complaints about @p value as @p schema; empty when it is valid. */
export function schemaErrors(schema: string, value: unknown): unknown[] {
  const validate = ajv.getSchema(`openapi#/components/schemas/${schema}`);
  if (!validate) throw new Error(`no schema ${schema}`);
  validate(value);
  return validate.errors ?? [];
}
