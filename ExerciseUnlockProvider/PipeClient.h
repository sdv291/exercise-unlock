#pragma once

#include "Protocol.h"

#include <windows.h>
#include <string>

namespace ExerciseUnlock::Client
{
    // One-shot sync request to the local Exercise Unlock service.
    // Returns S_OK if a full Response landed within timeoutMs, otherwise
    // an HRESULT wrapping the last Win32 error. On failure the response
    // buffer is untouched.
    HRESULT QueryStatus(Ipc::Response& out, unsigned timeoutMs = 500);

    // Human-readable one-line rendering of a status response for the
    // FIELD_STATUS text on the tile. Never fails.
    std::wstring FormatStatus(const Ipc::Response& r);

    // Rendering used when the service is unreachable.
    std::wstring FormatUnreachable();

    // Fire-and-forget request to zero the rep counters. Called by
    // Credential::ReportResult after a successful unlock.
    HRESULT ResetCounts(unsigned timeoutMs = 500);

    // Ask the service to start the post-unlock countdown timer using
    // whatever the child has earned right now. Also zeros the rep
    // counters (server-side atomic sequence). Called from
    // Credential::ReportResult on successful sign-in.
    HRESULT StartSession(unsigned timeoutMs = 500);
}
