import { api, newIdempotencyKey, unwrap } from './client';
import type { JobAccepted, Upload, UploadRequest } from './types';

// Every mutation takes its Idempotency-Key from the caller, which keeps it for
// retries of the same action: a lost answer repeated under the same key returns
// the first job instead of starting a second one (api-contract.md, "Общие правила").
const headers = (csrfToken: string, idempotencyKey: string) => ({
  'X-CSRF-Token': csrfToken,
  'Idempotency-Key': idempotencyKey,
});

export function createUpload(csrfToken: string, idempotencyKey: string, body: UploadRequest): Promise<Upload> {
  return unwrap(api.POST('/firmware/uploads', { params: { header: headers(csrfToken, idempotencyKey) }, body }));
}

/** Every upload the device holds, one per target at most, whatever its state. */
export const listUploads = (signal?: AbortSignal): Promise<Upload[]> =>
  unwrap(api.GET('/firmware/uploads', { signal })).then((list) => list.uploads);

export const getUpload = (uploadId: string, signal?: AbortSignal): Promise<Upload> =>
  unwrap(api.GET('/firmware/uploads/{upload_id}', { params: { path: { upload_id: uploadId } }, signal }));

/**
 * One chunk, as raw bytes. The document types the body as a string (format
 * binary); the bytes go out untouched as application/octet-stream - the default
 * serializer would turn them into JSON.
 */
export function writeUploadChunk(
  csrfToken: string,
  idempotencyKey: string,
  uploadId: string,
  offset: number,
  bytes: Uint8Array,
  signal?: AbortSignal,
): Promise<JobAccepted> {
  return unwrap(
    api.PUT('/firmware/uploads/{upload_id}/data', {
      params: { path: { upload_id: uploadId }, query: { offset }, header: headers(csrfToken, idempotencyKey) },
      body: bytes as unknown as string,
      bodySerializer: (body: unknown) => body as BodyInit,
      headers: { 'Content-Type': 'application/octet-stream' },
      signal,
    }),
  );
}

export function verifyUpload(csrfToken: string, idempotencyKey: string, uploadId: string): Promise<JobAccepted> {
  return unwrap(
    api.POST('/firmware/uploads/{upload_id}/verify', {
      params: { path: { upload_id: uploadId }, header: headers(csrfToken, idempotencyKey) },
      body: {},
    }),
  );
}

export function deleteUpload(csrfToken: string, idempotencyKey: string, uploadId: string): Promise<JobAccepted> {
  return unwrap(
    api.DELETE('/firmware/uploads/{upload_id}', {
      params: { path: { upload_id: uploadId }, header: headers(csrfToken, idempotencyKey) },
    }),
  );
}

/**
 * Write the verified file into the ESP32-C6 over UART. Every file this device
 * accepts is a whole flash image (raw_full_flash, kind recovery_bundle), so the
 * acknowledgement is always sent - after the page has asked the person.
 */
export function startCoprocessorUpdate(csrfToken: string, idempotencyKey: string, uploadId: string): Promise<JobAccepted> {
  return unwrap(
    api.POST('/coprocessor/updates', {
      params: { header: headers(csrfToken, idempotencyKey) },
      body: { upload_id: uploadId, method: 'uart', acknowledge_recovery: true },
    }),
  );
}

export function cancelJob(csrfToken: string, idempotencyKey: string, jobId: string): Promise<JobAccepted> {
  return unwrap(
    api.POST('/jobs/{job_id}/cancel', {
      params: { path: { job_id: jobId }, header: headers(csrfToken, idempotencyKey) },
      body: {},
    }),
  );
}

export { newIdempotencyKey };
