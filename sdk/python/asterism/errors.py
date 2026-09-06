"""Errors distinguish transport uncertainty from an observed engine failure."""


class TransportError(Exception):
    """The connection cannot deliver a trustworthy reply."""


class ProtocolError(TransportError):
    """The peer violated the negotiated wire contract."""


class RequestTimeout(TimeoutError):
    """Local wait expired. The remote operation may still be running."""


class RpcError(Exception):
    def __init__(self, code: int, message: str, data=None):
        super().__init__(message)
        self.code, self.data = code, data


class ToolError(Exception):
    def __init__(self, payload: dict):
        super().__init__(payload.get("message") or payload["error"])
        self.code, self.payload = payload["error"], payload
