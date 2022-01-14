/* -*-  mode: c++; indent-tabs-mode: nil -*- */
//! SubgridAggregateEvaluator restricts a field to the subgrid version of the same field.

/*
  ATS is released under the three-clause BSD License.
  The terms of use and "as is" disclaimer for this license are
  provided in the top-level COPYRIGHT file.

  Authors: Ahmad Jan (jana@ornl.gov)
*/

/*!

.. _subgrid-aggregate-evaluator-spec:
.. admonition:: subgrid-aggregate-evaluator-spec

   * `"source domain name`" ``[string]`` Domain name of the source mesh.

   KEYS:
   - `"field`" **SOURCE_DOMAIN-KEY**  Default set from this evaluator's name.

*/


#ifndef AMANZI_RELATIONS_SUBGRID_AGGREGATOR_EVALUATOR_HH_
#define AMANZI_RELATIONS_SUBGRID_AGGREGATOR_EVALUATOR_HH_

#include "Factory.hh"
#include "EvaluatorSecondaryMonotype.hh"

namespace Amanzi {
namespace Relations {

class SubgridAggregateEvaluator : public EvaluatorSecondaryMonotypeCV {

 public:
  // constructor format for all derived classes
  explicit
  SubgridAggregateEvaluator(Teuchos::ParameterList& plist);

  SubgridAggregateEvaluator(const SubgridAggregateEvaluator& other) = default;
  Teuchos::RCP<Evaluator> Clone() const override;

  void
  EnsureCompatibility(State& S) override;

 protected:
  // Required methods from EvaluatorSecondaryMonotypeCV
  void Evaluate_(const State& S,
                      const std::vector<CompositeVector*>& result) override;
  void EvaluatePartialDerivative_(const State& S,
          const Key& wrt_key, const Tag& wrt_tag,
          const std::vector<CompositeVector*>& result) override;

 protected:
  Key source_domain_;
  Key domain_;
  Key var_key_;

 private:
  static Utils::RegisteredFactory<Evaluator,SubgridAggregateEvaluator> factory_;
};

} // namespace
} // namespace

#endif

