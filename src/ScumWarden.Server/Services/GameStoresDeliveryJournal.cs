using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace ScumWarden.Server.Services;

internal enum GameStoresJournalState
{
    None,
    Reserved,
    Dispatching,
    Delivered,
    Uncertain,
    Corrupt
}

internal sealed record GameStoresJournalEntry(
    string Key,
    string Hash,
    string BucketId,
    string SteamId,
    string Name,
    string Source,
    string Owner,
    DateTimeOffset Utc);

internal sealed record GameStoresJournalSnapshot(GameStoresJournalState State, GameStoresJournalEntry? Entry);

/// <summary>
/// Immutable, cross-owner delivery evidence for non-idempotent GameStores grants.
/// Marker files deliberately never use the mutable PluginConfigStore state writers:
/// a stale or damaged marker must block reissue rather than fall back to a .live file.
/// </summary>
internal sealed class GameStoresDeliveryJournal
{
    internal const string Schema = "scum-nedjin-gamestores-delivery-journal-v1";
    internal const string DirectoryName = "gamestores-delivery-journal";
    internal const string OperationLockFileName = "operation.lock";

    private static readonly GameStoresJournalState[] MarkerStates =
    {
        GameStoresJournalState.Reserved,
        GameStoresJournalState.Dispatching,
        GameStoresJournalState.Delivered,
        GameStoresJournalState.Uncertain
    };

    private readonly string _directory;

    public GameStoresDeliveryJournal(PluginConfigStore store)
    {
        _directory = Path.Combine(store.StateRootPath, DirectoryName);
    }

