export type Json = null | boolean | number | string | Json[] | { [key: string]: Json };
export type TaskState = 'unconfirmed' | 'unavailable' | 'superseded' | 'succeeded' | 'incomplete';
export interface Criterion {
  id: string;
  requirement: string;
  command: 'build' | 'test' | 'lint' | 'diagnostics';
  path: string;
  adapter: 'auto' | 'cmake' | 'cargo' | 'npm' | 'python';
  /** Bit mask of earlier criteria (bit zero refers to criteria[0]). */
  depends_on: number;
}
export interface Definition { goal: string; constraints: string; criteria: Criterion[] }
export interface Proof extends Criterion { status: string; action_id: string; snapshot: string; receipt_sha256: string }
export interface WorkState {
  task_state: TaskState;
  work_revision?: number;
  work_sequence?: number;
  goal?: string;
  constraints?: string;
  criteria?: Proof[];
}
export interface Event { cursor: number; kind: 0 | 1 | 2; text: string; truncated: boolean }
export interface Poll extends Partial<WorkState> {
  task_id: string;
  done: boolean;
  cursor_gap: boolean;
  next_cursor: number;
  events: Event[];
  outcome?: string;
  answer?: string;
}
/** Archived evidence, not a resumed execution or a replayable event cursor. */
export interface TaskRecord extends WorkState {
  task_id: string;
  state: 'interrupted' | 'turn_committed' | 'finished';
  turn_committed: boolean;
  action_uncertain: boolean;
  admitted_work_revision: number;
  execution_resumed: false;
  events_replayed: false;
  input: string;
  answer: string;
  action_id: string;
  last_action: string;
  last_observation: string;
  outcome?: string;
}
export interface Approval {
  status: 'none' | 'pending' | 'approved' | 'denied' | 'consumed' | 'invalidated' | 'interrupted';
  id?: string;
  sequence?: number;
  session_wide?: boolean;
  profile?: string;
  turn_id?: string;
  tool_ref?: string;
  command?: string;
  arguments?: string;
  arguments_sha256?: string;
  package_sha256?: string;
  snapshot?: string;
  workspace?: string;
}
/** All JavaScript durations are milliseconds. Aborting a wait does not cancel the engine task. */
export interface RequestOptions { timeout?: number; signal?: AbortSignal }
export interface StartOptions extends RequestOptions { cwd?: string; env?: Record<string, string> }
export interface WatchOptions extends RequestOptions { cursor?: number; interval?: number }
export class TransportError extends Error {}
export class ProtocolError extends TransportError {}
export class RequestTimeout extends Error {}
export class RpcError extends Error { code: number; data?: Json }
export class ToolError extends Error { code: string; payload: Record<string, Json> }
export class Client {
  private constructor();
  static start(command: string[], options?: StartOptions): Promise<Client>;
  readonly serverInfo: { name: string; version: string };
  readonly stderr: string;
  close(): Promise<void>;
  [Symbol.asyncDispose](): Promise<void>;
  callTool(name: string, args: Record<string, Json>, options?: RequestOptions): Promise<Record<string, Json>>;
  session(slug?: string): Session;
  task(id: string): Task;
}
export class Session {
  private constructor();
  readonly client: Client;
  readonly slug: string;
  submit(message: string, options?: RequestOptions): Promise<Task>;
  recover(taskId: string, options?: RequestOptions): Promise<TaskRecord>;
  work(): Promise<WorkState>;
  defineWork(expectedRevision: number, definition: Definition): Promise<WorkState>;
  invalidateWork(expectedRevision: number): Promise<WorkState>;
  approval(): Promise<Approval>;
}
export class Task {
  private constructor();
  readonly client: Client;
  readonly id: string;
  poll(cursor?: number, options?: RequestOptions): Promise<Poll>;
  cancel(): Promise<Poll>;
  release(): Promise<Poll>;
  updates(options?: WatchOptions): AsyncGenerator<Poll>;
  wait(options?: WatchOptions): Promise<Poll>;
}
