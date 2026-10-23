/* Compile only, then read symbol sizes with nm -S. This records layout on each
 * actual compiler without mistaking host sizeof for target measurements.
 * Not linked into firmware and not a measured runtime/stack qualification. */
#include "kaos_admission.h"
#include "kaos_artifact.h"
#include "kaos_control.h"

unsigned char profile_admission[sizeof(core_admission)];
unsigned char profile_record[sizeof(core_admission_record)];
unsigned char profile_scope[sizeof(core_request_scope)];
unsigned char profile_control[sizeof(core_control)];
unsigned char profile_report[sizeof(core_report)];
unsigned char profile_artifact[sizeof(core_artifact)];
unsigned char profile_exchange[sizeof(core_coap_exchange)];
unsigned char profile_message[sizeof(core_coap_message)];
unsigned char profile_lifetime[sizeof(core_lifetime)];
