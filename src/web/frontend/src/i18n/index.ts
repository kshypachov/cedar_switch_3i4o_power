import { type MessageKey, ru } from './ru';

export type { MessageKey };

type Params = Record<string, string | number>;

/** The text for @p key, with {name} placeholders filled from @p params. */
export function t(key: MessageKey, params?: Params): string {
  const template: string = ru[key];
  if (!params) return template;
  return template.replace(/\{(\w+)\}/g, (whole, name: string) =>
    name in params ? String(params[name]) : whole,
  );
}

/** A key that may not exist, e.g. an error code the device added later. */
export function tMaybe(key: string): string | undefined {
  return (ru as Record<string, string>)[key];
}

export function isMessageKey(key: string): key is MessageKey {
  return key in ru;
}
