using Microsoft.Data.Sqlite;
using Microsoft.Extensions.Options;
using ScumWarden.Server.Configuration;
using System.Globalization;
using System.Text.RegularExpressions;

namespace ScumWarden.Server.Services;

public sealed record PlayerSkillsReadResult(bool Ok, object? Data, string? Error);

public sealed class ScumDatabaseReader
{
    private static readonly Regex SteamIdRegex = new(@"^\d{16,20}$", RegexOptions.Compiled);
    private readonly IOptionsMonitor<WardenOptions> _options;

    public ScumDatabaseReader(IOptionsMonitor<WardenOptions> options)
    {
        _options = options;
    }

    public StoreStatus GetStatus()
    {
        if (_options.CurrentValue.ClientTestMode)
        {
            return new StoreStatus(true, "client-test", "Client test mode: simulated SCUM.db is available.");
        }

        var path = _options.CurrentValue.DatabasePath;
        return File.Exists(path)
            ? new StoreStatus(true, path, "SCUM.db exists.")
            : new StoreStatus(false, path, "SCUM.db not found.");
    }

    public IReadOnlyList<Dictionary<string, object?>> GetSquads(int limit) =>
        _options.CurrentValue.ClientTestMode ? ClientTestSimulator.Squads() : ReadSquads(limit);

    public object GetSquadsSnapshot(int limit)
    {
        var rows = GetSquads(limit);
        return new
        {
            source = _options.CurrentValue.ClientTestMode ? "client-test" : "SCUM.db squad/squad_member/user_profile",
            readOnly = true,
            rows,
            memberRows = rows.Count(row => row.TryGetValue("memberId", out var memberId) && memberId is not null),
            squads = BuildSquadSummaries(rows),
            generatedAtUtc = DateTimeOffset.UtcNow
        };
    }

    public object GetSquadsDiagnostics(int limit = 250)
    {
        if (_options.CurrentValue.ClientTestMode)
        {
            var squads = ClientTestSimulator.Squads();
            var flags = ClientTestSimulator.Flags();
            return new
            {
                source = "client-test",
                databaseExists = true,
                squadApiRows = squads.Count,
                flagApiRows = flags.Count,
                requiredTables = new
                {
                    squad = true,
                    squad_member = true,
                    user_profile = true,
                    base_table = true,
                    base_element = true,
                    base_element_flag = true
                },
                tables = Array.Empty<object>(),
                accessModel = new
                {
                    squadsReadable = squads.Count >= 0,
                    flagsReadable = flags.Count >= 0,
                    canCorrelateSquadsToUsers = true,
                    canCorrelateFlagsToOwners = true,
                    readOnly = true,
                    allowsScumDbWrite = false
                },
                errors = Array.Empty<string>(),
                generatedAtUtc = DateTimeOffset.UtcNow
            };
        }

        var path = _options.CurrentValue.DatabasePath;
        var errors = new List<string>();
        var tableNeedles = new[] { "squad", "user_profile", "base", "flag" };
        var tableNames = Array.Empty<string>();
        var tables = new List<object>();
        var squadApiRows = 0;
        var flagApiRows = 0;

        if (!File.Exists(path))
        {
            return new
            {
                source = "SCUM.db",
                databasePath = path,
                databaseExists = false,
                squadApiRows,
                flagApiRows,
                requiredTables = new
                {
                    squad = false,
                    squad_member = false,
                    user_profile = false,
                    base_table = false,
                    base_element = false,
                    base_element_flag = false
                },
                tables,
                accessModel = new
                {
                    squadsReadable = false,
                    flagsReadable = false,
                    canCorrelateSquadsToUsers = false,
                    canCorrelateFlagsToOwners = false,
                    readOnly = true,
                    allowsScumDbWrite = false
                },
                errors = new[] { "SCUM.db not found." },
                generatedAtUtc = DateTimeOffset.UtcNow
            };
        }

        try
        {
            using var connection = new SqliteConnection($"Data Source={path};Mode=ReadOnly;Cache=Shared");
            connection.Open();
            tableNames = GetTables(connection).ToArray();
            foreach (var table in tableNames.Where(t => tableNeedles.Any(n => t.Contains(n, StringComparison.OrdinalIgnoreCase))))
            {
                tables.Add(new
                {
                    name = table,
                    count = CountRows(connection, table),
                    columns = GetTableColumns(connection, table)
                });
            }
        }
        catch (Exception ex)
        {
            errors.Add("schema diagnostics failed: " + ex.GetBaseException().Message);
        }

