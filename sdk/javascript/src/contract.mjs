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


export function taskRecord(value, identity) {
  const state = value.state, committed = value.turn_committed, action = value.action_id;
  if (value.task_id !== identity || !['interrupted', 'turn_committed', 'finished'].includes(state) ||
      typeof committed !== 'boolean' || typeof value.action_uncertain !== 'boolean' ||
      value.execution_resumed !== false || value.events_replayed !== false || !integer(value.admitted_work_revision)) {
    throw new ProtocolError('invalid durable task lifecycle');
  }
  if (!['input', 'answer', 'action_id', 'last_action', 'last_observation'].every(k => typeof value[k] === 'string') ||
      !states.has(value.task_state)) throw new ProtocolError('missing durable evidence');
  if ((state === 'interrupted' && committed) || (state === 'turn_committed' && !committed) ||
      (action && !/^[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$/.test(action)) ||
      (!action && (value.action_uncertain || value.last_action || value.last_observation))) {
    throw new ProtocolError('inconsistent durable state');
  }
  if (state === 'finished') {
    if (typeof value.outcome !== 'string' || !/^ASNGN_(OK|ERR_(IO|PARSE|CONFIG|MODEL|NOT_FOUND|INVALID|DENIED|TIMEOUT|CANCELLED|BUSY|PROTOCOL|CONTEXT|UNSUPPORTED|SIBLING|NOMEM|LIMIT))$/.test(value.outcome) ||
        (value.outcome === 'ASNGN_OK' && !committed)) throw new ProtocolError('invalid durable outcome');
  } else if ('outcome' in value) throw new ProtocolError('unfinished task has a terminal outcome');
  return value;
}
