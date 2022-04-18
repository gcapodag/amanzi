
#include <iostream>

#include <Epetra_Comm.h>
#include <Epetra_MpiComm.h>
#include "Epetra_SerialComm.h"

#include "Teuchos_ParameterList.hpp"
#include "Teuchos_ParameterXMLFileReader.hpp"
#include "Teuchos_XMLParameterListHelpers.hpp"
#include "Teuchos_CommandLineProcessor.hpp"
#include "Teuchos_TimeMonitor.hpp"
#include "Teuchos_StandardParameterEntryValidators.hpp"
#include "Teuchos_VerboseObjectParameterListHelpers.hpp"

#include "VerboseObject_objs.hh"
#include "VerboseObject.hh"

#include "ats_version.hh"
#include "amanzi_version.hh"
#include "tpl_versions.h"

#include "AmanziComm.hh"
#include "AmanziTypes.hh"
#include "GeometricModel.hh"
#include "State.hh"

#include "errors.hh"
#include "exceptions.hh"
#include "dbc.hh"

#include "ats_mesh_factory.hh"
#include "elm_ats_coordinator.hh"
#include "elm_ats_driver.hh"

// registration files
#include "state_evaluators_registration.hh"

#include "ats_relations_registration.hh"
#include "ats_transport_registration.hh"
#include "ats_energy_pks_registration.hh"
#include "ats_energy_relations_registration.hh"
#include "ats_flow_pks_registration.hh"
#include "ats_flow_relations_registration.hh"
#include "ats_deformation_registration.hh"
#include "ats_bgc_registration.hh"
#include "ats_surface_balance_registration.hh"
#include "ats_mpc_registration.hh"
//#include "ats_sediment_transport_registration.hh"
#include "mdm_transport_registration.hh"
#include "multiscale_transport_registration.hh"
#ifdef ALQUIMIA_ENABLED
#include "pks_chemistry_registration.hh"
#endif

// include fenv if it exists
#include "boost/version.hpp"
#if (BOOST_VERSION / 100 % 1000 >= 46)
#include "boost/config.hpp"
#ifndef BOOST_NO_FENV_H
#ifdef _GNU_SOURCE
#define AMANZI_USE_FENV
#include "boost/detail/fenv.hpp"
#endif
#endif
#endif

#include "boost/filesystem.hpp"

#include "pk_helpers.hh"

