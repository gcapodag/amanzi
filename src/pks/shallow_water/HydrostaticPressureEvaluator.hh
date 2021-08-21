/*
  Shallow Water PK

  Copyright 2010-201x held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.
*/

#ifndef AMANZI_HYDROSTATICPRESSURE_EVALUATOR_HH_
#define AMANZI_HYDROSTATICPRESSURE_EVALUATOR_HH_

#include <string>
#include <vector>

#include "Teuchos_RCP.hpp"
#include "Teuchos_ParameterList.hpp"

// Amanzi
#include "secondary_variable_field_evaluator.hh"

namespace Amanzi {
namespace ShallowWater {

class HydrostaticPressureEvaluator : public SecondaryVariableFieldEvaluator {
 public:
  HydrostaticPressureEvaluator(Teuchos::ParameterList& plist);

  // interface methods from FieldEvaluator
  virtual Teuchos::RCP<FieldEvaluator> Clone() const;

  virtual void EvaluateField_(const Teuchos::Ptr<State>& S,
          const Teuchos::Ptr<CompositeVector>& result);

  virtual void EvaluateFieldPartialDerivative_(const Teuchos::Ptr<State>& S,
          Key wrt_key, const Teuchos::Ptr<CompositeVector>& result);

 private:
  std::string ponded_depth_key_;
};

}  // namespace ShallowWater
}  // namespace Amanzi

#endif

