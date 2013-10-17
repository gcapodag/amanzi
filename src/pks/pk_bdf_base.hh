/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */

/* -------------------------------------------------------------------------
ATS

License: see $ATS_DIR/COPYRIGHT
Author: Ethan Coon

Default base with default implementations of methods for a PK integrated using
BDF.
------------------------------------------------------------------------- */

#ifndef AMANZI_PK_BDF_BASE_HH_
#define AMANZI_PK_BDF_BASE_HH_

#include "Teuchos_TimeMonitor.hpp"

#include "BDFFnBase.hh"
#include "BDF1_TI.hh"
#include "pk_default_base.hh"

namespace Amanzi {

class PKBDFBase : public virtual PKDefaultBase,
                  public BDFFnBase<TreeVector> {

 public:

  PKBDFBase(Teuchos::ParameterList& plist,
            Teuchos::ParameterList& FElist,
            const Teuchos::RCP<TreeVector>& solution) :
      PKDefaultBase(plist, FElist, solution) {}

  // Virtual destructor
  virtual ~PKBDFBase() {}

  // Default implementations of PK methods.
  // -- setup
  virtual void setup(const Teuchos::Ptr<State>& S);

  // -- initialize
  virtual void initialize(const Teuchos::Ptr<State>& S);

  // -- Choose a time step compatible with physics.
  virtual double get_dt();

  // -- Advance from state S0 to state S1 at time S0.time + dt.
  virtual bool advance(double dt);

  // -- Check the admissibility of a solution.
  virtual bool is_admissible(Teuchos::RCP<const TreeVector> up) { return true; }
 
  // -- Possibly modify the predictor that is going to be used as a
  //    starting value for the nonlinear solve in the time integrator.
  virtual bool modify_predictor(double h, Teuchos::RCP<TreeVector> up) { return false; }

  // -- Possibly modify the correction before it is applied
  virtual bool modify_correction(double h, Teuchos::RCP<const TreeVector> res,
          Teuchos::RCP<const TreeVector> u, Teuchos::RCP<TreeVector> du) {
    return false; }


 protected: // data
  // preconditioner assembly control
  bool assemble_preconditioner_;

  // timestep control
  double dt_;
  Teuchos::RCP<BDF1_TI<TreeVector> > time_stepper_;

  // timing
  Teuchos::RCP<Teuchos::Time> step_walltime_;

};

} // namespace

#endif
