/*
  This is the Linear Solver component of the Amanzi code.

  License: BSD
  Authors: Konstantin Lipnikov (lipnikov@lanl.gov)

  Base factory for preconditioners.
  Usage:
*/

#include "Teuchos_RCP.hpp"

#include "errors.hh"

#include "Preconditioner.hh"
#include "PreconditionerFactory.hh"
#include "PreconditionerIdentity.hh"
#include "PreconditionerHypre.hh"
#include "PreconditionerML.hh"
#include "PreconditionerBlockILU.hh"

namespace Amanzi {
namespace AmanziPreconditioners {

/* ******************************************************************
 * Initialization of the preconditioner
 ****************************************************************** */
Teuchos::RCP<Preconditioner> PreconditionerFactory::Create(
    const std::string& name, const Teuchos::ParameterList& prec_list)
{
  if (prec_list.isSublist(name)) {
    const Teuchos::ParameterList& slist = prec_list.sublist(name);
    std::string type = "Identity";
    if (slist.isParameter("preconditioner type"))
      type = slist.get<std::string>("preconditioner type");

    if (type == "BoomerAMG") {
      Teuchos::ParameterList hypre_list = slist.sublist("BoomerAMG Parameters");
      Teuchos::RCP<PreconditionerHypre> prec = Teuchos::rcp(new PreconditionerHypre());
      prec->Init(name, hypre_list);
      return prec;
    } else if (type == "ML") {
      Teuchos::ParameterList ml_list = slist.sublist("ML Parameters");
      Teuchos::RCP<PreconditionerML> prec = Teuchos::rcp(new PreconditionerML());
      prec->Init(name, ml_list);
      return prec;
    } else if (type == "Block ILU") {
      Teuchos::ParameterList ilu_list = slist.sublist("Block ILU Parameters");
      Teuchos::RCP<PreconditionerBlockILU> prec = Teuchos::rcp(new PreconditionerBlockILU());
      prec->Init(name, ilu_list);
    } else if (type == "Identity") {  // Identity preconditioner is default.
      Teuchos::RCP<PreconditionerIdentity> prec = Teuchos::rcp(new PreconditionerIdentity());
      prec->Init(name, prec_list);
      return prec;
    } else {
      std::stringstream msgstream;
      msgstream << "PreconditionerFactory: Unknown preconditioner type " << type;
      Errors::Message message(msgstream.str());
      Exceptions::amanzi_throw(message);
    }
  } else {
    Teuchos::RCP<PreconditionerIdentity> prec = Teuchos::rcp(new PreconditionerIdentity());
    prec->Init(name, prec_list);
    return prec;
  }
}

}  // namespace AmanziPreconditioners
}  // namespace Amanzi
