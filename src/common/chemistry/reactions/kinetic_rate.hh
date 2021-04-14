/*
  Chemistry 

  Copyright 2010-201x held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Abstract base class for all kinetic rates
*/

#ifndef AMANZI_CHEMISTRY_KINETIC_RATE_HH_
#define AMANZI_CHEMISTRY_KINETIC_RATE_HH_

#include <vector>
#include <string>

#include "VerboseObject.hh"

#include "species.hh"
#include "secondary_species.hh"
#include "mineral.hh"
#include "string_tokenizer.hh"

namespace Amanzi {
namespace AmanziChemistry {

class MatrixBlock;

class KineticRate {
 public:
  virtual ~KineticRate() {};

  virtual void Update(const SpeciesArray& primary_species,
                      const std::vector<Mineral>& minerals) = 0;
  virtual void AddContributionToResidual(const std::vector<Mineral>& minerals,
                                         const double bulk_volume,
                                         std::vector<double> *residual) = 0;
  virtual void AddContributionToJacobian(const SpeciesArray& primary_species,
                                         const std::vector<Mineral>& minerals,
                                         const double bulk_volume,
                                         MatrixBlock* J) = 0;
  virtual void Display(const Teuchos::Ptr<VerboseObject> vo) const = 0;

  void SetSpeciesIds(const SpeciesArray& species,
                     const std::string& species_type,
                     const std::vector<std::string>& in_names,
                     const std::vector<double>& in_stoichiometry,
                     std::vector<int>* out_ids,
                     std::vector<double>* out_stoichiometry);

  void DisplayReaction(const Teuchos::Ptr<VerboseObject> vo) const;

  void set_debug(const bool value) { debug_ = value; };
  bool debug() const { return debug_; };

  std::string name() const { return name_; };
  int identifier() const { return identifier_; };

  double reaction_rate() const { return reaction_rate_; }

 protected:
  KineticRate();

  void set_name(const std::string& in_name) { name_ = in_name; }
  void set_identifier(int in_id) { identifier_ = in_id; }
  void set_reaction_rate(double rate) { reaction_rate_ = rate; }

  std::vector<std::string> reactant_names;
  std::vector<double> reactant_stoichiometry;
  std::vector<int> reactant_ids;

 private:
  bool debug_;
  std::string name_;
  int identifier_;  // the index identifier of the associated mineral!
  double reaction_rate_;  // volumetric rate: [moles/sec/m^3 bulk]
};

}  // namespace AmanziChemistry
}  // namespace Amanzi

#endif

