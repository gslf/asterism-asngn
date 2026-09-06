"""Validate lifecycle and cursor fields before they guide a caller's decisions."""
import re
from .errors import ProtocolError, ToolError
from .transport import decode

STATES = {"unconfirmed", "unavailable", "superseded", "succeeded", "incomplete"}


def integer(value):
    return type(value) is int and 0 <= value <= 9007199254740991


def payload(result):
    if not isinstance(result, dict) or type(result.get("isError")) is not bool:
        raise ProtocolError("invalid tool result")
    content = result.get("content")
    if not isinstance(content, list) or len(content) != 1 or not isinstance(content[0], dict) or content[0].get("type") != "text":
        raise ProtocolError("expected one JSON text result")
    try:
        value = decode(content[0]["text"])
    except (KeyError, TypeError, ValueError, RecursionError) as error:
        raise ProtocolError("invalid tool JSON") from error
    if not isinstance(value, dict):
        raise ProtocolError("tool payload must be an object")
    if result["isError"]:
        if not isinstance(value.get("error"), str) or not isinstance(value.get("message"), str):
            raise ProtocolError("invalid tool failure")
        raise ToolError(value)
    return value


def poll(value, identity, cursor):
    if value.get("task_id") != identity or type(value.get("done")) is not bool or type(value.get("cursor_gap")) is not bool:
        raise ProtocolError("invalid task lifecycle")
    end, events = value.get("next_cursor"), value.get("events")
    if not integer(end) or end < cursor or not isinstance(events, list) or len(events) > 256:
        raise ProtocolError("invalid event page")
    start = end - len(events)
    if (start > cursor) != value["cursor_gap"] or start < cursor:
        raise ProtocolError("inconsistent cursor gap")
    for index, event in enumerate(events):
        if not isinstance(event, dict) or not integer(event.get("cursor")) or event["cursor"] != start + index or \
                type(event.get("kind")) is not int or event["kind"] not in (0, 1, 2) or \
                not isinstance(event.get("text"), str) or type(event.get("truncated")) is not bool:
            raise ProtocolError("invalid stream event")
    if value["done"] and (not isinstance(value.get("outcome"), str) or
                          not isinstance(value.get("answer"), str) or (not isinstance(value.get("task_state"), str) or value["task_state"] not in STATES)):
        raise ProtocolError("missing terminal outcome")
    return value


def task_record(value, identity):
    """A journal observation must never be mistaken for a running task or a replay."""
    state = value.get("state")
    if value.get("task_id") != identity or state not in ("interrupted", "turn_committed", "finished") or \
            any(type(value.get(k)) is not bool for k in ("turn_committed", "action_uncertain")) or \
            value.get("execution_resumed") is not False or value.get("events_replayed") is not False or \
            not integer(value.get("admitted_work_revision")):
        raise ProtocolError("invalid durable task lifecycle")
    if any(not isinstance(value.get(k), str) for k in ("input", "answer", "action_id", "last_action", "last_observation")) or \
            not isinstance(value.get("task_state"), str) or value["task_state"] not in STATES:
        raise ProtocolError("missing durable evidence")
    committed, action = value["turn_committed"], value["action_id"]
    if (state == "interrupted" and committed) or (state == "turn_committed" and not committed) or \
            (action and not re.fullmatch(r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}", action)) or \
            (not action and (value["action_uncertain"] or value["last_action"] or value["last_observation"])):
        raise ProtocolError("inconsistent durable state")
    if state == "finished":
        outcome = value.get("outcome")
        if not isinstance(outcome, str) or not re.fullmatch(
                r"ASNGN_(OK|ERR_(IO|PARSE|CONFIG|MODEL|NOT_FOUND|INVALID|DENIED|TIMEOUT|CANCELLED|BUSY|PROTOCOL|CONTEXT|UNSUPPORTED|SIBLING|NOMEM|LIMIT))", outcome) or \
                (outcome == "ASNGN_OK" and not committed):
            raise ProtocolError("invalid durable outcome")
    elif "outcome" in value:
        raise ProtocolError("unfinished task has a terminal outcome")
    return value
