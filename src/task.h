// Scheduled task that starts the helper elevated without a UAC prompt. Registered once, from an
// elevated helper, after the user consented; it runs an admin-only copy of LimitsAndTemps.exe so
// unelevated code can't swap the file that gets elevated.
#pragma once

#include <string>

namespace HelperTask
{
// True when this user's task exists and its copy is identical to the running exe.
bool IsUsable();

// Starts the task with the helper arguments. Returns false if the task couldn't be started.
bool Run(const std::wstring& parentPid, const std::wstring& mapName);

// Elevated only: copies the running exe to Program Files and registers the task for the current user.
bool Register(std::wstring& error);

// Elevated only: removes the current user's task, and the copy when no other user's task needs it.
void Unregister();
}
