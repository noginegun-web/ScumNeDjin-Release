namespace ScumWarden.Server.Configuration;

public sealed class WardenOptions
{
    public string ApiKey { get; set; } = "";
    public string ServerName { get; set; } = "Local SCUM";
    public bool ClientTestMode { get; set; } = false;
    public string ClientTestPlayerName { get; set; } = "ClientTester";
    public string ServerRoot { get; set; } = "";
    public string BridgePath { get; set; } = "";
    public string RuntimeDataRoot { get; set; } = "";
    public string DatabasePath { get; set; } = "";
    public string LogDirectory { get; set; } = "";
    public string SaveFilesLogDirectory { get; set; } = "";
    public string RuntimeLogPath { get; set; } = "";
    public string PublicPanelUrl { get; set; } = "http://127.0.0.1:7088/";
    public string MapImageUrl { get; set; } = "";
    public MapBoundsOptions MapBounds { get; set; } = new();
    public int BridgeTimeoutMs { get; set; } = 5000;
    public int LogPollMilliseconds { get; set; } = 1000;
}

public sealed class MapBoundsOptions
{
    public double MinX { get; set; } = -768000;
    public double MaxX { get; set; } = 768000;
    public double MinY { get; set; } = -768000;
    public double MaxY { get; set; } = 768000;
    public bool InvertX { get; set; } = true;
    public bool InvertY { get; set; } = true;
    public bool SwapAxes { get; set; } = false;
}