namespace ATS {

void
ELM_ATSDriver::setup(MPI_Fint *f_comm, const char *infile)
{
  // -- create communicator & get process rank
  //auto comm = Amanzi::getDefaultComm();
  auto c_comm = MPI_Comm_f2c(*f_comm);
  auto comm = setComm(c_comm);
  auto rank = comm->MyPID();

  // convert input file to std::string for easier handling
  // infile must be null-terminated
  std::string input_filename(infile);

  // parse the input file and check validity
  if (input_filename.empty()) {
    if (rank == 0)
      std::cerr << "ERROR: no input file provided" << std::endl;
  } else if (!boost::filesystem::exists(input_filename)) {
    if (rank == 0)
      std::cerr << "ERROR: input file \"" << input_filename << "\" does not exist." << std::endl;
  }

  // -- parse input file
  Teuchos::RCP<Teuchos::ParameterList> plist = Teuchos::getParametersFromXmlFile(input_filename);

  // -- set default verbosity level to no output
  Amanzi::VerboseObject::global_default_level = Teuchos::VERB_NONE;

  // create the geometric model and regions
  Teuchos::ParameterList reg_params = plist->sublist("regions");
  Teuchos::RCP<Amanzi::AmanziGeometry::GeometricModel> gm =
    Teuchos::rcp(new Amanzi::AmanziGeometry::GeometricModel(3, reg_params, *comm) );

  // Create the state.
  Teuchos::ParameterList state_plist = plist->sublist("state");
  S_ = Teuchos::rcp(new Amanzi::State(state_plist));

  // create and register meshes
  ATS::Mesh::createMeshes(*plist, comm, gm, *S_);

  // keys
  domain_sub_ = plist->get<std::string>("domain name", "domain");
  domain_srf_ = Amanzi::Keys::readDomainHint(*plist, domain_sub_, "subsurface", "surface");
  sub_src_key_ = Amanzi::Keys::readKey(*plist, domain_sub_, "subsurface source", "source_sink");
  srf_src_key_ = Amanzi::Keys::readKey(*plist, domain_srf_, "surface source", "source_sink");
  pres_key_ = Amanzi::Keys::readKey(*plist, domain_sub_, "pressure", "pressure");
  pd_key_ = Amanzi::Keys::readKey(*plist, domain_srf_, "ponded depth", "ponded_depth");
  satl_key_ = Amanzi::Keys::readKey(*plist, domain_sub_, "saturation_liquid", "saturation_liquid");
  por_key_ = Amanzi::Keys::readKey(*plist, domain_sub_, "porosity", "porosity");


  srf_mol_dens_key_ = Amanzi::Keys::readKey(*plist, domain_srf_, "surface molar density", "molar_density_liquid");
  srf_mass_dens_key_ = Amanzi::Keys::readKey(*plist, domain_srf_, "surface mass density", "mass_density_liquid");


  sub_mol_dens_key_ = Amanzi::Keys::readKey(*plist, domain_sub_, "molar density", "molar_density_liquid");
  sub_mass_dens_key_ = Amanzi::Keys::readKey(*plist, domain_sub_, "mass density", "mass_density_liquid");

  // assume for now that mesh info has been communicated
  mesh_subsurf_ = S_->GetMesh(domain_sub_);
  mesh_surf_ = S_->GetMesh(domain_srf_);

  // build columns to allow indexing by column
  mesh_subsurf_->build_columns();

  // check that number of surface cells = number of columns
  ncolumns_ = mesh_surf_->num_entities(Amanzi::AmanziMesh::CELL, Amanzi::AmanziMesh::Parallel_type::OWNED);
  AMANZI_ASSERT(ncolumns_ == mesh_subsurf_->num_columns(false));

  // get num cells per column - include consistency check later
  auto& col_zero = mesh_subsurf_->cells_of_column(0);
  ncol_cells_ = col_zero.size();

  // require primary variables
  // -- subsurface water source
  S_->Require<Amanzi::CompositeVector,Amanzi::CompositeVectorSpace>(sub_src_key_, Amanzi::Tags::NEXT,  sub_src_key_)
    .SetMesh(mesh_subsurf_)->SetComponent("cell", Amanzi::AmanziMesh::CELL, 1);
  RequireEvaluatorPrimary(sub_src_key_, Amanzi::Tags::NEXT, *S_);
  // -- surface water source-sink
  S_->Require<Amanzi::CompositeVector,Amanzi::CompositeVectorSpace>(srf_src_key_, Amanzi::Tags::NEXT,  srf_src_key_)
    .SetMesh(mesh_surf_)->SetComponent("cell", Amanzi::AmanziMesh::CELL, 1);
  RequireEvaluatorPrimary(srf_src_key_, Amanzi::Tags::NEXT, *S_);
  S_->Require<Amanzi::CompositeVector,Amanzi::CompositeVectorSpace>("dz", Amanzi::Tags::NEXT,  "dz")
    .SetMesh(mesh_subsurf_)->SetComponent("cell", Amanzi::AmanziMesh::CELL, 1);
  RequireEvaluatorPrimary("dz", Amanzi::Tags::NEXT, *S_);
  S_->Require<Amanzi::CompositeVector,Amanzi::CompositeVectorSpace>("depth", Amanzi::Tags::NEXT,  "depth")
    .SetMesh(mesh_subsurf_)->SetComponent("cell", Amanzi::AmanziMesh::CELL, 1);
  RequireEvaluatorPrimary("depth", Amanzi::Tags::NEXT, *S_);
  // -- porosity
  //S_->Require<Amanzi::CompositeVector,Amanzi::CompositeVectorSpace>(por_key_, Amanzi::Tags::NEXT,  por_key_)
  //  .SetMesh(mesh_subsurf_)->SetComponent("cell", Amanzi::AmanziMesh::CELL, 1);
  //RequireEvaluatorPrimary(por_key_, Amanzi::Tags::NEXT, *S_);

  // create ELM coordinator object
  elm_coordinator_ = std::make_unique<ELM_ATSCoordinator>(*plist, S_, comm);
  // call coordinator setup
  elm_coordinator_->setup();
}

void ELM_ATSDriver::initialize()
{
  elm_coordinator_->initialize();

  S_->GetW<Amanzi::CompositeVector>(sub_src_key_, Amanzi::Tags::NEXT, sub_src_key_).PutScalar(0.);
  S_->GetRecordW(sub_src_key_, Amanzi::Tags::NEXT, sub_src_key_).set_initialized();
  S_->GetW<Amanzi::CompositeVector>(srf_src_key_, Amanzi::Tags::NEXT, srf_src_key_).PutScalar(0.);
  S_->GetRecordW(srf_src_key_, Amanzi::Tags::NEXT, srf_src_key_).set_initialized();
  S_->GetW<Amanzi::CompositeVector>("dz", Amanzi::Tags::NEXT, "dz").PutScalar(0.);
  S_->GetRecordW("dz", Amanzi::Tags::NEXT, "dz").set_initialized();
  S_->GetW<Amanzi::CompositeVector>("depth", Amanzi::Tags::NEXT, "depth").PutScalar(0.);
  S_->GetRecordW("depth", Amanzi::Tags::NEXT, "depth").set_initialized();
  //S_->GetW<Amanzi::CompositeVector>(por_key_, Amanzi::Tags::NEXT, por_key_).PutScalar(0.);
  //S_->GetRecordW(por_key_, Amanzi::Tags::NEXT, por_key_).set_initialized();

}


void ELM_ATSDriver::advance(double *dt)
{

  elm_coordinator_->advance(*dt);

  // update ATS->ELM data if necessary
  S_->GetEvaluator(pres_key_, Amanzi::Tags::NEXT).Update(*S_, pres_key_);
  const Epetra_MultiVector& pres = *S_->Get<Amanzi::CompositeVector>(pres_key_, Amanzi::Tags::NEXT).ViewComponent("cell", false);
  S_->GetEvaluator(satl_key_, Amanzi::Tags::NEXT).Update(*S_, satl_key_);
  const Epetra_MultiVector& satl = *S_->Get<Amanzi::CompositeVector>(satl_key_, Amanzi::Tags::NEXT).ViewComponent("cell", false);
  //S_->GetEvaluator(por_key_, Amanzi::Tags::NEXT).Update(*S_, por_key_);
  //const Epetra_MultiVector& poro = *S_->Get<Amanzi::CompositeVector>(por_key_, Amanzi::Tags::NEXT).ViewComponent("cell", false);

}

void ELM_ATSDriver::advance_test()
{
  // use dt from ATS for now
  double dt = elm_coordinator_->get_dt(false);

  while (S_->get_time() < elm_coordinator_->get_end_time()) {
    advance(&dt);
    dt = elm_coordinator_->get_dt(false);
  };
}

/*
assume that incoming data is in form
soil_infiltration(ncols)
soil_evaporation(ncols)
root_transpiration(ncells)
with ncells = ncells_per_column * ncols

assume surface can be indexed:
for (Amanzi::AmanziMesh::Entity_ID col=0; col!=ncolumns_; ++col)
{  Epetra_MultiVector[0][col] = elm_data[col]; }

assume subsurface can be indexed:
for (Amanzi::AmanziMesh::Entity_ID col=0; col!=ncolumns_; ++col)
{  
  auto& col_iter = mesh_subsurf_->cells_of_column(col);
  for (std::size_t i=0; i!=col_iter.size(); ++i) 
  {  Epetra_MultiVector[0][col_iter[i]] = elm_data[col*ncol_cells_+i]; }
}

assume evaporation and infiltration have same sign
*/

void
ELM_ATSDriver::set_sources(double *soil_infiltration, double *soil_evaporation,
  double *root_transpiration, int *ncols, int *ncells)
{
  // get densities to scale source fluxes
  S_->GetEvaluator(srf_mol_dens_key_, Amanzi::Tags::NEXT).Update(*S_, srf_mol_dens_key_);
  const Epetra_MultiVector& srf_mol_dens = *S_->Get<Amanzi::CompositeVector>(srf_mol_dens_key_, Amanzi::Tags::NEXT)
      .ViewComponent("cell", false);
  S_->GetEvaluator(srf_mass_dens_key_, Amanzi::Tags::NEXT).Update(*S_, srf_mass_dens_key_);
  const Epetra_MultiVector& srf_mass_dens = *S_->Get<Amanzi::CompositeVector>(srf_mass_dens_key_, Amanzi::Tags::NEXT)
      .ViewComponent("cell", false);
  S_->GetEvaluator(sub_mol_dens_key_, Amanzi::Tags::NEXT).Update(*S_, sub_mol_dens_key_);
  const Epetra_MultiVector& sub_mol_dens = *S_->Get<Amanzi::CompositeVector>(sub_mol_dens_key_, Amanzi::Tags::NEXT)
      .ViewComponent("cell", false);
  S_->GetEvaluator(sub_mass_dens_key_, Amanzi::Tags::NEXT).Update(*S_, sub_mass_dens_key_);
  const Epetra_MultiVector& sub_mass_dens = *S_->Get<Amanzi::CompositeVector>(sub_mass_dens_key_, Amanzi::Tags::NEXT)
      .ViewComponent("cell", false);

  // get sources
  Epetra_MultiVector& surf_ss = *S_->GetW<Amanzi::CompositeVector>(srf_src_key_, Amanzi::Tags::NEXT, srf_src_key_)
      .ViewComponent("cell", false);
  Epetra_MultiVector& subsurf_ss = *S_->GetW<Amanzi::CompositeVector>(sub_src_key_, Amanzi::Tags::NEXT, sub_src_key_)
      .ViewComponent("cell", false);
  
  AMANZI_ASSERT(*ncols == ncolumns_ == surf_ss.MyLength());
  AMANZI_ASSERT(*ncells == ncolumns_ * ncol_cells_);
  AMANZI_ASSERT(*ncells == subsurf_ss.MyLength());

  // scale evaporation and infiltration and add to surface source
  // assume evaporation and infiltration have same sign?
  // negative out of subsurface and positive into subsurface?
  // or positive evap is out and positive infil is in?
  // for now, assume same sign

  // scale root_transpiration and add to subsurface source
  for (Amanzi::AmanziMesh::Entity_ID col=0; col!=ncolumns_; ++col) {
    double srf_mol_h20_kg = srf_mol_dens[0][col] / srf_mass_dens[0][col];
    surf_ss[0][col] = soil_evaporation[col] * srf_mol_h20_kg;
    surf_ss[0][col] += (soil_infiltration[col] * srf_mol_h20_kg);

    auto& col_iter = mesh_subsurf_->cells_of_column(col);
    for (std::size_t i=0; i!=col_iter.size(); ++i) {
      double sub_mol_h20_kg = sub_mol_dens[0][col_iter[i]] / sub_mass_dens[0][col_iter[i]];
      subsurf_ss[0][col_iter[i]] = root_transpiration[col*ncol_cells_+i] * sub_mol_h20_kg;
    }
  }
  // mark sources as changed
  ChangedEvaluatorPrimary(srf_src_key_, Amanzi::Tags::NEXT, *S_);
  ChangedEvaluatorPrimary(sub_src_key_, Amanzi::Tags::NEXT, *S_);
}


void
ELM_ATSDriver::get_waterstate(double *surface_pressure, double *soil_pressure, double *saturation, int *ncols, int *ncells)
{

  S_->GetEvaluator(pd_key_, Amanzi::Tags::NEXT).Update(*S_, pd_key_);
  const Epetra_MultiVector& pd = *S_->Get<Amanzi::CompositeVector>(pd_key_, Amanzi::Tags::NEXT)
      .ViewComponent("cell", false);
  S_->GetEvaluator(pres_key_, Amanzi::Tags::NEXT).Update(*S_, pres_key_);
  const Epetra_MultiVector& pres = *S_->Get<Amanzi::CompositeVector>(pres_key_, Amanzi::Tags::NEXT)
      .ViewComponent("cell", false);
  S_->GetEvaluator(satl_key_, Amanzi::Tags::NEXT).Update(*S_, satl_key_);
  const Epetra_MultiVector& sat = *S_->Get<Amanzi::CompositeVector>(satl_key_, Amanzi::Tags::NEXT)
      .ViewComponent("cell", false);

  AMANZI_ASSERT(*ncols == ncolumns_ == pd.MyLength());
  AMANZI_ASSERT(*ncells == ncolumns_ * ncol_cells_);
  AMANZI_ASSERT(*ncells == pres.MyLength());

  for (Amanzi::AmanziMesh::Entity_ID col=0; col!=ncolumns_; ++col) {
    surface_pressure[col] = pd[0][col];

    auto& col_iter = mesh_subsurf_->cells_of_column(col);
    for (std::size_t i=0; i!=col_iter.size(); ++i) {
      soil_pressure[col*ncol_cells_+i] = pres[0][col_iter[i]];
      saturation[col*ncol_cells_+i] = sat[0][col_iter[i]];
    }
  }

}

void ELM_ATSDriver::get_mesh_info(int *ncols_local, int *ncols_global, int *ncells_per_col,
    double *dz, double *depth, double *surf_area_m2, double *lat, double *lon)
{
  *ncols_local = static_cast<int>(mesh_surf_->num_entities(Amanzi::AmanziMesh::Entity_kind::CELL, Amanzi::AmanziMesh::Parallel_type::OWNED));
  *ncols_global = static_cast<int>(mesh_surf_->num_entities(Amanzi::AmanziMesh::Entity_kind::CELL, Amanzi::AmanziMesh::Parallel_type::ALL));
  *ncells_per_col = static_cast<int>(ncol_cells_);

  col_depth(dz, depth);
  ChangedEvaluatorPrimary("dz", Amanzi::Tags::NEXT, *S_);
  ChangedEvaluatorPrimary("depth", Amanzi::Tags::NEXT, *S_);

  // dummy lat lon for now
  *lat = 0.5;
  *lon = 0.5;

  for (Amanzi::AmanziMesh::Entity_ID col=0; col!=ncolumns_; ++col) {
    // -- get the surface cell's equivalent subsurface face
    Amanzi::AmanziMesh::Entity_ID f = mesh_surf_->entity_get_parent(Amanzi::AmanziMesh::CELL, col);
    surf_area_m2[col] = mesh_subsurf_->face_area(f);
  }
}


// helper function for collecting column dz and depth
void ELM_ATSDriver::col_depth(double *dz, double *depth) {

  // get dz and depth
  Epetra_MultiVector& dz_ats = *S_->GetW<Amanzi::CompositeVector>("dz", Amanzi::Tags::NEXT, "dz")
      .ViewComponent("cell", false);
  Epetra_MultiVector& depth_ats = *S_->GetW<Amanzi::CompositeVector>("depth", Amanzi::Tags::NEXT, "depth")
      .ViewComponent("cell", false);
  for (Amanzi::AmanziMesh::Entity_ID col=0; col!=ncolumns_; ++col) {

    Amanzi::AmanziMesh::Entity_ID f_above = mesh_surf_->entity_get_parent(Amanzi::AmanziMesh::CELL, col);

    auto& col_iter = mesh_subsurf_->cells_of_column(col);

    // assumed constant, so not using now
    auto ncells_per_col = col_iter.size();

    Amanzi::AmanziGeometry::Point surf_centroid = mesh_subsurf_->face_centroid(f_above);
    Amanzi::AmanziGeometry::Point neg_z(3);
    neg_z.set(0.,0.,-1);

    for (std::size_t i=0; i!=col_iter.size(); ++i) {
      // depth centroid
      //(*depth)[i] = surf_centroid[2] - mesh_subsurf_->cell_centroid(col_iter[i])[2];
      depth[col*ncol_cells_+i] = surf_centroid[2] - mesh_subsurf_->cell_centroid(col_iter[i])[2];

      // dz
      // -- find face_below
      Amanzi::AmanziMesh::Entity_ID_List faces;
      std::vector<int> dirs;
      mesh_subsurf_->cell_get_faces_and_dirs(col_iter[i], &faces, &dirs);

      // -- mimics implementation of build_columns() in Mesh
      double mindp = 999.0;
      Amanzi::AmanziMesh::Entity_ID f_below = -1;
      for (std::size_t j=0; j!=faces.size(); ++j) {
        Amanzi::AmanziGeometry::Point normal = mesh_subsurf_->face_normal(faces[j]);
        if (dirs[j] == -1) normal *= -1;
        normal /= Amanzi::AmanziGeometry::norm(normal);

        double dp = -normal * neg_z;
        if (dp < mindp) {
          mindp = dp;
          f_below = faces[j];
        }
      }

      // -- fill the val
      //(*dz)[i] = mesh_subsurf_->face_centroid(f_above)[2] - mesh_subsurf_->face_centroid(f_below)[2];
      dz[col*ncol_cells_+i] = mesh_subsurf_->face_centroid(f_above)[2] - mesh_subsurf_->face_centroid(f_below)[2];
      AMANZI_ASSERT( dz[col*ncol_cells_+i] > 0. );
      f_above = f_below;
    }
  }

}


} // namespace ATS
