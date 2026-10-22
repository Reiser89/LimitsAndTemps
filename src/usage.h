// AI agent usage limits, read with the sign-in the agents themselves keep on disk (no wx: also used by --dump).
#pragma once

#include <string>
#include <vector>

struct UsageWindow
{
    double percent;     // NaN when the agent doesn't report this window
    long long resetsAt; // Unix seconds; 0 when unknown
};

struct AgentUsage
{
    std::wstring name;
    UsageWindow fiveHour;
    UsageWindow week;
    UsageWindow fableWeek; // Claude only; resets with `week`
    std::wstring error;    // why values are missing or stale; empty after a good answer
    bool rateLimited;      // the server answered 429: wait longer before asking again
    bool claude;           // Claude Code, whose limits can also come from its status line
};

// Agents whose config directory exists, in display order, without values. No network.
std::vector<AgentUsage> InstalledAgents();

// Asks each installed agent's usage endpoint once. Blocks on the network, so call it off the UI thread.
std::vector<AgentUsage> FetchUsage();

// Claude Code hands its limits to the status-line command after every reply. With a copy of this exe in
// %LOCALAPPDATA%\LimitsAndTemps registered as that command (in Claude Code's settings.json, behind any existing
// one), they are saved locally: no request, and no readable sign-in needed.
enum StatusLineState
{
    STATUSLINE_OFF,
    STATUSLINE_ON,
    STATUSLINE_STALE, // registered, but not pointing at the AppData copy or the copy is gone
};
StatusLineState GetStatusLine();
bool ConnectStatusLine(std::wstring& error);    // also refreshes the AppData copy and the registered path
bool DisconnectStatusLine(std::wstring& error); // restores the previous status line, if any
unsigned long long StatusLineStamp();           // last write of the saved limits (FILETIME), 0 if none
bool ReadStatusLine(AgentUsage& claude);        // the saved limits; false if none
int RunStatusLine(bool pass);                   // --statusline [--pass]: save the limits; pass stdin on or print a summary
