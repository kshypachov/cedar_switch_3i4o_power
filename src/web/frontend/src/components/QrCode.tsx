import { encode } from 'uqr';

import { t } from '../i18n';

/**
 * The QR code for @p payload, drawn by the browser (plan section 6: the SDK
 * builds the payload, the page builds the picture). One SVG path, no markup
 * from a string, so the payload can never become HTML.
 */
export function QrCode({ payload }: { payload: string }) {
  const { data, size } = encode(payload, { ecc: 'M', border: 2 });
  let path = '';
  data.forEach((row, y) =>
    row.forEach((dark, x) => {
      if (dark) path += `M${x} ${y}h1v1h-1z`;
    }),
  );
  return (
    <svg
      className="qr"
      role="img"
      aria-label={t('matter.qr_label')}
      viewBox={`0 0 ${size} ${size}`}
      shapeRendering="crispEdges"
      data-payload={payload}
    >
      <rect width={size} height={size} fill="#ffffff" />
      <path d={path} fill="#000000" />
    </svg>
  );
}
