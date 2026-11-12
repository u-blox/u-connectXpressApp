/* Commissioning as the user sees it: six steps, one line each in the app's log (matter.log info, the default)
 * and in the Events pane. The Matter SDK's own log is detail. The steps are those of u-connectMatter. */
#ifndef UCXM_PROGRESS_H
#define UCXM_PROGRESS_H

#include <cstddef>

namespace ucxm {

enum ProgressStep
{
    kStepPaired = 1,   // PASE: the commissioner knows the passcode
    kStepFabric,       // attestation, certificates, fabric committed
    kStepNetworkGiven, // Wi-Fi credentials or Thread dataset received
    kStepNetworkJoin,
    kStepNetworkUp,
    kStepComplete,
};

void ProgressReset(); // a commissioning window opened: a new attempt starts
// Steps before it that were not seen count as done; a step already passed is not shown again
void ProgressDone(ProgressStep step);
// step 0: the step after the last one done
void ProgressFailed(int step, const char * reason);
// For the live card, from any thread: "3/6 network credentials received" or "failed at 5/6: why" while a
// commissioning is under way or has failed; false before the first step and after the last
bool ProgressLine(char * out, size_t size);

} // namespace ucxm

#endif // UCXM_PROGRESS_H
