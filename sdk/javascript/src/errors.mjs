/** Transport failures do not establish whether a remote operation executed. */
class SDKError extends Error {
  constructor(message) { super(message); this.name = new.target.name; }
}
export class TransportError extends SDKError {}
export class ProtocolError extends TransportError {}
export class RequestTimeout extends SDKError {}
export class RpcError extends SDKError {
  constructor(code, message, data) {
    super(message);
    this.code = code;
    this.data = data;
  }
}
export class ToolError extends SDKError {
  constructor(payload) {
    super(payload.message || payload.error);
    this.code = payload.error;
    this.payload = payload;
  }
}
