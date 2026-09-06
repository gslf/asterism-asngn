/** JSON decimal strings become bigint; counts never pass through Number. */
import { ProtocolError } from './errors.mjs';

const fields = ['calls', 'unsettled_calls', 'unknown_calls', 'failed_calls', 'cancelled_calls',
  'known_input_tokens', 'known_output_tokens', 'unsettled_tokens', 'unknown_tokens', 'charged_tokens'];
const keys = (value, expected) => value && typeof value === 'object' && !Array.isArray(value) &&
  Object.keys(value).length === expected.length && expected.every(key => Object.hasOwn(value, key));

function totals(value) {
  if (!keys(value, fields)) throw new ProtocolError('invalid consumption totals');
  const out = {};
  for (const name of fields) {
    const text = value[name];
    if (typeof text !== 'string' || /^(0|[1-9][0-9]{0,18})$/.exec(text)?.[0] !== text ||
        BigInt(text) > 9223372036854775807n) throw new ProtocolError('invalid consumption counter');
    out[name] = BigInt(text);
  }
  const settled = out.calls - out.unsettled_calls;
  if (settled < out.unknown_calls || settled < out.failed_calls + out.cancelled_calls ||
      out.charged_tokens !== fields.slice(5, 9).reduce((sum, key) => sum + out[key], 0n) ||
      (out.unsettled_calls === 0n && out.unsettled_tokens !== 0n) ||
      (out.unknown_calls === 0n && out.unknown_tokens !== 0n) ||
      (settled === out.unknown_calls && (out.known_input_tokens !== 0n || out.known_output_tokens !== 0n))) {
    throw new ProtocolError('inconsistent consumption totals');
  }
  return out;
}

export function consumption(value) {
  if (!keys(value, ['schema', 'scope', 'unit', 'time_basis', 'utc_day', 'lifetime', 'today']) ||
      value.schema !== 1 || value.scope !== 'engine_store' || value.unit !== 'tokens' ||
      value.time_basis !== 'reservation_utc_day' || !Number.isSafeInteger(value.utc_day) ||
      Math.abs(value.utc_day) > 106751991167300) throw new ProtocolError('invalid consumption snapshot');
  const lifetime = totals(value.lifetime), today = totals(value.today);
  if (fields.some(key => today[key] > lifetime[key])) throw new ProtocolError('daily consumption exceeds lifetime totals');
  return { ...value, lifetime, today };
}
