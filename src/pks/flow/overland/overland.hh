/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */

/* -----------------------------------------------------------------------------
This is the overland flow component of ATS.
License: BSD
Authors: Ethan Coon (ecoon@lanl.gov)
----------------------------------------------------------------------------- */

#ifndef PK_FLOW_OVERLAND_HH_
#define PK_FLOW_OVERLAND_HH_

#include "boundary_function.hh"
#include "MatrixMFD.hh"
#include "upwinding.hh"

#include "pk_factory.hh"
#include "pk_physical_bdf_base.hh"

namespace Amanzi {

class MPCSurfaceSubsurfaceDirichletCoupler;

namespace Flow {

namespace FlowRelations {
  class OverlandConductivityModel;
  class HeightModel;
}


class OverlandFlow : public PKPhysicalBDFBase {

public:
  OverlandFlow(const Teuchos::RCP<Teuchos::ParameterList>& plist,
               Teuchos::ParameterList& FElist,
               const Teuchos::RCP<TreeVector>& solution) :
      PKDefaultBase(plist, FElist, solution),
      PKPhysicalBDFBase(plist, FElist, solution),
      standalone_mode_(false),
      is_source_term_(false),
      perm_update_required_(true),
      update_flux_(UPDATE_FLUX_ITERATION),
      full_jacobian_(false) {
    plist_->set("primary variable key", "ponded_depth");
    plist_->set("domain name", "surface");
  }

  // Virtual destructor
  virtual ~OverlandFlow() {}

  // main methods
  // -- Initialize owned (dependent) variables.
  virtual void setup(const Teuchos::Ptr<State>& S);

  // -- Initialize owned (dependent) variables.
  virtual void initialize(const Teuchos::Ptr<State>& S);

  // -- Commit any secondary (dependent) variables.
  virtual void commit_state(double dt, const Teuchos::RCP<State>& S);

  // -- Update diagnostics for vis.
  virtual void calculate_diagnostics(const Teuchos::RCP<State>& S);

  // ConstantTemperature is a BDFFnBase
  // computes the non-linear functional g = g(t,u,udot)
  void Functional(double t_old, double t_new, Teuchos::RCP<TreeVector> u_old,
           Teuchos::RCP<TreeVector> u_new, Teuchos::RCP<TreeVector> g);

  // applies preconditioner to u and returns the result in Pu
  virtual void ApplyPreconditioner(Teuchos::RCP<const TreeVector> u, Teuchos::RCP<TreeVector> Pu);

  // updates the preconditioner
  virtual void UpdatePreconditioner(double t, Teuchos::RCP<const TreeVector> up, double h);

  // error monitor
  virtual double ErrorNorm(Teuchos::RCP<const TreeVector> u,
                       Teuchos::RCP<const TreeVector> du);

  virtual bool ModifyPredictor(double h, Teuchos::RCP<const TreeVector> u0,
          Teuchos::RCP<TreeVector> u);

  // evaluating consistent faces for given BCs and cell values
  virtual void CalculateConsistentFaces(const Teuchos::Ptr<CompositeVector>& u);

protected:
  // setup methods
  virtual void SetupOverlandFlow_(const Teuchos::Ptr<State>& S);
  virtual void SetupPhysicalEvaluators_(const Teuchos::Ptr<State>& S);

  // boundary condition members
  virtual void UpdateBoundaryConditions_(const Teuchos::Ptr<State>& S);

  virtual void FixBCsForOperator_(const Teuchos::Ptr<State>& S);
  virtual void FixBCsForPrecon_(const Teuchos::Ptr<State>& S);
  virtual void FixBCsForConsistentFaces_(const Teuchos::Ptr<State>& S);

  virtual void ApplyBoundaryConditions_(const Teuchos::RCP<State>& S,
          const Teuchos::RCP<CompositeVector>& pres );

  // computational concerns in managing abs, rel perm
  // -- builds tensor K, along with faced-based Krel if needed by the rel-perm method
  virtual bool UpdatePermeabilityData_(const Teuchos::Ptr<State>& S);

  // physical methods
  // -- diffusion term
  void ApplyDiffusion_(const Teuchos::Ptr<State>& S,const Teuchos::Ptr<CompositeVector>& g);
  // -- accumulation term
  void AddAccumulation_(const Teuchos::Ptr<CompositeVector>& g);
  // -- source terms
  void AddSourceTerms_(const Teuchos::Ptr<CompositeVector>& g);

  void test_ApplyPreconditioner(double t, Teuchos::RCP<const TreeVector> up, double h);

 protected:
  friend class Amanzi::MPCSurfaceSubsurfaceDirichletCoupler;

  enum FluxUpdateMode {
    UPDATE_FLUX_ITERATION = 0,
    UPDATE_FLUX_TIMESTEP = 1,
    UPDATE_FLUX_VIS = 2,
    UPDATE_FLUX_NEVER = 3
  };

  // control switches
  bool standalone_mode_; // domain mesh == surface mesh
  FluxUpdateMode update_flux_;
  Operators::UpwindMethod upwind_method_;
  bool is_source_term_;
  bool modify_predictor_with_consistent_faces_;
  bool symmetric_;
  bool perm_update_required_;
  bool tpfa_;

  // coupling term
  bool full_jacobian_;

  // work data space
  Teuchos::RCP<Operators::Upwinding> upwinding_;

  // mathematical operators
  Teuchos::RCP<Operators::MatrixMFD> matrix_;
  // note PC is in PKPhysicalBDFBase

  // boundary condition data
  Teuchos::RCP<Functions::BoundaryFunction> bc_zero_gradient_;
  Teuchos::RCP<Functions::BoundaryFunction> bc_head_;
  Teuchos::RCP<Functions::BoundaryFunction> bc_flux_;
  Teuchos::RCP<Functions::BoundaryFunction> bc_seepage_head_;

  // factory registration
  static RegisteredPKFactory<OverlandFlow> reg_;
};

}  // namespace AmanziFlow
}  // namespace Amanzi

#endif