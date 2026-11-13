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
// The commissioning clock (the times in the log, on the card and in matter.status). It starts where the
// commissioner does: with its Bluetooth LE link when one came up shortly before it pairs, else with its pairing
// request. ProgressLinkUp is called on the AT client's thread, ProgressBegin on the Matter thread
void ProgressLinkUp();
void ProgressBegin();
// Steps before it that were not seen count as done; a step already passed is not shown again
void ProgressDone(ProgressStep step);
// step 0: the step after the last one done
void ProgressFailed(int step, const char * reason);
// For the live card, from any thread: "3/6 network credentials received" or "failed at 5/6: why" while a
// commissioning is under way or has failed; false before the first step and after the last
bool ProgressLine(char * out, size_t size);
// For the live card: how long the commissioning has taken and when each step so far was done, counted from its
// start: "5.4 s so far (steps at 1.2 5.0 5.4 s)", and as a finished command reads once it is complete:
// "ok, 18.4 s (steps at 1.2 5.0 5.4 5.5 12.3 18.4 s)"; false before the first step. Needs room for 68 characters
bool ProgressTimes(char * out, size_t size);
// "18.4", the seconds the commissioning of this session took; false when there was none or it is not complete
bool ProgressTotal(char * out, size_t size);

} // namespace ucxm

#endif // UCXM_PROGRESS_H
