#pragma once

#include <initguid.h>
#include <guiddef.h>

// {8FDE7B3A-3E5A-4F1C-9C88-6B5F1B7D9E20}
// Exercise Unlock Credential Provider CLSID. If you change it, update
// Register.reg / Unregister.reg to match.
DEFINE_GUID(CLSID_ExerciseUnlockProvider,
    0x8fde7b3a, 0x3e5a, 0x4f1c, 0x9c, 0x88, 0x6b, 0x5f, 0x1b, 0x7d, 0x9e, 0x20);
