import { api, newIdempotencyKey, unwrap } from './client';
import type { JobAccepted, SystemFirmware } from './types';

// The STM32 update (api-contract.md, "Обновление STM32"). The upload itself goes
// through api/firmware.ts with target "stm32u585".

export const getSystemFirmware = (signal?: AbortSignal): Promise<SystemFirmware> =>
  unwrap(api.GET('/system/firmware', { signal }));

/**
 * Request the MCUboot swap of a verified stm32u585 upload; the device restarts
 * about two seconds after the job reaches `rebooting`.
 */
export function startSystemUpdate(
  csrfToken: string,
  idempotencyKey: string,
  uploadId: string,
  acknowledgeDowngrade: boolean,
): Promise<JobAccepted> {
  return unwrap(
    api.POST('/system/updates', {
      params: { header: { 'X-CSRF-Token': csrfToken, 'Idempotency-Key': idempotencyKey } },
      body: { upload_id: uploadId, acknowledge_downgrade: acknowledgeDowngrade },
    }),
  );
}

export { newIdempotencyKey };
