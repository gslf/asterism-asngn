import { ProtocolError } from './errors.mjs';

/** Check duplicate keys before JSON.parse discards that information. */
export function decode(text) {
  const tokens = /\s*("(?:[^"\\\x00-\x1f]|\\(?:["\\/bfnrt]|u[0-9a-fA-F]{4}))*"|-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?|true|false|null|[{}\[\],:])/gy;
  const stack = [];
  let position = 0, match;
  while ((match = tokens.exec(text))) {
    position = tokens.lastIndex;
    const token = match[1], top = stack.at(-1);
    if (token === '{' || token === '[') {
      if (stack.length >= 128) throw new ProtocolError('JSON nesting limit exceeded');
      stack.push({ keys: token === '{' ? new Set() : null, key: token === '{' });
    } else if (token === '}' || token === ']') {
      stack.pop();
    } else if (top?.keys) {
      if (token === ',') top.key = true;
      else if (token === ':') top.key = false;
      else if (top.key && token[0] === '"') {
        const key = JSON.parse(token);
        if (top.keys.has(key)) throw new ProtocolError('duplicate JSON key');
        top.keys.add(key);
      }
    }
  }
  if (text.slice(position).trim()) throw new ProtocolError('invalid JSON token');
  try {
    return JSON.parse(text, (_key, value) => {
      if (typeof value === 'number' && !Number.isFinite(value)) throw new Error('non-finite number');
      return value;
    });
  } catch (error) {
    throw new ProtocolError(`invalid JSON: ${error.message}`);
  }
}

export function encode(value) {
  return JSON.stringify(value, (_key, item) => {
    if (item === undefined || typeof item === 'function' || typeof item === 'symbol' ||
        (typeof item === 'number' && !Number.isFinite(item))) throw new TypeError('value is not JSON');
    return item;
  });
}

export function positive(value, name) {
  if (typeof value !== 'number' || !Number.isFinite(value) || value <= 0 || value > 2147483647) {
    throw new TypeError(`${name} must be between 0 and 2147483647 (exclusive of 0)`);
  }
  return value;
}
export const object = value => value !== null && typeof value === 'object' && !Array.isArray(value);
export const integer = value => Number.isSafeInteger(value) && value >= 0;
