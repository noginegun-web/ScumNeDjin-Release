# Safe player-aware automation

Automatic modules must act on a real, server-resolved player rather than on a
panel row, a stale log identity or a guessed runtime object. This project uses a
bounded bridge path for that purpose:

```text
SCUM login evidence -> settle delay -> one strict GameThread cache seed
                 -> GameState.PlayerArray -> live player cache -> automation
```

## Invariants

- The login signal only schedules work; it never performs Unreal reflection on
  the log-processing thread.
- After the settle delay, the bridge runs one coalesced cache seed on the strict
  game-thread path and reads only `GameState.PlayerArray`.
- A later login supersedes an older pending seed. The seed has a hard limit of
  one attempt; failures are logged and fail closed.
- Global object enumeration, periodic live-player scans and cache refreshes
  triggered merely by opening the panel remain disabled.
- A module that needs a live target reads the cache and skips safely when the
  player is absent. It must not compensate by starting its own broad scan.

## Applying the pattern

Use the cache only after the bridge reports runtime-ready state. Keep durable
queues and delivery state independent from the runtime cache, so a restart or a
temporarily absent player cannot turn into a duplicate delivery. When a player
is not currently resolvable, retain or expire the durable state according to
that module's explicit business rule; do not infer a gameplay outcome from a
transport-level success response.

## Verification

`/api/health` and `/api/status` prove bridge liveness, not gameplay success.
For a player-sensitive automatic action, require all of the following:

1. a fresh heartbeat and the expected bridge version;
2. a bridge log line showing the bounded cache seed;
3. the module's own start/result log record; and
4. client-visible confirmation where the action changes gameplay.

Before changing this path, test against a backup on a controlled server. Do not
replace it with a broad runtime scan simply because a panel query appears to
work.