        try
        {
            squadApiRows = ReadSquads(limit).Count;
        }
        catch (Exception ex)
        {
            errors.Add("squad reader failed: " + ex.GetBaseException().Message);
        }

        try
        {
            flagApiRows = ReadFlags(limit).Count;
        }
        catch (Exception ex)
        {
            errors.Add("flag reader failed: " + ex.GetBaseException().Message);
        }

        var hasSquad = tableNames.Contains("squad", StringComparer.OrdinalIgnoreCase);
        var hasSquadMember = tableNames.Contains("squad_member", StringComparer.OrdinalIgnoreCase);
        var hasUserProfile = tableNames.Contains("user_profile", StringComparer.OrdinalIgnoreCase);
        var hasBase = tableNames.Contains("base", StringComparer.OrdinalIgnoreCase);
        var hasBaseElement = tableNames.Contains("base_element", StringComparer.OrdinalIgnoreCase);
        var hasBaseElementFlag = tableNames.Contains("base_element_flag", StringComparer.OrdinalIgnoreCase);

        return new
        {
            source = "SCUM.db",
            databasePath = path,
            databaseExists = true,
            squadApiRows,
            flagApiRows,
            requiredTables = new
            {
                squad = hasSquad,
                squad_member = hasSquadMember,
                user_profile = hasUserProfile,
                base_table = hasBase,
                base_element = hasBaseElement,
                base_element_flag = hasBaseElementFlag
            },
            accessModel = new
            {
                squadsReadable = squadApiRows >= 0 && hasSquad,
                flagsReadable = flagApiRows >= 0 && hasBaseElementFlag,
                canCorrelateSquadsToUsers = hasSquad && hasSquadMember && hasUserProfile,
                canCorrelateFlagsToOwners = hasBase && hasBaseElement && hasBaseElementFlag && hasUserProfile,
                readOnly = true,
                allowsScumDbWrite = false
            },
            tables,
            errors,
            generatedAtUtc = DateTimeOffset.UtcNow
        };
    }

    public IReadOnlyList<Dictionary<string, object?>> GetFlags(int limit) =>
        _options.CurrentValue.ClientTestMode ? ClientTestSimulator.Flags() : ReadFlags(limit);

    public IReadOnlyList<Dictionary<string, object?>> GetVehicles(int limit) =>
        _options.CurrentValue.ClientTestMode ? ClientTestSimulator.Vehicles() : ReadBestEffort(new[] { "vehicle" }, limit);

    public IReadOnlyList<Dictionary<string, object?>> GetEconomySnapshot(int limit) =>
        _options.CurrentValue.ClientTestMode ? ClientTestSimulator.Economy() : ReadBestEffort(new[] { "account", "balance", "currency", "fame", "prisoner" }, limit);

    public object GetPlayerInventory(string? steamId, string? name)
    {
        if (_options.CurrentValue.ClientTestMode)
        {
            return ClientTestSimulator.Inventory(steamId, name);
        }

        var path = _options.CurrentValue.DatabasePath;
        var hasTarget = !string.IsNullOrWhiteSpace(steamId) || !string.IsNullOrWhiteSpace(name);
        if (!File.Exists(path))
        {
            return new
            {
                source = "SCUM.db missing",
                rows = Array.Empty<object>(),
                quickSlots = Array.Empty<object>(),
                errors = new[] { new { area = "db", message = "SCUM.db не найден." } },
                generatedAtUtc = DateTimeOffset.UtcNow
            };
        }

        if (!hasTarget)
        {
            return new
            {
                source = "SCUM.db recursive inventory",
                rows = Array.Empty<object>(),
                quickSlots = Array.Empty<object>(),
                warnings = new[] { new { area = "identity", message = "Выберите конкретного игрока: без SteamID или имени инвентарь не читается." } },
                generatedAtUtc = DateTimeOffset.UtcNow
            };
        }

