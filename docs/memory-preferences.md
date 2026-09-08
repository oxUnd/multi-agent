# Memory and preference behavior

Explicit preferences have one authoritative, versioned store. Model-extracted
facts and rules are advisory candidates; they cannot overwrite explicit settings.
The resolved settings are injected before each model request, including requests
after a streaming interruption or a preference tool call. They apply to progress
updates, plans, questions and final replies.

## Scope and updates

- `personal`: shared across sessions belonging to the same user, default for
  explicit requests such as “以后都用中文”.
- `project`: the runtime's resolved working directory; requires an explicit
  project restriction.
- `session`: only the current session; requires an explicit session restriction.
- Requests such as “这次用英文” remain in the current task and are not persisted.

Within explicit settings, session overrides project, which overrides personal.
The newest revision of a key replaces the previous revision **within its scope**.
Removing an override restores inheritance. Different users never share personal
preferences. Existing sessions cannot be rebound to a different user.

Direct input ingestion handles conservative language, detail and preferred-name
directives without an extra model call. Other explicit preferences use the
`memory_preference` tool with a stable semantic key, scope and evidence from the
latest actual user message. Ambiguous text, questions, complaints, translations
and quoted instructions do not automatically become preferences. Commands provide
an exact route independent of natural-language interpretation.

```text
/memory set language Chinese
/memory set language English project
/memory set response.detail concise
/memory set code.indent tabs project
/memory unset language session
/memory show
/memory history
/memory explain
/memory jobs
```

Values containing spaces can be quoted. `language` / `preferred_language` map to
`response.language`, `style` / `response_style` map to `response.detail`.
History retains revisions, deletions and source text. `explain` shows the current
resolved value and competing scope history. A saved personal value may be masked
by a session override; command output shows what is actually effective.

`/memory clear all` clears the current session's factual/episodic memory,
candidates and preference overrides. It retains personal and project settings;
use `unset` with the relevant scope to remove those. A tombstone prevents replay
of the original input from restoring a deleted preference.

## Persistence and recovery

Preferences are committed while accepting input, before requesting an answer.
Task cancellation or failure does not discard the accepted preference. Input
event tokens make retries idempotent. Persisted user-message order wins even if
an older input commits later; the input recognizer and preference tool share the
same event identity and reject contradictory interpretations of one input.
Background extraction never calls the
preference write path.

Advisory extraction jobs are persisted before enqueueing. The next accepted input
recovers queued work and jobs whose worker process has exited. Failed jobs retry
on subsequent inputs after a 30-second delay, up to three attempts. `/memory jobs`
distinguishes queued, running, completed, failed and cancelled work. A queued event
is not an assertion that extraction has completed. Episode writes are idempotent
per job, including recovery after an interrupted completion marker.

Clearing session memory advances its generation and cancels older jobs. A result
that returns after a clear cannot reinsert facts, candidates or episodes.

## Existing data

Migration is additive and runs when a session first accepts input. Existing
facts, profile text and rules remain inspectable as session archives. A language
preference migrates only when its original user text proves a persistent positive
directive. Its original session scope is retained; a newly explicit setting takes
precedence over a migrated setting. Other uncertain records remain candidates.
No legacy record is automatically promoted to a personal setting.

Profiles and old standing rules are not independently injected as instructions.
Current explicit preferences are resolved first, followed by current factual
memory; episodes and historical changes are retrieved when relevant. There is a
512-byte minimum budget for explicit settings so a tiny general memory budget
cannot silently omit the reply language. Read failures in the authoritative
preference store fail the request rather than silently dropping the settings.

## Verification

`ctest --test-dir build --output-on-failure` includes memory unit tests, runtime
tool tests and real CLI PTY coverage (Python dependencies: `pexpect`, `pyte`).
The PTY test checks captured HTTP model requests, streaming steering, restart,
temporary language requests, scope inheritance, deletion, terminal resize and
composer prompt behavior. Memory tests cover delayed extraction, deletion during
extraction, multi-connection writes, migration, user isolation and durable retry.
