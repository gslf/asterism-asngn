import { decode, integer, object } from './json.mjs';
import { ProtocolError, ToolError } from './errors.mjs';

export const states = new Set(['unconfirmed', 'unavailable', 'superseded', 'succeeded', 'incomplete']);

export function payload(result) {
  if (!object(result) || typeof result.isError !== 'boolean' || !Array.isArray(result.content) ||
      result.content.length !== 1 || result.content[0]?.type !== 'text' || typeof result.content[0].text !== 'string') {
    throw new ProtocolError('expected one JSON text result');
  }
  const value = decode(result.content[0].text);
  if (!object(value)) throw new ProtocolError('tool payload must be an object');
  if (result.isError) {
    if (typeof value.error !== 'string' || typeof value.message !== 'string') throw new ProtocolError('invalid tool failure');
    throw new ToolError(value);
  }
  return value;
}

export function poll(value, identity, cursor) {
  if (value.task_id !== identity || typeof value.done !== 'boolean' || typeof value.cursor_gap !== 'boolean') {
    throw new ProtocolError('invalid task lifecycle');
  }
  const end = value.next_cursor, events = value.events;
  if (!integer(end) || end < cursor || !Array.isArray(events) || events.length > 256) throw new ProtocolError('invalid event page');
  const start = end - events.length;
  if ((start > cursor) !== value.cursor_gap || start < cursor) throw new ProtocolError('inconsistent cursor gap');
  events.forEach((event, index) => {
    if (!object(event) || !integer(event.cursor) || event.cursor !== start + index || ![0, 1, 2].includes(event.kind) ||
        typeof event.text !== 'string' || typeof event.truncated !== 'boolean') throw new ProtocolError('invalid stream event');
  });
  if (value.done && (typeof value.outcome !== 'string' || typeof value.answer !== 'string' || !states.has(value.task_state))) {
    throw new ProtocolError('missing terminal outcome');
  }
  return value;
}
