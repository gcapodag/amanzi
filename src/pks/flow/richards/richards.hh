/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */

/*
  A base two-phase, thermal Richard's equation with water vapor.

  License: BSD
  Authors: Ethan Coon (ATS version) (ecoon@lanl.gov)
*/

#ifndef PK_FLOW_RICHARDS_HH_
#define PK_FLOW_RICHARDS_HH_

#include "wrm_partition.hh"
#include "boundary_function.hh"
#include "MatrixMFD.hh"
#include "upwinding.hh"

#include "pk_factory.hh"
#include "pk_physical_bdf_base.hh"

namespace Amanzi {

// forward declarations
class MPCCoupledFlowEnergy;
class MPCDiagonalFlowEnergy;
class MPCSurfaceSubsurfaceDirichletCoupler;
class PredictorDelegateBCFlux;
namespace WhetStone { class Tensor; }

namespace Flow {

class Richards : public PKPhysicalBDFBase {

public:
  Richards(const Teuchos::RCP<Teuchos::ParameterList>& plist,
           Teuchos::ParameterList& FElist,
           const Teuchos::RCP<TreeVector>& solution);

  // Virtual destructor
  virtual ~Richards() {}

  // main methods
  // -- Setup data.
  virtual void setup(const Teuchos::Ptr<State>& S);

  // -- Initialize owned (dependent) variables.
  virtual void initialize(const Teuchos::Ptr<State>& S);

  // -- Commit any secondary (dependent) variables.
  virtual void commit_state(double dt, const Teuchos::RCP<State>& S);

  // -- Update diagnostics for vis.
  virtual void calculate_diagnostics(const Teuchos::RCP<State>& S);