        try
        {
            using var connection = new SqliteConnection($"Data Source={path};Mode=ReadOnly;Cache=Shared");
            connection.Open();
            var steamIds = ResolveSteamIds(connection, steamId, name);
            var rows = ReadInventoryRows(connection, steamIds);
            var quickSlots = ReadQuickSlots(connection, steamIds);

            if (rows.Count > 0 || quickSlots.Count > 0)
            {
                return new
                {
                    source = "SCUM.db recursive inventory",
                    rows,
                    quickSlots,
                    generatedAtUtc = DateTimeOffset.UtcNow
                };
            }

            if (steamIds.Count == 0)
            {
                return new
                {
                    source = "SCUM.db recursive inventory",
                    rows = Array.Empty<object>(),
                    quickSlots = Array.Empty<object>(),
                    warnings = new[] { new { area = "identity", message = "Профиль игрока пока не найден в SCUM.db." } },
                    generatedAtUtc = DateTimeOffset.UtcNow
                };
            }
        }
        catch (Exception ex)
        {
            return new
            {
                source = "SCUM.db recursive inventory",
                rows = ReadBestEffortFiltered(new[] { "inventory", "item", "container" }, steamId, name, 500),
                quickSlots = Array.Empty<object>(),
                errors = new[] { new { area = "inventory", message = ex.GetBaseException().Message } },
                generatedAtUtc = DateTimeOffset.UtcNow
            };
        }

        return new
        {
            source = "SCUM.db recursive inventory",
            rows = Array.Empty<object>(),
            quickSlots = Array.Empty<object>(),
            warnings = new[] { new { area = "inventory", message = "Инвентарь игрока пока не найден в SCUM.db." } },
            generatedAtUtc = DateTimeOffset.UtcNow
        };
    }

    public object GetPlayerWallet(string? steamId, string? name) =>
        _options.CurrentValue.ClientTestMode
            ? ClientTestSimulator.Wallet(steamId, name)
            : new
            {
                source = "SCUM.db best-effort",
                rows = ReadBestEffortFiltered(new[] { "account", "balance", "currency", "fame", "prisoner", "wallet" }, steamId, name, 200),
                generatedAtUtc = DateTimeOffset.UtcNow
            };

    public object GetPlayerAttributes(string? steamId, string? name) =>
        _options.CurrentValue.ClientTestMode
            ? ClientTestSimulator.Attributes(steamId, name)
            : new
            {
                source = "SCUM.db best-effort",
                rows = ReadBestEffortFiltered(new[] { "prisoner", "profile", "skill", "attribute" }, steamId, name, 200),
                generatedAtUtc = DateTimeOffset.UtcNow
            };

    public PlayerSkillsReadResult GetPlayerSkills(string? steamId, string? name, string? runtimeKey)
    {
        // This endpoint is a read-only SCUM.db lookup. A runtime key alone is
        // intentionally not an identity source: it can be stale and cannot be
        // matched safely by the database reader without a SteamID64 or name.
        _ = runtimeKey;

        var cleanSteamId = (steamId ?? "").Trim();
        if (!SteamIdRegex.IsMatch(cleanSteamId))
        {
            cleanSteamId = "";
        }

        var cleanName = (name ?? "").Trim();
        if (cleanSteamId.Length == 0 && cleanName.Length == 0)
        {
            return new PlayerSkillsReadResult(false, null, "player identity is missing");
        }

        return _options.CurrentValue.ClientTestMode
            ? new PlayerSkillsReadResult(true, ClientTestSimulator.Skills(cleanSteamId, cleanName), null)
            : ReadPlayerSkills(cleanSteamId, cleanName);
    }

    private PlayerSkillsReadResult ReadPlayerSkills(string cleanSteamId, string cleanName)
    {
        var path = _options.CurrentValue.DatabasePath;
        if (!File.Exists(path))
        {
            return new PlayerSkillsReadResult(false, null, "SCUM.db is unavailable");
        }

        try
        {
            using var connection = new SqliteConnection($"Data Source={path};Mode=ReadOnly;Cache=Shared");
            connection.Open();
            using var command = connection.CreateCommand();
            command.CommandText = """
SELECT up.user_id AS steamId,
       up.name AS playerName,
       COALESCE(ps.name, '') AS name,
       COALESCE(ps.name, '') AS skillName,
       COALESCE(ps.level, 0) AS level,
       COALESCE(ps.experience, 0) AS experience
FROM user_profile up
JOIN prisoner_skill ps ON ps.prisoner_id = up.prisoner_id
WHERE up.user_id IS NOT NULL
  AND up.user_id <> ''
  AND ((@steamId <> '' AND up.user_id = @steamId)
       OR (@steamId = '' AND @name <> '' AND up.name = @name COLLATE NOCASE))
ORDER BY lower(COALESCE(ps.name, '')), ps.name
LIMIT 128
""";
            command.Parameters.AddWithValue("@steamId", cleanSteamId);
            command.Parameters.AddWithValue("@name", cleanName);

            var skills = new List<object>();
            string resolvedSteamId = cleanSteamId;
            string resolvedName = cleanName;
            using var reader = command.ExecuteReader();
            while (reader.Read())
            {
                if (!reader.IsDBNull(0)) resolvedSteamId = reader.GetString(0);
                if (!reader.IsDBNull(1)) resolvedName = reader.GetString(1);
                skills.Add(new
                {
                    steamId = reader.IsDBNull(0) ? "" : reader.GetString(0),
                    playerName = reader.IsDBNull(1) ? "" : reader.GetString(1),
                    name = reader.IsDBNull(2) ? "" : reader.GetString(2),
                    skillName = reader.IsDBNull(3) ? "" : reader.GetString(3),
                    level = reader.IsDBNull(4) ? 0 : reader.GetDouble(4),
                    experience = reader.IsDBNull(5) ? 0d : reader.GetDouble(5)
                });
            }

            return new PlayerSkillsReadResult(true, new
            {
                source = "scum-db-prisoner-skill",
                readOnly = true,
                steamId = resolvedSteamId,
                name = resolvedName,
                skills,
                generatedAtUtc = DateTimeOffset.UtcNow
            }, null);
        }
        catch
        {
            return new PlayerSkillsReadResult(false, null, "SCUM.db prisoner_skill query failed");
        }
    }

