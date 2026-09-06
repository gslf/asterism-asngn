"""Exact counters and cross-field invariants for the consumption projection."""
import re
from .errors import ProtocolError
from .types import Consumption

FIELDS = ("calls", "unsettled_calls", "unknown_calls", "failed_calls", "cancelled_calls",
          "known_input_tokens", "known_output_tokens", "unsettled_tokens", "unknown_tokens", "charged_tokens")


def totals(value):
    if not isinstance(value, dict) or set(value) != set(FIELDS):
        raise ProtocolError("invalid consumption totals")
    out = {}
    for name in FIELDS:
        text = value[name]
        if (not isinstance(text, str) or not re.fullmatch(r"0|[1-9][0-9]{0,18}", text)
                or int(text) > 2**63 - 1):
            raise ProtocolError("invalid consumption counter")
        out[name] = int(text)
    settled = out["calls"] - out["unsettled_calls"]
    if (settled < out["unknown_calls"] or settled < out["failed_calls"] + out["cancelled_calls"]
            or out["charged_tokens"] != sum(out[key] for key in FIELDS[5:9])
            or (not out["unsettled_calls"] and out["unsettled_tokens"])
            or (not out["unknown_calls"] and out["unknown_tokens"])
            or (settled == out["unknown_calls"] and (out["known_input_tokens"] or out["known_output_tokens"]))):
        raise ProtocolError("inconsistent consumption totals")
    return out


def consumption(value) -> Consumption:
    if (not isinstance(value, dict) or set(value) != {"schema", "scope", "unit", "time_basis", "utc_day", "lifetime", "today"}
            or type(value["schema"]) is not int or value["schema"] != 1
            or value["scope"] != "engine_store" or value["unit"] != "tokens"
            or value["time_basis"] != "reservation_utc_day" or type(value["utc_day"]) is not int
            or abs(value["utc_day"]) > (2**63 - 1)//86400):
        raise ProtocolError("invalid consumption snapshot")
    lifetime, today = totals(value["lifetime"]), totals(value["today"])
    if any(today[key] > lifetime[key] for key in FIELDS):
        raise ProtocolError("daily consumption exceeds lifetime totals")
    return {**value, "lifetime": lifetime, "today": today}
