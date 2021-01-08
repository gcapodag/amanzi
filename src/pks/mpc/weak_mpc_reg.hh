/* -*-  mode: c++; indent-tabs-mode: nil -*- */
/* -------------------------------------------------------------------------
ATS

License: see $ATS_DIR/COPYRIGHT
Author: Ethan Coon

Implementation for the derived WeakMPC class.  Provides only the advance()
method missing from MPC.hh.  In weak coupling, we simply loop over the
sub-PKs, calling their advance() methods and returning failure if any fail.

See additional documentation in the base class src/pks/mpc/MPC.hh
------------------------------------------------------------------------- */

#include "weak_mpc.hh"
#include "mpc_weak_domain_decomposition.hh"

namespace Amanzi {
RegisteredPKFactory<WeakMPC> WeakMPC::reg_("weak MPC");

} // namespace
