import { listUploads } from '../../api/firmware';
import type { Upload } from '../../api/types';
import { t } from '../../i18n';

export type UploadTarget = Upload['target'];

/** What the device holds for this screen's target and for the other one. */
export interface DeviceUploads {
  own: Upload | null;
  other: Upload | null;
}

/**
 * Ask the device which uploads it holds. The device keeps one upload at a time
 * across both processors, and a page learns about an upload it did not start
 * (another browser, a script, a reload that lost its note of the id) only here -
 * createUpload just refuses a second one with 409 busy.
 */
export async function findDeviceUploads(target: UploadTarget, signal?: AbortSignal): Promise<DeviceUploads> {
  const uploads = await listUploads(signal);
  return {
    own: uploads.find((u) => u.target === target) ?? null,
    other: uploads.find((u) => u.target !== target) ?? null,
  };
}

/** The other processor's upload that keeps this screen from uploading, with its delete. */
export function OtherUpload({ upload, disabled, onDelete }: { upload: Upload; disabled: boolean; onDelete: () => void }) {
  return (
    <div className="notice notice-warning" role="alert" data-testid="other-upload">
      <p>
        {t('update.other_upload', {
          target: t(`update.target.${upload.target}`),
          filename: upload.filename,
          state: t(`update.upload_state.${upload.state}`),
        })}
      </p>
      <div className="actions">
        <button type="button" className="button-secondary" disabled={disabled} onClick={onDelete}>
          {t('update.other_upload_delete')}
        </button>
      </div>
    </div>
  );
}
