namespace ScumWarden.Server.Services;

public sealed class WardenEventStore
{
    private readonly object _gate = new();
    private readonly LinkedList<WardenEvent> _events = new();

    public void Add(WardenEvent entry)
    {
        lock (_gate)
        {
            if (IsDuplicate(entry))
            {
                return;
            }

            _events.AddFirst(entry);
            while (_events.Count > 5000)
            {
                _events.RemoveLast();
            }
        }
    }

    public IReadOnlyList<WardenEvent> Get(string? type, int limit)
    {
        lock (_gate)
        {
            return _events
                .Where(e => string.IsNullOrWhiteSpace(type) || e.Type.Equals(type, StringComparison.OrdinalIgnoreCase))
                .Take(Math.Clamp(limit, 1, 1000))
                .ToArray();
        }
    }

    public int Clear(string? type)
    {
        lock (_gate)
        {
            if (string.IsNullOrWhiteSpace(type))
            {
                var count = _events.Count;
                _events.Clear();
                return count;
            }

            var removed = 0;
            var node = _events.First;
            while (node is not null)
            {
                var next = node.Next;
                if (node.Value.Type.Equals(type, StringComparison.OrdinalIgnoreCase))
                {
                    _events.Remove(node);
                    removed++;
                }

                node = next;
            }

            return removed;
        }
    }

    private bool IsDuplicate(WardenEvent entry)
    {
        var key = DedupeKey(entry);
        if (key.Length == 0)
        {
            return false;
        }

        var windowSeconds = entry.Type.Equals("kill", StringComparison.OrdinalIgnoreCase) ? 30 : 15;
        return _events
            .Take(200)
            .Any(existing =>
                existing.Type.Equals(entry.Type, StringComparison.OrdinalIgnoreCase) &&
                DedupeKey(existing).Equals(key, StringComparison.OrdinalIgnoreCase) &&
                Math.Abs((existing.TimestampUtc - entry.TimestampUtc).TotalSeconds) <= windowSeconds);
    }

    private static string DedupeKey(WardenEvent entry)
    {
        if (entry.Type.Equals("chat", StringComparison.OrdinalIgnoreCase))
        {
            return ChatKey(entry);
        }

        if (entry.Type.Equals("kill", StringComparison.OrdinalIgnoreCase))
        {
            var killer = FirstDataText(entry, "killerSteamId", "killer", "attackerSteamId", "attacker").ToLowerInvariant();
            var victim = FirstDataText(entry, "victimSteamId", "victim", "targetSteamId", "target").ToLowerInvariant();
            var weapon = DataText(entry, "weapon").ToLowerInvariant();
            var distance = DataText(entry, "distance").ToLowerInvariant();
            var message = (entry.Message ?? "").Trim().ToLowerInvariant();
            return string.Join('|', "kill", killer, victim, weapon, distance, message);
        }

        if (entry.Type.Equals("login", StringComparison.OrdinalIgnoreCase) ||
            entry.Type.Equals("presence", StringComparison.OrdinalIgnoreCase))
        {
            var actor = FirstDataText(entry, "steamId", "steam", "SteamID", "name", "player").ToLowerInvariant();
            var state = DataText(entry, "state").ToLowerInvariant();
            var message = (entry.Message ?? "").Trim().ToLowerInvariant();
            return string.Join('|', "presence", actor, state, message);
        }

        if (entry.Type.Equals("action", StringComparison.OrdinalIgnoreCase))
        {
            var actor = FirstDataText(entry, "steamId", "steam", "SteamID", "name", "player").ToLowerInvariant();
            var category = DataText(entry, "category").ToLowerInvariant();
            var x = DataText(entry, "x");
            var y = DataText(entry, "y");
            var z = DataText(entry, "z");
            var message = (entry.Message ?? "").Trim().ToLowerInvariant();
            return string.Join('|', "action", category, actor, x, y, z, message);
        }

        if (entry.Type.Equals("command", StringComparison.OrdinalIgnoreCase))
        {
            var actor = FirstDataText(entry, "steamId", "steam", "SteamID", "name", "player").ToLowerInvariant();
            var command = FirstDataText(entry, "command", "content").ToLowerInvariant();
            var message = (entry.Message ?? "").Trim().ToLowerInvariant();
            return string.Join('|', "command", actor, command.Length > 0 ? command : message);
        }

        return "";
    }

    private static string ChatKey(WardenEvent entry)
    {
        var message = (entry.Message ?? "").Trim().ToLowerInvariant();
        if (message.Length == 0)
        {
            return "";
        }

        var channel = DataText(entry, "channel").ToLowerInvariant();
        var steam = DataText(entry, "steamId", "steam", "SteamID").ToLowerInvariant();
        var name = DataText(entry, "name", "playerName").ToLowerInvariant();
        return string.Join('|', channel, steam.Length > 0 ? steam : name, message);
    }

    private static string DataText(WardenEvent entry, params string[] keys)
    {
        foreach (var key in keys)
        {
            if (entry.Data.TryGetValue(key, out var value))
            {
                return Convert.ToString(value)?.Trim() ?? "";
            }
        }

        return "";
    }

    private static string FirstDataText(WardenEvent entry, params string[] keys)
    {
        foreach (var key in keys)
        {
            var value = DataText(entry, key);
            if (!string.IsNullOrWhiteSpace(value))
            {
                return value;
            }
        }

        return "";
    }
}
