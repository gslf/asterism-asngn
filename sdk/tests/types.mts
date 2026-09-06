import { Client, type Definition, type Poll, ToolError } from '../javascript/src/index.mjs';

const definition: Definition = { goal: 'Fix', constraints: 'Keep tests', criteria: [{
  id: 'build', requirement: 'Build succeeds', command: 'build', path: '.', adapter: 'cmake', depends_on: 0,
}] };
export async function useClient(): Promise<Poll> {
  const client = await Client.start(['asngn-mcp']);
  try {
    const session = client.session('example');
    await session.defineWork(0, definition);
    const task = await session.submit('Build');
    for await (const page of task.updates({ cursor: 0, timeout: 10_000, signal: new AbortController().signal })) {
      const gap: boolean = page.cursor_gap;
      void gap;
    }
    const result = await task.wait();
    await task.release();
    const archived = await session.recover(task.id);
    const resumed: false = archived.execution_resumed;
    void resumed;
    return result;
  } catch (error) {
    if (error instanceof ToolError) { const code: string = error.code; void code; }
    throw error;
  } finally { await client.close(); }
}
// @ts-expect-error There is no permission-granting SDK method.
Client.prototype.approve();
// @ts-expect-error Cursors are numeric, not opaque strings.
Client.prototype.task('id').poll('old');
