// The bits of the service runtime that the detectors (SquatDetector,
// PushUpDetector, PostureClassifier - compiled in unchanged for the test
// screen) reach for. Everything stays inside this process: the test reps
// land in a State of our own, never in the service's saved counters.

#include "State.h"
#include "PersistentState.h"

namespace ExerciseUnlock::Svc
{
    static State g_state;

    State& GetState() { return g_state; }

    // No unlock session in the calibration tool: the detectors always run.
    bool IsSessionActive() { return false; }

    // The service's state.dat holds the child's earned reps; test reps
    // must not overwrite it.
    void PersistentState::Load() {}
    void PersistentState::Save() {}
}
