/*
  This is the Linear Solver component of the Amanzi code.

  License: BSD
  Authors: Konstantin Lipnikov (lipnikov@lanl.gov)

  HYPRE Euclid parallel ILU preconditioner.

  Usage:
*/

#include "Teuchos_RCP.hpp"
#include "Ifpack.h"

#include "exceptions.hh"
#include "PreconditionerEuclid.hh"

namespace Amanzi {
namespace AmanziPreconditioners {

/* ******************************************************************
 * Apply the preconditioner.
 ****************************************************************** */
int PreconditionerEuclid::ApplyInverse(const Epetra_MultiVector& v, Epetra_MultiVector& hv)
{
  returned_code_ = IfpHypre_->ApplyInverse(v, hv);
  return (returned_code_ == 0) ? 0 : 1;
}


/* ******************************************************************
 * Initialize the preconditioner.
 ****************************************************************** */
void PreconditionerEuclid::Init(const std::string& name, const Teuchos::ParameterList& list)
{
  plist_ = list;
#ifdef HAVE_HYPRE
  funcs_.push_back(Teuchos::rcp(new FunctionParameter((Hypre_Chooser)1, &HYPRE_EuclidSetStats,
          plist_->get<int>("verbosity", 0))));

  if (plist_->isParameter("ILU(k) fill level"))
    funcs_.push_back(Teuchos::rcp(new FunctionParameter((Hypre_Chooser)1, &HYPRE_EuclidSetLevel,
            plist_->get<double>("ILU(k) fill level"))));

  if (plist_->isParameter("rescale rows"))
    bool rescale_rows = plist_->get<bool>("rescale rows");
    funcs_.push_back(Teuchos::rcp(new FunctionParameter((Hypre_Chooser)1, &HYPRE_EuclidSetRowScale,
            rescale_rows ? 1 : 0)));

  if (plist_->isParameter("ILUT drop tolerance"))
    funcs_.push_back(Teuchos::rcp(new FunctionParameter((Hypre_Chooser)1, &HYPRE_EuclidSetILUT,
            plist_->get<double>("ILUT drop tolerance"))));
#else
  Errors::Message msg("Hypre (Euclid) is not available in this installation of Amanzi.  To use Hypre, please reconfigure.");
  Exceptions::amanzi_throw(msg);
#endif
}


/* ******************************************************************
 * Rebuild the preconditioner suing the given matrix A.
 ****************************************************************** */
void PreconditionerEuclid::Update(const Teuchos::RCP<Epetra_RowMatrix>& A)
{
#ifdef HAVE_HYPRE
  IfpHypre_ = Teuchos::rcp(new Ifpack_Hypre(&*A));

  Teuchos::ParameterList hypre_list("Preconditioner List");
  hypre_list.set("Preconditioner", Euclid);
  hypre_list.set("SolveOrPrecondition", Preconditioner);
  hypre_list.set("SetPreconditioner", true);
  hypre_list.set("NumFunctions", funcs_.size());
  hypre_list.set<Teuchos::RCP<FunctionParameter>*>("Functions", &funcs_[0]);

  IfpHypre_->SetParameters(hypre_list);
  IfpHypre_->Initialize();
  IfpHypre_->Compute();
#endif
}

}  // namespace AmanziPreconditioners
}  // namespace Amanzi