    private IReadOnlyList<Dictionary<string, object?>> ReadSquads(int limit)
    {
        var path = _options.CurrentValue.DatabasePath;
        if (!File.Exists(path))
        {
            return Array.Empty<Dictionary<string, object?>>();
        }

        try
        {
            using var connection = new SqliteConnection($"Data Source={path};Mode=ReadOnly;Cache=Shared");
            connection.Open();
            using var command = connection.CreateCommand();
            command.CommandText = """
SELECT s.id AS squadId,
       s.name AS squadName,
       s.message AS message,
       s.information AS information,
       s.score AS score,
       s.member_limit AS memberLimit,
       s.last_member_login_time AS lastMemberLoginUtc,
       s.last_member_logout_time AS lastMemberLogoutUtc,
       m.id AS memberId,
       m.rank AS memberRank,
       m.user_profile_id AS memberProfileId,
       up.user_id AS memberSteamId,
       up.name AS memberName,
       up.fame_points AS famePoints,
       up.money_balance AS moneyBalance
FROM squad s
LEFT JOIN squad_member m ON m.squad_id = s.id
LEFT JOIN user_profile up ON up.id = m.user_profile_id
ORDER BY s.score DESC, s.id, m.rank DESC
LIMIT $limit
""";
            command.Parameters.AddWithValue("$limit", Math.Clamp(limit, 1, 1000));
            using var reader = command.ExecuteReader();
            var rows = new List<Dictionary<string, object?>>();
            while (reader.Read())
            {
                var row = new Dictionary<string, object?>(StringComparer.OrdinalIgnoreCase);
                for (var i = 0; i < reader.FieldCount; i++)
                {
                    row[reader.GetName(i)] = reader.IsDBNull(i) ? null : reader.GetValue(i);
                }

                row["_table"] = "squad_member_join";
                rows.Add(row);
            }

            return rows;
        }
        catch (Exception ex)
        {
            var fallback = ReadBestEffort(new[] { "squad" }, limit).ToList();
            foreach (var row in fallback)
            {
                row["_source"] = "squad_reader_fallback";
                row["_error"] = ex.GetBaseException().Message;
            }

            return fallback;
        }
    }

