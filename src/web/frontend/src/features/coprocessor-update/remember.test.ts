import { afterEach, describe, expect, it, vi } from 'vitest';

import { recall, remember, STORAGE_KEY } from './remember';

afterEach(() => {
  window.localStorage.clear();
});

describe('what the screen remembers across a reload', () => {
  it('keeps the upload and the install job, and forgets them', () => {
    expect(recall()).toEqual({ uploadId: null, installJobId: null });
    remember({ uploadId: 'upload_0001' });
    remember({ installJobId: 'job_0100' });
    expect(recall()).toEqual({ uploadId: 'upload_0001', installJobId: 'job_0100' });
    remember({ installJobId: null });
    expect(recall()).toEqual({ uploadId: 'upload_0001', installJobId: null });
    remember({ uploadId: null });
    expect(window.localStorage.getItem(STORAGE_KEY)).toBeNull();
  });

  it('ignores what is not an id', () => {
    window.localStorage.setItem(STORAGE_KEY, JSON.stringify({ uploadId: '../etc', installJobId: 7 }));
    expect(recall()).toEqual({ uploadId: null, installJobId: null });
    window.localStorage.setItem(STORAGE_KEY, '{not json');
    expect(recall()).toEqual({ uploadId: null, installJobId: null });
  });

  it('works without storage', () => {
    vi.spyOn(Storage.prototype, 'getItem').mockImplementation(() => {
      throw new DOMException('denied', 'SecurityError');
    });
    vi.spyOn(Storage.prototype, 'setItem').mockImplementation(() => {
      throw new DOMException('denied', 'SecurityError');
    });
    expect(recall()).toEqual({ uploadId: null, installJobId: null });
    expect(() => remember({ uploadId: 'upload_0001' })).not.toThrow();
  });
});
