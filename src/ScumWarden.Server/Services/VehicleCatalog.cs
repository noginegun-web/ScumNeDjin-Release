namespace ScumWarden.Server.Services;

public static class VehicleCatalog
{
    private static readonly Dictionary<string, string> Aliases = new(StringComparer.OrdinalIgnoreCase)
    {
        ["bp_kinglet_duster"] = "BPC_Kinglet_Duster",
        ["kinglet_duster"] = "BPC_Kinglet_Duster",
        ["kingletduster"] = "BPC_Kinglet_Duster",
        ["bp_kinglet_mariner"] = "BPC_Kinglet_Mariner",
        ["kinglet_mariner"] = "BPC_Kinglet_Mariner",
        ["kingletmariner"] = "BPC_Kinglet_Mariner",
        ["bp_kinglet_scout"] = "BPC_Kinglet_Scout",
        ["kinglet_scout"] = "BPC_Kinglet_Scout",
        ["kingletscout"] = "BPC_Kinglet_Scout",
        ["bp_quad_01_a"] = "BPC_RIS",
        ["ris"] = "BPC_RIS",
        ["bp_cruiser_01"] = "BPC_Cruiser",
        ["cruiser"] = "BPC_Cruiser",
        ["bp_tractor_01_a"] = "BPC_Tractor",
        ["tractor"] = "BPC_Tractor",
        ["bp_sup"] = "BPC_SUP",
        ["sup"] = "BPC_SUP",
        ["bp_improvised_raft_big"] = "BPC_BigRaft",
        ["bigraft"] = "BPC_BigRaft",
        ["big_raft"] = "BPC_BigRaft",
        ["bp_wheelbarrow_01"] = "BP_WheelBarrow_Metal",
        ["metalwheelbarrow"] = "BP_WheelBarrow_Metal",
        ["metal_wheelbarrow"] = "BP_WheelBarrow_Metal"
    };

    public static string NormalizeVehicleId(string? vehicleId)
    {
        var raw = (vehicleId ?? string.Empty).Trim();
        if (raw.StartsWith("Vehicle:", StringComparison.OrdinalIgnoreCase))
        {
            raw = raw["Vehicle:".Length..].Trim();
        }

        if (raw.EndsWith("_C", StringComparison.OrdinalIgnoreCase))
        {
            raw = raw[..^2].Trim();
        }

        if (raw.Length == 0)
        {
            return "";
        }

        var key = AliasKey(raw);
        return Aliases.TryGetValue(key, out var mapped) ? mapped : raw;
    }

    private static string AliasKey(string value)
    {
        var chars = value
            .Trim()
            .Select(ch => char.IsLetterOrDigit(ch) ? char.ToLowerInvariant(ch) : '_')
            .ToArray();
        var key = new string(chars).Trim('_');
        while (key.Contains("__", StringComparison.Ordinal))
        {
            key = key.Replace("__", "_", StringComparison.Ordinal);
        }

        return key;
    }
}