    private static IReadOnlyList<Dictionary<string, object?>> BuildSquadSummaries(IReadOnlyList<Dictionary<string, object?>> rows)
    {
        var grouped = new Dictionary<string, Dictionary<string, object?>>(StringComparer.OrdinalIgnoreCase);

        foreach (var row in rows)
        {
            var squadId = Convert.ToString(GetValue(row, "squadId") ?? GetValue(row, "id"), CultureInfo.InvariantCulture) ?? "";
            var squadName = Convert.ToString(GetValue(row, "squadName") ?? GetValue(row, "name"), CultureInfo.InvariantCulture) ?? "";
            var key = string.IsNullOrWhiteSpace(squadId) ? squadName : squadId;
            if (string.IsNullOrWhiteSpace(key))
            {
                key = "squad";
            }

            if (!grouped.TryGetValue(key, out var squad))
            {
                squad = new Dictionary<string, object?>(StringComparer.OrdinalIgnoreCase)
                {
                    ["squadId"] = squadId,
                    ["squadName"] = squadName,
                    ["message"] = GetValue(row, "message"),
                    ["information"] = GetValue(row, "information"),
                    ["score"] = GetValue(row, "score"),
                    ["memberLimit"] = GetValue(row, "memberLimit"),
                    ["lastMemberLoginUtc"] = GetValue(row, "lastMemberLoginUtc"),
                    ["lastMemberLogoutUtc"] = GetValue(row, "lastMemberLogoutUtc"),
                    ["members"] = new List<Dictionary<string, object?>>()
                };
                grouped[key] = squad;
            }

            var memberId = GetValue(row, "memberId");
            var memberName = Convert.ToString(GetValue(row, "memberName") ?? GetValue(row, "name"), CultureInfo.InvariantCulture) ?? "";
            var memberSteam = Convert.ToString(GetValue(row, "memberSteamId") ?? GetValue(row, "steamId"), CultureInfo.InvariantCulture) ?? "";
            if (memberId is null && string.IsNullOrWhiteSpace(memberName) && string.IsNullOrWhiteSpace(memberSteam))
            {
                continue;
            }

            var members = (List<Dictionary<string, object?>>)squad["members"]!;
            members.Add(new Dictionary<string, object?>(StringComparer.OrdinalIgnoreCase)
            {
                ["memberId"] = memberId,
                ["memberProfileId"] = GetValue(row, "memberProfileId"),
                ["name"] = memberName,
                ["steamId"] = memberSteam,
                ["rank"] = GetValue(row, "memberRank") ?? GetValue(row, "rank"),
                ["famePoints"] = GetValue(row, "famePoints"),
                ["moneyBalance"] = GetValue(row, "moneyBalance")
            });
        }

        return grouped.Values.ToArray();
    }

    private static object? GetValue(IReadOnlyDictionary<string, object?> row, string key) =>
        row.TryGetValue(key, out var value) ? value : null;

    private IReadOnlyList<Dictionary<string, object?>> ReadFlags(int limit)
    {
        var path = _options.CurrentValue.DatabasePath;
        if (!File.Exists(path))
        {
            return Array.Empty<Dictionary<string, object?>>();
        }

        try
        {
            using var connection = new SqliteConnection($"Data Source={path};Mode=ReadOnly;Cache=Shared");
            connection.Open();
            using var command = connection.CreateCommand();
            command.CommandText = """
SELECT f.element_id AS elementId,
       b.id AS baseId,
       b.name AS baseName,
       COALESCE(e.location_x, b.location_x) AS x,
       COALESCE(e.location_y, b.location_y) AS y,
       COALESCE(e.location_z, 0) AS z,
       up.name AS ownerName,
       up.user_id AS ownerSteamId,
       f.overtake_end_time AS overtakeEndTime,
       f.overtaker_user_profile_id AS overtakerProfileId,
       f.expanded_elements AS expandedElements
FROM base_element_flag f
LEFT JOIN base_element e ON e.element_id = f.element_id
LEFT JOIN base b ON b.id = e.base_id
LEFT JOIN user_profile up ON up.id = COALESCE(
       NULLIF(b.owner_user_profile_id, -1),
       NULLIF(b.user_profile_id, -1),
       NULLIF(e.owner_profile_id, -1),
       (SELECT e2.owner_profile_id
        FROM base_element e2
        WHERE e2.base_id = b.id
          AND e2.owner_profile_id IS NOT NULL
          AND e2.owner_profile_id > 0
        ORDER BY e2.element_id
        LIMIT 1))
ORDER BY f.element_id DESC
LIMIT $limit
""";
            command.Parameters.AddWithValue("$limit", Math.Clamp(limit, 1, 1000));
            using var reader = command.ExecuteReader();
            var rows = new List<Dictionary<string, object?>>();
            while (reader.Read())
            {
                var row = new Dictionary<string, object?>(StringComparer.OrdinalIgnoreCase);
                for (var i = 0; i < reader.FieldCount; i++)
                {
                    row[reader.GetName(i)] = reader.IsDBNull(i) ? null : reader.GetValue(i);
                }

                row["_table"] = "base_element_flag_join";
                rows.Add(row);
            }

            return rows;
        }
        catch
        {
            return ReadBestEffort(new[] { "flag", "base" }, limit);
        }
    }

