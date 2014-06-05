/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */
/* -------------------------------------------------------------------------
   ATS

   License: see $ATS_DIR/COPYRIGHT
   Author: Ethan Coon

   Interface for a StrongMPC which uses a preconditioner in which the
   block-diagonal cell-local matrix is dense.  If the system looks something
   like:

   A( y1, y2, x, t ) = 0
   B( y1, y2, x, t ) = 0

   where y1,y2 are spatially varying unknowns that are discretized using the MFD
   method (and therefore have both cell and face unknowns), an approximation to
   the Jacobian is written as

   [  dA_c/dy1_c  dA_c/dy1_f   dA_c/dy2_c       0      ]
   [  dA_f/dy1_c  dA_f/dy1_f      0              0      ]
   [  dB_c/dy1_c     0          dB_c/dy2_c  dB_c/dy2_f ]
   [      0           0          dB_f/dy2_c  dB_f/dy2_f ]


   Note that the upper left block is the standard preconditioner for the A
   system, and the lower right block is the standard precon for the B system,
   and we have simply added cell-based couplings, dA_c/dy2_c and dB_c/dy1_c.

   In the temperature/pressure system, these correspond to d_water_content /
   d_temperature and d_energy / d_pressure.

   ------------------------------------------------------------------------- */

#include <fstream>
#include "EpetraExt_RowMatrixOut.h"

#include "LinearOperatorFactory.hh"
#include "FieldEvaluator.hh"
#include "MatrixMFD_Factory.hh"
#include "MatrixMFD.hh"
#include "MatrixMFD_Coupled.hh"

#include "mpc_coupled_cells.hh"

namespace Amanzi {

void MPCCoupledCells::setup(const Teuchos::Ptr<State>& S) {
  StrongMPC<PKPhysicalBDFBase>::setup(S);

  decoupled_ = plist_->get<bool>("decoupled",false);

  A_key_ = plist_->get<std::string>("conserved quantity A");
  B_key_ = plist_->get<std::string>("conserved quantity B");
  y1_key_ = plist_->get<std::string>("primary variable A");
  y2_key_ = plist_->get<std::string>("primary variable B");
  dA_dy2_key_ = std::string("d")+A_key_+std::string("_d")+y2_key_;
  dB_dy1_key_ = std::string("d")+B_key_+std::string("_d")+y1_key_;

  Key mesh_key = plist_->get<std::string>("mesh key");
  mesh_ = S->GetMesh(mesh_key);

  // set up debugger
  db_ = Teuchos::rcp(new Debugger(mesh_, name_, *plist_));

  // Create the precon
  Teuchos::ParameterList& pc_sublist = plist_->sublist("Coupled PC");
  mfd_preconditioner_ = Operators::CreateMatrixMFD_Coupled(pc_sublist, mesh_);

  // Set the sub-blocks from the sub-PK's preconditioners.
  Teuchos::RCP<Operators::MatrixMFD> pcA = sub_pks_[0]->preconditioner();
  Teuchos::RCP<Operators::MatrixMFD> pcB = sub_pks_[1]->preconditioner();
  mfd_preconditioner_->SetSubBlocks(pcA, pcB);

  // setup and initialize the preconditioner
  mfd_preconditioner_->SymbolicAssembleGlobalMatrices();
  mfd_preconditioner_->InitPreconditioner();

  // setup and initialize the linear solver for the preconditioner
  if (plist_->isSublist("Coupled Solver")) {
    Teuchos::ParameterList linsolve_sublist = plist_->sublist("Coupled Solver");
    AmanziSolvers::LinearOperatorFactory<TreeMatrix,TreeVector,TreeVectorSpace> fac;
    linsolve_preconditioner_ = fac.Create(linsolve_sublist, mfd_preconditioner_);
  } else {
    linsolve_preconditioner_ = mfd_preconditioner_;
  }
}


// updates the preconditioner
void MPCCoupledCells::UpdatePreconditioner(double t, Teuchos::RCP<const TreeVector> up,
        double h) {
  StrongMPC<PKPhysicalBDFBase>::UpdatePreconditioner(t,up,h);

  // Update and get the off-diagonal terms.
  if (!decoupled_) {
    S_next_->GetFieldEvaluator(A_key_)
        ->HasFieldDerivativeChanged(S_next_.ptr(), name_, y2_key_);
    S_next_->GetFieldEvaluator(B_key_)
        ->HasFieldDerivativeChanged(S_next_.ptr(), name_, y1_key_);
    Teuchos::RCP<const CompositeVector> dA_dy2 = S_next_->GetFieldData(dA_dy2_key_);
    Teuchos::RCP<const CompositeVector> dB_dy1 = S_next_->GetFieldData(dB_dy1_key_);

    // write for debugging
    std::vector<std::string> vnames;
    vnames.push_back("  dwc_dT"); vnames.push_back("  de_dp"); 
    std::vector< Teuchos::Ptr<const CompositeVector> > vecs;
    vecs.push_back(dA_dy2.ptr()); vecs.push_back(dB_dy1.ptr());
    db_->WriteVectors(vnames, vecs, false);

    // scale by 1/h
    mfd_preconditioner_->SetOffDiagonals(dA_dy2->ViewComponent("cell",false),
            dB_dy1->ViewComponent("cell",false), 1./h);

    // Assemble the precon, form Schur complement
    mfd_preconditioner_->ComputeSchurComplement();
    mfd_preconditioner_->UpdatePreconditioner();
  }
}


// applies preconditioner to u and returns the result in Pu
void MPCCoupledCells::ApplyPreconditioner(Teuchos::RCP<const TreeVector> u, Teuchos::RCP<TreeVector> Pu) {
  if (decoupled_) return StrongMPC<PKPhysicalBDFBase>::ApplyPreconditioner(u,Pu);
  linsolve_preconditioner_->ApplyInverse(*u, *Pu);
}


} //  namespace