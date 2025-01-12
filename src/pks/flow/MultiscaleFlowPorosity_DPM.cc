/*
  Flow PK 

  Copyright 2010-201x held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Author: Konstantin Lipnikov (lipnikov@lanl.gov)
*/

#include <string>

#include "FlowDefs.hh"
#include "MultiscaleFlowPorosity_DPM.hh"
#include "WRMFactory.hh"

namespace Amanzi {
namespace Flow {

/* ******************************************************************
* This model is minor extension of the WRM.
****************************************************************** */
MultiscaleFlowPorosity_DPM::MultiscaleFlowPorosity_DPM(Teuchos::ParameterList& plist)
{
  WRMFactory factory;
  wrm_ = factory.Create(plist);

  Teuchos::ParameterList& slist = plist.sublist("dual porosity parameters");
  alpha_ = slist.get<double>("mass transfer coefficient", 0.0);
  tol_ = slist.get<double>("tolerance", FLOW_DPM_NEWTON_TOLERANCE);
}


/* ******************************************************************
* It should be called only once; otherwise, create an evaluator.
****************************************************************** */
double MultiscaleFlowPorosity_DPM::ComputeField(double phi, double n_l, double pcm)
{
  return wrm_->saturation(pcm) * phi * n_l;
}


/* ******************************************************************
* Main capability: cell-based Newton solver. It returns water storage, 
* pressure in the matrix. max_itrs is input/output parameter.
****************************************************************** */
double MultiscaleFlowPorosity_DPM::WaterContentMatrix(
    double pcf0, WhetStone::DenseVector& pcm,
    double wcm0, double dt, double phi, double n_l, int& max_itrs)
{
  double patm(1e+5), zoom, pmin, pmax;
  zoom = std::fabs(pcm(0)) + patm;
  pmin = pcm(0) - zoom; 
  pmax = pcm(0) + zoom; 

  // setup local parameters 
  double sat0, alpha_mod;
  sat0 = wcm0 / (phi * n_l);
  alpha_mod = alpha_ * dt / (phi * n_l);

  // setup iterative parameters
  double f0, f1, ds, dp, dsdp, guess, result(pcm(0));
  double delta(1.0e+10), delta1(1.0e+10), delta2(1.0e+10);
  int count(max_itrs);

  while (--count && (fabs(result * tol_) < fabs(delta))) {
    delta2 = delta1;
    delta1 = delta;

    ds = wrm_->saturation(result) - sat0;
    dp = result - pcf0;
    dsdp = wrm_->dSdPc(result);

    f0 = ds - alpha_mod * dp;
    if (f0 == 0.0) break;

    f1 = dsdp - alpha_mod;
    delta = f0 / f1;

    // If the last two steps have not converged, try bisection:
    if (fabs(delta * 2) > fabs(delta2)) {
      delta = (delta > 0) ? (result - pmin) / 2 : (result - pmax) / 2;
    }
    guess = result;
    result -= delta;
    if (result <= pmin) {
      delta = (guess - pmin) / 2;
      result = guess - delta;
      if ((result == pmin) || (result == pmax)) break;

    } else if (result >= pmax) {
      delta = (guess - pmax) / 2;
      result = guess - delta;
      if ((result == pmin) || (result == pmax)) break;
    }

    // update brackets:
    if (delta > 0.0) {
      pmax = guess;
    } else {
      pmin = guess;
    }
  }
  max_itrs -= count - 1;

  pcm(0) = result;
  return wrm_->saturation(pcm(0)) * phi * n_l;
}

}  // namespace Flow
}  // namespace Amanzi
  
  
