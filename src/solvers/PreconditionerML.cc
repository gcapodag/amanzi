/*
  This is the Linear Solver component of the Amanzi code.

  License: BSD
  Authors: Konstantin Lipnikov (lipnikov@lanl.gov)

  Conjugate gradient method.
  Usage:
*/

#include "Teuchos_RCP.hpp"

#include "exceptions.hh"
#include "PreconditionerML.hh"

namespace Amanzi {
namespace AmanziPreconditioners {

/* ******************************************************************
 * Apply the preconditioner. 
 * ML's return code is set to 0 if successful, see Trilinos webpages. 
 ****************************************************************** */
int PreconditionerML::ApplyInverse(const Epetra_MultiVector& v, Epetra_MultiVector& hv)
{
  returned_code_ = ML_->ApplyInverse(v, hv);
  return (returned_code_ == 0) ? 0 : 1;
}


/* ******************************************************************
 * Initialize the preconditioner.
 ****************************************************************** */
void PreconditionerML::Init(const std::string& name, const Teuchos::ParameterList& list)
{
  list_ = list;
  initialized_ = false;
}


/* ******************************************************************
 * Rebuild the preconditioner suing the given matrix A.
 ****************************************************************** */
void PreconditionerML::Update(const Teuchos::RCP<Epetra_RowMatrix>& A)
{
  if (initialized_) ML_->DestroyPreconditioner();
  ML_ = Teuchos::rcp(new ML_Epetra::MultiLevelPreconditioner(*A, list_, false));
  ML_->ComputePreconditioner();
  initialized_ = true; 
}


/* ******************************************************************
 * Destroy the preconditioner and auxiliary data structures.
 ****************************************************************** */
void PreconditionerML::Destroy()
{
  if (initialized_) ML_->DestroyPreconditioner();
  initialized_ = false;
}

}  // namespace AmanziPreconditioners
}  // namespace Amanzi