import { describe, expect, it } from 'vitest';

import { compareVersions, parseVersion } from './version';

describe('MCUboot image versions', () => {
  it('reads major.minor.revision+build', () => {
    expect(parseVersion('1.2.3+4')).toEqual({ major: 1, minor: 2, revision: 3, build: 4 });
    expect(parseVersion('0.0.1')).toEqual({ major: 0, minor: 0, revision: 1, build: 0 });
    expect(parseVersion(' 255.255.65535+4294967295 ')).toEqual({ major: 255, minor: 255, revision: 65535, build: 4294967295 });
  });

  it.each(['', '1', '1.2', 'v1.2.3', '1.2.3+', '1.2.3-rc1', '256.0.0', '0.256.0', '0.0.65536', '1.2.3+4294967296', '1.2.3.4'])(
    'refuses %j',
    (text) => {
      expect(parseVersion(text)).toBeNull();
    },
  );

  it.each([
    ['1.0.1+0', '1.0.0+0', 'newer'],
    ['1.1.0+0', '1.0.9+0', 'newer'],
    ['2.0.0+0', '1.9.9+9', 'newer'],
    ['1.0.0+1', '1.0.0+0', 'newer'],
    ['1.0.0+0', '1.0.0', 'same'],
    ['1.0.0+0', '1.0.0+0', 'same'],
    ['1.0.0+0', '1.0.1+0', 'older'],
    ['1.0.9+0', '1.1.0+0', 'older'],
    ['1.9.9+9', '2.0.0+0', 'older'],
    ['1.0.0+0', '1.0.0+1', 'older'],
    ['10.0.0', '9.0.0', 'newer'],
    ['1', '1.0.0', 'unknown'],
    ['1.0.0', 'd30ae91-dirty', 'unknown'],
  ] as const)('%s against running %s is %s', (candidate, running, expected) => {
    expect(compareVersions(candidate, running)).toBe(expected);
  });
});
