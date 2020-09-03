#ifndef _SAFEGUARD_H_
#define _SAFEGUARD_H_

// We allow several recursions if icerun is involved, just in case icerun is
// e.g. used to invoke a script that calls make that invokes compilations. In
// this case, it is allowed to have icerun->icecc->compiler. However,
// icecc->icecc recursion is a problem, so just one recursion exceeds the limit.
// Also note that if the total number of such recursive invocations exceedds the
// number of allowed local jobs, iceccd will not assign another local job and
// the whole build will get stuck.

#define SAFEGUARD_MAX_LEVEL (2)

enum SafeguardStep
{
    SafeguardStepCompiler = SAFEGUARD_MAX_LEVEL,
    SafeguardStepCustom = 1
};

void
dcc_increment_safeguard(SafeguardStep step);

int
dcc_recursion_safeguard(void);

#endif // _SAFEGUARD_H_
