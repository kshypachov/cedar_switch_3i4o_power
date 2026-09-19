import { API_BASE, api, newIdempotencyKey, unwrap } from './client';
import type { CoredumpStatus, JobAccepted, SystemFirmware } from './types';

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

/** Whether a coredump from a crash is stored, and its size and reason. */
export const getCoredump = (signal?: AbortSignal): Promise<CoredumpStatus> =>
  unwrap(api.GET('/system/coredump', { signal }));

/** Forget the stored coredump; succeeds when there is none too. */
export async function clearCoredump(csrfToken: string): Promise<void> {
  await unwrap(api.DELETE('/system/coredump', { params: { header: { 'X-CSRF-Token': csrfToken } } }));
}

/**
 * The dump is a download, like the logs export: a plain same-origin link, so the
 * browser saves cedar-coredump.bin and the session cookie goes with it.
 */
export const coredumpUrl = `${API_BASE}/system/coredump/data`;

export { newIdempotencyKey };
