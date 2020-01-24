/*
  This is the multiphase component of the Amanzi code. 

  Copyright 2010-201x held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Authors: Quan Bui (mquanbui@math.umd.edu)
*/

#include <cmath>
#include <string>

#include "MultiphaseDefs.hh"
#include "WRMmp_Simple.hh"

namespace Amanzi {
namespace Multiphase {

/* ******************************************************************
* Setup fundamental parameters for this model.                                            
****************************************************************** */
WRMmp_Simple::WRMmp_Simple(Teuchos::ParameterList& plist)
{
  double S_rw = plist.get<double>("residual saturation liquid", MULTIPHASE_WRM_EXCEPTION);
  double S_rn = plist.get<double>("residual saturation gas", MULTIPHASE_WRM_EXCEPTION);
  double coef = plist.get<double>("coefficient", MULTIPHASE_WRM_EXCEPTION);

  Init_(S_rw, S_rn, coef);
}

void WRMmp_Simple::Init_(double S_rw, double S_rn, double coef)
{
  S_rw_ = S_rw;
  S_rn_ = S_rn;
  coef_ = coef;
  exponent_ = 1.0;
}


/* ******************************************************************
* Relative permeability formula.                                          
****************************************************************** */
double WRMmp_Simple::k_relative(double Sw, std::string phase_name)
{
  double Swe = 0.0;
  Swe = (Sw - S_rw_)/(1.0 - S_rw_ - S_rn_);
  if (phase_name == "liquid") {
    return pow(Swe, 2.0);
  }
  else if (phase_name == "gas") {
    return pow(1.0 - Swe, 2.0);
  }

  return 0.0;
}


/* ******************************************************************
* Derivative of relative permeability wrt liquid saturation. 
****************************************************************** */
double WRMmp_Simple::dKdS(double Sw, std::string phase_name)
{
  double Swe = 0.0;
  double factor = 1.0/(1.0 - S_rw_ - S_rn_);
  Swe = (Sw - S_rw_)/(1.0 - S_rw_ - S_rn_);
  if (phase_name == "liquid") {
    return 2.0 * Swe * factor;
  }
  else if (phase_name == "gas") {
    return - 2.0 * (1.0 - Swe) * factor;
  }
}


/* ******************************************************************
* Capillary pressure formula.
****************************************************************** */
double WRMmp_Simple::capillaryPressure(double Sw)
{
  /*
  double gamma = 3.0;
  double Pd = 1.0;
  double Swe = 0.0;
  Swe = (Sw - S_rw_)/(1.0 - S_rw_ - S_rn_);
  return Pd * pow(Swe, -1.0/gamma);
  */
  // use simple linear capillary pressure for now
  return coef_ * pow(1.0 - Sw, exponent_);
}


/* ******************************************************************
* Derivative of capillary pressure. Hard-coded Brooks-Corey
* with Pd = 1, gamma = 3. Assume the saturation is of the wetting phase
****************************************************************** */
double WRMmp_Simple::dPc_dS(double Sw)
{
  /*
  double gamma = 3.0;
  double Pd = 1.0;
  double Swe = (Sw - S_rw_)/(1.0 - S_rw_ - S_rn_);
  double factor = 1.0/(1.0 - S_rw_ - S_rn_);
  return Pd * factor * (-1.0/gamma) * pow(Swe, -1.0/gamma - 1.0); 
  */
  return -exponent_ * coef_ * pow(1.0 - Sw, exponent_ - 1.0);
}

}  // namespace Multiphase
}  // namespace Amanzi

