// CPU temperatures need administrator rights, so LimitsAndTemps.exe relaunches itself elevated
// with --helper and talks to that process through shared memory (see docs/ipc.md).
#pragma once

#include <string>
#include <vector>

#include "sensors.h"

// App side: launches the helper and fetches its readings.
class HelperClient
{
public:
    HelperClient();
    ~HelperClient();

    // Launches the helper, elevated via UAC if needed. installPawnIo lets it install the PawnIO
    // driver when missing. Returns false with a user-readable message on failure (including a declined UAC prompt).
    bool Start(bool installPawnIo, std::wstring& error);
    void Stop();
    bool IsRunning() const;

    // True when the PawnIO driver service is registered and its driver file exists.
    static bool IsPawnIoInstalled();

    // Asks the helper for fresh readings; returns them and its status message (empty when all is well).
    void Read(std::vector<TempReading>& out, std::wstring& status);

private:
    bool CreateChannel();

    std::wstring m_name;
    void* m_map;
    void* m_view;
    void* m_request;
    void* m_done;
    void* m_process;
    long m_lastSeq;
    std::vector<TempReading> m_last;
    std::wstring m_status;

    HelperClient(const HelperClient&);
    HelperClient& operator=(const HelperClient&);
};

// Helper side: entry points for "--helper ..." and "--dump <file>".
int RunHelper(int argc, wchar_t** argv);
int RunDump(const wchar_t* file);