    public async Task<IDisposable> AcquireOperationLeaseAsync(CancellationToken cancellationToken)
    {
        Directory.CreateDirectory(_directory);
        var deadline = DateTimeOffset.UtcNow.AddSeconds(30);
        var path = Path.Combine(_directory, OperationLockFileName);
        Exception? lastError = null;

        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            try
            {
                // FileShare.None is a cross-process lease shared with the native runtime.
                // Unlike Mutex, this handle is safe to hold across async continuations.
                return new FileStream(
                    path,
                    FileMode.OpenOrCreate,
                    FileAccess.ReadWrite,
                    FileShare.None,
                    bufferSize: 1,
                    FileOptions.Asynchronous | FileOptions.WriteThrough);
            }
            catch (IOException ex)
            {
                lastError = ex;
            }
            catch (UnauthorizedAccessException ex)
            {
                lastError = ex;
            }

            if (DateTimeOffset.UtcNow >= deadline)
            {
                throw new IOException("GameStores operation lease is busy or unavailable; delivery was not started.", lastError);
            }

            await Task.Delay(50, cancellationToken);
        }
    }

    public GameStoresJournalEntry CreateEntry(
        string key,
        string bucketId,
        string steamId,
        string name,
        string source,
        string owner)
    {
        var canonicalKey = CanonicalizeKey(key);
        if (canonicalKey.Length == 0)
        {
            throw new InvalidOperationException("GameStores journal requires a stable order key.");
        }

        return new GameStoresJournalEntry(
            canonicalKey,
            HashKey(canonicalKey),
            (bucketId ?? string.Empty).Trim(),
            (steamId ?? string.Empty).Trim(),
            (name ?? string.Empty).Trim(),
            (source ?? string.Empty).Trim(),
            (owner ?? string.Empty).Trim(),
            DateTimeOffset.UtcNow);
    }

    public GameStoresJournalSnapshot Inspect(string key)
    {
        var canonicalKey = CanonicalizeKey(key);
        if (canonicalKey.Length == 0)
        {
            return new GameStoresJournalSnapshot(GameStoresJournalState.Corrupt, null);
        }

        var hash = HashKey(canonicalKey);
        var found = new Dictionary<GameStoresJournalState, GameStoresJournalEntry>();
        var corrupt = false;
        foreach (var state in MarkerStates)
        {
            var path = MarkerPath(hash, state);
            if (!File.Exists(path))
            {
                continue;
            }

            if (!TryReadEntry(path, state, canonicalKey, hash, out var entry))
            {
                corrupt = true;
                continue;
            }

            found[state] = entry;
        }

        // A delivered marker is terminal proof even if an earlier transition marker
        // is stale or damaged. It must prevent reissue and let confirmation proceed.
        if (found.TryGetValue(GameStoresJournalState.Delivered, out var delivered))
        {
            return new GameStoresJournalSnapshot(GameStoresJournalState.Delivered, delivered);
        }

        if (corrupt)
        {
            return new GameStoresJournalSnapshot(GameStoresJournalState.Corrupt, null);
        }

        foreach (var state in new[]
        {
            GameStoresJournalState.Uncertain,
            GameStoresJournalState.Dispatching,
            GameStoresJournalState.Reserved
        })
        {
            if (found.TryGetValue(state, out var entry))
            {
                return new GameStoresJournalSnapshot(state, entry);
            }
        }

        return new GameStoresJournalSnapshot(GameStoresJournalState.None, null);
    }

    public bool TryWrite(GameStoresJournalEntry entry, GameStoresJournalState state, string reason = "")
    {
        if (state is GameStoresJournalState.None or GameStoresJournalState.Corrupt)
        {
            return false;
        }

        Directory.CreateDirectory(_directory);
        var path = MarkerPath(entry.Hash, state);
        var payload = JsonSerializer.Serialize(new
        {
            schema = Schema,
            state = StateName(state),
            key = entry.Key,
            hash = entry.Hash,
            bucketId = entry.BucketId,
            steamId = entry.SteamId,
            name = entry.Name,
            source = entry.Source,
            owner = entry.Owner,
            utc = entry.Utc,
            reason = reason ?? string.Empty
        });

        var temporary = path + ".tmp." + Environment.ProcessId + "." + Guid.NewGuid().ToString("N");
        try
        {
            var bytes = new UTF8Encoding(encoderShouldEmitUTF8Identifier: false).GetBytes(payload);
            using (var stream = new FileStream(
                       temporary,
                       FileMode.CreateNew,
                       FileAccess.Write,
                       FileShare.None,
                       bufferSize: 4096,
                       FileOptions.WriteThrough))
            {
                stream.Write(bytes, 0, bytes.Length);
                stream.Flush(true);
            }

            // Never replace an existing marker. A winner owns its immutable state.
            File.Move(temporary, path, overwrite: false);
            return true;
        }
        catch (IOException)
        {
            return false;
        }
        catch (UnauthorizedAccessException)
        {
            return false;
        }
        finally
        {
            try
            {
                if (File.Exists(temporary))
                {
                    File.Delete(temporary);
                }
            }
            catch (IOException)
            {
            }
            catch (UnauthorizedAccessException)
            {
            }
        }
    }

    public IReadOnlyList<GameStoresJournalEntry> DeliveredEntries()
    {
        if (!Directory.Exists(_directory))
        {
            return Array.Empty<GameStoresJournalEntry>();
        }

        var result = new List<GameStoresJournalEntry>();
        try
        {
            foreach (var path in Directory.EnumerateFiles(_directory, "*.delivered.json", SearchOption.TopDirectoryOnly))
            {
                var fileName = Path.GetFileName(path);
                var suffix = ".delivered.json";
                if (!fileName.EndsWith(suffix, StringComparison.Ordinal) || fileName.Length <= suffix.Length)
                {
                    continue;
                }

                var hash = fileName[..^suffix.Length];
                if (TryReadEntry(path, GameStoresJournalState.Delivered, expectedKey: null, expectedHash: hash, out var entry))
                {
                    result.Add(entry);
                }
            }
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }

        return result;
    }

    public static string CanonicalizeKey(string? key)
    {
        var value = key ?? string.Empty;
        var first = 0;
        var last = value.Length;
        while (first < last && IsAsciiWhitespace(value[first]))
        {
            first++;
        }

        while (last > first && IsAsciiWhitespace(value[last - 1]))
        {
            last--;
        }

        var builder = new StringBuilder(last - first);
        for (var index = first; index < last; index++)
        {
            var character = value[index];
            builder.Append(character is >= 'A' and <= 'Z' ? (char)(character + ('a' - 'A')) : character);
        }

        return builder.ToString();
    }

    public static string HashKey(string canonicalKey) =>
        Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(canonicalKey ?? string.Empty)));

    private string MarkerPath(string hash, GameStoresJournalState state) =>
        Path.Combine(_directory, hash + MarkerSuffix(state));

    private static string MarkerSuffix(GameStoresJournalState state) => state switch
    {
        GameStoresJournalState.Reserved => ".reserved.json",
        GameStoresJournalState.Dispatching => ".dispatching.json",
        GameStoresJournalState.Delivered => ".delivered.json",
        GameStoresJournalState.Uncertain => ".uncertain.json",
        _ => string.Empty
    };

    private static bool TryReadEntry(
        string path,
        GameStoresJournalState expectedState,
        string? expectedKey,
        string expectedHash,
        out GameStoresJournalEntry entry)
    {
        entry = default!;
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(path, Encoding.UTF8));
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object ||
                !String(root, "schema").Equals(Schema, StringComparison.Ordinal) ||
                !String(root, "state").Equals(StateName(expectedState), StringComparison.Ordinal) ||
                !String(root, "hash").Equals(expectedHash, StringComparison.Ordinal) ||
                !DateTimeOffset.TryParse(String(root, "utc"), out var utc))
            {
                return false;
            }

            var key = CanonicalizeKey(String(root, "key"));
            if (key.Length == 0 || !HashKey(key).Equals(expectedHash, StringComparison.Ordinal) ||
                (expectedKey is not null && !key.Equals(expectedKey, StringComparison.Ordinal)))
            {
                return false;
            }

            entry = new GameStoresJournalEntry(
                key,
                expectedHash,
                String(root, "bucketId"),
                String(root, "steamId"),
                String(root, "name"),
                String(root, "source"),
                String(root, "owner"),
                utc);
            return true;
        }
        catch (IOException)
        {
            return false;
        }
        catch (UnauthorizedAccessException)
        {
            return false;
        }
        catch (JsonException)
        {
            return false;
        }
    }

    private static string String(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? string.Empty
            : string.Empty;

    private static string StateName(GameStoresJournalState state) => state switch
    {
        GameStoresJournalState.Reserved => "reserved",
        GameStoresJournalState.Dispatching => "dispatching",
        GameStoresJournalState.Delivered => "delivered",
        GameStoresJournalState.Uncertain => "uncertain",
        _ => string.Empty
    };

    private static bool IsAsciiWhitespace(char character) =>
        character == ' ' || character is >= '\t' and <= '\r';
}