    private static IReadOnlyList<string> ResolveSteamIds(SqliteConnection connection, string? steamId, string? name)
    {
        var cleanSteam = (steamId ?? "").Trim();
        if (SteamIdRegex.IsMatch(cleanSteam))
        {
            return new[] { cleanSteam };
        }

        var cleanName = (name ?? "").Trim();
        if (cleanName.Length == 0)
        {
            return Array.Empty<string>();
        }

        var matches = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var row in ReadRows(connection, "user_profile", 5000))
        {
            if (!row.TryGetValue("user_id", out var userIdValue))
            {
                continue;
            }

            var rowSteam = Convert.ToString(userIdValue, CultureInfo.InvariantCulture) ?? "";
            if (!SteamIdRegex.IsMatch(rowSteam))
            {
                continue;
            }

            if (row.Values.Any(value =>
                Convert.ToString(value, CultureInfo.InvariantCulture)?.Contains(cleanName, StringComparison.OrdinalIgnoreCase) == true))
            {
                matches.Add(rowSteam);
            }
        }

        return matches.ToArray();
    }

    private static List<Dictionary<string, object?>> ReadQuickSlots(SqliteConnection connection, IReadOnlyList<string> steamIds)
    {
        using var command = connection.CreateCommand();
        command.CommandText = """
WITH latest_prisoner_entity AS (
    SELECT prisoner_id,
           MAX(entity_id) AS entity_id
    FROM prisoner_entity
    GROUP BY prisoner_id
)
SELECT up.user_id,
       q.slot_index,
       COALESCE(e.class, q.item_entity_setup, '') AS item_class
FROM user_profile up
JOIN latest_prisoner_entity lpe ON lpe.prisoner_id = up.prisoner_id
JOIN prisoner_inventory_quick_access_slot q ON q.prisoner_entity_id = lpe.entity_id
LEFT JOIN entity e ON e.id = q.item_entity_id
WHERE up.user_id IS NOT NULL
  AND up.user_id <> ''
  AND (@steam_id IS NULL OR up.user_id = @steam_id)
ORDER BY up.user_id, q.slot_index
""";

        return ReadForSteamTargets(connection, command, steamIds, reader =>
        {
            var raw = reader.IsDBNull(2) ? "" : reader.GetString(2);
            return new Dictionary<string, object?>(StringComparer.OrdinalIgnoreCase)
            {
                ["steamId"] = reader.IsDBNull(0) ? "" : reader.GetString(0),
                ["slotIndex"] = reader.IsDBNull(1) ? 0 : reader.GetInt32(1),
                ["itemClass"] = raw,
                ["itemId"] = CleanEntityId(raw),
                ["label"] = CleanEntityName(raw)
            };
        }, 60);
    }

    private static List<Dictionary<string, object?>> ReadInventoryRows(SqliteConnection connection, IReadOnlyList<string> steamIds)
    {
        using var command = connection.CreateCommand();
        command.CommandText = """
WITH RECURSIVE latest_prisoner_entity AS (
    SELECT prisoner_id,
           MAX(entity_id) AS entity_id
    FROM prisoner_entity
    GROUP BY prisoner_id
),
player_roots AS (
    SELECT up.user_id AS steam_id,
           pe.entity_id AS player_entity_id,
           ec.id AS inventory_component_id
    FROM user_profile up
    JOIN latest_prisoner_entity lpe ON lpe.prisoner_id = up.prisoner_id
    JOIN prisoner_entity pe ON pe.prisoner_id = lpe.prisoner_id
                           AND pe.entity_id = lpe.entity_id
    JOIN entity_component ec ON ec.entity_id = pe.entity_id
    WHERE up.user_id IS NOT NULL
      AND up.user_id <> ''
      AND (@steam_id IS NULL OR up.user_id = @steam_id)
      AND ec.name = 'Inventory'
),
inventory_tree AS (
    SELECT pr.steam_id,
           pr.player_entity_id,
           pr.player_entity_id AS container_entity_id,
           eice.entity_id,
           0 AS depth,
           eice.data AS slot,
           printf('%08d:%08d', eice.data, eice.entity_id) AS path
    FROM player_roots pr
    JOIN entity_inventory_component_entry eice ON eice.entity_component_id = pr.inventory_component_id
    UNION ALL
    SELECT it.steam_id,
           it.player_entity_id,
           it.entity_id AS container_entity_id,
           child.entity_id,
           it.depth + 1,
           child.data AS slot,
           it.path || '>' || printf('%08d:%08d', child.data, child.entity_id) AS path
    FROM inventory_tree it
    JOIN entity_component ec ON ec.entity_id = it.entity_id
                           AND ec.name = 'Inventory'
    JOIN entity_inventory_component_entry child ON child.entity_component_id = ec.id
    WHERE it.depth < 6
)
SELECT it.steam_id,
       it.player_entity_id,
       it.container_entity_id,
       it.entity_id,
       it.depth,
       it.slot,
       COALESCE(e.class, '') AS item_class,
       EXISTS(
           SELECT 1
           FROM entity_component ec2
           WHERE ec2.entity_id = it.entity_id
             AND ec2.name = 'Inventory'
       ) AS has_inventory
FROM inventory_tree it
JOIN entity e ON e.id = it.entity_id
ORDER BY it.steam_id, it.path
""";

        return ReadForSteamTargets(connection, command, steamIds, reader =>
        {
            var raw = reader.IsDBNull(6) ? "" : reader.GetString(6);
            var depth = reader.IsDBNull(4) ? 0 : reader.GetInt32(4);
            var slot = reader.IsDBNull(5) ? 0 : reader.GetInt32(5);
            return new Dictionary<string, object?>(StringComparer.OrdinalIgnoreCase)
            {
                ["steamId"] = reader.IsDBNull(0) ? "" : reader.GetString(0),
                ["playerEntityId"] = reader.IsDBNull(1) ? 0 : reader.GetInt32(1),
                ["containerEntityId"] = reader.IsDBNull(2) ? 0 : reader.GetInt32(2),
                ["entityId"] = reader.IsDBNull(3) ? 0 : reader.GetInt32(3),
                ["depth"] = depth,
                ["slot"] = slot,
                ["slotKind"] = ClassifyInventorySlot(slot, depth),
                ["itemClass"] = raw,
                ["itemId"] = CleanEntityId(raw),
                ["label"] = CleanEntityName(raw),
                ["hasInventory"] = !reader.IsDBNull(7) && Convert.ToInt32(reader.GetValue(7), CultureInfo.InvariantCulture) != 0 ? 1 : 0
            };
        }, 1000);
    }

    private static List<Dictionary<string, object?>> ReadForSteamTargets(
        SqliteConnection connection,
        SqliteCommand prototype,
        IReadOnlyList<string> steamIds,
        Func<SqliteDataReader, Dictionary<string, object?>> map,
        int maxRows)
    {
        var rows = new List<Dictionary<string, object?>>();
        var targets = steamIds.Count == 0 ? new string?[] { null } : steamIds.Select(id => (string?)id).ToArray();
        foreach (var target in targets)
        {
            using var command = connection.CreateCommand();
            command.CommandText = prototype.CommandText;
            command.Parameters.AddWithValue("@steam_id", string.IsNullOrWhiteSpace(target) ? DBNull.Value : target);
            using var reader = command.ExecuteReader();
            while (reader.Read())
            {
                rows.Add(map(reader));
                if (rows.Count >= maxRows)
                {
                    return rows;
                }
            }
        }

        return rows;
    }

    private static string ClassifyInventorySlot(int slot, int depth)
    {
        if (depth == 0 && slot == 2)
        {
            return "hands";
        }

        if (depth == 0 && slot == 5)
        {
            return "equipped";
        }

        return depth == 0 ? "root" : "container";
    }

    private static string CleanEntityId(string raw)
    {
        var value = (raw ?? "").Trim();
        if (value.Length == 0)
        {
            return "";
        }

        value = value.Replace("_ES", "", StringComparison.OrdinalIgnoreCase);
        var slash = value.LastIndexOfAny(new[] { '/', '\\' });
        if (slash >= 0)
        {
            value = value[(slash + 1)..];
        }

        var dot = value.LastIndexOf('.');
        if (dot >= 0)
        {
            value = value[(dot + 1)..];
        }

        return value.EndsWith("_C", StringComparison.OrdinalIgnoreCase) ? value[..^2] : value;
    }

    private static string CleanEntityName(string raw)
    {
        var id = CleanEntityId(raw);
        return string.IsNullOrWhiteSpace(id)
            ? "Unknown"
            : id.Replace("_Item_Container", " Container", StringComparison.OrdinalIgnoreCase)
                .Replace("_", " ", StringComparison.OrdinalIgnoreCase)
                .Trim();
    }

    private IReadOnlyList<Dictionary<string, object?>> ReadBestEffort(IReadOnlyList<string> tableNeedles, int limit)
    {
        var path = _options.CurrentValue.DatabasePath;
        if (!File.Exists(path))
        {
            return Array.Empty<Dictionary<string, object?>>();
        }

        try
        {
            using var connection = new SqliteConnection($"Data Source={path};Mode=ReadOnly;Cache=Shared");
            connection.Open();
            var tables = GetTables(connection)
                .Where(t => tableNeedles.Any(n => t.Contains(n, StringComparison.OrdinalIgnoreCase)))
                .Take(4)
                .ToArray();

            var rows = new List<Dictionary<string, object?>>();
            foreach (var table in tables)
            {
                foreach (var row in ReadRows(connection, table, Math.Max(1, limit / Math.Max(1, tables.Length))))
                {
                    row["_table"] = table;
                    rows.Add(row);
                    if (rows.Count >= limit)
                    {
                        return rows;
                    }
                }
            }

            return rows;
        }
        catch
        {
            return Array.Empty<Dictionary<string, object?>>();
        }
    }

    private IReadOnlyList<Dictionary<string, object?>> ReadBestEffortFiltered(IReadOnlyList<string> tableNeedles, string? steamId, string? name, int limit)
    {
        var path = _options.CurrentValue.DatabasePath;
        if (!File.Exists(path))
        {
            return Array.Empty<Dictionary<string, object?>>();
        }

        try
        {
            using var connection = new SqliteConnection($"Data Source={path};Mode=ReadOnly;Cache=Shared");
            connection.Open();
            var tables = GetTables(connection)
                .Where(t => tableNeedles.Any(n => t.Contains(n, StringComparison.OrdinalIgnoreCase)))
                .Take(12)
                .ToArray();

            var rows = new List<Dictionary<string, object?>>();
            foreach (var table in tables)
            {
                foreach (var row in ReadRows(connection, table, 1000))
                {
                    if (!MatchesIdentity(row, steamId, name))
                    {
                        continue;
                    }

                    row["_table"] = table;
                    rows.Add(row);
                    if (rows.Count >= limit)
                    {
                        return rows;
                    }
                }
            }

            return rows;
        }
        catch
        {
            return Array.Empty<Dictionary<string, object?>>();
        }
    }

    private static IReadOnlyList<string> GetTables(SqliteConnection connection)
    {
        using var command = connection.CreateCommand();
        command.CommandText = "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name";
        var result = new List<string>();
        using var reader = command.ExecuteReader();
        while (reader.Read())
        {
            result.Add(reader.GetString(0));
        }

        return result;
    }

    private static long CountRows(SqliteConnection connection, string table)
    {
        using var command = connection.CreateCommand();
        command.CommandText = $"SELECT COUNT(*) FROM [{table.Replace("]", "]]")}]";
        return Convert.ToInt64(command.ExecuteScalar(), CultureInfo.InvariantCulture);
    }

    private static IReadOnlyList<object> GetTableColumns(SqliteConnection connection, string table)
    {
        using var command = connection.CreateCommand();
        command.CommandText = $"PRAGMA table_info([{table.Replace("]", "]]")}])";
        var result = new List<object>();
        using var reader = command.ExecuteReader();
        while (reader.Read())
        {
            result.Add(new
            {
                name = reader.GetString(1),
                type = reader.IsDBNull(2) ? "" : reader.GetString(2)
            });
        }

        return result;
    }

    private static IEnumerable<Dictionary<string, object?>> ReadRows(SqliteConnection connection, string table, int limit)
    {
        using var command = connection.CreateCommand();
        command.CommandText = $"SELECT * FROM [{table.Replace("]", "]]")}] LIMIT $limit";
        command.Parameters.AddWithValue("$limit", Math.Clamp(limit, 1, 1000));
        using var reader = command.ExecuteReader();
        while (reader.Read())
        {
            var row = new Dictionary<string, object?>(StringComparer.OrdinalIgnoreCase);
            for (var i = 0; i < reader.FieldCount; i++)
            {
                var value = reader.IsDBNull(i) ? null : reader.GetValue(i);
                if (value is byte[] bytes)
                {
                    value = Convert.ToBase64String(bytes);
                }

                row[reader.GetName(i)] = value;
            }

            yield return row;
        }
    }

    private static bool MatchesIdentity(IReadOnlyDictionary<string, object?> row, string? steamId, string? name)
    {
        var cleanSteam = (steamId ?? "").Trim();
        var cleanName = (name ?? "").Trim();
        if (cleanSteam.Length == 0 && cleanName.Length == 0)
        {
            return true;
        }

        foreach (var value in row.Values)
        {
            var text = Convert.ToString(value) ?? "";
            if (cleanSteam.Length > 0 && text.Contains(cleanSteam, StringComparison.OrdinalIgnoreCase))
            {
                return true;
            }

            if (cleanName.Length > 0 && text.Contains(cleanName, StringComparison.OrdinalIgnoreCase))
            {
                return true;
            }
        }

        return false;
    }
}
