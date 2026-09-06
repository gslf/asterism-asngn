"""Public data is ordinary JSON; annotations add no runtime dependency."""
from typing import Literal, NotRequired, TypedDict

TaskState = Literal["unconfirmed", "unavailable", "superseded", "succeeded", "incomplete"]


class Criterion(TypedDict):
    id: str
    requirement: str
    command: Literal["build", "test", "lint", "diagnostics"]
    path: str
    adapter: Literal["auto", "cmake", "cargo", "npm", "python"]
    depends_on: int


class Definition(TypedDict):
    goal: str
    constraints: str
    criteria: list[Criterion]


class Proof(Criterion):
    status: str
    action_id: str
    snapshot: str
    receipt_sha256: str


class WorkState(TypedDict):
    task_state: TaskState
    work_revision: NotRequired[int]
    work_sequence: NotRequired[int]
    goal: NotRequired[str]
    constraints: NotRequired[str]
    criteria: NotRequired[list[Proof]]


class Event(TypedDict):
    cursor: int
    kind: Literal[0, 1, 2]
    text: str
    truncated: bool


class Poll(TypedDict):
    task_id: str
    done: bool
    cursor_gap: bool
    next_cursor: int
    events: list[Event]
    outcome: NotRequired[str]
    answer: NotRequired[str]
    task_state: NotRequired[TaskState]
    work_revision: NotRequired[int]
    work_sequence: NotRequired[int]
    goal: NotRequired[str]
    constraints: NotRequired[str]
    criteria: NotRequired[list[Proof]]


class Approval(TypedDict):
    status: Literal["none", "pending", "approved", "denied", "consumed", "invalidated", "interrupted"]
    id: NotRequired[str]
    sequence: NotRequired[int]
    session_wide: NotRequired[bool]
    profile: NotRequired[str]
    turn_id: NotRequired[str]
    tool_ref: NotRequired[str]
    command: NotRequired[str]
    arguments: NotRequired[str]
    arguments_sha256: NotRequired[str]
    package_sha256: NotRequired[str]
    snapshot: NotRequired[str]
    workspace: NotRequired[str]


class TaskRecord(WorkState):
    task_id: str
    state: Literal["interrupted", "turn_committed", "finished"]
    turn_committed: bool
    action_uncertain: bool
    admitted_work_revision: int
    execution_resumed: Literal[False]
    events_replayed: Literal[False]
    input: str
    answer: str
    action_id: str
    last_action: str
    last_observation: str
    outcome: NotRequired[str]