  // ConstantTemperature is a BDFFnBase
  // computes the non-linear functional g = g(t,u,udot)
  virtual void Functional(double t_old, double t_new, Teuchos::RCP<TreeVector> u_old,
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

  // problems with pressures -- setting a range of admissible pressures
  virtual bool IsAdmissible(Teuchos::RCP<const TreeVector> up);

  // evaluating consistent faces for given BCs and cell values
  virtual void CalculateConsistentFaces(const Teuchos::Ptr<CompositeVector>& u);

protected:
  // Create of physical evaluators.
  virtual void SetupPhysicalEvaluators_(const Teuchos::Ptr<State>& S);
  virtual void SetupRichardsFlow_(const Teuchos::Ptr<State>& S);

  // boundary condition members
  virtual void UpdateBoundaryConditions_();
  virtual void ApplyBoundaryConditions_(const Teuchos::Ptr<CompositeVector>& pres);

  // -- builds tensor K, along with faced-based Krel if needed by the rel-perm method
  virtual void SetAbsolutePermeabilityTensor_(const Teuchos::Ptr<State>& S);
  virtual bool UpdatePermeabilityData_(const Teuchos::Ptr<State>& S);

  // physical methods
  // -- diffusion term
  virtual void ApplyDiffusion_(const Teuchos::Ptr<State>& S,
          const Teuchos::Ptr<CompositeVector>& g);

  virtual void AddVaporDiffusionResidual_(const Teuchos::Ptr<State>& S,
          const Teuchos::Ptr<CompositeVector>& g);
  virtual void ComputeVaporDiffusionCoef(const Teuchos::Ptr<State>& S, 
                                         Teuchos::RCP<CompositeVector>& vapor_diff, 
                                         std::string var_name);
 


  // -- accumulation term
  virtual void AddAccumulation_(const Teuchos::Ptr<CompositeVector>& g);

  // -- Add any source terms into the residual.
  virtual void AddSources_(const Teuchos::Ptr<State>& S,
                           const Teuchos::Ptr<CompositeVector>& f);
  virtual void AddSourcesToPrecon_(const Teuchos::Ptr<State>& S, double h);
  
  // -- gravity contributions to matrix or vector
  virtual void AddGravityFluxes_(const Teuchos::Ptr<const Epetra_Vector>& g_vec,
          const Teuchos::Ptr<const CompositeVector>& rel_perm,
          const Teuchos::Ptr<const CompositeVector>& rho,
          const Teuchos::Ptr<Operators::MatrixMFD>& matrix);

  virtual void AddGravityFluxesToVector_(const Teuchos::Ptr<const Epetra_Vector>& g_vec,
          const Teuchos::Ptr<const CompositeVector>& rel_perm,
          const Teuchos::Ptr<const CompositeVector>& rho,
          const Teuchos::Ptr<CompositeVector>& darcy_flux);

  // Nonlinear version of CalculateConsistentFaces()
  virtual void CalculateConsistentFacesForInfiltration_(
      const Teuchos::Ptr<CompositeVector>& u);
  virtual bool ModifyPredictorConsistentFaces_(double h, Teuchos::RCP<TreeVector> u);
  virtual bool ModifyPredictorWC_(double h, Teuchos::RCP<TreeVector> u);
  virtual bool ModifyPredictorFluxBCs_(double h, Teuchos::RCP<TreeVector> u);

  virtual void PreconWC_(Teuchos::RCP<const TreeVector> u, Teuchos::RCP<TreeVector> Pu);

protected:
  enum FluxUpdateMode {
    UPDATE_FLUX_ITERATION = 0,
    UPDATE_FLUX_TIMESTEP = 1,
    UPDATE_FLUX_VIS = 2,
    UPDATE_FLUX_NEVER = 3
  };

  // control switches
  FluxUpdateMode update_flux_;
  Operators::UpwindMethod Krel_method_;
  int niter_;
  bool infiltrate_only_if_unfrozen_;
  bool modify_predictor_with_consistent_faces_;
  bool modify_predictor_wc_;
  bool symmetric_;
  bool precon_wc_;
  bool is_source_term_;
  bool explicit_source_;
  bool precon_used_;
  bool clobber_surf_kr_;
  bool tpfa_;
  
  // coupling terms
  bool coupled_to_surface_via_head_; // surface-subsurface Dirichlet coupler
  bool coupled_to_surface_via_flux_; // surface-subsurface Neumann coupler

  // -- water coupler coupling parameters
  double surface_head_cutoff_;
  double surface_head_cutoff_alpha_;
  double surface_head_eps_;

  // permeability
  Teuchos::RCP<std::vector<WhetStone::Tensor> > K_;  // absolute permeability
  Teuchos::RCP<Operators::Upwinding> upwinding_;
  Teuchos::RCP<FlowRelations::WRMPartition> wrms_;
  bool upwind_from_prev_flux_;

  // mathematical operators
  Teuchos::RCP<Operators::MatrixMFD> matrix_;
  Teuchos::RCP<Operators::MatrixMFD> matrix_vapor_;
  //Teuchos::RCP<Operators::MatrixMFD> matrix_vapor_en_;
  Teuchos::RCP<Operators::MatrixMFD> face_matrix_;

  // residual vector for vapor diffusion
  Teuchos::RCP<CompositeVector> res_vapor;
  // note PC is in PKPhysicalBDFBase

  // custom enorm tolerances
  double flux_tol_;

  // boundary condition data
  Teuchos::RCP<Functions::BoundaryFunction> bc_pressure_;
  Teuchos::RCP<Functions::BoundaryFunction> bc_head_;
  Teuchos::RCP<Functions::BoundaryFunction> bc_flux_;
  Teuchos::RCP<Functions::BoundaryFunction> bc_seepage_;
  Teuchos::RCP<Functions::BoundaryFunction> bc_infiltration_;

  // delegates
  bool modify_predictor_bc_flux_;
  bool modify_predictor_first_bc_flux_;
  Teuchos::RCP<PredictorDelegateBCFlux> flux_predictor_;

  // is this a dynamic mesh problem
  bool dynamic_mesh_;

  // is vapor turned on
  bool vapor_diffusion_;

  // using constraint equations scaled by rel perm?
  bool scaled_constraint_;

  // scale for perm
  double perm_scale_;

 private:
  // factory registration
  static RegisteredPKFactory<Richards> reg_;

  // Richards has a friend in couplers...
  friend class Amanzi::MPCCoupledFlowEnergy;
  friend class Amanzi::MPCDiagonalFlowEnergy;
  friend class Amanzi::MPCSurfaceSubsurfaceDirichletCoupler;

};

}  // namespace AmanziFlow
}  // namespace Amanzi

#endif