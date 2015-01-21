/*
  This is the transport component of the Amanzi code. 

  Copyright 2010-2012 held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Author: Konstantin Lipnikov (lipnikov@lanl.gov)
*/

#include <algorithm>
#include <vector>

#include "Epetra_Vector.h"
#include "Teuchos_RCP.hpp"

#include "Mesh.hh"
#include "errors.hh"

#include "Transport_PK.hh"

namespace Amanzi {
namespace Transport {

/* ****************************************************************
* Routine completes initialization of objects in the state.
**************************************************************** */
void Transport_PK::InitializeFields()
{
  // set popular default values when Flow is off
  if (S_->HasField("water_saturation")) {
    if (!S_->GetField("water_saturation", passwd_)->initialized()) {
      S_->GetFieldData("water_saturation", passwd_)->PutScalar(1.0);
      S_->GetField("water_saturation", passwd_)->set_initialized();
    }
  }

  if (S_->HasField("prev_water_saturation")) {
    if (!S_->GetField("prev_water_saturation", passwd_)->initialized()) {
      *S_->GetFieldData("prev_water_saturation", passwd_) = *S_->GetFieldData("water_saturation", passwd_);
      S_->GetField("prev_water_saturation", passwd_)->set_initialized();
    }
  }
}


/* ****************************************************************
* Construct default state for unit tests.
**************************************************************** */
void Transport_PK::CreateDefaultState(
    Teuchos::RCP<const AmanziMesh::Mesh>& mesh, int ncomponents) 
{
  std::string name("state"); 
  S_->RequireScalar("fluid_density", name);

  if (!S_->HasField("porosity")) {
    S_->RequireField("porosity", name)->SetMesh(mesh)->SetGhosted(true)
        ->SetComponent("cell", AmanziMesh::CELL, 1);
  }
 
  if (!S_->HasField("water_saturation")) {
    S_->RequireField("water_saturation", name)->SetMesh(mesh)->SetGhosted(true)
        ->SetComponent("cell", AmanziMesh::CELL, 1);
  }
  
  if (!S_->HasField("prev_water_saturation")) {
    S_->RequireField("prev_water_saturation", name)->SetMesh(mesh_)->SetGhosted(true)
        ->SetComponent("cell", AmanziMesh::CELL, 1);
  }

  if (!S_->HasField("darcy_flux")) {
    S_->RequireField("darcy_flux", name)->SetMesh(mesh_)->SetGhosted(true)
        ->SetComponent("face", AmanziMesh::FACE, 1);
  }
  
  if (!S_->HasField("total_component_concentration")) {
    std::vector<std::vector<std::string> > subfield_names(1);
    for (int i = 0; i != ncomponents; ++i) {
      subfield_names[0].push_back(component_names_[i]);
    }
    S_->RequireField("total_component_concentration", name, subfield_names)->SetMesh(mesh_)
        ->SetGhosted(true)->SetComponent("cell", AmanziMesh::CELL, ncomponents);
  }

  // initialize fields
  S_->Setup();

  // set popular default values
  S_->GetFieldData("porosity", name)->PutScalar(0.2);
  S_->GetField("porosity", name)->set_initialized();

  *(S_->GetScalarData("fluid_density", name)) = 1000.0;
  S_->GetField("fluid_density", name)->set_initialized();

  S_->GetFieldData("water_saturation", name)->PutScalar(1.0);
  S_->GetField("water_saturation", name)->set_initialized();

  S_->GetFieldData("prev_water_saturation", name)->PutScalar(1.0);
  S_->GetField("prev_water_saturation", name)->set_initialized();

  S_->GetFieldData("total_component_concentration", name)->PutScalar(0.0);
  S_->GetField("total_component_concentration", name)->set_initialized();

  S_->GetFieldData("darcy_flux", name)->PutScalar(0.0);
  S_->GetField("darcy_flux", name)->set_initialized();

  S_->InitializeFields();
}


/* *******************************************************************
* Routine verifies that the velocity field is divergence free                 
******************************************************************* */
void Transport_PK::Policy(Teuchos::Ptr<State> S)
{
  if (mesh_->get_comm()->NumProc() > 1) {
    if (!S->GetFieldData("total_component_concentration")->Ghosted()) {
      Errors::Message msg;
      msg << "Field \"total component concentration\" has no ghost values."
          << " Transport PK is giving up.\n";
      Exceptions::amanzi_throw(msg);
    }
  }
}


/* *******************************************************************
* Calculates extrema of specified solutes and print them.
******************************************************************* */
void Transport_PK::VV_PrintSoluteExtrema(const Epetra_MultiVector& tcc_next, double dT_MPC)
{
  int num_components = tcc_next.NumVectors();
  double tccmin_vec[num_components];
  double tccmax_vec[num_components];

  tcc_next.MinValue(tccmin_vec);
  tcc_next.MaxValue(tccmax_vec);

  for (int n = 0; n < runtime_solutes_.size(); n++) {
    int i = FindComponentNumber(runtime_solutes_[n]);
    double tccmin, tccmax;
    tcc_next.Comm().MinAll(&(tccmin_vec[i]), &tccmin, 1);  // find the global extrema
    tcc_next.Comm().MaxAll(&(tccmax_vec[i]), &tccmax, 1); 

    int nregions = runtime_regions_.size();
    double solute_flux(0.0);
    bool flag(false);

    for (int k = 0; k < nregions; k++) {
      if (mesh_->valid_set_name(runtime_regions_[k], AmanziMesh::FACE)) {
        flag = true;
        AmanziMesh::Entity_ID_List block;
        mesh_->get_set_entities(runtime_regions_[k], AmanziMesh::FACE, AmanziMesh::OWNED, &block);
        int nblock = block.size();

        for (int m = 0; m < nblock; m++) {
          int f = block[m];

          Amanzi::AmanziMesh::Entity_ID_List cells;
          mesh_->face_get_cells(f, Amanzi::AmanziMesh::USED, &cells);
          int dir, c = cells[0];

          const AmanziGeometry::Point& normal = mesh_->face_normal(f, false, c, &dir);
          double u = (*darcy_flux)[0][f] * dir;
          if (u > 0) solute_flux += u * tcc_next[i][c];
        }
      }
    }

    double tmp = solute_flux;
    mesh_->get_comm()->SumAll(&tmp, &solute_flux, 1);

    *vo_->os() << runtime_solutes_[n] << ": min/max=" << tccmin << " " << tccmax;
    if (flag) *vo_->os() << ", flux=" << solute_flux << " [m^3/s]";
    *vo_->os() << std::endl;
  }

  // old capability
  mass_tracer_exact += VV_TracerVolumeChangePerSecond(0) * dT_MPC;
  double mass_tracer = 0.0;
  for (int c = 0; c < ncells_owned; c++) {
    double vol = mesh_->cell_volume(c);
    mass_tracer += (*ws)[0][c] * (*phi)[0][c] * tcc_next[0][c] * vol;
  }

  double mass_tracer_tmp = mass_tracer, mass_exact_tmp = mass_tracer_exact, mass_exact;
  mesh_->get_comm()->SumAll(&mass_tracer_tmp, &mass_tracer, 1);
  mesh_->get_comm()->SumAll(&mass_exact_tmp, &mass_exact, 1);

  double mass_loss = mass_exact - mass_tracer;
  *vo_->os() << "(obsolete) solute #0: reservoir mass=" << mass_tracer 
             << " [kg], mass left=" << mass_loss << " [kg]" << std::endl;
}


/********************************************************************
* Check completeness of influx boundary conditions.                        
****************************************************************** */
void Transport_PK::VV_CheckInfluxBC() const
{
  int number_components = tcc->ViewComponent("cell")->NumVectors();
  std::vector<int> influx_face(nfaces_wghost);

  for (int i = 0; i < number_components; i++) {
    influx_face.assign(nfaces_wghost, 0);

    for (int m = 0; m < bcs.size(); m++) {
      std::vector<int>& tcc_index = bcs[m]->tcc_index();
      int ncomp = tcc_index.size();

      for (int k = 0; k < ncomp; k++) {
        if (i == tcc_index[k]) {
          std::vector<int>& faces = bcs[m]->faces();
          int nbfaces = faces.size();

          for (int n = 0; n < nbfaces; ++n) {
            int f = faces[n];
            influx_face[f] = 1;
          }
        }
      }
    }

    for (int m = 0; m < bcs.size(); m++) {
      std::vector<int>& tcc_index = bcs[m]->tcc_index();
      int ncomp = tcc_index.size();

      for (int k = 0; k < ncomp; k++) {
        if (i == tcc_index[k]) {
          std::vector<int>& faces = bcs[m]->faces();
          int nbfaces = faces.size();

          for (int n = 0; n < nbfaces; ++n) {
            int f = faces[n];
            if ((*darcy_flux)[0][f] < 0 && influx_face[f] == 0) {
              char component[3];
              std::sprintf(component, "%3d", i);

              Errors::Message msg;
              msg << "No influx boundary condition has been found for component " << component << ".\n";
              Exceptions::amanzi_throw(msg);
            }
          }
        }
      }
    }
  }
}


/* *******************************************************************
 * Check that global extrema diminished                          
 ****************************************************************** */
void Transport_PK::VV_CheckGEDproperty(Epetra_MultiVector& tracer) const
{
  int i, num_components = tracer.NumVectors();
  double tr_min[num_components];
  double tr_max[num_components];

  tracer.MinValue(tr_min);
  tracer.MaxValue(tr_max);

  for (i = 0; i < num_components; i++) {
    if (tr_min[i] < 0) {
      std::cout << "Transport_PK: concentration violates GED property" << std::endl;
      std::cout << "    Make an Amanzi ticket or turn off internal transport tests" << std::endl;
      std::cout << "    MyPID = " << MyPID << std::endl;
      std::cout << "    component = " << i << std::endl;
      std::cout << "    time = " << T_physics << std::endl;
      std::cout << "    min/max values = " << tr_min[i] << " " << tr_max[i] << std::endl;

      Errors::Message msg;
      msg << "Concentration violates GED property." << "\n";
      Exceptions::amanzi_throw(msg);
    }
  }
}


/* *******************************************************************
 * Check that the tracer is between 0 and 1.                        
 ****************************************************************** */
void Transport_PK::VV_CheckTracerBounds(Epetra_MultiVector& tracer,
                                        int component,
                                        double lower_bound,
                                        double upper_bound,
                                        double tol) const
{
  Epetra_MultiVector& tcc_prev = *tcc->ViewComponent("cell");

  for (int c = 0; c < ncells_owned; c++) {
    double value = tracer[component][c];
    if (value < lower_bound - tol || value > upper_bound + tol) {
      std::cout << "Transport_PK: tracer violates bounds" << std::endl;
      std::cout << "    Make an Amanzi ticket or turn off internal transport tests" << std::endl;
      std::cout << "    MyPID = " << MyPID << std::endl;
      std::cout << "    component = " << component << std::endl;
      std::cout << "    simulation time = " << T_physics << std::endl;
      std::cout << "      cell = " << c << std::endl;
      std::cout << "      center = " << mesh_->cell_centroid(c) << std::endl;
      std::cout << "      value (old) = " << tcc_prev[component][c] << std::endl;
      std::cout << "      value (new) = " << value << std::endl;

      Errors::Message msg;
      msg << "Tracer violates bounds." << "\n";
      Exceptions::amanzi_throw(msg);
    }
  }
}


/* ******************************************************************
* Calculate change of tracer volume per second due to boundary flux.
* This is the simplified version (lipnikov@lanl.gov).
****************************************************************** */
double Transport_PK::VV_TracerVolumeChangePerSecond(int idx_tracer)
{
  double volume = 0.0;

  for (int m = 0; m < bcs.size(); m++) {
    std::vector<int>& tcc_index = bcs[m]->tcc_index();
    int ncomp = tcc_index.size();

    for (int i = 0; i < ncomp; i++) {
      if (tcc_index[i] == idx_tracer) {
        std::vector<int>& faces = bcs[m]->faces();
        std::vector<std::vector<double> >& values = bcs[m]->values();
        int nbfaces = faces.size();

        for (int n = 0; n < nbfaces; ++n) {
          int f = faces[n];
          int c2 = (*downwind_cell_)[f];

          if (f < nfaces_owned && c2 >= 0) {
            double u = fabs((*darcy_flux)[0][f]);
            volume += u * values[n][i];
          }
        }
      }
    }
  }
  return volume;
}


/* *******************************************************************
* Error estimate uses analytic function and solution.
* ***************************************************************** */
void Transport_PK::CalculateLpErrors(
    AnalyticFunction f, double t, Epetra_Vector* sol, double* L1, double* L2)
{
  *L1 = *L2 = 0.0;
  for (int c = 0; c < sol->MyLength(); c++) {
    const AmanziGeometry::Point& xc = mesh_->cell_centroid(c);
    double d = (*sol)[c] - f(xc, t);

    double volume = mesh_->cell_volume(c);
    *L1 += fabs(d) * volume;
    *L2 += d * d * volume;
  }

  *L2 = sqrt(*L2);
}


/* *******************************************************************
 * Calculates best least square fit for data (h[i], error[i]).                       
 ****************************************************************** */
double bestLSfit(const std::vector<double>& h, const std::vector<double>& error)
{
  double a = 0.0, b = 0.0, c = 0.0, d = 0.0, tmp1, tmp2;

  int n = h.size();
  for (int i = 0; i < n; i++) {
    tmp1 = log(h[i]);
    tmp2 = log(error[i]);
    a += tmp1;
    b += tmp2;
    c += tmp1 * tmp1;
    d += tmp1 * tmp2;
  }

  return (a * b - n * d) / (a * a - n * c);
}


}  // namespace Transport
}  // namespace Amanzi

