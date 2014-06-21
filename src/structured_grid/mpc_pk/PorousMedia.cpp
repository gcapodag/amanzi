#include <winstd.H>

#include <algorithm>
#include <vector>
#include <cmath>
#include <ios>
#include <iomanip>

#include <MultiGrid.H>
#include <TagBox.H>
#include <time.h> 
#include "PMAMR_Labels.H"

static std::map<std::string,std::string>& AMR_to_Amanzi_label_map = Amanzi::AmanziInput::AMRToAmanziLabelMap();

#include <PorousMedia.H>
#include <PROB_PM_F.H>
#include <POROUSMEDIA_F.H>
#include <VISCOPERATOR_F.H>
#include <RSAMRdata.H>
#include <RockUtil_F.H>  // FIXME: Should functions in this file be called from here?

#include <Advection.H>
#include <AmanziChemHelper_Structured.H>

#ifdef _OPENMP
#include "omp.h"
#endif

// Amanzi chemistry stuff
#include "exceptions.hh"
#include "errors.hh"
#ifdef ALQUIMIA_ENABLED
#else
#include "simple_thermo_database.hh"
#include "chemistry_verbosity.hh"
#include "chemistry_exception.hh"
#include "chemistry_output.hh"
extern Amanzi::AmanziChemistry::ChemistryOutput* Amanzi::AmanziChemistry::chem_out;
#endif

Teuchos::ParameterList PorousMedia::input_parameter_list;

#define SHOWVALARR(val)                        \
{                                              \
    std::cout << #val << " = ";                \
    for (int i=0;i<val.size();++i)             \
    {                                          \
        std::cout << val[i] << " " ;           \
    }                                          \
    std::cout << std::endl;                    \
}                                             
#define SHOWVALARRA(val) { SHOWVALARR(val); BoxLib::Abort();}
#define SHOWVAL(val) { std::cout << #val << " = " << val << std::endl;}
#define SHOWVALA(val) { SHOWVAL(val); BoxLib::Abort();}

static void
verify_is_clean(const std::string& note, const MultiFab& mf,int sComp=0, int nComp=-1, int nGrow=0, bool dump=false)
{
#ifndef NDEBUG
  int num_comp = nComp<0 ? mf.nComp() : nComp;
  if (mf.contains_nan(sComp,num_comp,nGrow)) {
    if (dump) {
      for (MFIter mfi(mf); mfi.isValid(); ++mfi) {
        Box box = Box(mfi.validbox()).grow(nGrow);
        if (mf[mfi].contains_nan(box,sComp,num_comp)) {
          std::cout << mf[mfi] << std::endl;
        }
      }
    }
    std::cout << "Data contains nans: " << note << std::endl;
    std::cout << "sComp, nComp, nGrow: " << sComp << ", " << num_comp << ", " << nGrow << std::endl;
    BoxLib::Abort();
  }
#endif
}

#define DEF_LIMITS(fab,fabdat,fablo,fabhi)	\
  const int* fablo = (fab).loVect();		\
  const int* fabhi = (fab).hiVect();		\
  Real* fabdat = (fab).dataPtr();

#define DEF_CLIMITS(fab,fabdat,fablo,fabhi)	\
  const int* fablo = (fab).loVect();		\
  const int* fabhi = (fab).hiVect();		\
  const Real* fabdat = (fab).dataPtr();

#define DEF_CILIMITS(fab,fabdat,fablo,fabhi)	\
  const int* fablo = (fab).loVect();		\
  const int* fabhi = (fab).hiVect();		\
  const int* fabdat = (fab).dataPtr();

// Couple of handy functions for gdb

extern "C" {
  void printBoxArray(const BoxArray& ba) 
  {
    std::cout << "BoxArray: " << std::endl;
    for (int i=0; i<ba.size(); ++i) {
      std::cout << "   " << i << ": " << ba[i];
      for (int d=0; d<BL_SPACEDIM; ++d) {
        std::cout << " " << ba[i].length(d);
      }
      std::cout << std::endl;
    }
    std::cout << "Total volume: " << ba.numPts() << std::endl;
    std::cout << " Is disjoint: " << ba.isDisjoint() << std::endl;
    std::cout << "  MinimalBox: " << ba.minimalBox() << std::endl;
  }
  void printFab(const FArrayBox& fab) 
  {
    std::cout << "FArrayBox: " << std::endl;
    std::cout << "   Box: " << fab.box() << std::endl;
    std::cout << fab << std::endl;
  }
}



//
// Static objects.
//
ErrorList      PorousMedia::err_list;
BCRec          PorousMedia::phys_bc;
BCRec          PorousMedia::pres_bc;
MacProj*       PorousMedia::mac_projector = 0;
Godunov*       PorousMedia::godunov       = 0;
RegionManager* PorousMedia::region_manager = 0;
RockManager*   PorousMedia::rock_manager = 0;

static double richard_time;
static double richard_time_min = 1.e6;

PM_Error_Value::PM_Error_Value (Real min_time, Real max_time, int max_level, 
                                const Array<const Region*>& regions_)
    : pmef(0), value(0), min_time(min_time), max_time(max_time), max_level(max_level)
{
    set_regions(regions_);
}

PM_Error_Value::PM_Error_Value (PMEF pmef,
                                Real value, Real min_time,
                                Real max_time, int max_level, 
                                const Array<const Region*>& regions_)
    : pmef(pmef), value(value), min_time(min_time), max_time(max_time), max_level(max_level)
{
    set_regions(regions_);
}

void
PM_Error_Value::set_regions(const Array<const Region*>& regions_)
{
    int nregions=regions_.size();

    // Get a copy of the pointers to regions in a structure that wont 
    //   remove them when it leaves scope
    regions.resize(nregions);
    for (int i=0; i<nregions; ++i) {
      regions[i] = regions_[i];
    }
}

void
PM_Error_Value::tagCells(int* tag, D_DECL(const int& tlo0,const int& tlo1,const int& tlo2),
                         D_DECL(const int& thi0,const int& thi1,const int& thi2),
                         const int* tagval, const int* clearval,
                         const Real* data, 
                         D_DECL(const int& dlo0,const int& dlo1,const int& dlo2),
                         D_DECL(const int& dhi0,const int& dhi1,const int& dhi2),
                         const Real* mask, 
                         D_DECL(const int& mlo0,const int& mlo1,const int& mlo2),
                         D_DECL(const int& mhi0,const int& mhi1,const int& mhi2),
                         const int* lo, const int* hi, const int* nvar,
                         const int* domain_lo, const int* domain_hi,
                         const Real* dx, const Real* xlo,
                         const Real* prob_lo, const Real* time,
                         const int* level) const
{
    BL_ASSERT(pmef);

    pmef(tag,D_DECL(tlo0,tlo1,tlo2),D_DECL(thi0,thi1,thi2),tagval,clearval,
         data,D_DECL(dlo0,dlo1,dlo2),D_DECL(dhi0,dhi1,dhi2),
         mask,D_DECL(mlo0,mlo1,mlo2),D_DECL(mhi0,mhi1,mhi2),
         lo, hi, nvar,domain_lo,domain_hi,dx,xlo,prob_lo,time,
         level,&value);
}

namespace
{
  const std::string solid("Solid");
  const std::string absorbed("Absorbed");
  const std::string ctotal("Total");
}

static bool initialized = false;
static bool physics_events_registered = false;

void
PorousMedia::CleanupStatics ()
{
    ic_array.clear();
    bc_array.clear();
    tic_array.clear();
    tbc_array.clear();
    initialized = false;
#ifdef ALQUIMIA_ENABLED
#else
    delete Amanzi::AmanziChemistry::chem_out;
#endif
    physics_events_registered = false;
}

void
PorousMedia::variableCleanUp ()
{
  desc_lst.clear();
  derive_lst.clear();
  err_list.clear();

  delete kappadata;
  kappadata = 0;
  delete phidata;
  phidata = 0;

  delete mac_projector;
  mac_projector = 0;

  delete godunov;
  godunov = 0;

  phase_list.clear();
  comp_list.clear();
  tracer_list.clear();

  source_array.clear();

  delete region_manager;
  region_manager = 0;

  delete rock_manager;
  rock_manager = 0;

}

void
PorousMedia::RegisterPhysicsBasedEvents()
{
  // Finalize the rock_manager setup, now that the Amr has the required info
  if (ParallelDescriptor::IOProcessor()) {
    std::cout << "Finalizing the RockManager" << std::endl;
  }
  PMAmr& pmamr = *(PMParent());
  int nlevels = pmamr.maxLevel() + 1;
  Array<Geometry> geom_array(nlevels);
  Array<IntVect> ref_array(nlevels-1);
  for (int i=0; i<nlevels; ++i) {
    geom_array[i] = pmamr.Geom(i);
    if (i<nlevels-1) {
      ref_array[i] = pmamr.refRatio(i);
    }
  }
  rock_manager->FinalizeBuild(geom_array,ref_array,PorousMedia::NGrowHYP());

  if (execution_mode==INIT_TO_STEADY) {
    std::string event_name = "Switch_Time";
    PMParent()->RegisterEvent(event_name,new EventCoord::TimeEvent(Array<Real>(1,switch_time)));
  }

  for (int i=0; i<bc_array.size(); ++i) {
    const std::string& event_name = bc_array[i].Label();
    PMParent()->RegisterEvent(event_name,new EventCoord::TimeEvent(bc_array[i].time()));
  }

  for (int n=0; n<tbc_array.size(); ++n) {
    for (int i=0; i<tbc_array[n].size(); ++i) {
      BL_ASSERT(soluteNames().size()>n);
      BL_ASSERT(tbc_array.size()>n);
      BL_ASSERT(tbc_array[n].size()>i);
      const std::string& event_name = tbc_array[n][i].Label() + "_" + soluteNames()[n];
      pmamr.RegisterEvent(event_name,new EventCoord::TimeEvent(tbc_array[n][i].time()));
    }
  }
}

PorousMedia::PorousMedia ()
{
  if (!initialized) {
    BoxLib::ExecOnFinalize(PorousMedia::CleanupStatics);
    initialized = true;
  }

  Ssync        = 0;
  advflux_reg  = 0;
  viscflux_reg = 0;
  u_mac_prev   = 0;
  u_macG_prev  = 0;
  u_mac_curr   = 0;
  u_macG_curr  = 0;
  u_macG_trac  = 0;
  u_corr       = 0;
  kappa        = 0;
  kpedge       = 0;
  kr_coef      = 0;
  cpl_coef     = 0;
  materialID   = 0;
  lambda       = 0;
  lambda_cc    = 0;
  lambdap1_cc  = 0;
  dlambda_cc   = 0;
  rock_phi     = 0;
  specific_storage = 0;
  diffusion    = 0;
  dt_eig       = 0;
  rhs_RhoD     = 0;

  component_saturations_cached = false;
  sat_old_cached = 0;
  sat_new_cached = 0;
  t_sat_old_cached = -1;
  t_sat_new_cached = -1;

  if (parent && !physics_events_registered) {
    RegisterPhysicsBasedEvents();
    physics_events_registered = true;
  }
}



void
PorousMedia::setup_bound_desc()
{
  bc_descriptor_map.clear();
  const Real* dx   = geom.CellSize();
  const Box& domain = geom.Domain();
  int nGrowHYP = PorousMedia::NGrowHYP();

  Array<Orientation> Faces;
  const BCRec& bc = desc_lst[State_Type].getBC(0);
  getDirichletFaces(Faces,State_Type,bc);

  if (setup_tracer_transport) {
    tbc_descriptor_map.resize(ntracers);
  }

#if 0
  for (int iface = 0; iface < Faces.size(); iface++)
  {
    const Orientation& face = Faces[iface];
    if (PorousMedia::grids_on_side_of_domain(grids,domain,face)) {

      // Grow here to pick up corners and edges
      Box ccBndBox  = BoxLib::adjCell(domain,face,nGrowHYP);
      for (int i=0; i<BL_SPACEDIM; ++i) {
        if (i != face.coordDir()) {
          ccBndBox.grow(i,nGrowHYP);
        }
      }

      // Find BCs for this face
      int idx = face.coordDir() + 3*face.isHigh();
      const std::string& purpose = PMAMR::RpurposeDEF[idx];

      const PArray<RegionData>& bcs = PorousMedia::BCs();
      Array<int> myBCs;
      for (int i=0; i<bcs.size(); ++i) {
        const Array<const Region*>& regions = bcs[i].Regions();
        int found = 0;
        for (int j=0; j<regions.size(); ++j) {
          if (regions[j]->purpose == purpose) {
            found++;
          }
        }
        if (found) {
          myBCs.push_back(i);
        }
      }
      if (myBCs.size() > 0) 
      {
        bc_descriptor_map[face] = BCDesc(ccBndBox,myBCs);
      }
      else {
        std::cerr << "No saturation BCs responsible for filling face: " << face << std::endl;
        BoxLib::Abort();
      }
    }
  }
#endif

  // Do tracers (requires bc on all faces)
  if (setup_tracer_transport) {
    for (OrientationIter oitr; oitr; ++oitr) {
      const Orientation& face = oitr();
      if (PorousMedia::grids_on_side_of_domain(grids,domain,face)) {

        // Grow here to pick up corners and edges
        Box ccBndBox  = BoxLib::adjCell(domain,face,nGrowHYP);        
        for (int i=0; i<BL_SPACEDIM; ++i) {
          if (i != face.coordDir()) {
            ccBndBox.grow(i,nGrowHYP);
          }
        }
        // Find BCs for this face
        int idx = face.coordDir() + 3*face.isHigh();
        const std::string& purpose = PMAMR::RpurposeDEF[idx];
        
        for (int n=0; n<ntracers; ++n) {
          const PArray<RegionData>& tbcs = PorousMedia::TBCs(n);
          Array<int> myTBCs;
          for (int i=0; i<tbcs.size(); ++i) {
            const Array<const Region*>& tregions = tbcs[i].Regions();
            int tfound = 0;
            for (int j=0; j<tregions.size(); ++j) {
              if (tregions[j]->purpose == purpose) {
                tfound++;
              }
            }
                            
            if (tfound) {
              myTBCs.push_back(i);
            }
          }
          if (myTBCs.size() > 0) 
          {
            tbc_descriptor_map[n][face] = BCDesc(ccBndBox,myTBCs);
          }
        }
      }
    }
  }

    
  // setup boundary descriptor for the pressure
  pbc_descriptor_map.clear();
  const BCRec& pbc = desc_lst[Press_Type].getBC(0);

  for (OrientationIter oitr; oitr; ++oitr) {
    Orientation face = oitr();

    if (PorousMedia::grids_on_side_of_domain(grids,domain,face)) 
    {
      Box ccBndBox  = BoxLib::adjCell(domain,face,1);
      if (ccBndBox.ok()) {

        // Find BCs for this face
        int idx = face.coordDir() + 3*face.isHigh();
        const std::string& purpose = PMAMR::RpurposeDEF[idx];

        const PArray<RegionData>& bcs = PorousMedia::BCs();
        Array<int> myBCs;
        for (int i=0; i<bcs.size(); ++i) {
          const Array<const Region*>& regions = bcs[i].Regions();
          int found = 0;
          for (int j=0; j<regions.size(); ++j) {
            if (regions[j]->purpose == purpose) {
              found++;
            }
          }

          if (found) {
            myBCs.push_back(i);
          }
        }
        if (myBCs.size() > 0) 
        {
          pbc_descriptor_map[face] = BCDesc(ccBndBox,myBCs);
        }
        else {
          //std::cerr << "No pressure BCs responsible for filling face: " << face << std::endl;
          //BoxLib::Abort();
        }
      }
    }
  }
}

PorousMedia::PorousMedia (Amr&            papa,
                          int             lev,
                          const Geometry& level_geom,
                          const BoxArray& bl,
                          Real            time)
  :
  AmrLevel(papa,lev,level_geom,bl,time),
  //
  // Make room for ncomps+ntracers in aux_boundary_data_old.
  // With AMANZI we only use the ntracers parts.  But by using ncomps+ntracers
  // we don't need to worry about the case when ntracers==0.
  //
  aux_boundary_data_old(bl,nGrowHYP,ncomps+ntracers,level_geom),
  FillPatchedOldState_ok(true)
{
  if (!initialized) {
    BoxLib::ExecOnFinalize(CleanupStatics);
    initialized = true;
  }

  //
  // Build metric coefficients for RZ calculations.
  //
  buildMetrics();

  //
  // Set up reflux registers.
  //
  advflux_reg  = 0;
  viscflux_reg = 0;
  if (level > 0 && do_reflux)
    {
      advflux_reg  = new FluxRegister(grids,crse_ratio,level,NUM_SCALARS);
      viscflux_reg = new FluxRegister(grids,crse_ratio,level,NUM_SCALARS);
    }

  //
  // Initialize work multifabs.
  //
  Ssync        = 0;
  u_mac_prev   = 0;
  u_macG_prev  = 0;
  u_mac_curr   = 0;
  u_macG_curr  = 0;
  u_macG_trac  = 0;
  rhs_RhoD     = 0;
  u_corr       = 0;
  kappa        = 0;
  kpedge       = 0;
  kr_coef      = 0;
  cpl_coef     = 0;
  materialID   = 0;
  lambda       = 0;
  lambda_cc    = 0;
  lambdap1_cc  = 0;
  dlambda_cc   = 0;
  rock_phi     = 0;
  specific_storage = 0;

  component_saturations_cached = false;
  sat_old_cached = 0;
  sat_new_cached = 0;
  t_sat_old_cached = -1;
  t_sat_new_cached = -1;
  //
  // Set up the godunov box.
  //
  SetGodunov();
  //
  // Set up diffusion.
  //
  int num_diffusion = ndiff;
  diffusion = new Diffusion(parent,this,
			    (level > 0) ? getLevel(level-1).diffusion : 0,
			    num_diffusion,viscflux_reg,volume,area,
			    is_diffusive,visc_coef);
  //
  // Allocate space for variable diffusion coefficients
  //
  diffn_cc   = 0;
  diffnp1_cc = 0;

  if (variable_scal_diff || ntracers>0) 
    {
      int num_diff = (diffuse_tracers ? ndiff+ntracers : ndiff);
      diffn_cc   = new MultiFab(grids, num_diff, 1);
      diffnp1_cc = new MultiFab(grids, num_diff, 1);
    }

  // Allocate space for the capillary pressure diffusive term
  pcn_cc   = 0;
  pcnp1_cc = 0;
  if (have_capillary)
  {
    pcn_cc     = new MultiFab(grids, 1, 2);
    pcnp1_cc   = new MultiFab(grids, 1, 2);
    (*pcn_cc).setVal(0.);
    (*pcnp1_cc).setVal(0.);
  }
  
  if ((model != PM_RICHARDS)
      && (model != PM_STEADY_SATURATED)
      && (model != PM_SATURATED)) {
    //
    // Set up the mac projector.
    //
    if (mac_projector == 0)
    {
      mac_projector = new MacProj(parent,parent->finestLevel(),
				  &phys_bc,do_any_diffuse);
    }
    mac_projector->install_level(level,this,volume,area);
  }
  //
  // Alloc MultiFab to hold advective update terms.
  //
  aofs = new MultiFab(grids,NUM_SCALARS,0);
  
  //
  // Alloc MultiFab to hold rock quantities
  //
  BL_ASSERT(kappa == 0);
  kappa = new MultiFab(grids,1,3);

  BL_ASSERT(rock_phi == 0);
  rock_phi = new MultiFab(grids,1,3);

  BL_ASSERT(materialID == 0);
  materialID = new iMultiFab(grids,1,3);

  if (model != PM_SINGLE_PHASE
      && (model != PM_SINGLE_PHASE_SOLID)
      && (model != PM_STEADY_SATURATED)
      && (model != PM_SATURATED) )
    {
      BL_ASSERT(kr_coef == 0);
      kr_coef = new MultiFab(grids,5,1);
      (*kr_coef).setVal(0.);

      BL_ASSERT(cpl_coef == 0);
      cpl_coef = new MultiFab(grids,5,3);
      (*cpl_coef).setVal(0.);

      BL_ASSERT(lambda_cc == 0);
      lambda_cc = new MultiFab(grids,ncomps,1);
      (*lambda_cc).setVal(1.);
    
      BL_ASSERT(lambdap1_cc == 0);
      lambdap1_cc = new MultiFab(grids,ncomps,1);
      (*lambdap1_cc).setVal(1.);

      BL_ASSERT(dlambda_cc == 0);
      dlambda_cc = new MultiFab(grids,3,1);
      (*dlambda_cc).setVal(0.);
    }

  if (model == PM_SATURATED) {
    specific_storage = new MultiFab(grids,1,0);
  }

  source = new MultiFab(grids,ncomps,0);
  source->setVal(0);

  BL_ASSERT(lambda == 0);
  lambda = new MultiFab[BL_SPACEDIM];
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      BoxArray edge_grids(grids);
      edge_grids.surroundingNodes(dir);
      lambda[dir].define(edge_grids,1,0,Fab_allocate);
      lambda[dir].setVal(1.e40);
    }
  //
  // Alloc MultiFab to hold u_mac
  //
  BL_ASSERT(u_mac_prev  == 0);
  BL_ASSERT(u_mac_curr  == 0);
  BL_ASSERT(u_macG_trac == 0);
  BL_ASSERT(rhs_RhoD == 0);
  u_mac_prev  = new MultiFab[BL_SPACEDIM];
  u_mac_curr  = new MultiFab[BL_SPACEDIM];
  u_macG_trac = new MultiFab[BL_SPACEDIM];
  u_macG_curr = new MultiFab[BL_SPACEDIM];
  u_macG_prev = new MultiFab[BL_SPACEDIM];
  rhs_RhoD    = new MultiFab[BL_SPACEDIM];
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      BoxArray edge_grids(grids);
      edge_grids.surroundingNodes(dir);
      u_mac_prev[dir].define(edge_grids,1,0,Fab_allocate);
      u_mac_prev[dir].setVal(1.e40);
      u_mac_curr[dir].define(edge_grids,1,0,Fab_allocate);
      u_mac_curr[dir].setVal(1.e40);
      rhs_RhoD[dir].define(edge_grids,1,0,Fab_allocate);
      rhs_RhoD[dir].setVal(1.e40);
      edge_grids.grow(1);
      u_macG_trac[dir].define(edge_grids,1,0,Fab_allocate);
      u_macG_trac[dir].setVal(1.e40);	
      u_macG_curr[dir].define(edge_grids,1,0,Fab_allocate);
      u_macG_curr[dir].setVal(1.e40);	
      u_macG_prev[dir].define(edge_grids,1,0,Fab_allocate);
      u_macG_prev[dir].setVal(1.e40);	
    }
  BL_ASSERT(kpedge == 0);
  kpedge     = new MultiFab[BL_SPACEDIM];
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      BoxArray edge_gridskp(grids);
      edge_gridskp.surroundingNodes(dir);
      kpedge[dir].define(edge_gridskp,1,0,Fab_allocate);
      kpedge[dir].setVal(1.e40);
    }

  // Must initialize to zero because we test on zero in estDt.
  dt_eig = 0;

  if (parent && !physics_events_registered) {
    RegisterPhysicsBasedEvents();
    physics_events_registered = true;
  }

  // Set up boundary condition work
  setup_bound_desc();
}

PorousMedia::~PorousMedia ()
{
  delete Ssync;
  delete advflux_reg;
  delete viscflux_reg;
  delete [] u_mac_prev;
  delete [] u_mac_curr;
  delete [] u_macG_prev;
  delete [] u_macG_curr;
  delete [] u_macG_trac;
  delete [] u_corr;
  delete [] rhs_RhoD;
  delete [] kpedge;
  delete [] lambda;
  delete kappa;
  delete rock_phi;
  delete specific_storage;
  delete kr_coef;
  delete cpl_coef;
  delete materialID;
  delete lambda_cc;
  delete lambdap1_cc;
  delete dlambda_cc;
  delete diffn_cc;
  delete diffnp1_cc;
  delete pcn_cc;
  delete pcnp1_cc;
  delete diffusion;
  delete aofs;
  delete sat_old_cached;
  delete sat_new_cached;
  delete source;

  if (level==0) {
    if (richard_solver != 0) {
      delete richard_solver;
    }
    
    if (richard_solver_control != 0) {
      delete richard_solver_control;
    }
    
    if (richard_solver_data != 0) {
      delete richard_solver_data;
    }
  }
}

void
PorousMedia::allocOldData ()
{
  for (int k = 0; k < num_state_type; k++)
    {
      state[k].allocOldData();
    }
}

void
PorousMedia::removeOldData ()
{
  AmrLevel::removeOldData();
}

void
PorousMedia::SetGodunov()
{
  if (godunov == 0)
    godunov = new Godunov();
}

void
PorousMedia::restart (Amr&          papa,
                      std::istream& is,
                      bool          bReadSpecial)
{
  AmrLevel::restart(papa,is,bReadSpecial);
  is >> dt_eig;

  if (verbose>2 && ParallelDescriptor::IOProcessor()) {
    Real dt_cfl = (cfl>0 ? cfl : 1)*dt_eig;
    std::cout << "Estimated time step from level " << level << " = " << dt_cfl << '\n';
  }
  //
  // Make room for ncomps+ntracers in aux_boundary_data_old.
  // With AMANZI we only use the ntracers parts.  But by using ncomps+ntracers
  // we don't need to worry about the case when ntracers==0.
  //
  aux_boundary_data_old.initialize(grids,nGrowHYP,ncomps+ntracers,Geom());

  FillPatchedOldState_ok = true;

  set_overdetermined_boundary_cells(state[State_Type].curTime());
  //
  // Set the godunov box.
  //
  SetGodunov();

  if ((model != PM_RICHARDS)
      && (model != PM_SATURATED)
      && (model != PM_STEADY_SATURATED) ) {

    if (mac_projector == 0) {
      mac_projector = new MacProj(parent,parent->finestLevel(),
				  &phys_bc,do_any_diffuse);
    }
    mac_projector->install_level(level,this,volume,area );
  }

  aofs = new MultiFab(grids,NUM_SCALARS,0);

  //
  // Build metric coefficients for RZ calculations.
  //
  buildMetrics();

  BL_ASSERT(advflux_reg == 0);
  if (level > 0 && do_reflux)
    {
      advflux_reg = new FluxRegister(grids,crse_ratio,level,NUM_SCALARS);
    }
  BL_ASSERT(viscflux_reg == 0);
  if (level > 0 && do_reflux)
    {
      viscflux_reg = new FluxRegister(grids,crse_ratio,level,NUM_SCALARS);
    }

  int num_diffusion = ndiff;
  diffusion = new Diffusion(parent, this,
			    (level > 0) ? getLevel(level-1).diffusion : 0,
			    num_diffusion, viscflux_reg, volume, area,
			    is_diffusive, visc_coef);
  //
  // Allocate the storage for variable diffusivity
  //
  diffn_cc   = 0;
  diffnp1_cc = 0;    
  if (variable_scal_diff || ntracers>0) 
    {
      diffn_cc   = new MultiFab(grids, ndiff, 1);
      diffnp1_cc = new MultiFab(grids, ndiff, 1);
    }
  //
  // Allocate the storage for capillary pressure
  //
  pcn_cc     = 0;
  pcnp1_cc   = 0;    
  if (have_capillary) 
    {
      pcn_cc     = new MultiFab(grids, 1, 2);
      pcnp1_cc   = new MultiFab(grids, 1, 2);
      (*pcn_cc).setVal(0.);
      (*pcnp1_cc).setVal(0.);
    }

  is_first_step_after_regrid = false;
  old_intersect_new          = grids;

  //
  // Alloc MultiFab to hold rock quantities
  //
  BL_ASSERT(kappa == 0);
  kappa = new MultiFab(grids,1,3); 

  BL_ASSERT(rock_phi == 0);
  rock_phi = new MultiFab(grids,1,3);

  BL_ASSERT(materialID == 0);
  materialID = new iMultiFab(grids,1,3);

  if ( (model != PM_SINGLE_PHASE)
       && (model != PM_SINGLE_PHASE_SOLID)
       && (model != PM_STEADY_SATURATED)
       && (model != PM_SATURATED) )
    {
      BL_ASSERT(kr_coef == 0);
      kr_coef = new MultiFab(grids,5,1);
      (*kr_coef).setVal(0.);

      BL_ASSERT(cpl_coef == 0);
      cpl_coef = new MultiFab(grids,5,3);
      (*cpl_coef).setVal(0.);

      BL_ASSERT(lambda_cc == 0);
      lambda_cc = new MultiFab(grids,ncomps,1);
      (*lambda_cc).setVal(1.);
    
      BL_ASSERT(lambdap1_cc == 0);
      lambdap1_cc = new MultiFab(grids,ncomps,1);
      (*lambdap1_cc).setVal(1.);

      BL_ASSERT(dlambda_cc == 0);
      dlambda_cc = new MultiFab(grids,3,1);
      (*dlambda_cc).setVal(0.);
    }

  if (model == PM_SATURATED) {
    specific_storage = new MultiFab(grids,1,0);
  }

  source = new MultiFab(grids,ncomps,0);
  source->setVal(0);

  BL_ASSERT(lambda == 0);
  lambda = new MultiFab[BL_SPACEDIM];
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      BoxArray edge_gridskp(grids);
      edge_gridskp.surroundingNodes(dir);
      lambda[dir].define(edge_gridskp,1,0,Fab_allocate);
      lambda[dir].setVal(1.e40);
    }

  BL_ASSERT(kpedge == 0);
  kpedge     = new MultiFab[BL_SPACEDIM];
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      BoxArray edge_gridskp(grids);
      edge_gridskp.surroundingNodes(dir);
      kpedge[dir].define(edge_gridskp,1,0,Fab_allocate);
      kpedge[dir].setVal(1.e40);
    }

  //
  // Alloc MultiFab to hold u_mac
  //
  u_mac_prev  = new MultiFab[BL_SPACEDIM];
  u_mac_curr  = new MultiFab[BL_SPACEDIM];
  u_macG_trac = new MultiFab[BL_SPACEDIM];
  rhs_RhoD    = new MultiFab[BL_SPACEDIM];
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      BoxArray edge_grids(grids);
      edge_grids.surroundingNodes(dir);
      u_mac_prev[dir].define(edge_grids,1,0,Fab_allocate);
    }

  std::string Level = BoxLib::Concatenate("Level_", level, 1);
  std::string FullPath = papa.theRestartFile();
  if (!FullPath.empty() && FullPath[FullPath.length()-1] != '/')
    FullPath += '/';
  FullPath += Level;
        
  std::string uxfile = "/umac_x";
  std::string uyfile = "/umac_y";
  uxfile = FullPath + uxfile;
  uyfile = FullPath + uyfile;
  VisMF::Read(u_mac_curr[0],uxfile);
  VisMF::Read(u_mac_curr[1],uyfile);

#if (BL_SPACEDIM == 3)
  std::string uzfile = "/umac_z";
  uzfile = FullPath + uzfile;
  VisMF::Read(u_mac_curr[2],uzfile);
#endif

  std::string utxfile = "/umact_x";
  std::string utyfile = "/umact_y";
  utxfile = FullPath + utxfile;
  utyfile = FullPath + utyfile;
  VisMF::Read(u_macG_trac[0],utxfile);
  VisMF::Read(u_macG_trac[1],utyfile);

#if (BL_SPACEDIM == 3)
  std::string utzfile = "/umact_z";
  utzfile = FullPath + utzfile;
  VisMF::Read(u_macG_trac[2],utzfile);
#endif

  BL_ASSERT(u_macG_curr == 0);
  u_macG_curr = AllocateUMacG();
  u_macG_prev = AllocateUMacG();
  for (int d=0; d<BL_SPACEDIM; ++d) {
    MultiFab::Copy(u_macG_curr[d],u_macG_trac[d],0,0,1,0);
    MultiFab::Copy(u_macG_prev[d],u_macG_trac[d],0,0,1,0);
  }
  
#ifdef MG_USE_FBOXLIB
  if ( (model != PM_RICHARDS)
       && (model != PM_STEADY_SATURATED)
       && (model != PM_SATURATED) )
    {
      std::string rxfile = "/rhs_RhoD_x";
      std::string ryfile = "/rhs_RhoD_y";
      rxfile = FullPath + rxfile;
      ryfile = FullPath + ryfile;
      VisMF::Read(rhs_RhoD[0],rxfile);
      VisMF::Read(rhs_RhoD[1],ryfile);
      
#if (BL_SPACEDIM == 3)
      std::string rzfile = "/rhs_RhoD_z";
      rzfile = FullPath + rzfile;
      VisMF::Read(rhs_RhoD[2],rzfile);
#endif
    }
#endif

  is_grid_changed_after_regrid = true;
  if (grids == papa.getLevel(level).boxArray())
    is_grid_changed_after_regrid = false;

  if (parent && !physics_events_registered) {
    RegisterPhysicsBasedEvents();
    physics_events_registered = true;
  }

  // Set up boundary condition work
  setup_bound_desc();
}

void
PorousMedia::buildMetrics ()
{
  //
  // Build volume and face area arrays.
  //
  geom.GetVolume(volume,grids,nGrowMG);
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      geom.GetFaceArea(area[dir],grids,dir,nGrowMG);
    }
}

//
// Reset the time levels to time (time) and timestep dt.
// This is done at the start of the timestep in the pressure iteration section.
//

void
PorousMedia::resetState (Real time,
                         Real dt_old,
                         Real dt_new)
{
  for (int k = 0; k < num_state_type; k++)
    {
      state[k].reset();
      state[k].setTimeLevel(time,dt_old,dt_new);
    }
}

//
// Set the time levels to time (time) and timestep dt.
//
void
PorousMedia::setTimeLevel (Real time,
                           Real dt_old,
                           Real dt_new)
{
  for (int k = 0; k < num_state_type; k++)
    state[k].setTimeLevel(time,dt_old,dt_new);
}

void
PorousMedia::set_vel_from_bcs(Real      time,
			      MultiFab* vel)
{
  //BoxLib::Abort("This hack is no longer valid");

  FArrayBox inflow;
  for (OrientationIter oitr; oitr; ++oitr) {
    Orientation face = oitr();

    PorousMedia* coarsest_pm = dynamic_cast<PorousMedia*>(&parent->getLevel(0));

    FArrayBox mask;
    if (coarsest_pm->get_inflow_velocity(face,inflow,mask,time)) {
      // NOTE: Without doing a pressure solve formally, it is not
      //  obvious how to do this in a way that makes sense.  The hack
      //  for the moment is to just do a setval over the entire 
      //  field....
      Real inflow_val = inflow(inflow.box().smallEnd(),0);
      for (int d=0; d<BL_SPACEDIM; ++d) {
	if (d==face.coordDir()) {
	  vel[d].setVal(inflow_val);
	} else {
	  vel[d].setVal(0);
	}
      }
    }
  }
}

typedef RockManager::ChemICMap ChemICMap;
typedef RockManager::ICLabelParmPair ICLabelParmPair;

void
PorousMedia::initData ()
{
    BL_PROFILE(BL_PROFILE_THIS_NAME() + "::initData()");
    // 
    // Initialize rock properties
    //
    init_rock_properties();

    //
    // Initialize the state and the pressure.
    //
    const Real* dx       = geom.CellSize();
    MultiFab&   S_new    = get_new_data(State_Type);
    MultiFab&   P_new    = get_new_data(Press_Type);
    MultiFab&   U_vcr    = get_new_data(  Vcr_Type);
    
    const Real  cur_time = state[State_Type].curTime();
    S_new.setVal(0.);
    P_new.setVal(0.);
    
    //
    // Initialized only based on solutions at the current level
    //
    for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
    {
      BL_ASSERT(grids[mfi.index()] == mfi.validbox());
        
      FArrayBox& sdat = S_new[mfi];
      FArrayBox& pdat = P_new[mfi];
      DEF_LIMITS(sdat,s_ptr,s_lo,s_hi);
      DEF_LIMITS(pdat,p_ptr,p_lo,p_hi);
      const Box& vbox = mfi.validbox();
      const int* lo = vbox.loVect();
      const int* hi = vbox.hiVect();
        
      for (int i=0; i<ic_array.size(); ++i)
      {
        const RegionData& ic = ic_array[i];
        const Array<const Region*>& ic_regions = ic.Regions();
        const std::string& type = ic.Type();
            
        if (type == "file") 
        {
          std::cerr << "Initialization of initial condition based on "
                    << "a file has not been implemented yet.\n";
          BoxLib::Abort("PorousMedia::initData()");
        }
        else if (type == "scalar") 
        {
          std::cerr << "IC: scalar - no longer supported\n";
          BoxLib::Abort("PorousMedia::initData()");
        }
        else if (type == "pressure") 
        {
          Array<Real> vals = ic();
          if (region_manager == 0) {
            BoxLib::Abort("static Region manager must be set up prior to initializing pressure");
          }
          for (int jt=0; jt<ic_regions.size(); ++jt) {
            region_manager->RegionPtrArray()[jt]->setVal(P_new[mfi],vals,dx,0,0,ncomps);
          }
        }
        else if (type == "linear_pressure")
        {
          Array<Real> vals = ic();
          BL_ASSERT(vals.size() > 2*BL_SPACEDIM);

          const Real* ref_val = &(vals[0]);
          const Real* gradp = &(vals[1]);
          const Real* ref_loc = &(vals[1+BL_SPACEDIM]);
          const Real* problo = geom.ProbLo();
          const Real* probhi = geom.ProbHi();

          FORT_LINEAR_PRESSURE(lo, hi, p_ptr, ARLIM(p_lo),ARLIM(p_hi), &ncomps,
                               dx, problo, probhi, ref_val, ref_loc, gradp);
        }
        else if (type == "zero_total_velocity")
        {	     
          BL_ASSERT(model != PM_SINGLE_PHASE && 
                    model != PM_SINGLE_PHASE_SOLID);
          int nc = 1;
          Array<Real> vals = ic();

          int rmID = rock_manager->ID();

          IArrayBox& mdat = (*materialID)[mfi];
          FArrayBox& kpdat = kpedge[BL_SPACEDIM-1][mfi];
          DEF_CILIMITS(mdat,m_ptr,m_lo,m_hi);
          DEF_CLIMITS(kpdat,kp_ptr,kp_lo,kp_hi);
                

          FORT_STEADYSTATE(s_ptr, ARLIM(s_lo),ARLIM(s_hi), 
                           density.dataPtr(),muval.dataPtr(),&ncomps,
                           kp_ptr, ARLIM(kp_lo),ARLIM(kp_hi), 
                           m_ptr, ARLIM(m_lo),ARLIM(m_hi),
                           &rmID,&cur_time,&vals[0], &nc, &gravity,
                           vbox.loVect(),vbox.hiVect());
		
          // set pressure
          if (model==PM_RICHARDS) {
            const int idx = mfi.index();
            FArrayBox pc(vbox,1);
            FArrayBox s(vbox,1); s.copy(sdat);
            IArrayBox m(vbox,1); m.copy((*materialID)[mfi]);
            rock_manager->CapillaryPressure(s.dataPtr(),m.dataPtr(),cur_time,pc.dataPtr(),vbox.numPts());
            (*pcnp1_cc)[mfi].copy(pc);
            P_new[mfi].setVal(0);
            P_new[mfi].copy((*pcnp1_cc)[mfi]);
            P_new[mfi].mult(-1.0);
            P_new[mfi].plus(atmospheric_pressure_atm);
          }
        }
        else if (type == "constant_velocity")
        {
          set_saturated_velocity();
        }
        else
        {
          std::cerr << "Unrecognized IC type: " << type << "\n";
          BoxLib::Abort("PorousMedia::initData()");
        }
      }
        
      if (ntracers > 0)
      {

        // Set chem-specific input/default data prior to any speciation calls 

        if (chemistry_helper != 0) {
            
          // Set provided default auxiliary chem data, then override with values input via RockManager
          const std::map<std::string,int>& aux_chem_variables_map = chemistry_helper->AuxChemVariablesMap();
          const std::map<std::string,Real>& aux_chem_defaults_map = chemistry_helper->AuxChemDefaultsMap();
          FArrayBox& fab = get_new_data(Aux_Chem_Type)[mfi];
          for (std::map<std::string,int>::const_iterator it=aux_chem_variables_map.begin(); it!=aux_chem_variables_map.end(); ++it) {
            const std::string& parameter = it->first;
            int comp = it->second;
            std::map<std::string,Real>::const_iterator it2 = aux_chem_defaults_map.find(parameter);
            if (it2 != aux_chem_defaults_map.end()) {
              Real value = it2->second;
              fab.setVal(value,comp);
            }
          }

          rock_manager->RockChemistryProperties(fab,dx,aux_chem_variables_map);

          if (chemistry_model_name=="Amanzi" && do_tracer_chemistry>0) {
              
            typedef std::map<std::string,Real> ICParmPair; // ic parameter and value
            typedef std::map<std::string, ICParmPair > ICLabelParmPair; // parameter/value associated label
            typedef std::map<std::string, ICLabelParmPair> ChemICMap; // 
            typedef std::map<std::string, std::map<std::string,int> > LabelIdx;
              
            const Real* dx = geom.CellSize();

            // This chunk will set (solute/region)-specific IC data in the aux_chem (such as "Free Ion Guess")
            // FIXME: "Surface Complexation Free Site Conc"
            for (ChemICMap::const_iterator it=solute_chem_ics.begin(); it!=solute_chem_ics.end(); ++it) {
              const std::string& ic_name = it->first;
              const ICLabelParmPair& solute_name_to_pp = it->second; 
              for (ICLabelParmPair::const_iterator it1=solute_name_to_pp.begin(); it1!=solute_name_to_pp.end(); ++it1) {
                const std::string& solute_name = it1->first;
                int iTracer = -1;
                for (int k=0; k<ntracers; ++k) {
                  if (solute_name == tNames[k]) iTracer=k;
                }
                if (iTracer<0) {
                  std::cout << "PorousMedia::initData  IC \""<< ic_name
                            << "\" attempting to initialize concentration of unknown species: \""
                            << solute_name << "\"" << std::endl;
                  BoxLib::Abort();
                }
                const ICParmPair& parm_pairs = it1->second;
                for (ICParmPair::const_iterator it2=parm_pairs.begin(); it2!=parm_pairs.end(); ++it2) {
                  const std::string& parameter = it2->first;
                  std::string key = solute_name+"_"+parameter;
                  std::map<std::string,int>::const_iterator it3 = aux_chem_variables_map.find(key);
                  if (it3 == aux_chem_variables_map.end()) {
                    std::cout << "PorousMedia::initData  Unable to locate parameter in aux_data, "
                              << parameter << "  key: " << key << std::endl;
                    BoxLib::Abort();
                  }
                  int comp = it3->second;
                  Real value = it2->second;

                  if (comp < 3) {
                    std::cout << "label " << parameter << std::endl;
                    BoxLib::Abort();
                  }

                  const PArray<RegionData>& rds = tic_array[iTracer];
                  for (int k=0; k<rds.size(); ++k) {
                    const Array<const Region*>& rock_regions = rds[k].Regions();
                    for (int j=0; j<rock_regions.size(); ++j) {
                      rock_regions[j]->setVal(fab,value,comp,dx,0);
                    }
                  }
                }
              }
            }
          }

          for (int iTracer=0; iTracer<ntracers; ++iTracer)
          {
            const PArray<RegionData>& rds = tic_array[iTracer];

            for (int i=0; i<rds.size(); ++i)
            {
              const RegionData& tic = rds[i];
              const Array<const Region*>& tic_regions = tic.Regions();
              const std::string& tic_type = tic.Type();
                    
              if (tic_type == "file") 
              {
                std::cerr << "Initialization of initial condition based on "
                          << "a file has not been implemented yet.\n";
                BoxLib::Abort("PorousMedia::initData()");
              }
              else if (tic_type == "concentration") {
                Array<Real> val = tic();
                for (int jt=0; jt<tic_regions.size(); ++jt) {
                  BL_ASSERT(val.size()>=1);
                  BL_ASSERT(sdat.nComp()>ncomps+iTracer);
                  BL_ASSERT(tic_regions.size()>jt);
                  tic_regions[jt]->setVal(sdat,val[val.size()-1],ncomps+iTracer,dx,0);

                  if (chemistry_model_name=="Alquimia" && do_tracer_chemistry>0) 
                  {
                    FArrayBox& aux =  get_new_data(Aux_Chem_Type)[mfi];
                    BL_ASSERT(chemistry_helper != 0);
                    int Naux = chemistry_helper->AuxChemVariablesMap().size();
                    BL_ASSERT(val.size() == Naux + 1);
                    for (int iAux=0; iAux<Naux; ++iAux) {
                      tic_regions[jt]->setVal(aux,val[iAux],iAux,dx,0);
                    }
                  }
                }
              }
              else {
                std::string m = "Unrecognized tracer ic type: " + tic_type;
                BoxLib::Abort(m.c_str());
              }
            }
          }
        }
      }

      if (chemistry_model_name=="Amanzi" && do_tracer_chemistry>0) {

        // "Speciate" the chemistry (set up remaining chem data)
        std::cout << "Speciating "<< std::endl;
        for (MFIter mfi(S_new); mfi.isValid(); ++mfi) {
          Box box = mfi.validbox();
          FArrayBox& sat   = S_new[mfi];
          sat.mult(1/density[0],0,1);

          FArrayBox& press = P_new[mfi];
          FArrayBox& phi = (*rock_phi)[mfi];
          FArrayBox& vol = volume[mfi];
          FArrayBox& fct = get_new_data(FuncCount_Type)[mfi];
          FArrayBox& aux = get_new_data(Aux_Chem_Type)[mfi];

          AmanziChemHelper_Structured* achp = dynamic_cast<AmanziChemHelper_Structured*>(chemistry_helper);
          achp->Initialize(sat,0,press,0,phi,0,vol,0,sat,ncomps,fct,0,aux,density[0],298,box);
          sat.mult(density[0],0,1);
        }
        std::cout << "Speciating complete" << std::endl;
      }
    }

    if (do_tracer_chemistry!=0) {
        get_new_data(FuncCount_Type).setVal(1);
    }

    if ( (model == PM_STEADY_SATURATED)
	 || (model == PM_SATURATED) ) {
      for (int n=0; n<ncomps; ++n) {
        S_new.setVal(density[n],n,1);
      }
    } else {
      calcInvPressure(S_new,P_new,cur_time,0,0,0); // Set sat from p, no grow cells
    }
    if (have_capillary) {
      if (model != PM_RICHARDS || do_richard_sat_solve) {
	calcCapillary(cur_time);
      }
    }
    U_vcr.setVal(0.);
    //
    // compute lambda
    //
    if ( (model != PM_STEADY_SATURATED)
	 && (model != PM_SATURATED) ) {
      if (model == PM_RICHARDS) {
        calcLambda(*lambdap1_cc,get_new_data(State_Type),cur_time,0,0,0); // Use rho.sat computed above
      } else {
        calcLambda(cur_time);
      }
    }

    //
    // Initialize u_mac_curr 
    //
    if ( (model != PM_RICHARDS)
	 && (model != PM_STEADY_SATURATED)
	 && (model != PM_SATURATED) ) {

      if (u_macG_curr == 0) {
        u_macG_curr = AllocateUMacG();
      }
      mac_project(u_mac_curr,rhs_RhoD,cur_time);
        
      if (level == 0) {
        create_umac_grown(u_mac_curr,u_macG_trac);
      } else {
        PArray<MultiFab> u_macG_crse(BL_SPACEDIM,PArrayManage);
        GetCrseUmac(u_macG_crse,cur_time);
        create_umac_grown(u_mac_curr,u_macG_crse,u_macG_curr); 
      }
      
      if (u_macG_prev == 0) {
        u_macG_prev = AllocateUMacG();
      }
      if (u_macG_trac == 0) {
        u_macG_trac = AllocateUMacG();
      }

      // Initialize u_macG_prev, u_macG_trac
      for (int d=0; d<BL_SPACEDIM; ++d) {
        MultiFab::Copy(u_macG_prev[d],u_macG_curr[d],0,0,1,u_macG_curr[d].nGrow());
        MultiFab::Copy(u_macG_trac[d],u_macG_curr[d],0,0,1,u_macG_curr[d].nGrow());
      }

    }
    is_grid_changed_after_regrid = false;
        
    // Call chemistry to relax initial data to equilibrium
    bool chem_relax_ics = false;
    if (do_tracer_chemistry>0  &&  ic_chem_relax_dt>0) {
      MultiFab& Fcnt = get_new_data(FuncCount_Type);
      Fcnt.setVal(1);
      int nGrow = 0;
      bool chem_ok = advance_chemistry(cur_time,ic_chem_relax_dt,nGrow);
      BL_ASSERT(chem_ok);
    }

    is_first_step_after_regrid = true;
    old_intersect_new          = grids;
}

#include <RichardSolver.H>

std::map<int,std::string> PETSc_Reasons;
static std::string
GetPETScReason(int flag) 
{
  PETSc_Reasons[2] = "SNES_CONVERGED_FNORM_ABS     ";
  PETSc_Reasons[3] = "SNES_CONVERGED_FNORM_RELATIVE"; // ||F|| < atol 
  PETSc_Reasons[4] = "SNES_CONVERGED_SNORM_RELATIVE"; // Newton computed step size small; || delta x || < stol 
  PETSc_Reasons[5] = "SNES_CONVERGED_ITS           "; // maximum iterations reached 
  PETSc_Reasons[7] = "SNES_CONVERGED_TR_DELTA      ";
  PETSc_Reasons[-1] = "SNES_DIVERGED_FUNCTION_DOMAIN"; // the new x location passed the function is not in the domain of F
  PETSc_Reasons[-2] = "SNES_DIVERGED_FUNCTION_COUNT ";
  PETSc_Reasons[-3] = "SNES_DIVERGED_LINEAR_SOLVE   "; // the linear solve failed
  PETSc_Reasons[-4] = "SNES_DIVERGED_FNORM_NAN      ";
  PETSc_Reasons[-5] = "SNES_DIVERGED_MAX_IT         ";
  PETSc_Reasons[-6] = "SNES_DIVERGED_LINE_SEARCH    "; // the line search failed 
  PETSc_Reasons[-7] = "SNES_DIVERGED_INNER          "; // inner solve failed
  PETSc_Reasons[-8] = "SNES_DIVERGED_LOCAL_MIN      "; // || J^T b || is small, implies converged to local minimum of F()
  PETSc_Reasons[-9] = "RS: dt too small             ";
  PETSc_Reasons[0]  = "SNES_CONVERGED_ITERATING     ";
  if (PETSc_Reasons.find(flag)==PETSc_Reasons.end()) {
    BoxLib::Abort("Unknown PETSc return flag");
  }
  return PETSc_Reasons[flag];
}

void
PorousMedia::BuildNLScontrolData(NLScontrol&        nlsc,
                                 RSdata&            rs_data,
                                 const std::string& IDstring)
{
  // For the moment, ignore IDstring: all solver setups identical

  if (!steady_use_PETSc_snes) {
    nlsc.use_PETSc_snes = false;
  }
  rs_data.upwind_krel = richard_upwind_krel;
  rs_data.pressure_maxorder = richard_pressure_maxorder;
  rs_data.semi_analytic_J = richard_semi_analytic_J;
  rs_data.variable_switch_saturation_threshold = richard_variable_switch_saturation_threshold;

  nlsc.max_ls_iterations = richard_max_ls_iterations;
  nlsc.min_ls_factor = richard_min_ls_factor;
  nlsc.ls_acceptance_factor = richard_ls_acceptance_factor;
  nlsc.ls_reduction_factor = richard_ls_reduction_factor;
  nlsc.monitor_line_search = richard_monitor_line_search;
  nlsc.errfd = richard_perturbation_scale_for_J;
  nlsc.maxit = steady_limit_iterations;
  nlsc.maxf = steady_limit_function_evals;
  nlsc.atol = steady_abs_tolerance;
  nlsc.rtol = steady_rel_tolerance;
  nlsc.stol = steady_abs_update_tolerance;
  nlsc.use_fd_jac = richard_use_fd_jac;
  nlsc.use_dense_Jacobian = richard_use_dense_Jacobian;
  nlsc.scale_soln_before_solve = richard_scale_solution_before_solve;
  nlsc.centered_diff_J = richard_centered_diff_J;

  nlsc.SetMaxConsecutiveFails(steady_max_consecutive_failures_1);
  nlsc.SetDtRetryFactor(steady_time_step_retry_factor_1);

  nlsc.SetMaxConsecutiveFails2(steady_max_consecutive_failures_2);
  nlsc.SetDtRetryFactor2(steady_time_step_retry_factor_2);
  nlsc.SetDtRetryFactorF(steady_time_step_retry_factor_f);

  nlsc.SetMinNewtonIterationsForDt(steady_min_iterations);
  nlsc.SetDtIncreaseFactor(steady_time_step_increase_factor);
  nlsc.SetMinNewtonIterationsForDt2(steady_min_iterations_2);
  nlsc.SetDtIncreaseFactor2(steady_time_step_increase_factor_2);

  nlsc.SetMaxNewtonIterationsForDt(steady_max_iterations);
  nlsc.SetDtReductionFactor(steady_time_step_reduction_factor);

  nlsc.SetMaxNewtonIterations(steady_limit_iterations);

  nlsc.SetMaxConsecutiveErrIncrease(steady_max_num_consecutive_increases);
  nlsc.SetConsecutiveErrIncreaseDtReduction(steady_consecutive_increase_reduction_factor);

  nlsc.SetMaxConsecutiveSuccess(steady_max_num_consecutive_success);

  nlsc.SetMaxDt(steady_max_time_step_size);

  // Now that all parameters are set, build data structures (which may depend on
  //  the settings of these parametes)
  rs_data.SetUpMemory(nlsc);
}

void
PorousMedia::richard_init_to_steady()
{
  //
  // Richard initialization
  //
  if ( (model == PM_RICHARDS)
       || (model == PM_STEADY_SATURATED)
       || (model == PM_SATURATED) ) {
    std::string tag = "Pre-Execution Mode Solve";
    if (richard_init_to_steady_verbose && ParallelDescriptor::IOProcessor()) {
      std::cout << tag << std::endl;
    }        
    
    if (level == 0) {
      int old_richard_solver_verbose = richard_solver_verbose;
      richard_solver_verbose = richard_init_to_steady_verbose;
      initial_iter = 1;

      Real cur_time = state[State_Type].curTime();
      Real prev_time = state[State_Type].prevTime();
      int  finest_level = parent->finestLevel();
      Array<Real> dt_save(finest_level+1);
      Array<int> nc_save(finest_level+1);
      int  n_factor;
      for (int k = 0; k <= finest_level; k++) {
        nc_save[k] = parent->nCycle(k);
        dt_save[k] = parent->dtLevel()[k];
        dt_save[k] = parent->getLevel(k).get_state_data(0).curTime()
          - getLevel(k).get_state_data(0).prevTime();
      }
	
      Real t_max = steady_max_psuedo_time;
      Real dt_init = steady_init_time_step;
      Real dt = dt_init;
      int k_max = steady_max_time_steps;
      Real t_eps = 1.e-8*dt_init;
	
      MultiFab tmp(grids,1,1);
      MultiFab tmpP(grids,1,1);
      int nc = 0; // Component of water in state
      PMAmr* p = dynamic_cast<PMAmr*>(parent); BL_ASSERT(p);
	
      bool solved = false;
      int total_num_Newton_iterations = 0;
      int total_rejected_Newton_steps = 0;

      int grid_seq_init_fine = (steady_do_grid_sequence ? 0 : finest_level);

      Array<Real> new_level_dt_factor;
      if (steady_do_grid_sequence) {
        BL_ASSERT(steady_grid_sequence_new_level_dt_factor.size()>0);
        new_level_dt_factor.resize(finest_level+1,steady_grid_sequence_new_level_dt_factor[0]);
        if (steady_grid_sequence_new_level_dt_factor.size()>1) {
	  int num = std::min(steady_grid_sequence_new_level_dt_factor.size(),new_level_dt_factor.size());
	  for (int i=0; i<num; ++i) {
	    new_level_dt_factor[i] = steady_grid_sequence_new_level_dt_factor[i];
	  }
        }
        if (new_level_dt_factor.size()<finest_level-1) {
          BoxLib::Abort("steady_grid_sequence_new_level_dt_factor requires either 1 or max_level entries");
        }
      }

      bool attempting_pure_steady;
      Real dt_thresh_pure_steady = richard_dt_thresh_pure_steady;

      for (int grid_seq_fine=grid_seq_init_fine; grid_seq_fine<=finest_level; ++grid_seq_fine) {

        attempting_pure_steady = false;
        int num_active_levels = grid_seq_fine + 1;
        if (ParallelDescriptor::IOProcessor() && richard_init_to_steady_verbose) {
          std::cout << "Number of active levels: " << num_active_levels << std::endl;
        }

	std::string tmp_record_file;
	if (!steady_record_file.empty()) {
	  tmp_record_file = BoxLib::Concatenate(steady_record_file+"_",num_active_levels,2);
	}

        if (ParallelDescriptor::IOProcessor() && richard_init_to_steady_verbose && !(tmp_record_file.empty())) {
          std::cout << "Recording solve details into: \"" << tmp_record_file << "\"" << std::endl;
        }

        if (num_active_levels > 1 && grid_seq_init_fine != finest_level) {
          dt *= new_level_dt_factor[num_active_levels-2];
        }

        Layout layout_sub(parent,num_active_levels);	  
        Real prev_abs_err, init_abs_err;
        Real rel_err = -1;
        Real abs_err = -1;

        bool first = true;
        Real t = 0;
        int k = 0;
        bool continue_iterations = (!solved)  &&  (k < k_max)  &&  (t < t_max);
	  
        NLSstatus ret;

	NLScontrol nlsc;
	RSAMRdata rs_data(0,num_active_levels,layout_sub,PMParent(),nlsc,rock_manager);
        BuildNLScontrolData(nlsc, rs_data, "InitGridSequence");
        RichardSolver* rs = 0;

        if (steady_use_PETSc_snes) {
          rs = new RichardSolver(rs_data,nlsc);
        }

        while (continue_iterations) {

          if (steady_use_PETSc_snes) {
            rs->SetCurrentTimestep(k);
          }

          // Advance the state data structures
          for (int lev=0;lev<finest_level+1;lev++) {
            PorousMedia& pm = getLevel(lev);
            for (int i = 0; i < num_state_type; i++) {
              pm.state[i].allocOldData();
              pm.state[i].swapTimeLevels(dt);
              pm.state[i].setTimeLevel(t+dt,dt,dt);
            }
          }

          cur_time = state[Press_Type].curTime();
          prev_time = state[Press_Type].prevTime();

          for (int lev=0;lev<finest_level+1;lev++)
          {
            PorousMedia& pm = getLevel(lev);
            for (int i = 0; i < num_state_type; i++)
            {
              MultiFab& od = pm.get_old_data(i);
              MultiFab& nd = pm.get_new_data(i);
              MultiFab::Copy(nd,od,0,0,od.nComp(),0);  // Guess for next time step
            }
          }
	    
          if (steady_use_PETSc_snes) {
            rs->ResetRhoSat();
          }
	    
          MultiFab& S_new = get_new_data(State_Type);
          MultiFab::Copy(tmp,get_new_data(Press_Type),nc,0,1,0);
          tmp.mult(-1.0);
	    
          if (richard_init_to_steady_verbose && ParallelDescriptor::IOProcessor()) {
            std::cout << tag << "  t=" << t 
                      << ", n=" << k << ", dt=" << dt << '\n';
          }

          if (do_multilevel_full) {
            nlsc.ResetCounters();
            rs_data.ResetJacobianCounter();

            // Save the initial state so we can recover on failure
            for (int lev=0;lev<num_active_levels;lev++)
            {
              PorousMedia&    fine_lev   = getLevel(lev);
              if (do_richard_sat_solve)
              {
                MultiFab& S_lev = fine_lev.get_new_data(State_Type);
                MFTower& IC = *(rs_data.InitialState);
                MultiFab::Copy(IC[lev],S_lev,0,0,1,1);
              }
              else
              {
                MultiFab& P_lev = fine_lev.get_new_data(Press_Type);
                MFTower& IC = *(rs_data.InitialState);
                MultiFab::Copy(IC[lev],P_lev,0,0,1,1);
              }
            }

            if (steady_use_PETSc_snes) 
            {              
	      if (!tmp_record_file.empty()) {
		rs->SetRecordFile(tmp_record_file);
	      }

              attempting_pure_steady = dt_thresh_pure_steady>0 && dt>dt_thresh_pure_steady;
              Real dt_solve = attempting_pure_steady ? -1 : dt;
              if (attempting_pure_steady && ParallelDescriptor::IOProcessor())
                std::cout << "     **************** Attempting pure steady solve" << '\n';
              int retCode = rs->Solve(t, t+dt_solve, k, nlsc);

              if (retCode < 0 && attempting_pure_steady) {
                dt_thresh_pure_steady *= 10;
                if (attempting_pure_steady && ParallelDescriptor::IOProcessor())
                  std::cout << "     **************** Steady solve failed, resuming transient..." << '\n';
                attempting_pure_steady = false;
                retCode = rs->Solve(t, t+dt, k, nlsc);
              }

              if (retCode >= 0) {
                ret = NLSstatus::NLS_SUCCESS;
                rs->ComputeDarcyVelocity(rs->GetPressureNp1(),t+dt);
              } 
              else {

                if (retCode == -3) {
                  ret = NLSstatus::NLS_LINEAR_FAIL;
                }
                else if (retCode == -9) {
                  ret = NLSstatus::NLS_CATASTROPHIC_FAIL;
                }
                else {
                  ret = NLSstatus::NLS_NONLINEAR_FAIL;
                  if (richard_solver_verbose>1 && ParallelDescriptor::IOProcessor())
                    std::cout << "     **************** Newton failed: " << GetPETScReason(retCode) << '\n';
                }
              }
            }
#ifdef MG_USE_FBOXLIB
            else
            {
              // Solve for the update using native solver
              ret = richard_composite_update(dt,nlsc);
            }
#endif
            total_num_Newton_iterations += nlsc.NLIterationsTaken();
          }
#ifdef MG_USE_FBOXLIB
          else {
            int curr_nwt_iter = steady_limit_iterations;
            ret = richard_scalar_update(dt,curr_nwt_iter,u_mac_curr);
            total_num_Newton_iterations += curr_nwt_iter;
          }
#endif
          if (ret == NLSstatus::NLS_SUCCESS) {
            prev_abs_err = abs_err;
            MultiFab::Add(tmp,get_new_data(Press_Type),nc,0,1,0);
            abs_err = tmp.norm2(0);
            if (first) {
              init_abs_err = abs_err;
              first = false;
            }
            else {
              rel_err = abs_err / init_abs_err;
            }
          }
          else {
            if (steady_abort_on_psuedo_timestep_failure) {
              BoxLib::Abort("Aborting as instructed when timestep fails");
            }
            if (ret == NLSstatus::NLS_CATASTROPHIC_FAIL) {
              BoxLib::Abort("Aborting ... catastrophic solver failure");
            }

            // Otherwise, reset state to initial value and try again
            total_rejected_Newton_steps++;
            for (int lev=0;lev<num_active_levels;lev++) {
              PorousMedia&    fine_lev   = getLevel(lev);
              for (int k = 0; k < num_state_type; k++) {
                fine_lev.state[k].reset();
              }

              if (do_richard_sat_solve) {
                MultiFab& S_lev = fine_lev.get_new_data(State_Type);
                MFTower& IC = *(rs_data.InitialState);
                MultiFab::Copy(S_lev,IC[lev],0,0,1,1);
              }
              else {
                MultiFab& P_lev = fine_lev.get_new_data(Press_Type);
                MFTower& IC = *(rs_data.InitialState);
                MultiFab::Copy(P_lev,IC[lev],0,0,1,1);
              }
            }
          } // Newton fail

          Real dt_new;
          bool cont = nlsc.AdjustDt(dt,ret,dt_new);

          if (ret == NLSstatus::NLS_SUCCESS) {
            k++;
            if (attempting_pure_steady) {
              solved = true;
            }
            else {
              t += dt;
              if (execution_mode==INIT_TO_STEADY) {
                solved = false; // Do not kick out early
              }
              else {
                solved = ((abs_err <= steady_abs_update_tolerance) 
                          || ((rel_err>0)  && (rel_err <= steady_rel_update_tolerance)) );
              }
            }
            if (richard_init_to_steady_verbose>1 && ParallelDescriptor::IOProcessor()) {
              std::cout << tag << "   Step successful, Niters=" << nlsc.NLIterationsTaken() << std::endl;
            }
          }
          else {
            if (richard_init_to_steady_verbose>1 && ParallelDescriptor::IOProcessor()) {
              std::cout << tag << "   Step failed ";
              if (ret==NLSstatus::NLS_NONLINEAR_FAIL) {
                std::cout << "(NL failure)";
              }
              else {
                std::cout << "(L failure) ";
              }
            }
          } // Newton fail
		    
          continue_iterations = cont && (!solved)  &&  (k < k_max)  &&  (t < t_max);
          if (continue_iterations) {
            dt = std::min(dt_new, t_max-t);
          }
        } // time-step

        delete rs;

        if (richard_init_to_steady_verbose && ParallelDescriptor::IOProcessor()) {
          std::cout << tag << " Total psuedo-time advanced: " << t << " in " << k << " steps" << std::endl;
          std::cout << tag << "      Newton iters: " << total_num_Newton_iterations << std::endl;
          std::cout << tag << "      Rejected steps: " << total_rejected_Newton_steps << std::endl;
        }

        // Set data on next level by interpolating the pressure field and inverting out rho.sat
        if (num_active_levels<=finest_level) {
          int curr_flev = num_active_levels - 1;
          PorousMedia& pmf = dynamic_cast<PorousMedia&>(getLevel(curr_flev + 1));
          pmf.setTimeLevel(t+dt,dt,dt);
          for (int lev=0; lev<=curr_flev; ++lev) {
            PorousMedia& pmfi = dynamic_cast<PorousMedia&>(getLevel(lev));
            pmfi.setTimeLevel(t+dt,dt,dt);
          }
          pmf.FillCoarsePatch(pmf.get_new_data(Press_Type),0,t+dt,Press_Type,0,ncomps);
	  if ( (model == PM_STEADY_SATURATED)
	       || (model == PM_SATURATED) ) {
            for (int i=0; i<ncomps; ++i) {
              pmf.get_new_data(State_Type).setVal(density[i],i,1);
            }
	  } else {
	    pmf.calcInvPressure(pmf.get_new_data(State_Type),pmf.get_new_data(Press_Type),cur_time,0,0,0);
	  }
          solved = false;
          ParallelDescriptor::Barrier();
        }
	  
        if (richard_init_to_steady_verbose && ParallelDescriptor::IOProcessor()) {
          if (solved || attempting_pure_steady) {
            std::cout << tag << " Success!  Steady solution found" << std::endl;
            if (grid_seq_fine!=finest_level) {
              std::cout << tag << " Adding another refinement level and re-solving..." << std::endl;
            }
          }
          else {
            std::cout << tag << " Warning: solution is not steady.  Continuing..." << std::endl;
          }
        }
      }

      ParallelDescriptor::Barrier();
      Real time_after_init = p->startTime();
      for (int lev = 0; lev <= finest_level; lev++) {
        getLevel(lev).setTimeLevel(time_after_init,dt_save[lev],dt_save[lev]);
      }
	
      richard_solver_verbose = old_richard_solver_verbose;
      initial_iter = 0;
	    
      //
      // Re-instate timestep.
      //	
      parent->setDtLevel(dt_save);
      parent->setNCycle(nc_save);
    }
  }
}

//
// Fills a new level n with best level n and coarser data available.
//

void
PorousMedia::init (AmrLevel& old)
{
  init_rock_properties();

  PorousMedia*  oldns     = (PorousMedia*) &old;
  const Real    dt_new    = parent->dtLevel(level);
  const Real    cur_time  = oldns->state[State_Type].curTime();
  const Real    prev_time = oldns->state[State_Type].prevTime();
  const Real    dt_old    = cur_time - prev_time;

  MultiFab&     S_new     = get_new_data(State_Type);
  MultiFab&     P_new     = get_new_data(Press_Type);
  MultiFab&     U_cor     = get_new_data(  Vcr_Type);

  U_cor.setVal(0.);

  dt_eig = oldns->dt_eig;
    
  setTimeLevel(cur_time,dt_old,dt_new);
    
  //Get best state data: from old. 
  int nGrow = 0;
  get_fillpatched_rhosat(cur_time,S_new,nGrow);
  for (FillPatchIterator fpi(old,S_new,nGrow,cur_time,State_Type,ncomps,ntracers);
       fpi.isValid();
       ++fpi) 
  {
    S_new[fpi.index()].copy(fpi(),0,ncomps,ntracers);
  }
  for (FillPatchIterator fpi(old,P_new,0,cur_time,Press_Type,0,1);
       fpi.isValid();
       ++fpi)
  {
    P_new[fpi.index()].copy(fpi());
  }

  const BoxArray& old_grids = oldns->grids;
  is_grid_changed_after_regrid = old_grids != grids;

  if ( !is_grid_changed_after_regrid  ) {
    for (int d=0; d<BL_SPACEDIM; ++d) {
      //MultiFab::Copy(rhs_RhoD[d],oldns->rhs_RhoD[d],0,0,1,0);
      MultiFab::Copy(u_mac_curr[d],oldns->u_mac_curr[d],0,0,1,0);
      MultiFab::Copy(u_macG_trac[d],oldns->u_macG_trac[d],0,0,1,0);
    }
  }
  else {

    PArray<MultiFab> u_macG_crse(BL_SPACEDIM,PArrayManage);
    if (level == 0) {
      for (int d=0; d<BL_SPACEDIM; ++d) {
        u_mac_curr[d].copy(oldns->u_mac_curr[d]);
      }
      create_umac_grown(u_mac_curr,u_macG_trac);
    }
    else {
      GetCrseUmac(u_macG_crse,cur_time);
      create_umac_grown(0,u_macG_crse,u_macG_trac);
      for (int d=0; d<BL_SPACEDIM; ++d) {
        MultiFab::Copy(u_mac_curr[d],u_macG_trac[d],0,0,1,0);
        MultiFab::Copy(u_mac_curr[d],oldns->u_macG_curr[d],0,0,1,0);
        u_mac_curr[d].copy(oldns->u_mac_curr[d]);
      }
      create_umac_grown(u_mac_curr,u_macG_crse,u_macG_trac);
    }
  }
  for (int d=0; d<BL_SPACEDIM; ++d) {
    MultiFab::Copy(u_mac_prev[d],u_mac_curr[d],0,0,1,0);
    MultiFab::Copy(u_macG_curr[d],u_macG_trac[d],0,0,1,0);
  }

  if (do_tracer_chemistry>0) {
      
    MultiFab& Aux_new = get_new_data(Aux_Chem_Type);
    MultiFab& Aux_old = oldns->get_new_data(Aux_Chem_Type);
    int Aux_ncomp = Aux_new.nComp();
    for (FillPatchIterator fpi(old,Aux_new,0,cur_time,Aux_Chem_Type,0,Aux_ncomp);
         fpi.isValid();
         ++fpi) {
      Aux_new[fpi.index()].copy(fpi(),0,0,Aux_ncomp);
    }

    MultiFab& FC_new  = get_new_data(FuncCount_Type); 
    for (FillPatchIterator fpi(old,FC_new,FC_new.nGrow(),cur_time,FuncCount_Type,0,1);
         fpi.isValid();
         ++fpi) {
      FC_new[fpi.index()].copy(fpi());
    }
  }
    
  old_intersect_new          = BoxLib::intersect(grids,oldns->boxArray());
  is_first_step_after_regrid = true;
}

void
PorousMedia::init ()
{
  init_rock_properties();

  BL_ASSERT(level > 0);
    
  MultiFab& S_new = get_new_data(State_Type);
  MultiFab& P_new = get_new_data(Press_Type);
  MultiFab& U_cor = get_new_data(  Vcr_Type);
   
  const Array<Real>& dt_amr = parent->dtLevel();
  Array<Real>        dt_new(level+1);

  for (int lev = 0; lev < level; lev++)
    dt_new[lev] = dt_amr[lev];
  //
  // Guess new dt from new data (interpolated from coarser level).
  //
  const Real dt = dt_new[level-1]/Real(parent->MaxRefRatio(level-1));
  dt_new[level] = dt;
  parent->setDtLevel(dt_new);

  //
  // Compute dt based on old data.
  //
  PorousMedia& old       = getLevel(level-1);
  const Real   cur_time  = old.state[State_Type].curTime();
  const Real   prev_time = old.state[State_Type].prevTime();
  const Real   dt_old    = (cur_time-prev_time)/Real(parent->MaxRefRatio(level-1));

  setTimeLevel(cur_time,dt_old,dt);
  //
  // Get best coarse state, pressure and velocity data.
  //
  FillCoarsePatch(S_new,0,cur_time,State_Type,0,S_new.nComp());
  FillCoarsePatch(P_new,0,cur_time,Press_Type,0,1);

  U_cor.setVal(0.);

  if ((model == PM_STEADY_SATURATED)
      || (model == PM_SATURATED)) {
    set_vel_from_bcs(cur_time,u_mac_curr);
  }  


  if (do_tracer_chemistry>0) {
    FillCoarsePatch(get_new_data(FuncCount_Type),0,cur_time,FuncCount_Type,0,1);
  }

  old_intersect_new = grids;
}

//
// ADVANCE FUNCTIONS
//

//
// This function ensures that the multifab registers and boundary
// flux registers needed for syncing the composite grid
//
//     u_mac, umacG, Ssync, fr_adv, fr_visc
//
// are initialized to zero.  These quantities and the  
// advective velocity registers (mac_reg) are compiled by first
// setting them to the coarse value acquired during a coarse timestep
// and then incrementing in the fine values acquired during the
// subcycled fine timesteps.  This compilation procedure occurs in
// different parts for different quantities
//
// * u_mac is set in mac_project.
// * fr_adv, fr_visc are set in scalar_advect
// * Ssync is set in subcycled calls to post_timestep
// * mac_reg is set in mac_project
//
// After these quantities have been compiled during a coarse
// timestep and subcycled fine timesteps.  The post_timestep function
// uses them to sync the fine and coarse levels.  If the coarse level
// is not the base level, post_timestep modifies the next coarsest levels
// registers appropriately.
//
// Note :: There is a little ambiguity as to which level owns the
// boundary flux registers.  The Multifab registers are quantities
// sized by the coarse level BoxArray and belong to the coarse level.
// The fine levels own the boundary registers, since they are sized by
// the boundaries of the fine level BoxArray.
//

void
PorousMedia::advance_setup (Real time,
                            Real dt,
                            int  iteration,
                            int  ncycle)
{
  BoxLib::Abort("PorousMedia::advance_setup is now deprecated.  Should be advancing state with multilevel_advance");

  // FIXME: Code below commented out, but left for future expansion...
#if 0
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::advance_setup()");

  const int finest_level = parent->finestLevel();

  if (level < finest_level)
    {
      if (Ssync == 0)
	Ssync = new MultiFab(grids,NUM_SCALARS,1);
      Ssync->setVal(0);
    }

  //
  // Set reflux registers to zero.
  //
  if (do_reflux && level < finest_level)
    {
      getAdvFluxReg(level+1).setVal(0);
      getViscFluxReg(level+1).setVal(0);
    }

  //
  // Alloc space for edge velocities (normal comp only).
  //
  if (u_macG_prev == 0)
    {
      u_macG_prev = new MultiFab[BL_SPACEDIM];

      for (int dir = 0; dir < BL_SPACEDIM; dir++)
        {
	  BoxArray edge_grids(grids);
	  edge_grids.surroundingNodes(dir).grow(1);
	  u_macG_prev[dir].define(edge_grids,1,0,Fab_allocate);
	  u_macG_prev[dir].setVal(1.e40);
        }
    }
  if (u_macG_curr == 0)
    {
      u_macG_curr = new MultiFab[BL_SPACEDIM];

      for (int dir = 0; dir < BL_SPACEDIM; dir++)
        {
	  BoxArray edge_grids(grids);
	  edge_grids.surroundingNodes(dir).grow(1);
	  u_macG_curr[dir].define(edge_grids,1,0,Fab_allocate);
	  u_macG_curr[dir].setVal(1.e40);
        }
    }
  //
  // Set up state multifabs for the advance.
  //
  for (int k = 0; k < num_state_type; k++)
    {
        state[k].allocOldData();
        state[k].swapTimeLevels(dt);
    }

  if ( (model != PM_STEADY_SATURATED)
       && (model != PM_SATURATED) ) {
    //
    // Compute lambda at cell centers
    //

      // FIXME: this test is messed up!
    if (model != PM_SINGLE_PHASE ||
	model != PM_SINGLE_PHASE_SOLID)
      {
	calcLambda(time); 
#ifdef MG_USE_FBOXLIB
	if (model != PM_RICHARDS)
#endif
	  calcDLambda(time);
	MultiFab::Copy(*lambdap1_cc,*lambda_cc,0,0,ncomps,1);
      }
    //
    // Compute diffusion coefficients
    //

    // FIXME: Is this appropriate for richard?
    if (variable_scal_diff)
      {
	calcDiffusivity(time,0,ncomps);
	MultiFab::Copy(*diffnp1_cc,*diffn_cc,0,0,ndiff,1);
      }
    //
    // Compute capillary diffusive coefficients
    //
    if (model != PM_RICHARDS && have_capillary)
      {
	calcCapillary(time);
	MultiFab::Copy(*pcnp1_cc,*pcn_cc,0,0,1,(*pcnp1_cc).nGrow());
      }  
    //
    // If we are not doing a full advection scheme, u_mac_curr 
    // must be recomputed if grid has changed after a timestep.
    //
#ifdef MG_USE_FBOXLIB
    if (model != PM_RICHARDS)
#endif
      {
	if ( (model==PM_STEADY_SATURATED)
             || (model==PM_SATURATED) ) {
	  set_vel_from_bcs(time,u_mac_curr);
	}
	else if (do_simple == 0 && (full_cycle == 1 || no_corrector == 1))
	  {
	    if (n_pressure_interval == 0)
	      mac_project(u_mac_curr,rhs_RhoD,time);
	    else
	      {
		if (level == 0)   it_pressure += 1;
		
		if (it_pressure == n_pressure_interval &&
		    parent->levelSteps(level)%parent->nCycle(level)==parent->nCycle(level)-1)
		  {
		    mac_project(u_mac_curr,rhs_RhoD,time);
		    if (level == parent->finestLevel()) it_pressure = 0;
		  }	    
	      }
	  }
	else if (is_grid_changed_after_regrid)
	  {
	    mac_project(u_mac_curr,rhs_RhoD,time);
	  }
      }
    //
    // Alloc MultiFab to hold correction velocity.
    //
    if (u_corr == 0)
      {
	u_corr = new MultiFab[BL_SPACEDIM];
	for (int dir = 0; dir < BL_SPACEDIM; dir++)
	  {
	    BoxArray edge_grids(grids);
	    edge_grids.surroundingNodes(dir).grow(1);
	    u_corr[dir].define(edge_grids,1,0,Fab_allocate);
	    u_corr[dir].setVal(0.);
	  }
      }
  }
    
  //
  // Swap the time levels of u_mac
  //
  std::swap(u_mac_curr,u_mac_prev);

  if (do_tracer_chemistry>0) {
    aux_boundary_data_old.setVal(1.e30);
  }

  //
  // Copy cell-centered correction velocity computed in 
  // previous timestep to current timestep.
  //
  MultiFab& Uc_old = get_old_data(Vcr_Type);
  MultiFab& Uc_new = get_new_data(Vcr_Type);
  MultiFab::Copy(Uc_new,Uc_old,0,0,BL_SPACEDIM,Uc_new.nGrow());
#endif
}

//
// Clean up after the advance function.
//
void
PorousMedia::advance_cleanup (Real dt,
                              int  iteration,
                              int  ncycle,
                              bool undo_advance_setup)
{
    if (undo_advance_setup) {
        std::swap(u_mac_curr,u_mac_prev);
    }
}

static std::string mode_status = "init";
static std::string mode_steady = "STEADY";
static std::string mode_transient = "TRANSIENT";

bool 
PorousMedia::ml_step_driver(Real  time,
			    int   amr_iteration,
			    int   amr_ncycle,
                            Real  dt_try,
                            Real& dt_taken,
                            Real& dt_suggest,
			    bool  attempt_to_recover_failed_step)
{
    Real dt_min = 1.e-20 * dt_try;
    int max_dt_iters = 1; // By default, do not subcycle this process
    if (model == PM_RICHARDS)  {
      max_dt_iters = max_dt_iters_flow;
    }

    Real dt_this_attempt = dt_try;
    int dt_iter = 0;
    bool step_ok = false;
    bool continue_dt_iteration = !step_ok  &&  (dt_this_attempt >= dt_min) && (dt_iter < max_dt_iters);
    
    while (continue_dt_iteration) {

      if (ntracers>0) {
	if (execution_mode==INIT_TO_STEADY) {
	  advect_tracers = do_tracer_advection && (time >= switch_time);
	  react_tracers = do_tracer_chemistry && (time >= switch_time);
	} else {
	  advect_tracers = do_tracer_advection;
	  react_tracers = do_tracer_chemistry;
	}
      } else {
	advect_tracers = react_tracers = false;
      }

      if (time < switch_time) {
	if (mode_status != mode_steady) {
	  mode_status = mode_steady;
	  if (verbose > 0 && ParallelDescriptor::IOProcessor()) {
	    std::cout << "init-to-steady mode: " << mode_status << ", switch to transient at t=" << switch_time << "\n\n";
	  }
	}
      } else {
	if (mode_status != mode_transient) {
	  mode_status = mode_transient;
	  if (verbose > 0 && ParallelDescriptor::IOProcessor()) {
	    std::cout << "init-to-steady mode: " << mode_status << "\n\n";
	  }
	}
      }

      step_ok = multilevel_advance(time,dt_this_attempt,amr_iteration,amr_ncycle,dt_suggest);
      if (step_ok) {
	dt_taken = dt_this_attempt;
      } else {
	dt_this_attempt = dt_suggest;
      }
      dt_iter++;

      continue_dt_iteration = !step_ok  &&  (dt_this_attempt >= dt_min) && (dt_iter < max_dt_iters);

      if (!step_ok && !attempt_to_recover_failed_step) {
	continue_dt_iteration = false;
      }
    }
    return step_ok && dt_taken >= dt_min &&  dt_iter <= max_dt_iters;
}

Real
PorousMedia::advance (Real time,
                      Real dt,
                      int  iteration,
                      int  ncycle)
{
  BoxLib::Abort("PorousMedia::advance is now deprecated.  Should be advancing state with multilevel_advance");

  // FIXME: Code below commented out, but left for future expansion...
#if 0
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::advance()");
  BL_ASSERT(!use_PETSc_snes_for_evolution); // should be calling the multilevel advance in this case

  Real dt_return = -1.e20;

  if (ntracers>0) {
    advect_tracers = do_tracer_advection;
    react_tracers = do_tracer_chemistry;

    if (execution_mode==INIT_TO_STEADY) {
      advect_tracers &= (time >= switch_time);
      react_tracers &= (time >= switch_time);
    }
  } else {
    advect_tracers = react_tracers = false;
  }

  if (do_multilevel_full) 
  {
      if (level == 0) {
          if (verbose > 1 && ParallelDescriptor::IOProcessor())
          {
              std::cout << "Advancing all levels:"
                        << " starting time = " << time
                        << " with dt = "               << dt << '\n';
          }
          bool success = multilevel_advance(time,dt,iteration,ncycle,dt_return);
      }
  }
  else
  {
    if (verbose > 1 && ParallelDescriptor::IOProcessor())
      {
	std::cout << "Advancing grids at level " << level
		  << " : starting time = "       << time
		  << " with dt = "               << dt << '\n';
      }

    advance_setup(time,dt,iteration,ncycle);

    FillPatchedOldState_ok = true;
    //
    // Advance the old state for a Strang-split dt/2.  Include grow cells in
    // advance, and squirrel these away for diffusion and Godunov guys to
    // access for overwriting non-advanced fill-patched grow cell data.
    //
    MultiFab& S_new = get_new_data(State_Type);
    MultiFab& S_old = get_old_data(State_Type);
    
    MultiFab& P_new = get_new_data(Press_Type);
    MultiFab& P_old = get_old_data(Press_Type);
    
    MultiFab::Copy(S_new,S_old,0,0,NUM_SCALARS,S_old.nGrow()); 
    MultiFab::Copy(P_new,P_old,0,0,1,P_old.nGrow());
    
    const Real pcTime = state[State_Type].curTime();
    
    FillStateBndry (pcTime,State_Type,0,ncomps+ntracers);
    FillStateBndry (pcTime,Press_Type,0,1);

    if (do_tracer_chemistry && ntracers > 0)
    {
      MultiFab& A_new = get_new_data(Aux_Chem_Type);
      MultiFab& A_old = get_old_data(Aux_Chem_Type);
      MultiFab::Copy(A_new,A_old,0,0,A_new.nComp(),A_new.nGrow());
    }

    if (do_tracer_chemistry>0)
      {
	if (do_full_strang)
	  {
	    if (verbose>2 && ParallelDescriptor::IOProcessor())
	      std::cout << "... advancing 1/2 strang step for chemistry\n";
	    
	    // Old state is chem-advanced in-place.  Hook set to get grow data
	    //  from squirreled away data rather than via a vanilla fillpatch
	    bool chem_ok = advance_chemistry(time,dt/2,nGrowHYP);
	    BL_ASSERT(chem_ok);
	    // FillPatchedOldState_ok = false;  FIXME: Reacted state not yet squirreled away anywhere
	  }
      }

#ifdef MG_USE_FBOXLIB
    if (model == PM_RICHARDS)
      {
	advance_richard(time,dt);
      }
    else
#endif
      {
	// 
	// FIXME: Should we leave this 
	// do_simple: 2 ==> Only solve the tracer equations; assume steady state.
	//            1 ==> Only solve the pressure equation at time 0.
	//            0 ==> Solve the pressure equation at every timestep.
	//
	if (do_simple == 2 && !is_grid_changed_after_regrid)
	  advance_tracer(time,dt);
	else if (do_simple == 1  && !is_grid_changed_after_regrid)
	  advance_simple(time,dt);
	else
	  advance_incompressible(time,dt);
      }
    
    is_grid_changed_after_regrid = false;
    
    // second half of the strang splitting
    if (do_tracer_chemistry>0)
      {      
	if (do_full_strang)
	  {
	    if (verbose>2 && ParallelDescriptor::IOProcessor())
	      std::cout << "Second 1/2 Strang step of chemistry\n";
	    
	    // New state is chem-advanced in-place.  Fillpatch hook unset
	    bool chem_ok = advance_chemistry(pcTime,dt/2.0,0);
	    BL_ASSERT(chem_ok);
	    FillPatchedOldState_ok = true;
	  }
	else
	  {
	    if (n_chem_interval == 0)
	      {
		if (verbose>2 && ParallelDescriptor::IOProcessor())
		  std::cout << "... advancing full strang step for chemistry\n";
		bool chem_ok = advance_chemistry(pcTime,dt,0);
		BL_ASSERT(chem_ok);
	      }
	    else
	      {
		if (level == 0)
		  {
		    it_chem += 1;
		    dt_chem += dt;
		  }
		
		if (it_chem == n_chem_interval &&
		    parent->levelSteps(level)%parent->nCycle(level)==parent->nCycle(level)-1 &&
		    level == parent->finestLevel())
		  {
		    if (verbose>2 && ParallelDescriptor::IOProcessor())
		      std::cout << "... advancing full strang step for chemistry with dt ="
				<< dt_chem << "\n";
		    
		    bool chem_ok = advance_chemistry(pcTime,dt_chem,0);
		    BL_ASSERT(chem_ok);
		    it_chem = 0;
		    dt_chem = 0;		    
		  }
	      }
	  }
      }
    
    // 
    // Check sum of components
    //
    if (verbose>3) check_sum();
    
    //
    // Clean up after the predicted value at t^n+1.
    //
    advance_cleanup(dt,iteration,ncycle);
  }

  return dt_return; 
#endif
  return 0;
}

bool
PorousMedia::multilevel_advance (Real  time,
				 Real  dt,
				 int   iteration,
				 int   ncycle,
                                 Real& dt_new)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::multilevel_advance()");

  bool step_ok = true;

  if (model == PM_RICHARDS)  {

    // Richards:
    //   Flow: Multilevel solve for p,s,u_mac
    //   Transport: Explicit Godunov
    //   Chemistry: Amanzi's chemistry PK
    //
    // Evolution strategy: 
    //   At level-0 advance, do flow evolve for all levels over dt_crse
    //   At level-n advance, do coupled transport/chem evolve for dt_fine
    //   In level-n post_timestep, do transport sync
    //
    // Timestep control:
    //   Based on difficulty of flow solve, but not too many transport/chemistry substeps

    if (level != 0) {
      return step_ok;
    }
    Real dt_suggest_flow = dt;
    step_ok = advance_multilevel_richards_flow(time,dt,dt_suggest_flow);
    dt_new = dt_suggest_flow; 
    if (!step_ok) {
      return false;
    }

    Real dt_suggest_tc = dt_new;
    if (advect_tracers > 0  ||  react_tracers > 0) {

      bool use_cached_sat = true;

      MultiFab* sat_old;
      MultiFab slocal;
      if (use_cached_sat) {
        for (int lev=level; lev<=parent->finestLevel(); ++lev) {
          PorousMedia& pml = dynamic_cast<PorousMedia&>(getLevel(lev));
          pml.state[State_Type].setOldTimeLevel(time);
          pml.state[State_Type].allocOldData();
          pml.state[State_Type].setNewTimeLevel(time+dt);
          pml.cache_component_saturations(nGrowHYP);
        }
        sat_old = sat_old_cached;
      }
      else {
        slocal.define(grids,ncomps,nGrowHYP,Fab_allocate);
        get_fillpatched_rhosat(time,slocal,nGrowHYP);
        for (int n=0; n<ncomps; ++n) {
          slocal.mult(1/density[n],n,1,nGrowHYP);
        }
        sat_old = &slocal;
      }

      bool do_subcycle_tc = true;
      bool do_recursive = true;
      advance_richards_transport_dt(time,sat_old);
      bool step_ok_tc = advance_richards_transport_chemistry(time,dt,iteration,dt_suggest_tc,
                                                             do_subcycle_tc,do_recursive,use_cached_sat);
      if (step_ok_tc) {
        reinstate_component_saturations();
      } else {
        dt_new = dt_suggest_tc;
        return false;
      }
      
    }
    dt_new = std::min(dt_new, dt_suggest_tc);
  }
  else if ( (model == PM_STEADY_SATURATED)
	    || (model == PM_SATURATED) ) {

    // Saturated flow
    //   Flow: Velocity set from IC directly
    //   Transport: Explicit Godunov
    //   Chemistry: Amanzi's chemistry PK
    //
    // Evolution strategy: 
    //   At level-n advance, do coupled transport/chem evolve for dt_fine
    //   In level-n post_timestep, do transport sync
    //
    // Timestep control:
    //   Time-explicit CFL, chemistry difficulty
    // Initialize velocity field, set "new time" for state the same across levels, copy over saturation/pressure

    if (level != 0) {
      return step_ok;
    }

    dt_new = dt;
    step_ok = true;
    Real dt_suggest_flow = -1;

    if (model == PM_STEADY_SATURATED) {
      advance_flow_nochange(time,dt);
    }
    else {

      dt_suggest_flow = dt;
      step_ok = advance_multilevel_richards_flow(time,dt,dt_suggest_flow);
      if (!step_ok) {
	dt_new = dt_suggest_flow; 
	return false;
      }
    }

    Real dt_suggest_tc = -1;
    if (advect_tracers > 0  ||  react_tracers > 0) {
      bool use_cached_sat = false;
      bool do_subcycle_tc = false;
      bool do_recursive = true;
      advance_saturated_transport_dt(time);
      bool step_ok_tc = advance_richards_transport_chemistry(time,dt,iteration,dt_suggest_tc,
                                                             do_subcycle_tc,do_recursive,use_cached_sat);
      if (!step_ok_tc) {
        dt_new = dt_suggest_tc;
        return false;
      }      
    }

    Real dt_suggest = -1;
    if (dt_suggest_flow > 0) {
      dt_suggest = dt_suggest_flow;
    }
    if (dt_suggest_tc > 0) {
      if (dt_suggest > 0) {
	dt_suggest = std::min(dt_suggest,dt_suggest_tc);
      } else {
	dt_suggest = dt_suggest_tc;
      }
    }
    dt_new = dt_suggest;  
  }

  if (level == 0) {
    Observation::setPMAmrPtr(PMParent());
    Real prev_time = state[State_Type].prevTime();
    Real curr_time = state[State_Type].curTime();
    PArray<Observation>& observations = PMParent()->TheObservations();
    for (int i=0; i<observations.size(); ++i) {
      observations[i].process(prev_time, curr_time, parent->levelSteps(0));
    }
  }

  return step_ok;
}

MultiFab*
PorousMedia::AllocateUMacG() const
{
  MultiFab* u_macG = new MultiFab[BL_SPACEDIM];
  for (int dir = 0; dir < BL_SPACEDIM; dir++) {
    BoxArray edge_grids = BoxArray(grids);
    edge_grids.surroundingNodes(dir).grow(1);
    u_macG[dir].define(edge_grids,1,0,Fab_allocate);
#ifndef NDEBUG
    u_macG[dir].setVal(1.e40);
#endif
  }
  return u_macG;
}

void
PorousMedia::get_fillpatched_rhosat(Real t_eval, MultiFab& RhoSat, int nGrow)
{
  BL_ASSERT(RhoSat.boxArray()== grids);
  BL_ASSERT(kappa->boxArray()== grids);
  BL_ASSERT(rock_phi->boxArray()== grids);
  BL_ASSERT(RhoSat.nGrow()>= nGrow);
  BL_ASSERT(kappa->nGrow()>= nGrow);
  BL_ASSERT(rock_phi->nGrow()>= nGrow);
  if (model == PM_RICHARDS) {
    MultiFab P(grids,ncomps,nGrow);
    for (FillPatchIterator fpi(*this,P,nGrow,t_eval,Press_Type,0,ncomps); fpi.isValid(); ++fpi) {
      P[fpi].copy(fpi());
    }
    calcInvPressure(RhoSat,P,t_eval,0,0,nGrow);
  }
  else if ( (model == PM_STEADY_SATURATED)
	    || (model == PM_SATURATED) ) {
    for (int n=0; n<ncomps; ++n) {
      RhoSat.setVal(density[n],n,1,nGrow);
    }
  }
  else {
    for (FillPatchIterator S_fpi(*this,RhoSat,nGrow,t_eval,State_Type,0,ncomps); S_fpi.isValid(); ++S_fpi) {
      RhoSat[S_fpi].copy(S_fpi());
    }
  }
}

void
PorousMedia::cache_component_saturations(int nGrow)
{
  component_saturations_cached = true;
  if (sat_old_cached == 0 || sat_old_cached->nGrow()<nGrow || sat_old_cached->boxArray()!=grids) {
    delete sat_old_cached;
    sat_old_cached = new MultiFab(grids,ncomps,nGrow);
  }
  t_sat_old_cached = state[State_Type].prevTime();
  get_fillpatched_rhosat(t_sat_old_cached,(*sat_old_cached),nGrow);
  for (int n=0; n<ncomps; ++n) {
    sat_old_cached->mult(1/density[n],n,1,nGrow);
  }

  if (sat_new_cached == 0 || sat_new_cached->nGrow()<nGrow || sat_new_cached->boxArray()!=grids) {
    delete sat_new_cached;
    sat_new_cached = new MultiFab(grids,ncomps,nGrow);
  }
  t_sat_new_cached = state[State_Type].curTime();
  get_fillpatched_rhosat(t_sat_new_cached,(*sat_new_cached),nGrow);
  for (int n=0; n<ncomps; ++n) {
    sat_new_cached->mult(1/density[n],n,1,nGrow);
  }
}

void
PorousMedia::reinstate_component_saturations()
{
  component_saturations_cached = false;
  BL_ASSERT(sat_old_cached && sat_old_cached->boxArray() == grids);
  MultiFab& S_old = get_old_data(State_Type);
  for (MFIter mfi(*sat_old_cached); mfi.isValid(); ++mfi) {
    S_old[mfi].copy((*sat_old_cached)[mfi],0,0,ncomps);
    for (int n=0; n<ncomps; ++n) {
      S_old[mfi].mult(density[n],n,1);
    }
  }
  BL_ASSERT(sat_new_cached && sat_new_cached->boxArray() == grids);
  MultiFab& S_new = get_new_data(State_Type);
  for (MFIter mfi(*sat_new_cached); mfi.isValid(); ++mfi) {
    S_new[mfi].copy((*sat_new_cached)[mfi],0,0,ncomps);
    for (int n=0; n<ncomps; ++n) {
      S_new[mfi].mult(density[n],n,1);
    }
  }
}

void
PorousMedia::advance_richards_transport_dt(Real      t,
                                           MultiFab* saturation)
{
  int finest_level = parent->finestLevel();
  Real dt_min = 1e20;
  for (int lev=level; lev<=finest_level; ++lev) {
    
    PorousMedia& pml = getLevel(lev);   
    if (lev == 0) {
      pml.create_umac_grown(pml.u_mac_curr,pml.u_macG_trac);
    } else {
      PArray<MultiFab> u_macG_crse(BL_SPACEDIM,PArrayManage);
      const PorousMedia* pm = dynamic_cast<const PorousMedia*>(&parent->getLevel(lev-1));
      Real t_crse_curr = pm->state[State_Type].curTime();
      pml.GetCrseUmac(u_macG_crse,t_crse_curr);
      pml.create_umac_grown(pml.u_mac_curr,u_macG_crse,pml.u_macG_trac); 
    }
    if (pml.u_macG_prev == 0) {
      pml.u_macG_prev = pml.AllocateUMacG();
    }
    if (pml.u_macG_curr == 0) {
      pml.u_macG_curr = pml.AllocateUMacG();
    }

    for (int d=0; d<BL_SPACEDIM; ++d) {
      MultiFab::Copy(pml.u_macG_prev[d],pml.u_macG_curr[d],0,0,1,0);
      MultiFab::Copy(pml.u_macG_curr[d],pml.u_macG_trac[d],0,0,1,0); // FIXME: Should not be necessary
    }
    pml.predictDT(pml.u_macG_trac,t);
    if (diffuse_tracers && be_cn_theta_trac==0) {
      Real dt_diff = pml.predictDT_diffusion_explicit(t, saturation);
      dt_eig = std::min(dt_eig, dt_diff);
    }
    dt_min = std::min(dt_min/parent->nCycle(lev),pml.dt_eig);
  }
  for (int lev=finest_level; lev>=0; --lev) {
    PorousMedia& pml = getLevel(lev);   
    pml.dt_eig = dt_min;
    dt_min = dt_min*parent->nCycle(lev);
  }
}

void
PorousMedia::set_saturated_velocity()
{
  BL_ASSERT(model == PM_STEADY_SATURATED);
  BL_ASSERT(do_constant_vel);

  if (u_macG_curr == 0) {
    u_macG_curr = AllocateUMacG();
  }
  if (u_macG_prev == 0) {
    u_macG_prev = AllocateUMacG();
  }

  if (u_macG_trac == 0) {
    u_macG_trac = AllocateUMacG();
  }

  for (int i=0; i<ic_array.size(); ++i) {
    const RegionData& ic = ic_array[i];
    const Array<const Region*>& ic_regions = ic.Regions();
    const std::string& type = ic.Type();
    if (type == "constant_velocity") {
      Array<Real> vals = ic();
      BL_ASSERT(vals.size() >= BL_SPACEDIM);
      for (int d=0; d<BL_SPACEDIM; ++d) {
	u_mac_curr[d].setVal(vals[d],u_mac_curr[d].nGrow());
	u_macG_curr[d].setVal(vals[d]);
	u_macG_prev[d].setVal(vals[d]);
	u_macG_trac[d].setVal(vals[d]);
      }
    }
  }
}

void 
PorousMedia::advance_flow_nochange(Real time, Real dt)
{
  int finest_level = parent->finestLevel();
  for (int lev=level; lev<=finest_level; ++lev) {
    PorousMedia& pml = getLevel(lev);
    StateData& sd = pml.get_state_data(State_Type);
    sd.setNewTimeLevel(time+dt);
    if (do_constant_vel) {
      pml.set_saturated_velocity();
    }
    else {
      for (int d=0; d<BL_SPACEDIM; ++d) {
	for (MFIter mfi(u_mac_curr[d]); mfi.isValid(); ++mfi) {
	  u_mac_curr[d][mfi].copy(u_macG_curr[d][mfi.index()],0,0,1);
	}
	MultiFab::Copy(u_macG_prev[d],u_macG_curr[d],0,0,1,0);
	MultiFab::Copy(u_macG_trac[d],u_macG_curr[d],0,0,1,0);
      }
    }

    sd.allocOldData();
    sd.setOldTimeLevel(time);
    MultiFab::Copy(sd.oldData(),sd.newData(),0,0,ncomps,0);

    StateData& pd = pml.get_state_data(Press_Type);
    pd.setNewTimeLevel(time+dt);
    pd.allocOldData();
    pd.setOldTimeLevel(time);
    MultiFab::Copy(pd.oldData(),pd.newData(),0,0,1,0);

    if (chemistry_helper) {
      const std::map<std::string,int>& aux_chem_variables_map = chemistry_helper->AuxChemVariablesMap();
      if (aux_chem_variables_map.size() > 0) {
        StateData& ad = pml.get_state_data(Aux_Chem_Type);
        ad.setNewTimeLevel(time+dt);
        ad.allocOldData();
        ad.setOldTimeLevel(time);
        MultiFab::Copy(ad.oldData(),ad.newData(),0,0,ad.oldData().nComp(),0);
      }
    }
  }
}

void
PorousMedia::advance_saturated_transport_dt(Real time)
{
  // Based on velocity fields in u_macG_trac at all levels, set dt_eig on all levels to satisfy
  // cfl restriction, accounting for recursive subcycling.
  //
  int finest_level = parent->finestLevel();
  Real dt_min = 1e20;
  for (int lev=level; lev<=finest_level; ++lev) {
    PorousMedia& pml = getLevel(lev); 
    pml.predictDT(pml.u_macG_trac,time);
    if (diffuse_tracers && be_cn_theta_trac==0) {
      int nGrow = 1;
      MultiFab saturation(pml.grids,ncomps,nGrow); saturation.setVal(1,0,ncomps,nGrow);
      Real dt_diff = pml.predictDT_diffusion_explicit(time,&saturation);
      pml.dt_eig = std::min(pml.dt_eig, dt_diff);
    }
    dt_min = std::min(dt_min/parent->nCycle(lev),pml.dt_eig);
  }
  for (int lev=finest_level; lev>=0; --lev) {
    PorousMedia& pml = getLevel(lev);
    pml.dt_eig = dt_min;
    dt_min = dt_min*parent->nCycle(lev);
  }
}

bool
PorousMedia::advance_richards_transport_chemistry (Real  t,
						   Real  dt,
						   int   iteration,
						   Real& dt_new, 
						   bool  do_subcycle,
						   bool  do_recursive,
                                                   bool  use_cached_sat)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::richards_transport_chemistry()");
  const Real strt_time = ParallelDescriptor::second();
  Real run_time_chem = 0;

  if (advect_tracers > 0 || react_tracers > 0) {

    Real dt_cfl = dt;
    if (advect_tracers) {
      dt_cfl = (cfl>0 ? cfl : 1)*dt_eig;
      Real t_eps = 1.e-6*dt_cfl;
      if (!do_subcycle && dt-dt_cfl > t_eps) {
	dt_new = dt_cfl;
        if (ParallelDescriptor::IOProcessor()) {
          std::cout << "  TRANSPORT: dt > CFL but !do_subcycle.  Suggest next dt: " << dt_new << std::endl;
        }
	return false;
      }
    }
    Real t_eps = 1.e-6*dt_cfl;

    Real t_subtr = t;
    Real tmax_subtr = t+dt;
    Real dt_subtr = std::min(dt_cfl,dt);
    bool continue_subtr = true;
    std::map<int,MultiFab*> saved_states;
    int n_subtr = 0;

    bool summary_transport_out = true;
    bool full_transport_out = false;

    while (continue_subtr) {

      // Adjust dt_sub to spread out dt changes and avoid small final step
      if (n_subtr==0 && dt-dt_subtr>t_eps) {
        int n_est = std::max(1, (int)((dt + t_eps) / dt_subtr));
        Real remain_dt = dt - (Real)(n_est)*dt_subtr;
        if (remain_dt > t_eps) {
          dt_subtr = (tmax_subtr - t_subtr)/(n_est+1);
        }
      }

      if (react_tracers  &&  do_full_strang) {
	const Real strt_time_chem = ParallelDescriptor::second();
	if (verbose > 0 && full_transport_out && ParallelDescriptor::IOProcessor() && n_subtr>1) {
	  for (int lev=0; lev<=level; ++lev) {
	    std::cout << "  ";
	  }
          std::string units_str = do_output_chemistry_time_in_years ? "Y" : "s";
          std::pair<Real,std::string> told_subtr_output = PMAmr::convert_time_units(t_subtr,units_str);
          std::pair<Real,std::string> tnew_subtr_output = PMAmr::convert_time_units(t_subtr+dt_subtr,units_str);
          std::pair<Real,std::string> dt_subtr_output = PMAmr::convert_time_units(dt_subtr,units_str);
          std::ios_base::fmtflags oldflags = std::cout.flags(); std::cout << std::scientific << std::setprecision(10);
	  std::cout << "CHEMISTRY:  FIRST HALF: Level: " << level
		    << " TIME = " << told_subtr_output.first << told_subtr_output.second
		    << " : " << tnew_subtr_output.first << tnew_subtr_output.second << std::endl;
          std::cout.flags(oldflags);
	}
	int nGrow_chem = 0;
	//int nGrow_chem = nGrowHYP;
	bool chem_ok = advance_chemistry(t_subtr,dt_subtr/2,nGrow_chem);
	BL_ASSERT(chem_ok);
	// FIXME: Have no code for chem-advancing grow cells
	//FillPatchedOldState_ok = false;

	run_time_chem = run_time_chem + ParallelDescriptor::second() - strt_time_chem;
      }

      if (verbose > 0 && full_transport_out &&  ParallelDescriptor::IOProcessor()) {
	for (int lev=0; lev<=level; ++lev) {
	  std::cout << "  ";
	}

        std::string units_str = do_output_transport_time_in_years ? "Y" : "s";
        std::pair<Real,std::string> told_subtr_output = PMAmr::convert_time_units(t_subtr,units_str);
        std::pair<Real,std::string> tnew_subtr_output = PMAmr::convert_time_units(t_subtr+dt_subtr,units_str);
        std::pair<Real,std::string> dt_subtr_output = PMAmr::convert_time_units(dt_subtr,units_str);
        std::ios_base::fmtflags oldflags = std::cout.flags(); std::cout << std::scientific << std::setprecision(10);
	std::cout << "TRANSPORT: Level: " << level;
        if (n_subtr!=0 || (t_subtr+dt_subtr < tmax_subtr - t_eps)) {
          std::cout << " Subcycle: " << n_subtr << " ";
        }
        std::cout << " TIME = " << told_subtr_output.first << told_subtr_output.second
		  << " : " << tnew_subtr_output.first << tnew_subtr_output.second
		  << " (DT: " << dt_subtr_output.first << dt_subtr_output.second << ")";
        std::cout << std::endl;
        std::cout.flags(oldflags);
      }
      n_subtr++;

      // Set time interval for this advection step
      state[State_Type].setNewTimeLevel(t_subtr+dt_subtr);
      state[State_Type].allocOldData();
      state[State_Type].setOldTimeLevel(t_subtr);

      // Set up "old" tracers from previous "new" tracers
      int first_tracer = ncomps;
      MultiFab::Copy(state[State_Type].oldData(),state[State_Type].newData(),first_tracer,first_tracer,ntracers,0);

      int nGrowF = 1;
      MultiFab Fext(grids,ncomps+ntracers,nGrowF);
      bool do_rho_scale = 0;
      getForce(Fext,nGrowF,0,ncomps+ntracers,t_subtr,do_rho_scale);

      if (diffuse_tracers) {
        // FIXME: getTracerViscTerms should add to Fext here, but need to get handle cached saturations on coarser levels
      }

      if (advect_tracers) {
	// Initialize flux registers
	if (do_reflux && level < parent->finestLevel()) {
	  getAdvFluxReg(level+1).setVal(0);
	}
	bool do_reflux_this_call = true;
	tracer_advection(u_macG_trac,do_reflux_this_call,use_cached_sat,&Fext);
      }

      // Initialize diffusive flux registers
      if (do_reflux && level < parent->finestLevel()) {
        getViscFluxReg(level+1).setVal(0);
      }

      if (diffuse_tracers) {
	// Diffuse (Sources incorporated in diffusion advance)
	MultiFab::Subtract(Fext,*aofs,first_tracer,ncomps,ntracers,0);// S_diffusion = Fext - Div(AdvectionFlux), ng=0
	bool reflux_on_this_call = true;
        tracer_diffusion (reflux_on_this_call,use_cached_sat,Fext);
      }
      else {
	// Explicit source advance
	Fext.mult(dt_subtr,0,ntracers);
	MultiFab::Add(get_new_data(State_Type),Fext,0,ncomps,ntracers,0);
      }

      bool step_ok_chem = true;
      if (react_tracers > 0) {
	const Real strt_time_chem = ParallelDescriptor::second();
	bool do_write = verbose > 0 &&  ParallelDescriptor::IOProcessor();
	if (do_write) {
	  for (int lev=0; lev<=level; ++lev) {
	    std::cout << "  ";
	  }
	  std::cout << "CHEMISTRY: Level: " << level << " TIME = ";
	}
	if (do_full_strang) {
	  if (do_write) {
	    std::cout << t_subtr+dt_subtr/2
		      << " : " << t_subtr+dt_subtr << std::endl;
	  }
	  step_ok_chem = advance_chemistry(t_subtr,dt_subtr/2,0);
	  BL_ASSERT(step_ok_chem);
	} else {
	  if (n_chem_interval == 0) {
	    if (do_write) {
	      std::cout << t_subtr
			<< " : " << t_subtr+dt_subtr << std::endl;
	    }
	    step_ok_chem = advance_chemistry(t_subtr,dt_subtr,0);
	    BL_ASSERT(step_ok_chem);
	  } else {
	    it_chem += 1;
	    dt_chem += dt_subtr;
            
	    if (it_chem == n_chem_interval) {
	      if (do_write) {
		std::cout << t_subtr + dt_subtr - dt_chem
			  << " : " << t_subtr+dt_subtr << std::endl;
	      }
	      step_ok_chem = advance_chemistry(t_subtr,dt_chem,0);      
	      BL_ASSERT(step_ok_chem);
	      FillPatchedOldState_ok = true; // FIXME: Is this out of place?
	      it_chem = 0;
	      dt_chem = 0;
	    }
	  }	    
	}
	run_time_chem = run_time_chem + ParallelDescriptor::second() - strt_time_chem;
      }

      // Do AMR recursive timestep
      bool fine_step_ok = true;
      if (do_recursive  && level < parent->finestLevel()) {
	// Advance grids at higher level.
        const int lev_fine = level+1;
        PorousMedia& pm_fine = dynamic_cast<PorousMedia&>(getLevel(lev_fine));
	const int ncycle = parent->nCycle(lev_fine);
	Real dt_fine = dt_subtr / ncycle;
	bool do_subcycle_fine = false;

	for (int i = 1; i <= ncycle && fine_step_ok; i++) {
	  Real dt_new_fine = dt_fine;
	  fine_step_ok = 
	    pm_fine.advance_richards_transport_chemistry(t_subtr+(i-1)*dt_fine, dt_fine, i,
							 dt_new_fine, do_subcycle_fine, do_recursive, use_cached_sat);
	  fine_step_ok &= dt_fine<=dt_new_fine;	
	}

        if (use_cached_sat) {
          reinstate_component_saturations();
        }
      }
      
      if (fine_step_ok) {

        post_timestep(iteration);

        t_subtr += dt_subtr;
        
        Real subcycle_time_remaining = tmax_subtr - t_subtr;
        if (subcycle_time_remaining < t_eps) {
          t_subtr = tmax_subtr;
          subcycle_time_remaining = 0;
        }
        
        if (subcycle_time_remaining > 0) {
          
          //predictDT(u_macG_trac,t_subtr); // based on the new "old" state
          if (dt_cfl < dt_subtr) {
            int num_subcycles = std::max(1,(int)(subcycle_time_remaining / dt_cfl) + 1);
            dt_subtr = subcycle_time_remaining / num_subcycles;
          }
          dt_subtr = std::min(dt_subtr,subcycle_time_remaining);
          BL_ASSERT(dt_subtr > 0);
        } else {
          continue_subtr = false;
        }
        
      } else {

        // recover from failed step by rolling back tracer update at this level
        if (ParallelDescriptor::IOProcessor()) {
          std::cout << "Richards transport/chem step failed, unwind failed attempt at level = " << level << std::endl;
        }
        MultiFab::Copy(state[State_Type].newData(),state[State_Type].oldData(),first_tracer,first_tracer,ntracers,0);
        return false;
	
      } // end of recover
    }

    if (summary_transport_out && ParallelDescriptor::IOProcessor() ) {

      std::string units_str = do_output_transport_time_in_years ? "Y" : "s";
      std::pair<Real,std::string> t_output = PMAmr::convert_time_units(t,units_str);
      std::pair<Real,std::string> tnew_output = PMAmr::convert_time_units(t+dt,units_str);
      std::pair<Real,std::string> dt_output = PMAmr::convert_time_units(dt,units_str);
      std::pair<Real,std::string> dt_subtr_output = PMAmr::convert_time_units(dt_subtr,units_str);
      std::ios_base::fmtflags oldflags = std::cout.flags(); std::cout << std::scientific << std::setprecision(10);
      for (int lev=0; lev<=level; ++lev) {
        std::cout << "  ";
      }
      std::cout << "TRANSPORT: Level: " << level
                << " TIME: " << t_output.first << t_output.second
                << " : " << tnew_output.first << tnew_output.second
                << " (DT: " << dt_output.first << dt_output.second << ")" << std::endl;
      if (n_subtr > 1) {
        for (int lev=0; lev<=level; ++lev) {
          std::cout << "  ";
        }
        std::cout << " (Nsub: " << n_subtr
                  << ", DTsub: " << dt_subtr_output.first << dt_subtr_output.second << ")" << std::endl;
      }
      std::cout.flags(oldflags);
    }

    // Bring state up to current time, and reinstate original dt info
    state[State_Type].setTimeLevel(t+dt,dt,dt);
    dt_new = (do_subcycle ? max_n_subcycle_transport : 1) * dt_cfl;
  }

  if (level == 0) {

    Real dt_cfl = dt;

    if (advect_tracers) {
      dt_cfl = (cfl>0 ? cfl : 1)*dt_eig;
    }

    Real dt_new_save = dt_new;
    if (dt_grow_max > 0) {
      Real max_cfl_dt = dt_new;
      Real max_grow_dt = dt * dt_grow_max;
      if (max_cfl_dt > max_grow_dt) {
        dt_new = max_grow_dt;
        if (ParallelDescriptor::IOProcessor()) {
          std::cout << "  TRANSPORT: dt suggested by CFL reduced by dt_grow_max from "
                    << max_cfl_dt << " to " << dt_new << " (=" << dt_new/(365.25*3600*24) << " y)" << std::endl;
        }
      }
    }
  }


  if (show_selected_runtimes > 0 && ParallelDescriptor::IOProcessor()) {
    const int IOProc   = ParallelDescriptor::IOProcessorNumber();
    Real      run_time = ParallelDescriptor::second() - strt_time - run_time_chem;
    ParallelDescriptor::ReduceRealMax(run_time,IOProc);
    
    std::cout << "PorousMedia advance transport time: " << run_time << '\n';
  }
  
  return true;
}

void
PorousMedia::advance_incompressible (Real time,
				     Real dt)
{
  BoxLib::Abort("PorousMedia::advance_incompressible is now deprecated.  Should be advancing state with multilevel_advance");

  // FIXME: Code below commented out, but left for future expansion...
#if 0
  // 
  // Time stepping for incompressible flow.  
  // For single-phase constant-density problem, use advance_simple.
  //
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::advance_incompressible()");
  BL_ASSERT(model != PM_RICHARDS);
  BL_ASSERT(model != PM_STEADY_SATURATED);
  BL_ASSERT(model != PM_SATURATED);
  react_tracers = do_tracer_chemistry;

  const Real cur_time = state[State_Type].curTime();
  MultiFab& S_new     = get_new_data(State_Type);
  int lscalar         = ncomps - 1; 

  if (n_pressure_interval !=0)
    check_minmax(u_mac_prev);

  MultiFab* rhod_tmp = 0;
  if (do_any_diffuse)
    {
      rhod_tmp = new MultiFab[BL_SPACEDIM];
      for (int dir =0; dir < BL_SPACEDIM; dir++) 
	{
	  BoxArray edge_grids(grids);
	  edge_grids.surroundingNodes(dir);
	  rhod_tmp[dir].define(edge_grids,1,0,Fab_allocate);
	  rhod_tmp[dir].setVal(0.0);
	  rhod_tmp[dir].plus(rhs_RhoD[dir],0,1,0);
	}
    }
     
  if (level == 0) 
    create_umac_grown(u_mac_prev,u_macG_prev);
  else 
    {
      PArray<MultiFab> u_macG_crse(BL_SPACEDIM,PArrayManage);
      GetCrseUmac(u_macG_crse,time);
      create_umac_grown(u_mac_prev,u_macG_crse,u_macG_prev); 
    }
  
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    MultiFab::Copy(u_macG_trac[dir],u_macG_prev[dir],0,0,1,0);
  
  //
  // Predictor: Advance the component conservation equations
  //
  int corrector = 0;
    
  if (no_corrector == 1)
    {
      corrector = 1;

      // copy u_mac_prev to u_mac_curr since we are not solving for u_mac_curr
      for (int dir = 0; dir < BL_SPACEDIM; dir++)
	{
	  MultiFab::Copy(u_mac_curr[dir],u_mac_prev[dir],0,0,1,0);
	  MultiFab::Copy(u_macG_curr[dir],u_macG_prev[dir],0,0,1,0);
	}

      // contribute to velocity register
      mac_projector->contribute_to_mac_reg(level,u_mac_prev);
      if (do_any_diffuse)
	{
	  for (int dir = 0; dir < BL_SPACEDIM; dir++) 
	    rhod_tmp[dir].mult(-1.0);
	  mac_projector->contribute_to_mac_reg_rhoD(level,rhod_tmp);
	}

      // Compute the advective term
      scalar_advection(u_macG_trac,dt,0,lscalar,true);

      // Add the advective and other terms to get scalars at t^{n+1}.
      scalar_update(dt,0,ncomps,corrector,u_macG_trac);

      if (advect_tracers > 0)
	{
          bool use_cached_sat = false;
          bool reflux_on_this_call = true;
	  tracer_advection(u_macG_trac,reflux_on_this_call,use_cached_sat);
	}

      predictDT(u_macG_prev, time);
    }

  else
    {
      // Compute the advective term
      scalar_advection(u_macG_trac,dt,0,lscalar,false);

      // Add the advective and other terms to get scalars at t^{n+1}.
      scalar_update(dt,0,ncomps,corrector);

      if (react_tracers>0)
	{
	  if (do_full_strang) {
	    bool chem_ok = advance_chemistry(time,dt/2.0,nGrowHYP);
	    BL_ASSERT(chem_ok);
	  }
	}
      
      //
      // Corrector Step
      //    
      if (model > 1)
	calcLambda(cur_time);

      // Do a MAC projection to define edge velocities at time t^(n+1)
      mac_project(u_mac_curr,rhs_RhoD,cur_time);
    
      if (do_any_diffuse)
	{
	  for (int dir = 0; dir < BL_SPACEDIM; dir++) 
	    {
	      rhod_tmp[dir].plus(rhs_RhoD[dir],0,1,0);
	      rhod_tmp[dir].mult(-0.5);
	    }
	  mac_projector->contribute_to_mac_reg_rhoD(level,rhod_tmp);
	}

      if (level == 0) 
	create_umac_grown(u_mac_curr,u_macG_curr);
      else 
	{
	  PArray<MultiFab> u_macG_crse(BL_SPACEDIM,PArrayManage);
	  GetCrseUmac(u_macG_crse,time+dt);
	  create_umac_grown(u_mac_curr,u_macG_crse,u_macG_curr);
	}

      // Create velocity at time t^{n+1/2}.
      MultiFab* u_mac_nph  = new MultiFab[BL_SPACEDIM];
      MultiFab* u_macG_nph = new MultiFab[BL_SPACEDIM];
      for (int dir = 0; dir < BL_SPACEDIM; dir++)
	{
	  BoxArray edge_grids(grids);
	  edge_grids.surroundingNodes(dir);
	  u_mac_nph[dir].define(edge_grids,1,0,Fab_allocate);
	  MultiFab::Copy(u_mac_nph[dir],u_mac_prev[dir],0,0,1,0);
	  u_mac_nph[dir].plus(u_mac_curr[dir],0,1,0);
	  u_mac_nph[dir].mult(0.5);
	  if (do_any_diffuse)
	    u_mac_nph[dir].plus(rhod_tmp[dir],0,1,0);

	  edge_grids.grow(1);
	  u_macG_nph[dir].define(edge_grids,1,0,Fab_allocate);
	  MultiFab::Copy(u_macG_nph[dir],u_macG_prev[dir],0,0,1,0);
	  u_macG_nph[dir].plus(u_macG_curr[dir],0,1,0);
	  u_macG_nph[dir].mult(0.5);

	  MultiFab::Copy(u_macG_trac[dir],u_macG_nph[dir],0,0,1,0);
	}

      mac_projector->contribute_to_mac_reg(level,u_mac_nph);

      // Re-advect component equations 
      corrector = 1;
      if (variable_scal_diff)
	calcDiffusivity (cur_time,0,ncomps);

      scalar_advection(u_macG_trac,dt,0,lscalar,true);
    
      scalar_update(dt,0,ncomps,corrector,u_macG_trac);

      if (advect_tracers > 0)
	{
          bool reflux_on_this_call = true;
          bool use_cached_sat = false;
	  tracer_advection(u_macG_trac,reflux_on_this_call,use_cached_sat);
	}

      // predict the next time step.  
      predictDT(u_macG_curr, time+dt);

      delete [] u_mac_nph;
      delete [] u_macG_nph;
    }
  
  if (do_any_diffuse) delete [] rhod_tmp;

  //
  // Check the divergence conditions of v_1 (water)
  //
  MultiFab divutmp(grids,1,0);
  divutmp.setVal(0);
  mac_projector->check_div_cond(level,divutmp,u_macG_trac,rhs_RhoD);
  MultiFab::Copy(S_new,divutmp,0,ncomps+ntracers,1,0);
  if (have_capillary) MultiFab::Copy(S_new,*pcnp1_cc,0,ncomps+ntracers+1,1,1);
#endif  
}

void
PorousMedia::advance_simple (Real time,
			     Real dt)
{
  BoxLib::Abort("PorousMedia::advance_simle is now deprecated.  Should be advancing state with multilevel_advance");

  // FIXME: Code below commented out, but left for future expansion...
#if 0
  // 
  // Time stepping for incompressible single-phase single-density flow.
  //
  if (level == 0) 
    create_umac_grown(u_mac_prev,u_macG_prev);
  else 
    {
      PArray<MultiFab> u_macG_crse(BL_SPACEDIM,PArrayManage);
      GetCrseUmac(u_macG_crse,time);
      create_umac_grown(u_mac_prev,u_macG_crse,u_macG_prev); 
    }

  //
  // Single advance the component conservation equations
  //
  int corrector = 0;

  // Compute the coefficients for diffusion operators.
  if (variable_scal_diff)
    {
      calcDiffusivity(time,0,ncomps);
      MultiFab::Copy(*diffnp1_cc,*diffn_cc,0,0,ndiff,diffn_cc->nGrow());
    }

  // Compute the advective term
  scalar_advection(u_macG_prev,dt,0,ncomps,true);
    
  // Add the advective and other terms to get scalars at t^{n+1}.
  scalar_update(dt,0,ncomps,corrector);

  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    u_mac_curr[dir].copy(u_mac_prev[dir]);
#endif
}

#ifdef MG_USE_FBOXLIB
void
PorousMedia::advance_richard (Real time,
			      Real dt)
{
  BoxLib::Abort("PorousMedia::advance_richard is now deprecated.  Should be advancing state with multilevel_advance");

  // FIXME: Code below commented out, but left for future expansion...
#if 0
  std::string tag = "Richard Time Step: ";
  // 
  // Time stepping for richard's equation
  //
  int curr_nwt_iter = 20;
  NLSstatus ret = richard_scalar_update(dt,curr_nwt_iter,u_mac_curr);
  if (ParallelDescriptor::IOProcessor())
    {
      std::cout << tag;
      if (ret == NLSstatus::NLS_LINEAR_FAIL) {
	std::cout << " - linear solver failure ";
      }
      else if ( ret == NLSstatus::NLS_NONLINEAR_FAIL) {
	std::cout << tag << " - nonlinear solver failure ";
      }
      std::cout << std::endl;
    }

  BL_ASSERT(ret == NLSstatus::NLS_SUCCESS);

  compute_vel_phase(u_mac_curr,0,time+dt);
    
  if (level == 0) {
    create_umac_grown(u_mac_curr,u_macG_trac);
  } else {
      PArray<MultiFab> u_macG_crse(BL_SPACEDIM,PArrayManage);
      GetCrseUmac(u_macG_crse,time);
      create_umac_grown(u_mac_curr,u_macG_crse,u_macG_trac); 
  }

  if (advect_tracers > 0)
    {
      bool reflux_on_this_call = true;
      bool use_cached_sat = false;
      tracer_advection(u_macG_trac,reflux_on_this_call,use_cached_sat);
    }

  // predict the next time step. 
  Real dt_nwt = dt; 
  predictDT(u_macG_trac,time+dt);
  if (curr_nwt_iter < 10 && dt_grow_max>=1) 
    dt_nwt *= dt_grow_max;
  //if (richard_time < 3.0*richard_time_min)
  //{
  //  dt_nwt = dt_nwt*change_max;
      //if (curr_nwt_iter <= richard_iter && curr_nwt_iter < 4 && dt_nwt < richard_max_dt)
      //	dt_nwt = dt_nwt*1.1;
      //else if (curr_nwt_iter > 5)
      //	dt_nwt = dt_nwt*0.75;
      //else if (curr_nwt_iter < 2) 
      //	dt_nwt = dt_nwt*1.1;
      //richard_iter = curr_nwt_iter;
  //}
  dt_eig = std::min(dt_eig,dt_nwt); 
#endif
}
#endif

bool
PorousMedia::advance_multilevel_richards_flow (Real  t_flow,
                                               Real  dt_flow,
                                               Real& dt_flow_new)
{
  if (model == PM_RICHARDS) {
    BL_ASSERT(do_multilevel_full);
    BL_ASSERT(use_PETSc_snes_for_evolution);
  }
  if (level != 0) {
    return true;
  }
  bool step_ok = false;
  dt_flow_new = dt_flow;
  // Lazily build structure to save state at time=t.  If we must subcycle, the
  // algorithm will overwrite old_time data as it goes.  This saved_state
  // must include all state types involved in this subcycle; we make a set
  // of ids and set them manually to minimize the overhead of this
  std::set<int> types_advanced;
  types_advanced.insert(State_Type);
  types_advanced.insert(Press_Type);

  int finest_level = parent->finestLevel();
  int nlevs = finest_level + 1;
  PMAmr* pm_amr = dynamic_cast<PMAmr*>(parent);
  if (!pm_amr) 
    BoxLib::Abort("Bad cast in PorousMedia::advance_multilevel_richards_flow");

  int num_active_levels = finest_level + 1;
  Layout layout_sub(parent,num_active_levels);
  if (steady_use_PETSc_snes) {

    // Prepare the state data structures
    for (int lev=0; lev<=finest_level; ++lev) {
      PorousMedia& pm = getLevel(lev);        
      for (std::set<int>::const_iterator it=types_advanced.begin(), End=types_advanced.end(); 
           it!=End; ++it) {      
        StateData& SD = pm.state[*it];
        SD.setNewTimeLevel(t_flow);
        SD.allocOldData();
        SD.swapTimeLevels(dt_flow);

        // Provide guess for solver
        MultiFab::Copy(SD.newData(),SD.oldData(),0,0,SD.newData().nComp(),SD.newData().nGrow());
      }
    }
  }
    
  int nc = 0; // Component of water in state

  // Save initial state, used in the native solver to build res_fix
  if (steady_use_PETSc_snes) {
    for (int lev=0;lev<nlevs;lev++)
    {
      PorousMedia&    fine_lev   = getLevel(lev);
      if (!do_richard_sat_solve)
      {
        MultiFab& P_lev = fine_lev.get_old_data(Press_Type);
        MFTower& IC = *(richard_solver->GetRSdata().InitialState);
        MultiFab::Copy(IC[lev],P_lev,0,0,1,1);
      }
      else
      {
        MultiFab& S_lev = fine_lev.get_old_data(State_Type);
        MFTower& IC = *(richard_solver->GetRSdata().InitialState);
        MultiFab::Copy(IC[lev],S_lev,0,0,1,1);
      }
    }
  }

  richard_solver_control->ResetCounters();
  richard_solver_data->ResetJacobianCounter();

  std::string units_str = do_output_flow_time_in_years ? "Y" : "s";
  std::pair<Real,std::string> told_flow_output = PMAmr::convert_time_units(t_flow,units_str);
  std::pair<Real,std::string> tnew_flow_output = PMAmr::convert_time_units(t_flow+dt_flow,units_str);
  std::pair<Real,std::string> dt_flow_output = PMAmr::convert_time_units(dt_flow,units_str);

  std::ios_base::fmtflags oldflags = std::cout.flags(); std::cout << std::scientific << std::setprecision(10);
  if (richard_solver_verbose > 1 && ParallelDescriptor::IOProcessor())
    std::cout << "  FLOW: Level " << level
              << " TIME = " << told_flow_output.first << told_flow_output.second
              << " : "      << tnew_flow_output.first << tnew_flow_output.second
              << ", DT: "   << dt_flow_output.first   << dt_flow_output.second
              << std::endl;
  std::cout.flags(oldflags);
    
  NLSstatus ret;
  if (steady_use_PETSc_snes) 
  {
    richard_solver->ResetRhoSat();
    richard_solver->SetCurrentTimestep(parent->levelSteps(0));
    if (!steady_record_file.empty()) {
      richard_solver->SetRecordFile(steady_record_file);
    }

    // Solve for the update using PETSc
    int retCode = richard_solver->Solve(t_flow, t_flow+dt_flow, 1, *richard_solver_control);
    if (retCode > 0) {
      ret = NLSstatus::NLS_SUCCESS;
      richard_solver->ComputeDarcyVelocity(richard_solver->GetPressureNp1(),t_flow+dt_flow);
    } 
    else {
      if (retCode == -3 || retCode == 0) {
        ret = NLSstatus::NLS_LINEAR_FAIL;
      }
      else {
        ret = NLSstatus::NLS_NONLINEAR_FAIL;
      }
    }
  }
#ifdef MG_USE_FBOXLIB
  else
  {
    // Sove for the update using native solver
    ret = richard_composite_update(dt_flow,*richard_solver_control);
  }
#endif

  bool cont = richard_solver_control->AdjustDt(dt_flow,ret,dt_flow_new);

  step_ok = (ret == NLSstatus::NLS_SUCCESS);


  std::pair<Real,std::string> dt_new_flow_output = PMAmr::convert_time_units(dt_flow_new,units_str);
  if (richard_solver_verbose > 1 && ParallelDescriptor::IOProcessor())
  {
    std::string resultStr = (step_ok ? "SUCCESS" : "FAIL");
        
    std::ios_base::fmtflags oldflags = std::cout.flags(); std::cout << std::scientific << std::setprecision(10);
    std::cout << "  FLOW: " << resultStr << ". (iters: " << richard_solver_control->NLIterationsTaken()
              << "). Suggest next dt: " << dt_new_flow_output.first << dt_new_flow_output.second << std::endl;
    std::cout.flags(oldflags);
  }
  
  if (!step_ok) {
    // Restore the state data structures
    for (int lev=0; lev<=finest_level; ++lev) {
      PorousMedia& pm = getLevel(lev);        
      for (std::set<int>::const_iterator it=types_advanced.begin(), End=types_advanced.end(); 
           it!=End; ++it) {      
        pm.state[*it].setNewTimeLevel(t_flow-dt_flow);
        pm.state[*it].swapTimeLevels(dt_flow);
      }
    }
  }
  
  return step_ok;
}

void
PorousMedia::advance_tracer (Real time,
			     Real dt)
{
  // 
  // Time stepping for tracers, assuming steady-state condition. 
  //

  BL_ASSERT(advect_tracers > 0);
  BL_ASSERT(ntracers > 0);
    
  bool reflux_on_this_call = true;
  bool use_cached_sat = false;
  tracer_advection(u_macG_trac,reflux_on_this_call,use_cached_sat);
}

void
PorousMedia::create_lambda (Real time) 
{
  // 
  // lambda_T is evaluated at edges.  
  // 

  if (model == PM_SINGLE_PHASE ||
      model == PM_SINGLE_PHASE_SOLID)
    {
      for (int dir=0; dir<BL_SPACEDIM; dir++)
	{
	  for (MFIter mfi(lambda[dir]); mfi.isValid(); ++mfi)
	    {
	      const Box& ebox = lambda[dir][mfi].box();
	      lambda[dir][mfi].copy(kpedge[dir][mfi],ebox,0,ebox,0,1);
	    }
	}
    }
  else
    {
      MultiFab& S = get_new_data(State_Type);

      const TimeLevel whichTime = which_time(State_Type,time);
      BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);    
      MultiFab* lcc = (whichTime == AmrOldTime) ? lambda_cc : lambdap1_cc;

      const int*  domlo    = geom.Domain().loVect();
      const int*  domhi    = geom.Domain().hiVect();

      for (FillPatchIterator S_fpi(*this,S,1,time,State_Type,0,ncomps);
	   S_fpi.isValid();
           ++S_fpi)
	{

	  const int i = S_fpi.index();
	  BL_ASSERT(grids[i] == S_fpi.validbox());

	  const int* lo     = S_fpi.validbox().loVect();
	  const int* hi     = S_fpi.validbox().hiVect();

	  const Real* ldat  = (*lcc)[i].dataPtr();
	  const int* l_lo   = (*lcc)[i].loVect();
	  const int* l_hi   = (*lcc)[i].hiVect();

	  const int* lx_lo  = lambda[0][i].loVect();
	  const int* lx_hi  = lambda[0][i].hiVect();
	  const Real* lxdat = lambda[0][i].dataPtr();

	  const int* ly_lo  = lambda[1][i].loVect();
	  const int* ly_hi  = lambda[1][i].hiVect();
	  const Real* lydat = lambda[1][i].dataPtr();

#if(BL_SPACEDIM==3)
	  const int* lz_lo  = lambda[2][i].loVect();
	  const int* lz_hi  = lambda[2][i].hiVect();
	  const Real* lzdat = lambda[2][i].dataPtr();
#endif

	  const int* kx_lo  = kpedge[0][i].loVect();
	  const int* kx_hi  = kpedge[0][i].hiVect();
	  const Real* kxdat = kpedge[0][i].dataPtr();

	  const int* ky_lo  = kpedge[1][i].loVect();
	  const int* ky_hi  = kpedge[1][i].hiVect();
	  const Real* kydat = kpedge[1][i].dataPtr();

#if(BL_SPACEDIM==3)
	  const int* kz_lo  = kpedge[2][i].loVect();
	  const int* kz_hi  = kpedge[2][i].hiVect();
	  const Real* kzdat = kpedge[2][i].dataPtr();
#endif

	  Array<int> bc;
	  bc = getBCArray(State_Type,i,0,1);

	  FORT_MK_MACCOEF (lxdat,ARLIM(lx_lo),ARLIM(lx_hi),
			   lydat,ARLIM(ly_lo),ARLIM(ly_hi),
#if (BL_SPACEDIM==3)
			   lzdat,ARLIM(lz_lo),ARLIM(lz_hi),
#endif
			   kxdat,ARLIM(kx_lo),ARLIM(kx_hi),
			   kydat,ARLIM(ky_lo),ARLIM(ky_hi),
#if (BL_SPACEDIM==3)
			   kzdat,ARLIM(kz_lo),ARLIM(kz_hi),
#endif
			   ldat,ARLIM(l_lo),ARLIM(l_hi),
			   lo,hi,domlo,domhi,bc.dataPtr());
	}
    }
}

void
PorousMedia::mac_project (MultiFab* u_mac, MultiFab* RhoD, Real time)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::mac_project()");
  BL_ASSERT(model != PM_RICHARDS);
  BL_ASSERT(model != PM_STEADY_SATURATED);
  BL_ASSERT(model != PM_SATURATED);

  if (verbose>3 && ParallelDescriptor::IOProcessor())
    std::cout << "... mac_projection at level " << level 
	      << " at time " << time << '\n';
  
  create_lambda(time);

  MultiFab RhoG(grids,1,1); 
  RhoG.setVal(0);
  for (int dir=0; dir < BL_SPACEDIM; dir ++)
    {
      RhoD[dir].setVal(0.0);
      u_mac[dir].setVal(0.0);
    }

  initialize_umac(u_mac,RhoG,RhoD,time);

  const TimeLevel whichTime = which_time(State_Type,time);
  BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);

  MultiFab* phi = 0;
  if (whichTime == AmrOldTime) 
    phi = &get_old_data(Press_Type);
  else if (whichTime == AmrNewTime) 
    phi = &get_new_data(Press_Type);

  // Always start with an initial guess of zero in the interior
  phi->setVal(0.);

  const BCRec& p_bc = desc_lst[Press_Type].getBC(0);

  // Set the boundary conditions *before* we define mac_bndry
  // so the values will end up in mac_bndry
  mac_projector->set_dirichlet_bcs(level, phi, RhoG, p_bc, 
				   press_lo, press_hi);
  phi->FillBoundary();

  PressBndry mac_bndry(grids,1,geom);
  const int src_comp   = 0;
  const int dest_comp  = 0;
  const int num_comp   = 1;
  if (level == 0)
    {
      mac_bndry.setBndryValues(*phi,src_comp,dest_comp,num_comp,p_bc);
    }
  else
    {
      MultiFab CPhi;
      GetCrsePressure(CPhi,time);
      BoxArray crse_boxes = BoxArray(grids).coarsen(crse_ratio);
      const int in_rad     = 0;
      const int out_rad    = 1;
      const int extent_rad = 2;
      BndryRegister crse_br(crse_boxes,in_rad,out_rad,extent_rad,num_comp);
      crse_br.copyFrom(CPhi,extent_rad,src_comp,dest_comp,num_comp);
      mac_bndry.setBndryValues(crse_br,src_comp,*phi,src_comp,
			       dest_comp,num_comp,crse_ratio,p_bc);
    }
  //
  // get source term
  //
  if (do_source_term) {
    int do_rho_scale = 1;
    int nGrow = 0;
    getForce(*source,nGrow,0,ncomps,time,do_rho_scale);
  }
  const Real strt_time = ParallelDescriptor::second();
  mac_projector->mac_project(level,u_mac,lambda,RhoD,source,
			     phi,mac_bndry,p_bc);

  if (model != PM_SINGLE_PHASE ||
      model != PM_SINGLE_PHASE_SOLID)
    {
      MultiFab* u_phase = new MultiFab[BL_SPACEDIM];
      for (int dir = 0; dir < BL_SPACEDIM; dir++)
	{
	  BoxArray edge_grids(grids);
	  edge_grids.surroundingNodes(dir);
	  u_phase[dir].define(edge_grids,1,0,Fab_allocate);
	  u_phase[dir].setVal(1.e40);
	}

      compute_vel_phase(u_phase,u_mac,time);
      umac_cpy_edge_to_cen(u_phase,Vcr_Type,1);

      delete [] u_phase;

    }

  // compute time spend in mac_project()
  if (show_selected_runtimes > 0)
    {
      const int IOProc   = ParallelDescriptor::IOProcessorNumber();
      Real      run_time = ParallelDescriptor::second() - strt_time;
      ParallelDescriptor::ReduceRealMax(run_time,IOProc);
      if (ParallelDescriptor::IOProcessor())
        std::cout << "PorousMedia:mac_project(): lev: " << level
                  << ", time: " << run_time << '\n';
    }
}

void
PorousMedia::initialize_umac (MultiFab* u_mac, MultiFab& RhoG, 
			      MultiFab* RhoD, Real time) 
{

  //
  // u_mac is initilized such that its divergence is 
  //   \nabla \rho g
  // RhoG is initialized such that p + RhoG*\Delta x is 
  //   the hydrostatic pressure 
  // RhoD is initizlized such that its divergence is 
  //   the diffusive term due to variable density formulation.
  //
  const int* domain_lo = geom.Domain().loVect();
  const int* domain_hi = geom.Domain().hiVect();
  const Real* dx       = geom.CellSize();

  MultiFab& S = get_new_data(State_Type);
  Array<Real> const_diff_coef(ncomps);
  for (int i=0;i<ncomps;i++)
    const_diff_coef[i] = visc_coef[i];
  const TimeLevel whichTime = which_time(State_Type,time);
  BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);
  MultiFab* pc;
  if (have_capillary)
    pc = (whichTime == AmrOldTime) ? pcn_cc : pcnp1_cc;
  else
    {
      pc = new MultiFab(grids,1,1);
      (*pc).setVal(0.);
    }
  MultiFab* lbd = (whichTime == AmrOldTime) ? lambda_cc : lambdap1_cc;
  for (FillPatchIterator S_fpi(*this,S,1,time,State_Type,0,ncomps);
       S_fpi.isValid();
       ++S_fpi)
    {
      const int  i   = S_fpi.index();
      const int* lo  = grids[i].loVect();
      const int* hi  = grids[i].hiVect();
	
      const int* lx_lo  = lambda[0][i].loVect();
      const int* lx_hi  = lambda[0][i].hiVect();
      const Real* lxdat = lambda[0][i].dataPtr();

      const int* ly_lo  = lambda[1][i].loVect();
      const int* ly_hi  = lambda[1][i].hiVect();
      const Real* lydat = lambda[1][i].dataPtr();

      const int* kx_lo  = kpedge[0][i].loVect();
      const int* kx_hi  = kpedge[0][i].hiVect();
      const Real* kxdat = kpedge[0][i].dataPtr();

      const int* ky_lo  = kpedge[1][i].loVect();
      const int* ky_hi  = kpedge[1][i].hiVect();
      const Real* kydat = kpedge[1][i].dataPtr();

      FArrayBox& Sfab   = S_fpi();
      const Real* ndat  = Sfab.dataPtr(); 
      const int*  n_lo  = Sfab.loVect();
      const int*  n_hi  = Sfab.hiVect();
	
      Box bx_mac(u_mac[0][i].box());
      const int* umlo   = bx_mac.loVect();
      const int* umhi   = bx_mac.hiVect();
      const Real* umdat = u_mac[0][i].dataPtr();

      Box by_mac(u_mac[1][i].box());
      const int* vmlo   = by_mac.loVect();
      const int* vmhi   = by_mac.hiVect();
      const Real* vmdat = u_mac[1][i].dataPtr();

      const int* rglo   = RhoG[i].loVect();
      const int* rghi   = RhoG[i].hiVect();
      const Real* rgdat = RhoG[i].dataPtr();

      const int* pclo   = (*pc)[i].loVect();
      const int* pchi   = (*pc)[i].hiVect();
      const Real* pcdat = (*pc)[i].dataPtr();

      const int* lbdlo   = (*lbd)[i].loVect();
      const int* lbdhi   = (*lbd)[i].hiVect();
      const Real* lbddat = (*lbd)[i].dataPtr();

      Box rx_mac(RhoD[0][i].box());
      const int* rxlo   = rx_mac.loVect();
      const int* rxhi   = rx_mac.hiVect();
      const Real* rxdat = RhoD[0][i].dataPtr();

      Box ry_mac(RhoD[1][i].box());
      const int* rylo   = ry_mac.loVect();
      const int* ryhi   = ry_mac.hiVect();
      const Real* rydat = RhoD[1][i].dataPtr();

      const int* p_lo   = (*rock_phi)[i].loVect();
      const int* p_hi   = (*rock_phi)[i].hiVect();
      const Real* pdat  = (*rock_phi)[i].dataPtr();

      Array<int> s_bc;
      s_bc = getBCArray(State_Type,i,0,1);

      Array<int> press_bc;
      press_bc = getBCArray(Press_Type,i,0,1);

#if (BL_SPACEDIM == 2)	
      FORT_INIT_UMAC (umdat,ARLIM(umlo),ARLIM(umhi),
		      vmdat,ARLIM(vmlo),ARLIM(vmhi),
		      pcdat,ARLIM(pclo),ARLIM(pchi),
		      lbddat,ARLIM(lbdlo),ARLIM(lbdhi),
		      lxdat,ARLIM(lx_lo),ARLIM(lx_hi),
		      lydat,ARLIM(ly_lo),ARLIM(ly_hi),
		      kxdat,ARLIM(kx_lo),ARLIM(kx_hi),
		      kydat,ARLIM(ky_lo),ARLIM(ky_hi),
		      rgdat,ARLIM(rglo),ARLIM(rghi),
		      rxdat,ARLIM(rxlo),ARLIM(rxhi),
		      rydat,ARLIM(rylo),ARLIM(ryhi),
		      ndat ,ARLIM(n_lo),ARLIM(n_hi),
		      pdat ,ARLIM(p_lo),ARLIM(p_hi),
		      const_diff_coef.dataPtr(),
		      s_bc.dataPtr(),press_bc.dataPtr(),
		      domain_lo,domain_hi,dx,lo,hi,
		      &wt_lo, &wt_hi,
		      inflow_bc_lo.dataPtr(),inflow_bc_hi.dataPtr());

#elif (BL_SPACEDIM == 3)
      Box bz_mac(u_mac[2][i].box());
      const int* wmlo  = bz_mac.loVect();
      const int* wmhi  = bz_mac.hiVect();
      const Real* wmdat = u_mac[2][i].dataPtr();

      const int* lz_lo  = lambda[2][i].loVect();
      const int* lz_hi  = lambda[2][i].hiVect();
      const Real* lzdat = lambda[2][i].dataPtr();

      const int* kz_lo  = kpedge[2][i].loVect();
      const int* kz_hi  = kpedge[2][i].hiVect();
      const Real* kzdat = kpedge[2][i].dataPtr();

      Box rz_mac(RhoD[2][i].box());
      const int* rzlo  = rz_mac.loVect();
      const int* rzhi  = rz_mac.hiVect();
      const Real* rzdat = RhoD[2][i].dataPtr();

      FORT_INIT_UMAC (umdat,ARLIM(umlo),ARLIM(umhi),
		      vmdat,ARLIM(vmlo),ARLIM(vmhi),
		      wmdat,ARLIM(wmlo),ARLIM(wmhi),
		      pcdat,ARLIM(pclo),ARLIM(pchi),
		      lbddat,ARLIM(lbdlo),ARLIM(lbdhi),
		      lxdat,ARLIM(lx_lo),ARLIM(lx_hi),
		      lydat,ARLIM(ly_lo),ARLIM(ly_hi),
		      lzdat,ARLIM(lz_lo),ARLIM(lz_hi),
		      kxdat,ARLIM(kx_lo),ARLIM(kx_hi),
		      kydat,ARLIM(ky_lo),ARLIM(ky_hi),
		      kzdat,ARLIM(kz_lo),ARLIM(kz_hi),
		      rgdat,ARLIM(rglo),ARLIM(rghi),
		      rxdat,ARLIM(rxlo),ARLIM(rxhi),
		      rydat,ARLIM(rylo),ARLIM(ryhi),
		      rzdat,ARLIM(rzlo),ARLIM(rzhi),
		      ndat,ARLIM(n_lo),ARLIM(n_hi),
		      pdat ,ARLIM(p_lo),ARLIM(p_hi),
		      const_diff_coef.dataPtr(),
		      s_bc.dataPtr(),press_bc.dataPtr(),
		      domain_lo,domain_hi,dx,lo,hi,
		      &wt_lo, &wt_hi,
		      inflow_bc_lo.dataPtr(),inflow_bc_hi.dataPtr());
#endif
    }
    
  RhoG.FillBoundary();

  FArrayBox inflow;
  for (OrientationIter oitr; oitr; ++oitr) {
      Orientation face = oitr();
      FArrayBox mask;
      if (get_inflow_velocity(face,inflow,mask,time)) {
          int shift = ( face.isHigh() ? -1 : +1 );
          inflow.shiftHalf(face.coordDir(),shift);
          for (MFIter mfi(u_mac[face.coordDir()]); mfi.isValid(); ++mfi) {
              FArrayBox& u = u_mac[face.coordDir()][mfi];
              Box ovlp = inflow.box() & u.box();
	      if (ovlp.ok()) {
                for (IntVect iv=ovlp.smallEnd(), End=ovlp.bigEnd(); iv<=End; ovlp.next(iv)) {
                  if (mask(iv,0) != 0) {
                    u(iv,0) = inflow(iv,0);
                  }
                }
              }
          }
      }
  }


  if (!have_capillary)
    delete pc;

}

bool
PorousMedia::get_inflow_velocity(const Orientation& face,
                                 FArrayBox&         ccBndFab,
                                 FArrayBox&         mask,
                                 Real               t)
{
    bool ret = false;
    if (pbc_descriptor_map.find(face) != pbc_descriptor_map.end()) 
    {
        const Box domain = geom.Domain();
        const int* domhi = domain.hiVect();
        const int* domlo = domain.loVect();
        const Real* dx   = geom.CellSize();
        Real t_eval = AdjustBCevalTime(State_Type,t,false);

        const BCDesc& bc_desc = pbc_descriptor_map[face];
        const Box bndBox = bc_desc.first;
        const Array<int>& face_bc_idxs = bc_desc.second;
        
        ccBndFab.resize(bndBox,1); ccBndFab.setVal(0);
        mask.resize(bndBox,1); mask.setVal(0);
        for (int i=0; i<face_bc_idxs.size(); ++i) {
          const RegionData& face_bc = bc_array[face_bc_idxs[i]];
          
          if (face_bc.Type() == "zero_total_velocity" || face_bc.Type() == "noflow") {
            ret = true;
            Array<Real> inflow_tmp = face_bc(t_eval);
            Real inflow_vel = inflow_tmp[0];
            const Array<const Region*>& regions = face_bc.Regions();
            for (int j=0; j<regions.size(); ++j)
            {
              regions[j]->setVal(ccBndFab,inflow_vel,0,dx,0);
              regions[j]->setVal(mask,1,0,dx,0);
            }
          }
	}
    }
    else {
      // Implement default zero-flux bc
      ret = true;
      Box bndBox = BoxLib::adjCell(geom.Domain(),face,1);
      ccBndFab.resize(bndBox,1); ccBndFab.setVal(0);
      mask.resize(bndBox,1); mask.setVal(1);
    }
    return ret;
}

void
PorousMedia::get_inflow_density(const Orientation& face,
				const RegionData&  face_bc,
				FArrayBox&         fab,
				const Box&         ccBndBox,
				Real               t)
{
  if (ParallelDescriptor::IOProcessor()) {
    std::cout << "get_inflow_density needs to be rewritten to work in parallel" << std::endl;
  }
  BoxLib::Abort();

#if 0
  Box ovlp = ccBndBox & fab.box();
  if ( (model == PM_STEADY_SATURATED)
       || (model == PM_SATURATED) ) {
    for (int n=0; n<ncomps; ++n) {
      fab.setVal(density[n],ovlp,n,1);
    }
  }
  else {
    const Real* dx   = geom.CellSize();
    FArrayBox mask;
    if (face_bc.Type() == "zero_total_velocity" || face_bc.Type() == "noflow") {
        Real t_eval = AdjustBCevalTime(State_Type,t,false);
        Array<Real> inflow_vel = face_bc(t_eval);
	FArrayBox cdat, ktdat, kdat, vdat;
	const int n_kr_coef = kr_coef->nComp();
	cdat.resize(ccBndBox,n_kr_coef);
	ktdat.resize(ccBndBox,BL_SPACEDIM);
	vdat.resize(ccBndBox,1);


        // FIXME: Pull kr params from database, order as old version expects
        FArrayBox cp;
        rock_manager->GetPropertyDirect(t,level,cp,ccBndBox,"capillary_pressure",0);
        cdat.setVal(0);
        cdat.setVal((Real)3,0);                // Model ID: old 3 = vG
        cdat.copy(cp,ccBndBox,1,ccBndBox,1,1); // "m" 
        cdat.copy(cp,ccBndBox,3,ccBndBox,2,1); // "Sr" 
        
        rock_manager->GetPropertyDirect(t,level,ktdat,ccBndBox,"permeability",0);
	kdat.resize(ccBndBox,1);
	kdat.copy(ktdat,ccBndBox,face.coordDir(),ccBndBox,0,1);

	const Array<const Region*>& regions = face_bc.Regions();
        
        mask.resize(ccBndBox,1); mask.setVal(-1);
	for (int j=0; j<regions.size(); ++j) {
	  regions[j]->setVal(vdat,inflow_vel[0],0,dx,0);
	  regions[j]->setVal(mask,1,0,dx,0);
        }

	DEF_LIMITS(fab,s_ptr,s_lo,s_hi);
	DEF_CLIMITS(vdat,v_ptr,v_lo,v_hi);
	DEF_CLIMITS(cdat,c_ptr,c_lo,c_hi);
	DEF_CLIMITS(kdat,k_ptr,k_lo,k_hi);
	DEF_CLIMITS(mask,m_ptr,m_lo,m_hi);
        const int* lo = ovlp.loVect();
        const int* hi = ovlp.hiVect();

	int nc = 1;
	FORT_STEADYSTATE_FAB(s_ptr, ARLIM(s_lo),ARLIM(s_hi), 
			     density.dataPtr(),muval.dataPtr(),&ncomps,
			     k_ptr, ARLIM(k_lo),ARLIM(k_hi), 
			     c_ptr, ARLIM(c_lo),ARLIM(c_hi), &n_kr_coef,
			     v_ptr, ARLIM(v_lo),ARLIM(v_hi), 
			     m_ptr, ARLIM(m_lo),ARLIM(m_hi), 
			     dx, &nc, &gravity, lo, hi);
    }
  }
#endif
}


void
PorousMedia::compute_vel_phase (MultiFab* u_phase, MultiFab* u_mac,
				Real time) 
{
  //
  // The phase velocity of component 1 is given by 
  //   v_1 = \lambda_1/\lambda_T ( v_T + \lambda_2 \nabla pc)
  //

  const int* domain_lo = geom.Domain().loVect();
  const int* domain_hi = geom.Domain().hiVect();
  const Real* dx       = geom.CellSize();

  MultiFab& S = get_data(State_Type,time);

  const TimeLevel whichTime = which_time(State_Type,time);
  BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);

  MultiFab* pc;
  if (have_capillary)
    pc = (whichTime == AmrOldTime) ? pcn_cc : pcnp1_cc;
  else
    {
      pc = new MultiFab(grids,1,1);
      (*pc).setVal(0.);
    }

  MultiFab* lbd = (whichTime == AmrOldTime) ? lambda_cc : lambdap1_cc;
    
  for (FillPatchIterator S_fpi(*this,S,1,time,State_Type,0,ncomps);
       S_fpi.isValid();
       ++S_fpi)
    {
      const int  i   = S_fpi.index();
      const int* lo  = grids[i].loVect();
      const int* hi  = grids[i].hiVect();

      const int* kx_lo  = kpedge[0][i].loVect();
      const int* kx_hi  = kpedge[0][i].hiVect();
      const Real* kxdat = kpedge[0][i].dataPtr();

      const int* ky_lo  = kpedge[1][i].loVect();
      const int* ky_hi  = kpedge[1][i].hiVect();
      const Real* kydat = kpedge[1][i].dataPtr();
	
      Box bx_mac(u_mac[0][i].box());
      const int* umlo   = bx_mac.loVect();
      const int* umhi   = bx_mac.hiVect();
      const Real* umdat = u_mac[0][i].dataPtr();

      Box by_mac(u_mac[1][i].box());
      const int* vmlo   = by_mac.loVect();
      const int* vmhi   = by_mac.hiVect();
      const Real* vmdat = u_mac[1][i].dataPtr();

      Box ax_mac(u_phase[0][i].box());
      const int* uplo   = ax_mac.loVect();
      const int* uphi   = ax_mac.hiVect();
      const Real* updat = u_phase[0][i].dataPtr();

      Box ay_mac(u_phase[1][i].box());
      const int* vplo   = ay_mac.loVect();
      const int* vphi   = ay_mac.hiVect();
      const Real* vpdat = u_phase[1][i].dataPtr();

      const int* pclo   = (*pc)[i].loVect();
      const int* pchi   = (*pc)[i].hiVect();
      const Real* pcdat = (*pc)[i].dataPtr();

      const int* lbdlo   = (*lbd)[i].loVect();
      const int* lbdhi   = (*lbd)[i].hiVect();
      const Real* lbddat = (*lbd)[i].dataPtr();

      Array<int> s_bc;
      s_bc = getBCArray(State_Type,i,0,1);

#if (BL_SPACEDIM == 2)	
      FORT_UPHASE (updat,ARLIM(uplo),ARLIM(uphi),
		   vpdat,ARLIM(vplo),ARLIM(vphi),
		   umdat,ARLIM(umlo),ARLIM(umhi),
		   vmdat,ARLIM(vmlo),ARLIM(vmhi),
		   pcdat,ARLIM(pclo),ARLIM(pchi),
		   lbddat,ARLIM(lbdlo),ARLIM(lbdhi),
		   kxdat,ARLIM(kx_lo),ARLIM(kx_hi),
		   kydat,ARLIM(ky_lo),ARLIM(ky_hi),
		   s_bc.dataPtr(),
		   domain_lo,domain_hi,dx,lo,hi);

#elif (BL_SPACEDIM == 3)
      Box bz_mac(u_mac[2][i].box());
      const int* wmlo  = bz_mac.loVect();
      const int* wmhi  = bz_mac.hiVect();
      const Real* wmdat = u_mac[2][i].dataPtr();

      Box az_mac(u_phase[2][i].box());
      const int* wplo  = az_mac.loVect();
      const int* wphi  = az_mac.hiVect();
      const Real* wpdat = u_phase[2][i].dataPtr();

      const int* kz_lo  = kpedge[2][i].loVect();
      const int* kz_hi  = kpedge[2][i].hiVect();
      const Real* kzdat = kpedge[2][i].dataPtr();

      FORT_UPHASE (updat,ARLIM(uplo),ARLIM(uphi),
		   vpdat,ARLIM(vplo),ARLIM(vphi),
		   wpdat,ARLIM(wplo),ARLIM(wphi),
		   umdat,ARLIM(umlo),ARLIM(umhi),
		   vmdat,ARLIM(vmlo),ARLIM(vmhi),
		   wmdat,ARLIM(wmlo),ARLIM(wmhi),
		   pcdat,ARLIM(pclo),ARLIM(pchi),
		   lbddat,ARLIM(lbdlo),ARLIM(lbdhi),
		   kxdat,ARLIM(kx_lo),ARLIM(kx_hi),
		   kydat,ARLIM(ky_lo),ARLIM(ky_hi),
		   kzdat,ARLIM(kz_lo),ARLIM(kz_hi),
		   s_bc.dataPtr(),
		   domain_lo,domain_hi,dx,lo,hi);
#endif
    }

  FArrayBox inflow;
  for (OrientationIter oitr; oitr; ++oitr) {
      Orientation face = oitr();
      FArrayBox mask;
      if (get_inflow_velocity(face,inflow,mask,time)) {
          int shift = ( face.isHigh() ? -1 : +1 );
          inflow.shiftHalf(face.coordDir(),shift);
          for (MFIter mfi(u_phase[face.coordDir()]); mfi.isValid(); ++mfi) {
              FArrayBox& u = u_phase[face.coordDir()][mfi];
              Box ovlp = inflow.box() & u.box();
	      if (ovlp.ok()) {
                for (IntVect iv=ovlp.smallEnd(), End=ovlp.bigEnd(); iv<=End; ovlp.next(iv)) {
                  if (mask(iv,0) != 0) {
                    u(iv,0) = inflow(iv,0);
                  }
                }
              }
          }
      }
  }

  if (!have_capillary)
    delete pc;
}

void
PorousMedia::compute_vel_phase (MultiFab* u_phase, 
				int nc,
				Real time) 
{
  //
  // The phase velocity of component n is given by 
  //   v_n = \lambda_n ( \nabla p_n - \rho \gvec)
  // We are going to assume the p stored in PRESS_TYPE 
  // correspond to p_n.  
  //

  const int* domain_lo = geom.Domain().loVect();
  const int* domain_hi = geom.Domain().hiVect();
  const Real* dx       = geom.CellSize();

  MultiFab& S = get_data(State_Type,time);
  MultiFab& P = get_data(Press_Type,time);

  const TimeLevel whichTime = which_time(State_Type,time);
  BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);

  MultiFab* lbd = (whichTime == AmrOldTime) ? lambda_cc : lambdap1_cc;
    
  for (FillPatchIterator S_fpi(*this,S,1,time,State_Type,0,ncomps);
       S_fpi.isValid();
       ++S_fpi)
    {
      const int  i   = S_fpi.index();
      const int* lo  = grids[i].loVect();
      const int* hi  = grids[i].hiVect();
      
      Box ax_mac(u_phase[0][i].box());
      const int* uplo   = ax_mac.loVect();
      const int* uphi   = ax_mac.hiVect();
      const Real* updat = u_phase[0][i].dataPtr();

      Box ay_mac(u_phase[1][i].box());
      const int* vplo   = ay_mac.loVect();
      const int* vphi   = ay_mac.hiVect();
      const Real* vpdat = u_phase[1][i].dataPtr();

      const int* plo   = P[i].loVect();
      const int* phi   = P[i].hiVect();
      const Real* pdat = P[i].dataPtr();

      const int* lbdlo   = (*lbd)[i].loVect();
      const int* lbdhi   = (*lbd)[i].hiVect();
      const Real* lbddat = (*lbd)[i].dataPtr();

      const int* kx_lo  = kpedge[0][i].loVect();
      const int* kx_hi  = kpedge[0][i].hiVect();
      const Real* kxdat = kpedge[0][i].dataPtr();

      const int* ky_lo  = kpedge[1][i].loVect();
      const int* ky_hi  = kpedge[1][i].hiVect();
      const Real* kydat = kpedge[1][i].dataPtr();

      Array<int> bc = getBCArray(Press_Type,i,0,1);
#if (BL_SPACEDIM == 2)	
      FORT_UPHASE_P (updat,ARLIM(uplo),ARLIM(uphi),
		     vpdat,ARLIM(vplo),ARLIM(vphi),
		     lbddat,ARLIM(lbdlo),ARLIM(lbdhi),
		     pdat,ARLIM(plo),ARLIM(phi),
		     kxdat,ARLIM(kx_lo),ARLIM(kx_hi),
		     kydat,ARLIM(ky_lo),ARLIM(ky_hi),
		     lo,hi,domain_lo,domain_hi,dx,bc.dataPtr());

#elif (BL_SPACEDIM == 3)
      Box az_mac(u_phase[2][i].box());
      const int* wplo  = az_mac.loVect();
      const int* wphi  = az_mac.hiVect();
      const Real* wpdat = u_phase[2][i].dataPtr();

      const int* kz_lo  = kpedge[2][i].loVect();
      const int* kz_hi  = kpedge[2][i].hiVect();
      const Real* kzdat = kpedge[2][i].dataPtr();

      FORT_UPHASE_P (updat,ARLIM(uplo),ARLIM(uphi),
		     vpdat,ARLIM(vplo),ARLIM(vphi),
		     wpdat,ARLIM(wplo),ARLIM(wphi),
		     lbddat,ARLIM(lbdlo),ARLIM(lbdhi),
		     pdat,ARLIM(plo),ARLIM(phi),
		     kxdat,ARLIM(kx_lo),ARLIM(kx_hi),
		     kydat,ARLIM(ky_lo),ARLIM(ky_hi),
		     kzdat,ARLIM(kz_lo),ARLIM(kz_hi),
		     lo,hi,domain_lo,domain_hi,dx,bc.dataPtr());
#endif
    }

  FArrayBox inflow;
  for (OrientationIter oitr; oitr; ++oitr) {
      Orientation face = oitr();
      FArrayBox mask;
      if (get_inflow_velocity(face,inflow,mask,time)) {
          int shift = ( face.isHigh() ? -1 : +1 );
          inflow.shiftHalf(face.coordDir(),shift);
          for (MFIter mfi(u_phase[face.coordDir()]); mfi.isValid(); ++mfi) {
              FArrayBox& u = u_phase[face.coordDir()][mfi];
              Box ovlp = inflow.box() & u.box();
              if (ovlp.ok()) {
                for (IntVect iv=ovlp.smallEnd(), End=ovlp.bigEnd(); iv<=End; ovlp.next(iv)) {
                  if (mask(iv,0) != 0) {
                    u(iv,0) = inflow(iv,0);
                  }
                }
              }
          }
      }
  }

}

// =========================================
// Functions related to advection equations.
// =========================================

//
// scalar_advection advects the scalars based on Godunov scheme.
//
void
PorousMedia::scalar_advection (MultiFab* u_macG,
                               Real dt,
                               int  fscalar,
                               int  lscalar,
                               bool reflux_on_this_call)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::scalar_advection()");
  //
  // Get simulation parameters.
  //
  const Real* dx        = geom.CellSize();
  const Real  prev_time = state[State_Type].prevTime();
  const Real  curr_time = state[State_Type].curTime();
  int nscal             = lscalar - fscalar + 1;

  //
  // Get the viscous terms.
  // For model 0-1 => diffusion term
  //             2 => capillary pressure term
  //
  MultiFab visc_terms(grids,nscal,1);
  visc_terms.setVal(0);
  int do_visc_terms = 1;
  if (be_cn_theta != 1.0 && (do_visc_terms || have_capillary) && !do_cpl_advect)
      getViscTerms(visc_terms,fscalar,nscal,prev_time);

  //
  // Divergence of velocity: set to zero for now.
  //
  MultiFab* divu_fp = new MultiFab(grids,1,1);
  (*divu_fp).setVal(0.);
  //
  // Set up the grid loop.
  //
  FArrayBox flux[BL_SPACEDIM], pctmp, phitmp, kappatmp;

  Array<int> state_bc;

  // S_new is only used as a container to hold
  // time t^{n+1} inflow boundary conditions
  MultiFab& S_new = get_new_data(State_Type); 
  for (MFIter mfi(S_new); mfi.isValid(); ++mfi) {
    setPhysBoundaryValues(S_new[mfi],State_Type,curr_time,fscalar,fscalar,nscal);
  }

  MultiFab fluxes[BL_SPACEDIM];
  
  if (reflux_on_this_call && do_reflux && level < parent->finestLevel())
    {
      for (int i = 0; i < BL_SPACEDIM; i++)
	{
	  BoxArray ba = grids;
	  ba.surroundingNodes(i);
	  fluxes[i].define(ba, nscal, 0, Fab_allocate);
	}
    }

  int nGrowF = 1;
  getForce(*source,nGrowF,0,nscal,prev_time);

  for (FillPatchIterator S_fpi(*this,get_old_data(State_Type),nGrowHYP,
			       prev_time,State_Type,fscalar,nscal);
       S_fpi.isValid();
       ++S_fpi)
    {
      const int i = S_fpi.index();
      godunov->Setup(grids[i], flux[0], flux[1], 
#if (BL_SPACEDIM == 3)  
		     flux[2], 
#endif
		     nscal,model);	   
      
      Real eigmax_m[BL_SPACEDIM] = {D_DECL(-1.e20,-1.e20,-1.e20)};
      
      int state_ind = 0;
      int use_conserv_diff = (advectionType[state_ind] == Conservative);
      
      godunov->Sum_tf_divu_visc(S_fpi(),state_ind,(*source)[S_fpi],state_ind,nscal,
				visc_terms[i],state_ind,
				(*divu_fp)[i],use_conserv_diff);
      
      state_bc = getBCArray(State_Type,i,state_ind,1);
      
      //
      // Polymer model.
      //
      if (model == PM_POLYMER) 
	{ 
	  godunov->AdvectStatePmr(grids[i], dx, dt, 
				  area[0][i], u_macG[0][i], flux[0], kpedge[0][i],
				  area[1][i], u_macG[1][i], flux[1], kpedge[1][i],
#if (BL_SPACEDIM == 3)                        
				  area[2][i], u_macG[2][i], flux[2], kpedge[2][i],
#endif
				  S_fpi(),S_new[i],(*source)[S_fpi],
				  (*divu_fp)[i] , state_ind,
				  (*aofs)[i]    , state_ind,
				  (*rock_phi)[i], (*kappa)[i],
				  use_conserv_diff,
				  state_ind,state_bc.dataPtr(),volume[i],
				  nscal,gravity,gravity_dir,eigmax_m);
	}

      //
      // Single phase  model.
      //
      else if (model == PM_SINGLE_PHASE ||
	       model == PM_SINGLE_PHASE_SOLID)
	{
	  godunov->AdvectStateLin(grids[i], dx, dt, 
				  area[0][i], u_macG[0][i], flux[0],
				  area[1][i], u_macG[1][i], flux[1], 
#if (BL_SPACEDIM == 3)                        
				  area[2][i], u_macG[2][i], flux[2],
#endif
				  S_fpi(),S_new[i],(*source)[S_fpi], state_ind,
				  (*aofs)[i]    , state_ind,
				  (*rock_phi)[i], state_ind,
				  state_bc.dataPtr(),volume[i],nscal);	
	}
      //
      // Two-phase two-component model.
      //
      else if (model == PM_TWO_PHASE)
	{
	  const int n_kr_coef = kr_coef->nComp();
	  if (do_cpl_advect) 
	    {
	      Box box = (*pcn_cc)[i].box();
	      pctmp.resize(box,1);
	      pctmp.copy((*pcn_cc)[i],box,0,box,0,1);
	      pctmp.plus((*pcnp1_cc)[i],box,0,0,1);
	      pctmp.mult(0.5);
	      godunov->AdvectStateCpl(grids[i], dx, dt, 
				      area[0][i], u_macG[0][i], flux[0], kpedge[0][i], lambda[0][i],
				      area[1][i], u_macG[1][i], flux[1], kpedge[1][i], lambda[1][i],
#if (BL_SPACEDIM == 3)                        
				      area[2][i], u_macG[2][i], flux[2], kpedge[2][i], lambda[2][i],
#endif
				      S_fpi(), S_new[i], (*source)[S_fpi],
				      (*divu_fp)[i] , state_ind,
				      (*aofs)[i]    , state_ind,
				      (*rock_phi)[i], (*kappa)[i],  pctmp,
				      (*lambda_cc)[i],(*dlambda_cc)[i], 
				      (*kr_coef)[i],n_kr_coef,
				      use_conserv_diff,
				      state_ind,state_bc.dataPtr(),volume[i],nscal);
	    }
	  else
	    godunov->AdvectStateRmn(grids[i], dx, dt, 
				    area[0][i], u_macG[0][i], flux[0], kpedge[0][i],
				    area[1][i], u_macG[1][i], flux[1], kpedge[1][i],
#if (BL_SPACEDIM == 3)                        
				    area[2][i], u_macG[2][i], flux[2], kpedge[2][i],
#endif
				    S_fpi(),S_new[i],(*source)[S_fpi],
				    (*divu_fp)[i] , state_ind,
				    (*aofs)[i]    , state_ind,
				    (*rock_phi)[i], (*kappa)[i], 
				    (*lambda_cc)[i],(*dlambda_cc)[i], 
				    (*kr_coef)[i],n_kr_coef,
				    use_conserv_diff,
				    state_ind,state_bc.dataPtr(),volume[i],nscal);
	
	}

      //
      // Set aofs of components in solid phase to zero.
      //
      if ((model == PM_SINGLE_PHASE_SOLID) && (nphases > 1))
	{
	  for (int ii=0; ii<ncomps; ii++) 
	    {
	      if (solid.compare(pNames[pType[ii]]) == 0)
		(*aofs)[i].setVal(0.,ii);	      
	    }
	}
      
      if (reflux_on_this_call)
        {
	  if (do_reflux)
	    {
	      if (level < parent->finestLevel())
		{
		  for (int d = 0; d < BL_SPACEDIM; d++)
		    fluxes[d][i].copy(flux[d]);
		}
	      
	      if (level > 0)
		{
		  for (int d = 0; d < BL_SPACEDIM; d++)
		    advflux_reg->FineAdd(flux[d],d,i,0,state_ind,nscal,dt);
		}
	    }
        }
      
      //
      // Allocate the eigenvalues into scalar array.
      //
      
      if (model == PM_TWO_PHASE)
	{
	  S_new[i].setVal(0.0,ncomps+ntracers);
	  S_new[i].setVal(0.0,ncomps+ntracers+1);
	  godunov->Getdfdn(S_new[i],ncomps+ntracers,ncomps,0,4);
	  godunov->Getdfdn(S_new[i],ncomps+ntracers+1,ncomps,1,4);
	}
    } 
  for (int d = 0; d < BL_SPACEDIM; d++)
    lambda[d].FillBoundary();

  //MultiFab::Copy(S_new,*aofs,0,ncomps+ntracers,1,0);
  //MultiFab::Copy(S_new,visc_terms,0,ncomps+ntracers+1,1,0);

  delete divu_fp;
  
  if (do_reflux && level < parent->finestLevel() && reflux_on_this_call)
    {
      for (int d = 0; d < BL_SPACEDIM; d++)
	getAdvFluxReg(level+1).CrseInit(fluxes[d],d,0,0,nscal,-dt);
    }
}

void
PorousMedia::scalar_update (Real      dt,
                            int       first_scalar,
                            int       num_comp,
			    int       corrector,
			    MultiFab* u_mac)
{
  BL_ASSERT(model != PM_RICHARDS); // Use richards_scalar_update instead

  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::scalar_update()");
  if (verbose > 3 && ParallelDescriptor::IOProcessor())
    std::cout << "... update scalars\n";

  int last_scalar = num_comp-1;

  scalar_advection_update(dt, first_scalar, last_scalar, corrector);
  if (do_any_diffuse) 
    scalar_diffusion_update(dt, first_scalar, last_scalar, corrector);
  if (have_capillary)
    {
      if (!do_cpl_advect)
	scalar_capillary_update(dt, corrector, u_mac);
      else
	{
	  if (do_cpl_advect == 2)
	    {
	      Real pcTime = state[State_Type].curTime();
	      calcCapillary(pcTime);
	    }
	  else
	    diff_capillary_update(dt, corrector, u_mac);
	}
    }

  if (idx_dominant > -1)
    scalar_adjust_constraint(first_scalar,last_scalar);
}

void
PorousMedia::scalar_advection_update (Real dt,
                                      int  first_scalar,
                                      int  last_scalar,
				      int  corrector)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::scalar_advection_update()");

  MultiFab&  S_old    = get_old_data(State_Type);
  MultiFab&  S_new    = get_new_data(State_Type);
  MultiFab&  Aofs     = *aofs;
  MultiFab&  Rockphi  = *rock_phi;
    
  // Compute inviscid estimate of scalars.
  // component first_scalar -> last_scalar: N
  Real pcTime = state[State_Type].curTime();
  int nscal = last_scalar - first_scalar + 1;

  int nGrowF = 0;
  getForce(*source,nGrowF,0,nscal,pcTime);
  for (MFIter mfi(S_new); mfi.isValid(); ++mfi) {
    const int i = mfi.index();
    godunov->Add_aofs_tf(S_old[i],S_new[i],first_scalar,nscal,
			 Aofs[i],first_scalar,(*source)[mfi],0,Rockphi[i],grids[i],dt);
  }

  FillStateBndry(pcTime,State_Type,first_scalar,nscal);
  S_new.FillBoundary();
  if (idx_dominant > -1 && last_scalar < ncomps)
    scalar_adjust_constraint(first_scalar,last_scalar);

  //
  // Write out the min and max of each component of the new state.
  //
  if (corrector || verbose > 3) check_minmax();
}

void
PorousMedia::scalar_diffusion_update (Real dt,
				      int first_scalar,
				      int last_scalar,
				      int corrector)

{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::scalar_diffusion_update()");

  if (verbose > 2 && ParallelDescriptor::IOProcessor())
    std::cout << "... diffuse scalars\n";

  BL_ASSERT(model == PM_SINGLE_PHASE || model == PM_SINGLE_PHASE_SOLID);

  const Real strt_time = ParallelDescriptor::second();

  // Build single component edge-centered array of MultiFabs for fluxes
  MultiFab** fluxSCn;
  MultiFab** fluxSCnp1;

  diffusion->allocFluxBoxesLevel(fluxSCn,  0,1);
  diffusion->allocFluxBoxesLevel(fluxSCnp1,0,1);


  MultiFab* rho;
  MultiFab& S_new = get_new_data(State_Type);
  rho = new MultiFab(grids,1,1);
  MultiFab::Copy(*rho,S_new,0,0,1,1);

  for (int kk = 1; kk<ncomps; kk++)
    {
      if (solid.compare(pNames[pType[kk]]) != 0) 
	MultiFab::Add(*rho,S_new,kk,0,1,1);
    }

  diffusion->set_rho(rho);

  for (int kk = first_scalar; kk <= last_scalar; kk++)
    {
      if (is_diffusive[kk])
        {
	  MultiFab*  delta_rhs   = 0;
	  MultiFab*  alpha       = 0;
	  MultiFab** cmp_diffn   = 0;
	  MultiFab** cmp_diffnp1 = 0;

	  alpha     = new MultiFab(grids, 1, 1);
	  MultiFab::Copy(*alpha,*rock_phi,0,0,1,alpha->nGrow());

	  if (variable_scal_diff)
            {
	      Real diffTime = state[State_Type].prevTime();
	      diffusion->allocFluxBoxesLevel(cmp_diffn, 0, 1);
	      getDiffusivity(cmp_diffn, diffTime, kk, 0, 1);

	      diffTime = state[State_Type].curTime();
	      diffusion->allocFluxBoxesLevel(cmp_diffnp1, 0, 1);
	      getDiffusivity(cmp_diffnp1, diffTime, kk, 0, 1);
            }
	    
	  diffusion->diffuse_scalar(dt,kk,be_cn_theta,
				    fluxSCn,fluxSCnp1,0,delta_rhs,
				    alpha,cmp_diffn,cmp_diffnp1);

	  if (variable_scal_diff)
            {
	      diffusion->removeFluxBoxesLevel(cmp_diffn);
	      diffusion->removeFluxBoxesLevel(cmp_diffnp1);
            }
	    
	  delete delta_rhs;
	  delete alpha;

	  //
	  // Increment the viscous flux registers
	  //
	  if (do_reflux && corrector)
            {
	      FArrayBox fluxtot;

	      for (int d = 0; d < BL_SPACEDIM; d++)
                {
		  MultiFab fluxes;

		  if (level < parent->finestLevel())
		    fluxes.define((*fluxSCn[d]).boxArray(), 1, 0, Fab_allocate);

		  for (MFIter fmfi(*fluxSCn[d]); fmfi.isValid(); ++fmfi)
                    {
		      const Box& ebox = (*fluxSCn[d])[fmfi].box();

		      fluxtot.resize(ebox,1);
		      fluxtot.copy((*fluxSCn[d])[fmfi],ebox,0,ebox,0,1);
		      fluxtot.plus((*fluxSCnp1[d])[fmfi],ebox,0,0,1);

		      if (level < parent->finestLevel())
			fluxes[fmfi].copy(fluxtot);

		      if (level > 0)
			getViscFluxReg().FineAdd(fluxtot,d,fmfi.index(),0,kk,1,dt);
                    }

		  if (level < parent->finestLevel())
		    getLevel(level+1).getViscFluxReg().CrseInit(fluxes,d,0,kk,1,-dt);
                }
            }
        }
    }

  delete rho;

  diffusion->removeFluxBoxesLevel(fluxSCn);
  diffusion->removeFluxBoxesLevel(fluxSCnp1);
    
  // Make sure values on bc is correct
  Real pcTime = state[State_Type].curTime();
  FillStateBndry(pcTime,State_Type,0,ncomps);

  if (show_selected_runtimes > 0)
  {
    const int IOProc   = ParallelDescriptor::IOProcessorNumber();
    Real      run_time = ParallelDescriptor::second() - strt_time;

    ParallelDescriptor::ReduceRealMax(run_time,IOProc);

    if (ParallelDescriptor::IOProcessor())
        std::cout << "PorousMedia::scalar_diffusion_update(): time: " << run_time << '\n';
  }
    
  // Write out the min and max of each component of the new state
  if (corrector && verbose>3) check_minmax();
    
}

void
PorousMedia::diffuse_adjust_dominant(MultiFab&              Phi_new,
				     int                    sComp,
				     Real                   dt,
				     MultiFab**             fluxn,
				     MultiFab**             fluxnp1)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::diffuse_adjust_dominant()");

  FArrayBox update;
  FArrayBox tmpfab;
  int nscal = 1;
  for (MFIter mfi(Phi_new); mfi.isValid(); ++mfi)
    {
      int iGrid = mfi.index();

      const Box& box = mfi.validbox();
      const int* lo    = mfi.validbox().loVect();
      const int* hi    = mfi.validbox().hiVect();

      update.resize(box,1);
      tmpfab.resize(box,1);
      tmpfab.setVal(0.);

      const int* p_lo  = (*rock_phi)[mfi].loVect();
      const int* p_hi  = (*rock_phi)[mfi].hiVect();
      const Real* pdat = (*rock_phi)[mfi].dataPtr();


      FORT_RECOMP_UPDATE(lo, hi,
			 update.dataPtr(),
			 ARLIM(update.loVect()),ARLIM(update.hiVect()),
			 pdat, ARLIM(p_lo), ARLIM(p_hi),
			 (*fluxn[0])[iGrid].dataPtr(),
			 ARLIM((*fluxn[0])[iGrid].loVect()),
			 ARLIM((*fluxn[0])[iGrid].hiVect()),
			 (*fluxn[1])[iGrid].dataPtr(),
			 ARLIM((*fluxn[1])[iGrid].loVect()),
			 ARLIM((*fluxn[1])[iGrid].hiVect()),
#if BL_SPACEDIM == 3
			 (*fluxn[2])[iGrid].dataPtr(),
			 ARLIM((*fluxn[2])[iGrid].loVect()),
			 ARLIM((*fluxn[2])[iGrid].hiVect()),
#endif
			 volume[iGrid].dataPtr(),
			 ARLIM(volume[iGrid].loVect()),ARLIM(volume[iGrid].hiVect()),
			 &nscal);

      update.mult(dt,box,0,1);
      tmpfab.plus(update,box,0,0,1);

      if (fluxnp1 != 0) 
	{
	  FORT_RECOMP_UPDATE(lo,hi,
			     update.dataPtr(),
			     ARLIM(update.loVect()),ARLIM(update.hiVect()),
			     pdat, ARLIM(p_lo), ARLIM(p_hi),
			     (*fluxnp1[0])[iGrid].dataPtr(),
			     ARLIM((*fluxnp1[0])[iGrid].loVect()),
			     ARLIM((*fluxnp1[0])[iGrid].hiVect()),
			     (*fluxnp1[1])[iGrid].dataPtr(),
			     ARLIM((*fluxnp1[1])[iGrid].loVect()),
			     ARLIM((*fluxnp1[1])[iGrid].hiVect()),
#if BL_SPACEDIM == 3
			     (*fluxnp1[2])[iGrid].dataPtr(),
			     ARLIM((*fluxnp1[2])[iGrid].loVect()),
			     ARLIM((*fluxnp1[2])[iGrid].hiVect()),
#endif
			     volume[iGrid].dataPtr(),
			     ARLIM(volume[iGrid].loVect()),ARLIM(volume[iGrid].hiVect()),
			     &nscal);

	  update.mult(dt,box,0,1);
	  tmpfab.plus(update,box,0,0,1);
	}

      tmpfab.plus(Phi_new[iGrid],box,sComp,0,1);
      Phi_new[mfi].copy(tmpfab,box,0,box,sComp,1);
    }

}

#include <Diffuser.H>
#include <TensorOp.H>
#include <MCMultiGrid.H>
#include <MCCGSolver.H>
#include <MFVector.H>
#include <TensorDiffusion_PK.H>
#include <ABecHelper.H>


using Amanzi::MFVector;
using Amanzi::AmanziTransport::getOp;

Real get_scaled_abs_tol (const MultiFab& rhs,
                         Real            reduction)
{
  Real norm_est = 0;
  for (MFIter Rhsmfi(rhs); Rhsmfi.isValid(); ++Rhsmfi) {
    norm_est = std::max(norm_est,rhs[Rhsmfi].norm(Rhsmfi.validbox(),0));
  }
  ParallelDescriptor::ReduceRealMax(norm_est);
  return norm_est * reduction;
}

template <>
void
LinSolver<MFVector,DiffuserOp<MFVector,ABecHelper> >::Solve(MFVector& X, const MFVector& Rhs, Real abs_tol, Real rel_tol)
{
  DiffuserOp<MFVector,ABecHelper>& diffuse_op = DiffuseOp();
  if (diffuse_op.isValid()) {
    MultiGrid mg(diffuse_op.LinOp());
    Real abs_tol_Rhs = get_scaled_abs_tol(Rhs,rel_tol);
    mg.solve(X,Rhs,rel_tol,std::min(abs_tol,abs_tol_Rhs));
  }
}

template <>
void
LinSolver<MFVector,DiffuserOp<MFVector,TensorOp> >::Solve(MFVector& X, const MFVector& Rhs, Real abs_tol, Real rel_tol)
{
  DiffuserOp<MFVector,TensorOp>& diffuse_op = DiffuseOp();
  if (diffuse_op.isValid()) {
    MCMultiGrid mg(diffuse_op.LinOp());
    Real abs_tol_Rhs = get_scaled_abs_tol(Rhs,rel_tol);
    verify_is_clean("X into solve",X,0,1,1,true);
    mg.solve(X,Rhs,rel_tol,std::min(abs_tol,abs_tol_Rhs));
    verify_is_clean("X out of solve",X,0,1,X.nGrow());
  }
}

BCRec defaultBC()
{
  return BCRec(D_DECL(EXT_DIR,EXT_DIR,EXT_DIR),
	       D_DECL(EXT_DIR,EXT_DIR,EXT_DIR));
}

void
PorousMedia::tracer_diffusion (bool reflux_on_this_call,
                               bool use_cached_sat,
                               const MultiFab& F)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::tracer_diffusion()");
  BL_ASSERT(diffuse_tracers);

  if (verbose > 2 && ParallelDescriptor::IOProcessor())
    std::cout << "... diffuse scalars\n";

  const Real strt_time = ParallelDescriptor::second();

  const Real  prev_time    = state[State_Type].prevTime();
  const Real  cur_time     = state[State_Type].curTime();
  const Real  dt           = cur_time - prev_time;

  MultiFab sat_old(grids,ncomps,nGrowHYP);
  MultiFab sat_new(grids,ncomps,nGrowHYP);
  if (use_cached_sat) {
    for (MFIter mfi(sat_old); mfi.isValid(); ++mfi) {
      const Box& gbox = sat_old[mfi].box();
      sat_old[mfi].linInterp((*sat_old_cached)[mfi],gbox,0,(*sat_new_cached)[mfi],gbox,0,
                             t_sat_old_cached,t_sat_new_cached,prev_time,gbox,0,ncomps);
      sat_new[mfi].linInterp((*sat_old_cached)[mfi],gbox,0,(*sat_new_cached)[mfi],gbox,0,
                             t_sat_old_cached,t_sat_new_cached,cur_time,gbox,0,ncomps);
    }
  }
  else {
    int nGrow = 1;
    Real t_sat_old = prev_time;
    Real t_sat_new = cur_time;
    get_fillpatched_rhosat(t_sat_old,sat_old,nGrow);
    get_fillpatched_rhosat(t_sat_new,sat_new,nGrow);
    for (int n=0; n<ncomps; ++n) {
      sat_old.mult(1/density[n],n,1,nGrow);
      sat_new.mult(1/density[n],n,1,nGrow);
    }
  }

  int first_tracer = ncomps;
  
  MultiFab *bn[BL_SPACEDIM], *b1n[BL_SPACEDIM];
  MultiFab *bnp1[BL_SPACEDIM], *b1np1[BL_SPACEDIM];
  PArray<MultiFab> flux(BL_SPACEDIM,PArrayManage);
  for (int d = 0; d < BL_SPACEDIM; d++) {
    const BoxArray eba = BoxArray(grids).surroundingNodes(d);
    flux.set(d, new MultiFab(eba,1,0));
    bn[d] = new MultiFab(eba,1,0);
    bnp1[d] = new MultiFab(eba,1,0);
    if (tensor_tracer_diffusion) {
      b1n[d] = new MultiFab(eba,1,0);
      b1np1[d] = new MultiFab(eba,1,0);
    }
  }
  calcDiffusivity(prev_time,first_tracer,1,&sat_old);
  calcDiffusivity(cur_time,first_tracer,1,&sat_new);
  if (tensor_tracer_diffusion) {
    getTensorDiffusivity(bn,b1n,prev_time);
    getTensorDiffusivity(bnp1,b1np1,cur_time);
  } else {
    getDiffusivity(bn,   prev_time, first_tracer, 0, 1);
    getDiffusivity(bnp1, cur_time,  first_tracer, 0, 1);
  }

  FillStateBndry(prev_time,State_Type,first_tracer,ntracers);
  FillStateBndry(cur_time,State_Type,first_tracer,ntracers);

  MultiFab Sc_old;
  MultiFab Sc_new;
  BoxArray cgrids;
  MultiFab& S_old = get_old_data(State_Type);
  MultiFab& S_new = get_new_data(State_Type);

  if (level > 0) {
    cgrids = BoxArray(grids).coarsen(crse_ratio);
    PorousMedia& pmc = getLevel(level-1);
    int nGrowDiffC = 2; // To accomodate sliding stencil (if max_order==3)
    Sc_old.define(pmc.boxArray(),ntracers,nGrowDiffC,Fab_allocate);
    for (FillPatchIterator fpi(pmc,Sc_old,nGrowDiffC,prev_time,State_Type,
                               first_tracer,ntracers); fpi.isValid(); ++fpi) {
      Sc_old[fpi].copy(fpi(),0,0,ntracers);
    }
    Sc_new.define(pmc.boxArray(),ntracers,nGrowDiffC,Fab_allocate);
    for (FillPatchIterator fpi(pmc,Sc_new,nGrowDiffC,cur_time,State_Type,
                               first_tracer,ntracers); fpi.isValid(); ++fpi) {
      Sc_new[fpi].copy(fpi(),0,0,ntracers);
    }
  }

  int nBndComp = MCLinOp::bcComponentsNeeded(1);
  Array<BCRec> tracer_bc(nBndComp,defaultBC());

  MFVector phi(*rock_phi);
  MFVector sphi_old(sat_old,0,1,1); sphi_old.MULTAY(phi,1);
  MFVector sphi_new(sat_new,0,1,1); sphi_new.MULTAY(phi,1);
  MFVector Volume(volume);

  int Wflag = 2;
  MultiFab* Whalf = 0;
  MultiFab* alpha = 0;
  int op_maxOrder = 3;
  
  Real a_old = 0;
  Real a_new = 1;
  Real b_old = -(1-be_cn_theta_trac)*dt;
  Real b_new = be_cn_theta_trac*dt;

  Diffuser<MFVector,ABecHelper>* scalar_diffuser = 0;
  Diffuser<MFVector,TensorOp>* tensor_diffuser = 0;
  ABecHelper *scalar_linop_old, *scalar_linop_new; scalar_linop_old = scalar_linop_new = 0;
  TensorOp *tensor_linop_old, *tensor_linop_new; tensor_linop_old = tensor_linop_new = 0;

  MFVector old_state(S_old,first_tracer,1,1);
  MFVector new_state(S_new,first_tracer,1,1);
  MFVector Rhs(F,0,1,0);

  for (int n=0; n<ntracers; ++n) {

    verify_is_clean("S_new before tracer diffusion",S_new,first_tracer+n,1,S_new.nGrow());
    verify_is_clean("S_old before tracer diffusion",S_old,first_tracer+n,1,S_old.nGrow());
    tracer_bc[0] = get_desc_lst()[State_Type].getBC(first_tracer+n);

    TensorDiffusionBndry *tbd_old, *tbd_new; tbd_old = tbd_new = 0;
    ViscBndry *vbd_old, *vbd_new; vbd_old = vbd_new = 0;
    if (tensor_tracer_diffusion) {  
      tbd_old = new TensorDiffusionBndry(grids,1,geom);
      tbd_new = new TensorDiffusionBndry(grids,1,geom);
    }
    else {
      vbd_old = new ViscBndry(grids,1,geom);
      vbd_new = new ViscBndry(grids,1,geom);
    }

    if (level == 0) {
      if (tensor_tracer_diffusion) {  
        tbd_old->setBndryValues(S_old,first_tracer+n,0,1,tracer_bc);
        tbd_new->setBndryValues(S_new,first_tracer+n,0,1,tracer_bc);
      } else {
        vbd_old->setBndryValues(S_old,first_tracer+n,0,1,tracer_bc[0]);
        vbd_new->setBndryValues(S_new,first_tracer+n,0,1,tracer_bc[0]);
      }
    } else {
      BndryRegister crse_br(cgrids,0,1,2,1);
      crse_br.copyFrom(Sc_old,Sc_old.nGrow(),n,0,1);
      if (tensor_tracer_diffusion) {  
        tbd_old->setBndryValues(crse_br,0,S_old,first_tracer+n,0,1,crse_ratio[0],tracer_bc);
      } else {
        vbd_old->setBndryValues(crse_br,0,S_old,first_tracer+n,0,1,crse_ratio[0],tracer_bc[0]);
      }

      crse_br.copyFrom(Sc_new,Sc_new.nGrow(),n,0,1);
      if (tensor_tracer_diffusion) {  
        tbd_new->setBndryValues(crse_br,0,S_new,first_tracer+n,0,1,crse_ratio[0],tracer_bc);
      } else {
        vbd_new->setBndryValues(crse_br,0,S_new,first_tracer+n,0,1,crse_ratio[0],tracer_bc[0]);
      }
    }

    if (tensor_tracer_diffusion) {
      BL_ASSERT(tensor_linop_old == 0);
      tensor_linop_old = getOp(a_old,b_old,*tbd_old,0,1,&(sphi_old.multiFab()),0,1,Whalf,0,
                               Wflag,bn,0,1,b1n,0,1,volume,area,alpha,0);
      BL_ASSERT(tensor_linop_new == 0);
      tensor_linop_new = getOp(a_new,b_new,*tbd_new,0,1,&(sphi_new.multiFab()),0,1,Whalf,0,
                               Wflag,bnp1,0,1,b1np1,0,1,volume,area,alpha,0);
      tensor_linop_old->maxOrder(op_maxOrder);
      tensor_linop_new->maxOrder(op_maxOrder);
      BL_ASSERT(tensor_diffuser == 0);
      tensor_diffuser = new Diffuser<MFVector,TensorOp>(tensor_linop_old,tensor_linop_new,Volume,&sphi_old,&sphi_new);
    } else {
      BL_ASSERT(scalar_linop_old == 0);
      scalar_linop_old = getOp(a_old,b_old,*vbd_old,0,&(sphi_old.multiFab()),0,1,Whalf,0,
                               Wflag,bn,0,1,volume,area,alpha,0,1);
      BL_ASSERT(scalar_linop_new == 0);
      scalar_linop_new = getOp(a_new,b_new,*vbd_new,0,&(sphi_new.multiFab()),0,1,Whalf,0,
                               Wflag,bnp1,0,1,volume,area,alpha,0,1);
      scalar_linop_old->maxOrder(op_maxOrder);
      scalar_linop_new->maxOrder(op_maxOrder);
      BL_ASSERT(scalar_diffuser == 0);
      scalar_diffuser = new Diffuser<MFVector,ABecHelper>(scalar_linop_old,scalar_linop_new,Volume,&sphi_old,&sphi_new);
    }

    MultiFab::Copy(old_state,S_old,first_tracer+n,0,1,0);
    MultiFab::Copy(new_state,S_new,first_tracer+n,0,1,0);
    MultiFab::Copy(Rhs,F,n+ncomps,0,1,0);

    if (tensor_tracer_diffusion) {
      tensor_diffuser->Diffuse(&old_state,new_state,Rhs,prev_time,cur_time,visc_abs_tol,visc_tol);
    }
    else {
      scalar_diffuser->Diffuse(&old_state,new_state,Rhs,prev_time,cur_time,visc_abs_tol,visc_tol);
    }

    MultiFab::Copy(S_new,new_state,0,first_tracer+n,1,0);
    verify_is_clean("S_new after tracer diffusion",S_new,first_tracer+n,1,S_new.nGrow());

    if (reflux_on_this_call) {
      if (tensor_tracer_diffusion) {  
        tensor_linop_old->compFlux(D_DECL(flux[0],flux[1],flux[2]),S_old,MCInhomogeneous_BC,first_tracer+n,0,1,0);
      }
      else {
        scalar_linop_old->compFlux(D_DECL(flux[0],flux[1],flux[2]),S_old,LinOp::Inhomogeneous_BC,first_tracer+n,0,1,0);
      }
      for (int d = 0; d < BL_SPACEDIM; d++) {
        flux[d].mult((1-be_cn_theta_trac)/geom.CellSize()[d]);
        if (level > 0) {
          for (MFIter mfi(*bn[d]); mfi.isValid(); ++mfi) {
            getViscFluxReg().FineAdd(flux[d][mfi],d,mfi.index(),0,first_tracer+n,1,dt);
          }
        }
        if (level < parent->finestLevel()) {
          getLevel(level+1).getViscFluxReg().CrseInit(flux[d],d,0,first_tracer+n,1,-dt,FluxRegister::COPY);
        }
      }

      if (tensor_tracer_diffusion) {
        tensor_linop_new->compFlux(D_DECL(flux[0],flux[1],flux[2]),S_new,MCInhomogeneous_BC,first_tracer+n,0,1,0);
      }
      else {
        scalar_linop_new->compFlux(D_DECL(flux[0],flux[1],flux[2]),S_new,LinOp::Inhomogeneous_BC,first_tracer+n,0,1,0);
      }
      for (int d = 0; d < BL_SPACEDIM; d++) {
        flux[d].mult(be_cn_theta_trac/geom.CellSize()[d]);
        if (level > 0) {
          for (MFIter mfi(*bnp1[d]); mfi.isValid(); ++mfi) {
            getViscFluxReg().FineAdd(flux[d][mfi],d,mfi.index(),0,first_tracer+n,1,dt);
          }
        }
        if (level < parent->finestLevel()) {
          getLevel(level+1).getViscFluxReg().CrseInit(flux[d],d,0,first_tracer+n,1,-dt,FluxRegister::ADD);
        }
      }
    }
    
    delete tensor_diffuser; tensor_diffuser = 0;
    delete scalar_diffuser; scalar_diffuser = 0;
    delete scalar_linop_old; scalar_linop_old = 0;
    delete scalar_linop_new; scalar_linop_new = 0;
    delete tensor_linop_old; tensor_linop_old = 0;
    delete tensor_linop_new; tensor_linop_new = 0;

    delete tbd_old; tbd_old = 0;
    delete tbd_new; tbd_new = 0;
    delete vbd_old; vbd_old = 0;
    delete vbd_new; vbd_new = 0;
  }
  for (int d = 0; d < BL_SPACEDIM; d++) {
    delete bn[d]; bn[d] = 0;
    delete bnp1[d]; bnp1[d] = 0;
    if (tensor_tracer_diffusion) {
      delete b1n[d]; b1n[d] = 0;
      delete b1np1[d]; b1np1[d] = 0;
    }
  }

#ifndef NDEBUG
  FillStateBndry(prev_time,State_Type,first_tracer,ntracers);
  FillStateBndry(cur_time,State_Type,first_tracer,ntracers);
#endif

  if (show_selected_runtimes > 0)
  {
    const int IOProc   = ParallelDescriptor::IOProcessorNumber();
    Real      run_time = ParallelDescriptor::second() - strt_time;

    ParallelDescriptor::ReduceRealMax(run_time,IOProc);

    if (ParallelDescriptor::IOProcessor())
        std::cout << "PorousMedia::tracer_diffusion(): time: " << run_time << '\n';
  }
}

//
// This routine advects the scalars
//
void
PorousMedia::tracer_advection (MultiFab* u_macG,
                               bool reflux_on_this_call,
                               bool use_cached_sat,
                               MultiFab* F)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::tracer_advection()");

  BL_ASSERT(advect_tracers > 0);
  BL_ASSERT(ntracers > 0);

  const Array<int>& idx_total = group_map["Total"];
  const int   first_tracer = ncomps;

  if (idx_total.size()) {

    const Real* dx           = geom.CellSize();
    const Real  prev_time    = state[State_Type].prevTime();
    const Real  cur_time     = state[State_Type].curTime();
    const Real  dt           = cur_time - prev_time;

    FArrayBox flux[BL_SPACEDIM];
    Array<int> state_bc;
    PArray<MultiFab> fluxes;
    int Flng = 0;
    if (reflux_on_this_call && do_reflux)
    {
      fluxes.resize(BL_SPACEDIM,PArrayManage);
      for (int i = 0; i < BL_SPACEDIM; i++)
      {
        BoxArray ba = grids;
        ba.surroundingNodes(i);
        fluxes.set(i, new MultiFab(ba, ntracers, Flng));
      }
    }

    int NGROWHYP = Advection::nGrowHyp();
    //int NGROWHYP = nGrowHyp;

    MultiFab sat_old(grids,ncomps,NGROWHYP);
    MultiFab sat_new(grids,ncomps,NGROWHYP);

    if (use_cached_sat) {
      for (MFIter mfi(sat_old); mfi.isValid(); ++mfi) {
        const Box& gbox = sat_old[mfi].box();
        sat_old[mfi].linInterp((*sat_old_cached)[mfi],gbox,0,(*sat_new_cached)[mfi],gbox,0,
                               t_sat_old_cached,t_sat_new_cached,prev_time,gbox,0,ncomps);
        sat_new[mfi].linInterp((*sat_old_cached)[mfi],gbox,0,(*sat_new_cached)[mfi],gbox,0,
                               t_sat_old_cached,t_sat_new_cached,cur_time,gbox,0,ncomps);
      }
    }
    else {
      Real t_sat_old = prev_time;
      Real t_sat_new = cur_time;
      get_fillpatched_rhosat(t_sat_old,sat_old,NGROWHYP);
      get_fillpatched_rhosat(t_sat_new,sat_new,NGROWHYP);
      for (int n=0; n<ncomps; ++n) {
	sat_old.mult(1/density[n],n,1,NGROWHYP);
	sat_new.mult(1/density[n],n,1,NGROWHYP);
      }
    }

    int Aidx = first_tracer;
    int Cidx, Sidx, SRCidx, Didx, DUidx;
    Cidx = Sidx = Didx = DUidx = 0;
    int use_conserv_diff = (advectionType[first_tracer] == Conservative);

#ifndef NDEBUG
    for (FillPatchIterator C_new_fpi(*this,get_old_data(State_Type),NGROWHYP,
                                     prev_time,State_Type,first_tracer,ntracers);
         C_new_fpi.isValid();  ++C_new_fpi) {
      if (C_new_fpi().contains_nan()) {
        BoxLib::Abort("C_new has nans");
      }
    }
#endif


    PArray<FArrayBox> U(BL_SPACEDIM, PArrayNoManage), Area(BL_SPACEDIM, PArrayNoManage);
    PArray<FArrayBox> Flux(BL_SPACEDIM, PArrayNoManage);
    int Ung = 1; // u_macG boxes are equivalent to valid region grown by 1
    int Uidx = 0;
    int FLidx = 0;
    int Ang = 0;
    int Sng = 1;
    bool is_conservative = false;
    BL_ASSERT(sat_old.nGrow()==sat_new.nGrow());

    FArrayBox SRCext, divu;
    for (FillPatchIterator C_old_fpi(*this,get_old_data(State_Type),NGROWHYP,
                                     prev_time,State_Type,first_tracer,ntracers),
           C_new_fpi(*this,get_new_data(State_Type),nGrowHYP,
                     cur_time,State_Type,first_tracer,ntracers);
         C_old_fpi.isValid() && C_new_fpi.isValid();  ++C_old_fpi,++C_new_fpi)
    {
      const int i = C_old_fpi.index();
      const Box& box = grids[i];

      const Box gbox = Box(box).grow(1);

      FArrayBox* SrcPtr = 0;
      if (F==0) {
        SRCext.resize(gbox,ntracers);
	SRCext.setVal(0);
	SrcPtr = &SRCext;
        SRCidx = 0;
      }
      else {
	SrcPtr = &((*F)[C_old_fpi]);
        SRCidx = ncomps; // If passed in, this source term will contain component sources as well
      }
      state_bc = getBCArray(State_Type,i,ncomps,ntracers);

      // FIXME: Need 3D version of BDS to replace old Godunov integrator
#if BL_SPACEDIM==2
      for (int d=0; d<BL_SPACEDIM; ++d) {
        U.clear(d); U.set(d, &(u_macG[d][i]));  BL_ASSERT(U[d].box() == BoxLib::grow(BoxLib::surroundingNodes(box,d),Ung));
        Flux.clear(d); Flux.set(d, &(fluxes[d][i]));
        Area.clear(d); Area.set(d, &(area[d][i]));
      }

      Advection::FluxDivergence(C_old_fpi(), C_new_fpi(), Cidx, NGROWHYP,
                                sat_old[C_old_fpi], sat_new[C_old_fpi], Sidx, sat_old.nGrow(),
                                U, Uidx,Ung, (*aofs)[i], Aidx, Ang, Flux, FLidx, Flng, 
                                *SrcPtr, SRCidx, Sng, *SrcPtr, 0, Sng, volume[i], Area, (*rock_phi)[i],
                                box, dx, dt, state_bc.dataPtr(), ntracers, is_conservative);

      Advection::AdvUpdate(C_old_fpi(),C_new_fpi(),Cidx,NGROWHYP,
                           sat_old[C_old_fpi], sat_new[C_old_fpi], Sidx, sat_old.nGrow(),
                           (*aofs)[i], Aidx, Ang, Flux, FLidx, Flng,  (*rock_phi)[i], rock_phi->nGrow(),
                           box, dt, ntracers);

      // Copy new tracer concentrations into "new" state
      get_new_data(State_Type)[i].copy(C_new_fpi(),Cidx,first_tracer,ntracers);

      if (reflux_on_this_call) {
	if (level > 0) {
	  for (int d = 0; d < BL_SPACEDIM; d++) {
	    advflux_reg->FineAdd(fluxes[d][i],d,i,0,first_tracer,ntracers,dt); // FINE += Fadv
	  }
	}
      }
#else
      BL_ASSERT(!use_conserv_diff);
      divu.resize(gbox,1); divu.setVal(0);

      godunov->Setup_tracer(grids[i], D_DECL(flux[0],flux[1],flux[2]), ntracers);

      state_bc = getBCArray(State_Type,i,ncomps,ntracers);
      BL_ASSERT(aofs->size()>i);
      BL_ASSERT(rock_phi->size()>i);
      BL_ASSERT(area[0].size()>i);
      BL_ASSERT(area[1].size()>i);

      godunov->AdvectTracer(grids[i], dx, dt, 
			    area[0][i], u_macG[0][i], flux[0], 
			    area[1][i], u_macG[1][i], flux[1], 
#if (BL_SPACEDIM == 3)                        
			    area[2][i], u_macG[2][i], flux[2], 
#endif
			    C_old_fpi(), C_new_fpi(), Cidx, 
                            sat_old[C_old_fpi], sat_new[C_new_fpi], Sidx, 
                            *SrcPtr, SRCidx, divu, DUidx,
			    (*aofs)[i], Aidx, (*rock_phi)[i], use_conserv_diff,
			    state_bc.dataPtr(), volume[i], ntracers);

      // Compute C_new such that:
      //
      //    (S_new.C_new.phi-S_old.C_old.phi)/dt + A = SRCext
      //         where A = (1/Vol).Div(Fadv.Area)
      //
      godunov->Add_aofs_tracer(C_old_fpi(),C_new_fpi(), Cidx, ntracers,
                               sat_old[i], sat_new[i], Sidx, ncomps,
                               (*aofs)[i], Aidx, *SrcPtr, SRCidx, (*rock_phi)[i],
                               grids[i], idx_total, dt); 

      // Copy new tracer concentrations into "new" state
      get_new_data(State_Type)[i].copy(C_new_fpi(),Cidx,first_tracer,ntracers);

      if (fluxes.size()>0) {
        for (int d = 0; d < BL_SPACEDIM; d++) {
          fluxes[d][i].copy(flux[d]); // fluxes = Fadv
        }
	if (level > 0) {
	  for (int d = 0; d < BL_SPACEDIM; d++) {
	    advflux_reg->FineAdd(fluxes[d][i],d,i,0,first_tracer,ntracers,dt); // FINE += Fadv
	  }
	}
      }
#endif
    }
    if (fluxes.size() > 0 && level < parent->finestLevel()) {
      for (int d = 0; d < BL_SPACEDIM; d++) {
        getAdvFluxReg(level+1).CrseInit(fluxes[d],d,0,first_tracer,ntracers,-dt); // CRSE = Fadv
      }
    }
  } else {
    MultiFab::Copy(get_new_data(State_Type),get_old_data(State_Type),first_tracer,first_tracer,ntracers,0);
  }


  get_new_data(State_Type).FillBoundary(first_tracer,ntracers);

  //
  // Write out the min and max of each component of the new state.
  //
  if (verbose > 3) {
    const int last_tracer = first_tracer + ntracers - 1;
    check_minmax(first_tracer,last_tracer);
  }
}

void 
PorousMedia::SetTracerDiffusionBndryData(ViscBndry& bndry,
					 Real       time)
{
  int state_type = State_Type;
  int nGrowDiff = 1;
  int first_tracer = ncomps;
  MultiFab S(grids,ntracers,nGrowDiff);

  for (FillPatchIterator fpi(*this,S,nGrowDiff,time,state_type,
			     first_tracer,ntracers);fpi.isValid(); ++fpi) {
    S[fpi].copy(fpi(),0,0,ntracers);
  }

  const BCRec& tracer_bc = get_desc_lst()[State_Type].getBC(first_tracer);
  if (level == 0) {
    bndry.setBndryValues(S,0,0,ntracers,tracer_bc);
  } else {
    BoxArray cgrids = BoxArray(grids).coarsen(crse_ratio);
    PorousMedia& pmc = getLevel(level-1);
    int nGrowDiffC = 2; // To accomodate sliding stencil (if max_order==3)
    MultiFab Sc(pmc.boxArray(),ntracers,nGrowDiffC);
    for (FillPatchIterator fpi(pmc,Sc,nGrowDiffC,time,state_type,
			       first_tracer,ntracers);fpi.isValid(); ++fpi) {
      Sc[fpi].copy(fpi(),0,0,ntracers);
    }
    BndryRegister crse_br(cgrids,0,1,2,ntracers);
    crse_br.copyFrom(Sc,nGrowDiff,0,0,ntracers);
    bndry.setBndryValues(crse_br,0,S,0,0,ntracers,crse_ratio,tracer_bc);
  }
}

void
PorousMedia::getTracerViscTerms(MultiFab&  D,
				Real       time,
				int        nGrow,
				PArray<MultiFab>& Dflux)
{
  int first_tracer = ncomps;
  
  TensorDiffusionBndry bd(grids,1,geom);
  MultiFab *beta[BL_SPACEDIM], *beta1[BL_SPACEDIM];
  for (int d = 0; d < BL_SPACEDIM; d++) {
    const BoxArray eba = BoxArray(grids).surroundingNodes(d);
    beta[d] = new MultiFab(eba,1,0);
    beta1[d] = new MultiFab(eba,1,0);
  }
  
  calcDiffusivity(time,first_tracer,1);
  if (tensor_tracer_diffusion) {
    getTensorDiffusivity(beta,beta1,time);
  } else {
    getDiffusivity(beta,time,first_tracer,0,1);
  }
  FillStateBndry(time,State_Type,first_tracer,ntracers);

  MultiFab Sc;
  BoxArray cgrids;
  MultiFab& S = get_data(State_Type,time);
  if (level > 0) {
    cgrids = BoxArray(grids).coarsen(crse_ratio);
    PorousMedia& pmc = getLevel(level-1);
    int nGrowDiffC = 2; // To accomodate sliding stencil (if max_order==3)
    Sc.define(pmc.boxArray(),ntracers,nGrowDiffC,Fab_allocate);
    for (FillPatchIterator fpi(pmc,Sc,nGrowDiffC,time,State_Type,
                               first_tracer,ntracers); fpi.isValid(); ++fpi) {
      Sc[fpi].copy(fpi(),0,0,ntracers);
    }
  }

  int nBndComp = MCLinOp::bcComponentsNeeded(1);
  Array<BCRec> tracer_bc(nBndComp,defaultBC());
  
  int Wflag = 2;
  MultiFab* Whalf = 0;
  MultiFab* alpha = 0;
  int op_maxOrder = 3;
  
  Real a = 0;
  Real b = -1;

  Diffuser<MFVector,ABecHelper>* scalar_diffuser = 0;
  Diffuser<MFVector,TensorOp>* tensor_diffuser = 0;
  ABecHelper *scalar_linop; scalar_linop = 0;
  TensorOp *tensor_linop; tensor_linop = 0;

  MultiFab volInv(grids,1,0);
  MultiFab::Copy(volInv,volume,0,0,1,0);
  volInv.invert(1,0);

  for (int n=0; n<ntracers; ++n) {
    tracer_bc[0] = get_desc_lst()[State_Type].getBC(first_tracer+n);

    TensorDiffusionBndry *tbd = 0;
    ViscBndry *vbd = 0;
    if (tensor_tracer_diffusion) {  
      tbd = new TensorDiffusionBndry(grids,1,geom);
    } else {
      vbd = new ViscBndry(grids,1,geom);
    }

    if (level == 0) {
      if (tensor_tracer_diffusion) {  
        tbd->setBndryValues(S,first_tracer+n,0,1,tracer_bc);
      } else {
        vbd->setBndryValues(S,first_tracer+n,0,1,tracer_bc[0]);
      }
    } else {
      BndryRegister crse_br(cgrids,0,1,2,1);
      crse_br.copyFrom(Sc,Sc.nGrow(),n,0,1);
      if (tensor_tracer_diffusion) {  
        tbd->setBndryValues(crse_br,0,S,first_tracer+n,0,1,crse_ratio[0],tracer_bc);
      } else {
        vbd->setBndryValues(crse_br,0,S,first_tracer+n,0,1,crse_ratio[0],tracer_bc[0]);
      }
    }

    if (tensor_tracer_diffusion) {  
      tensor_linop = getOp(a,b,*tbd,0,1,0,0,0,Whalf,0,Wflag,beta,0,1,beta1,0,1,volume,area,alpha,0);
      tensor_linop->maxOrder(op_maxOrder);
      tensor_linop->apply(D,S,0,MCInhomogeneous_BC,true,first_tracer+n,n,1,0);
    } else {
      scalar_linop = getOp(a,b,*vbd,0,0,0,0,Whalf,0,Wflag,beta,0,1,volume,area,alpha,0,1);
      scalar_linop->maxOrder(op_maxOrder);
      scalar_linop->apply(D,S,0,LinOp::Inhomogeneous_BC,true,first_tracer+n,n,1,0);
    }
    MultiFab::Multiply(D,volInv,0,n,1,0);
    delete scalar_linop;
    delete tensor_linop;
  }
  for (int d = 0; d < BL_SPACEDIM; d++) {
    delete beta[d];
    if (tensor_tracer_diffusion) {
      delete beta1[d];
    }
  }
  //
  // Ensure consistent grow cells
  //    
  if (nGrow > 0) {
    for (MFIter mfi(D); mfi.isValid(); ++mfi) {
      FArrayBox& vt  = D[mfi];
      const Box& box = mfi.validbox();
      FORT_VISCEXTRAP(vt.dataPtr(),ARLIM(vt.loVect()),ARLIM(vt.hiVect()),
		      box.loVect(),box.hiVect(),&ntracers);
    }
    bool local = false;
    bool do_corners = true;
    bool cross = false;
    D.FillBoundary(0,ntracers,local,cross);
    geom.FillPeriodicBoundary(D,0,ntracers,do_corners,local);
  }
#ifndef NDEBUG
  if (D.contains_nan(0,ntracers,D.nGrow())) {
    BoxLib::Abort("D has nans");
  }
#endif

  for (int d=0; d<BL_SPACEDIM; ++d) {
    Dflux[d].setVal(0);
  }
}

DistributionMapping
PorousMedia::getFuncCountDM (const BoxArray& bxba, int ngrow)
{
  //
  // Sometimes "mf" is the valid region of the State.
  // Sometimes it's the region covered by AuxBoundaryData.
  // When ngrow>0 were doing AuxBoundaryData with nGrow()==ngrow.
  // Taken from LMC/HeatTransfer.cpp
  //
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::getFuncCountDM()");

  DistributionMapping rr;
  rr.RoundRobinProcessorMap(bxba.size(),ParallelDescriptor::NProcs());

  MultiFab fctmpnew;
  fctmpnew.define(bxba, 1, 0, rr, Fab_allocate);
  fctmpnew.setVal(1);

  if (ngrow == 0)
    {
      //
      // Working on valid region of state.
      //
      fctmpnew.copy(get_new_data(FuncCount_Type));  // Parallel copy.
    }
  else
    {
      //
      // Can't directly use a parallel copy from FuncCount_Type to fctmpnew.
      //
      MultiFab& FC = get_new_data(FuncCount_Type);

      BoxArray ba = FC.boxArray();
      ba.grow(ngrow);
      MultiFab grownFC(ba, 1, 0);
      grownFC.setVal(1);
                
      for (MFIter mfi(FC); mfi.isValid(); ++mfi)
	grownFC[mfi].copy(FC[mfi]);

      fctmpnew.copy(grownFC);  // Parallel copy.
    }

  int count = 0;
  Array<long> vwrk(bxba.size());
  for (MFIter mfi(fctmpnew); mfi.isValid(); ++mfi)
      vwrk[count++] = static_cast<long>(std::max(1.,fctmpnew[mfi].sum(0)));

  fctmpnew.clear();

#if BL_USE_MPI
  const int IOProc = ParallelDescriptor::IOProcessorNumber();

  Array<int> nmtags(ParallelDescriptor::NProcs(),0);
  Array<int> offset(ParallelDescriptor::NProcs(),0);

  for (int i = 0; i < vwrk.size(); i++)
    nmtags[rr.ProcessorMap()[i]]++;

  BL_ASSERT(nmtags[ParallelDescriptor::MyProc()] == count);

  for (int i = 1; i < offset.size(); i++)
    offset[i] = offset[i-1] + nmtags[i-1];

  Array<long> vwrktmp = vwrk;

  MPI_Gatherv(vwrk.dataPtr(),
	      count,
	      ParallelDescriptor::Mpi_typemap<long>::type(),
	      vwrktmp.dataPtr(),
	      nmtags.dataPtr(),
	      offset.dataPtr(),
	      ParallelDescriptor::Mpi_typemap<long>::type(),
	      IOProc,
	      ParallelDescriptor::Communicator());

  if (ParallelDescriptor::IOProcessor())
    {
      //
      // We must now assemble vwrk in the proper order.
      //
      std::vector< std::vector<int> > table(ParallelDescriptor::NProcs());

      for (int i = 0; i < vwrk.size(); i++)
	table[rr.ProcessorMap()[i]].push_back(i);

      int idx = 0;
      for (int i = 0; i < table.size(); i++)
	for (int j = 0; j < table[i].size(); j++)
	  vwrk[table[i][j]] = vwrktmp[idx++]; 
    }
  //
  // Send the properly-ordered vwrk to all processors.
  //
  ParallelDescriptor::Bcast(vwrk.dataPtr(), vwrk.size(), IOProc);
#endif

  DistributionMapping res;
  //
  // This call doesn't invoke the MinimizeCommCosts() stuff.
  //
  res.KnapSackProcessorMap(vwrk,ParallelDescriptor::NProcs());

  return res;
}

static
void
TagUnusedGrowCells(MultiFab&    state, 
		   int          state_idx,
		   const BCRec& bc,
		   PorousMedia& pm, 
		   int          ngrow, 
		   Real         tagVal,
		   int          comp,
		   int          nComp)
{
  // Don't use any grow cells that are not f-f
  state.setBndry(tagVal,comp,nComp);
  state.FillBoundary(comp,nComp);
  pm.Geom().FillPeriodicBoundary(state,comp,nComp);
}

static
BoxArray
ChemistryGrids (const MultiFab& state,
                const Amr*      parent,
                int             level,
                int             ngrow)
{
    //
    // Let's chop the grids up a bit.
    //
    // We want to try and level out the chemistry work.
    //
    const int NProcs = ParallelDescriptor::NProcs();

    BoxArray ba = state.boxArray();

    if (ngrow>0) {
        BoxList bl = BoxList(ba).accrete(ngrow);
        ba = BoxArray(BoxLib::removeOverlap(bl));
    }

    bool done = false;

    for (int cnt = 1; !done; cnt *= 2)
    {
        const int ChunkSize = parent->maxGridSize(level)/cnt;

        if (ChunkSize < 16)
            //
            // Don't let grids get too small. 
            //
            break;

        IntVect chunk(D_DECL(ChunkSize,ChunkSize,ChunkSize));

        for (int j = 0; j < BL_SPACEDIM && ba.size() < 3*NProcs; j++)
        {
            chunk[j] /= 2;

            ba.maxSize(chunk);

            if (ba.size() >= 3*NProcs) done = true;
        }
    }

    return ba;
}

//
// ODE-solve for chemistry: cell-by-cell
//
bool
PorousMedia::advance_chemistry (Real time,
				Real dt,
				int  ngrow)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::advance_chemistry()");
  const Real strt_time = ParallelDescriptor::second();

  bool chem_ok = true;

  //
  // Mark the states to be affected, prepare the data structures
  //
  std::set<int> types_advanced;
  types_advanced.insert(State_Type);
  types_advanced.insert(Press_Type);
  types_advanced.insert(FuncCount_Type);
  types_advanced.insert(Aux_Chem_Type);
  for (std::set<int>::const_iterator it=types_advanced.begin(), End=types_advanced.end(); 
       it!=End; ++it) {      
    state[*it].setNewTimeLevel(time);
    state[*it].allocOldData();
    state[*it].swapTimeLevels(dt); // Set old_time=new_time, swap state data ptrs
  }

  //
  // Get some refs, initialize result with old state
  //
  MultiFab& S_old = get_old_data(State_Type);
  MultiFab& P_old = get_old_data(Press_Type);
  MultiFab& Aux_old = get_old_data(Aux_Chem_Type);
  MultiFab& Fcnt_old = get_old_data(FuncCount_Type);

  MultiFab& S_new = get_new_data(State_Type);
  MultiFab& P_new = get_new_data(Press_Type);
  MultiFab& Aux_new = get_new_data(Aux_Chem_Type);
  MultiFab& Fcnt_new = get_new_data(FuncCount_Type);

  MultiFab::Copy(S_new,S_old,0,0,ncomps,S_new.nGrow());
  MultiFab::Copy(P_new,P_old,0,0,ncomps,P_new.nGrow());
  MultiFab::Copy(Aux_new,Aux_old,0,0,Aux_new.nComp(),Aux_new.nGrow());
  BL_ASSERT(S_old.nComp() >= ncomps+ntracers);


  //
  // Copy state into redistributed multifab for better load balance
  //
  int          ngrow_tmp = 0;
  BoxArray            ba = ChemistryGrids(S_old, parent, level, ngrow_tmp);
  MultiFab stateTemp, pressTemp, phiTemp, volTemp, fcnCntTemp, auxTemp;

  if (0) {
    DistributionMapping dm = getFuncCountDM(ba,ngrow_tmp);
    stateTemp.define(ba, S_old.nComp(), 0, dm, Fab_allocate);
    pressTemp.define(ba, P_old.nComp(), 0, dm, Fab_allocate);
    auxTemp.define(ba, Aux_old.nComp(), 0, dm, Fab_allocate);
  }
  else {
    stateTemp.define(ba, S_old.nComp(), 0, Fab_allocate);
    pressTemp.define(ba, P_old.nComp(), 0, Fab_allocate);
    auxTemp.define(ba, Aux_old.nComp(), 0, Fab_allocate);
  }
  const DistributionMapping& dm = stateTemp.DistributionMap();

  stateTemp.copy(S_old,0,0,ncomps+ntracers);  // Parallel copy.
  pressTemp.copy(P_old,0,0,ncomps);           // Parallel copy.
  auxTemp.copy(Aux_old,0,0,Aux_old.nComp());  // Parallel copy.
  Real tagVal = -1;
  if (ngrow_tmp>0) {
    for (int n=0; n<ncomps+ntracers; ++n) {      
      const BCRec& theBC = AmrLevel::desc_lst[State_Type].getBC(n);
      TagUnusedGrowCells(S_old,State_Type,theBC,*this,ngrow_tmp,tagVal,n,1);
    }
  }
  
  phiTemp.define(ba, 1, 0, dm, Fab_allocate);
  if (ngrow_tmp == 0) {
    phiTemp.copy(*rock_phi,0,0,1);
  } else {
    BL_ASSERT(rock_phi->nGrow() >= ngrow_tmp);
    MultiFab phiGrow(BoxArray(rock_phi->boxArray()).grow(ngrow_tmp), 1, 0);
    for (MFIter mfi(*rock_phi); mfi.isValid(); ++mfi)
      phiGrow[mfi].copy((*rock_phi)[mfi],0,0,1);
    phiTemp.copy(phiGrow,0,0,1);  // Parallel copy.
  }

  fcnCntTemp.define(ba, 1, 0, dm, Fab_allocate); 
  if (ngrow_tmp == 0) {
    fcnCntTemp.copy(Fcnt_old,0,0,1);
  } else {
    MultiFab FcnGrow(BoxArray(Fcnt_old.boxArray()).grow(ngrow_tmp), 1, 0);
    for (MFIter mfi(Fcnt_old); mfi.isValid(); ++mfi) {
      FcnGrow[mfi].copy(Fcnt_old[mfi],0,0,1);
    }
    fcnCntTemp.copy(FcnGrow,0,0,1);  // Parallel copy.
  }

  volTemp.define(ba, 1, 0, dm, Fab_allocate);
  for (MFIter mfi(volTemp); mfi.isValid(); ++mfi) {
    geom.GetVolume(volTemp[mfi], volTemp.boxArray(), mfi.index(), 0);
  }  

  //  HACK...should be unnecessary
  for (MFIter mfi(stateTemp); mfi.isValid(); ++mfi) {
    setPhysBoundaryValues(stateTemp[mfi],State_Type,time,0,0,ncomps+ntracers);
  }

  //
  // Do the chemistry advance
  //
  Real dt_sub_chem = dt;
  int nsub_chem = 1;
  if (max_chemistry_time_step > 0) {
    Real trat = dt / max_chemistry_time_step;
    if (trat > 1) {
      nsub_chem = (int) trat;
      if (nsub_chem != trat) {
        nsub_chem++;
      }
      dt_sub_chem = dt / (Real)(nsub_chem);

      if (ParallelDescriptor::IOProcessor()) {
	for (int lev=0; lev<=level; ++lev) {
	  std::cout << "  ";
	}
        std::cout << "  CHEMISTRY: Level: " << level << " Subcycling chemistry." << std::endl;
      }
    }
  }

  for (int i=0; i<nsub_chem; ++i) {
    if (nsub_chem > 1 && ParallelDescriptor::IOProcessor()) {
      for (int lev=0; lev<=level; ++lev) {
        std::cout << "  ";
      }
      Real tstart = state[State_Type].prevTime() + i*dt_sub_chem;
      Real tend = tstart + dt_sub_chem;
      Real teps = 1.e-8*dt_sub_chem;
      if (std::abs(tend - state[State_Type].curTime()) < teps) {
        tend = state[State_Type].curTime();
        dt_sub_chem = tend - tstart;
      }
      std::cout << "  CHEMISTRY: Level: " << level << " TIME: " << tstart << " : " << tend << " (DT=" << dt_sub_chem << "[s])"<< std::endl;
    }

    for (MFIter mfi(stateTemp); mfi.isValid() && chem_ok; ++mfi) {
      Box box = mfi.validbox();
      FArrayBox& sat_fab   = stateTemp[mfi];
      sat_fab.mult(1/density[0],0,1);
      FArrayBox& press_fab = pressTemp[mfi];
      FArrayBox& phi_fab = phiTemp[mfi];
      FArrayBox& vol_fab = volTemp[mfi];
      FArrayBox& fct_fab = fcnCntTemp[mfi];
      FArrayBox& aux_fab = auxTemp[mfi];
      chemistry_helper->Advance(sat_fab,0,press_fab,0,phi_fab,0,vol_fab,0,sat_fab,ncomps,
                                fct_fab,0,aux_fab,density[0],298,box,dt_sub_chem);
      sat_fab.mult(density[0],0,1);
    }
  }

  phiTemp.clear();
  volTemp.clear();
    
  S_new.copy(stateTemp,ncomps,ncomps,ntracers); // Parallel copy, tracers only
  stateTemp.clear();
  Aux_new.copy(auxTemp,0,0,Aux_new.nComp()); // Parallel copy, everything.
  auxTemp.clear();
    	
  S_new.FillBoundary();
  Aux_new.FillBoundary();
	
  geom.FillPeriodicBoundary(S_new,true);
  geom.FillPeriodicBoundary(Aux_new,true);

  if (ngrow == 0 || ngrow_tmp == 0) {
      Fcnt_new.copy(fcnCntTemp,0,0,1); // Parallel copy.
      fcnCntTemp.clear();
  }
  else {
    //
    // Can't directly use a parallel copy to update FuncCount_Type.
    //
    MultiFab grownFcnt(BoxArray(Fcnt_new.boxArray()).grow(ngrow), 1, 0);
    grownFcnt.setVal(1);
    for (MFIter mfi(Fcnt_new); mfi.isValid(); ++mfi) {
      grownFcnt[mfi].copy(Fcnt_new[mfi]);
    }
    
    grownFcnt.copy(fcnCntTemp); // Parallel copy.
    fcnCntTemp.clear();
    for (MFIter mfi(grownFcnt); mfi.isValid(); ++mfi)
      Fcnt_new[mfi].copy(grownFcnt[mfi]);
  }
  
  // Bring all states up to current time, and reinstate original dt info
  for (std::set<int>::const_iterator it=types_advanced.begin(), End=types_advanced.end(); it!=End; ++it) {
    state[*it].setTimeLevel(time+dt,dt,dt);
  }

  if (show_selected_runtimes > 0 && ParallelDescriptor::IOProcessor()) {
    const int IOProc   = ParallelDescriptor::IOProcessorNumber();
    Real      run_time = ParallelDescriptor::second() - strt_time;
    ParallelDescriptor::ReduceRealMax(run_time,IOProc);
    
    std::cout << "PorousMedia::advance_chemistry time: " << run_time << '\n';
  }
  
  return chem_ok;
}
    
void
PorousMedia::set_preferred_boundary_values (MultiFab& S,
					    int       state_index,
					    int       src_comp,
					    int       dst_comp,
					    int       num_comp,
					    Real      time) const
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::set_preferred_boundary_values()");

  if (state_index == State_Type)
  {
      const TimeLevel whichTime = which_time(State_Type,time);
      //
      // To get chem-advanced data instead of FP'd data at old time.
      //
      // For AMANZI the chem-advanced data are the tracers.
      //
      if (!FillPatchedOldState_ok && whichTime == AmrOldTime)
      {
          if (src_comp == ncomps && num_comp == ntracers)
          {
              aux_boundary_data_old.copyTo(S, src_comp, dst_comp, num_comp);
          }
      }
  }
}

//
// Compute capillary update.  This assumes there are only 2 phases and
// incompressible.  We only solve for component 1, and solution to 
// component 2 are deduced from component 1.
//
void
PorousMedia::scalar_capillary_update (Real      dt,
				      int       corrector,
				      MultiFab* u_mac)

{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::scalar_capillary_update()");

  BL_ASSERT(nphases == 2);
  BL_ASSERT(have_capillary == 1);

  const Real strt_time = ParallelDescriptor::second();

  // Build single component edge-centered array of MultiFabs for fluxes
  MultiFab** fluxSCn;
  MultiFab** fluxSCnp1;
  const int nGrow = 0;
  const int nComp = 1;
  diffusion->allocFluxBoxesLevel(fluxSCn,  nGrow,nComp);
  diffusion->allocFluxBoxesLevel(fluxSCnp1,nGrow,nComp);

  int nc = 0; 
  int nd = 1;
  MultiFab*  delta_rhs = 0;
  MultiFab*  alpha     = 0;
  MultiFab** cmp_pcn   = 0;
  MultiFab** cmp_pcnp1 = 0;
  MultiFab** cmp_pcnp1_dp = 0;
  MultiFab&  S_new = get_new_data(State_Type);

  MultiFab* sat_res_mf = new MultiFab(grids,1,1);
  sat_res_mf->setVal(1.);
  for (MFIter mfi(*sat_res_mf); mfi.isValid();++mfi)
    {
      const Box& box = (*sat_res_mf)[mfi].box();
      (*sat_res_mf)[mfi].minus((*cpl_coef)[mfi],box,3,0,1);
    }
  sat_res_mf->mult(density[nc]);
  diffusion->set_rho(sat_res_mf); 

  MultiFab* S_nwt = new MultiFab(grids,1,1);
  MultiFab::Copy(*S_nwt,S_new,nc,0,nComp,1);

  alpha = new MultiFab(grids, 1, 1);
  MultiFab::Copy(*alpha,*rock_phi,0,0,1,alpha->nGrow());
  
  // Newton method.
  // initialization
  Real pcTime = state[State_Type].prevTime();
  diffusion->allocFluxBoxesLevel(cmp_pcn,0,1);
  calcCapillary(pcTime);
  calcDiffusivity_CPL(cmp_pcn,lambda_cc); 
  diffusion->diffuse_init_CPL(dt,nc,be_cn_theta,
			      fluxSCn,0,delta_rhs,
			      alpha,cmp_pcn,pcn_cc,S_nwt);
  pcTime = state[State_Type].curTime();
  FillStateBndry (pcTime,State_Type,0,ncomps);
  diffusion->allocFluxBoxesLevel(cmp_pcnp1,0,1);
  diffusion->allocFluxBoxesLevel(cmp_pcnp1_dp,0,1);
  calcCapillary(pcTime);
  calcLambda(pcTime);
  calcDiffusivity_CPL(cmp_pcnp1,lambdap1_cc);
  calcDiffusivity_CPL_dp(cmp_pcnp1_dp,lambdap1_cc,pcTime,1);

  int  max_itr_nwt = 20;
#if (BL_SPACEDIM == 3)
  Real max_err_nwt = 1e-8;
#else
  Real max_err_nwt = 1e-8;
#endif
  int  itr_nwt = 0;
  Real err_nwt = 1e10;
  Real be_theta = be_cn_theta;
  while ((itr_nwt < max_itr_nwt) && (err_nwt > max_err_nwt)) 
    {
      diffusion->diffuse_iter_CPL(dt,nc,ncomps,be_theta,
				  0,alpha,cmp_pcnp1,cmp_pcnp1_dp,
				  pcnp1_cc,S_nwt,&err_nwt);

      if (verbose > 3 && ParallelDescriptor::IOProcessor())
	std::cout << "Newton iteration " << itr_nwt 
	          << " : Error = "       << err_nwt << "\n"; 

      //scalar_adjust_constraint(0,ncomps-1);
      //FillStateBndry(pcTime,State_Type,0,ncomps);
      //calcCapillary(pcTime);
      //calcLambda(pcTime);
      calcDiffusivity_CPL(cmp_pcnp1,lambdap1_cc);
      calcDiffusivity_CPL_dp(cmp_pcnp1_dp,lambdap1_cc,pcTime,1);
      itr_nwt += 1;

      if (verbose > 3)
	check_minmax();
    }
    
  diffusion->compute_flux(nc,dt,be_cn_theta,fluxSCnp1,pcnp1_cc,cmp_pcnp1);

  if (verbose > 3 && ParallelDescriptor::IOProcessor())
    {
      if (itr_nwt < max_itr_nwt)
	std::cout << "Newton converged at iteration " << itr_nwt
		  << " with error " << err_nwt << '\n';
      else
	std::cout << "Newton failed to converged: termination error is "
		  <<  err_nwt << '\n'; 
    }

  //
  // add to phase velocity
  //
  if (u_mac != 0) {
      
    FArrayBox fluxtot;

    for (int d = 0; d < BL_SPACEDIM; d++) 
      {
	for (MFIter fmfi(*fluxSCn[d]); fmfi.isValid(); ++fmfi) 
	  {
	    const Box& ebox = (*fluxSCn[d])[fmfi].box();
	    fluxtot.resize(ebox,nComp);
	    fluxtot.copy((*fluxSCn[d])[fmfi],ebox,0,ebox,0,nComp);
	    if (no_corrector == 1)
	      fluxtot.mult(2.0);
	    else
	      fluxtot.plus((*fluxSCnp1[d])[fmfi],ebox,0,0,nComp);

	    fluxtot.mult(-1.0/density[nc]);
	    fluxtot.divide(area[d][fmfi],0,0,1);
	    u_mac[d][fmfi].plus(fluxtot,ebox,0,0,nComp);
	  }
	u_mac[d].FillBoundary();
      }
  }

  //
  // Increment the viscous flux registers
  // The fluxes are - beta \nabla p_c. We accummulate flux assuming 
  // it is on the LHS.  Thus, we need to multiply by -dt due to the sign change.
  // 

  if (do_reflux && corrector) {

    FArrayBox fluxtot;
	  
    for (int d = 0; d < BL_SPACEDIM; d++) 
      {
	MultiFab fluxes;

	if (level < parent->finestLevel())
	  fluxes.define((*fluxSCn[d]).boxArray(), ncomps, 0, Fab_allocate);

	for (MFIter fmfi(*fluxSCn[d]); fmfi.isValid(); ++fmfi)
	  {
	    // for component nc
	    const Box& ebox = (*fluxSCn[d])[fmfi].box();

	    fluxtot.resize(ebox,ncomps);
	    fluxtot.copy((*fluxSCn[d])[fmfi],ebox,0,ebox,nc,1);
	    fluxtot.plus((*fluxSCnp1[d])[fmfi],ebox,0,nc,1);

	    (*fluxSCn[d])[fmfi].mult(-density[nd]/density[nc]);
	    (*fluxSCnp1[d])[fmfi].mult(-density[nd]/density[nc]);
	    fluxtot.copy((*fluxSCn[d])[fmfi],ebox,0,ebox,nd,1);
	    fluxtot.plus((*fluxSCnp1[d])[fmfi],ebox,0,nd,1);

	    if (level < parent->finestLevel())
	      fluxes[fmfi].copy(fluxtot);

	    if (level > 0)
	      getViscFluxReg().FineAdd(fluxtot,d,fmfi.index(),0,0,ncomps,-dt);
	  }

	if (level < parent->finestLevel())
	  getLevel(level+1).getViscFluxReg().CrseInit(fluxes,d,0,0,ncomps,dt);
		  
      }
  }
    
  //     nc = 0; 
  //     MultiFab::Copy(*S_nwt,S_new,nc,0,nComp,1);
  //     diffusion->check_consistency(dt,nc,mone,be_cn_theta,
  // 				 rho_flag,0,alpha,
  // 				 cmp_pcn,cmp_pcnp1,
  // 				 pcn_cc, pcnp1_cc,
  // 				 S_nwt,&err_nwt);

  delete delta_rhs;
  delete S_nwt;
  delete sat_res_mf;
  delete alpha;

  diffusion->removeFluxBoxesLevel(cmp_pcn);
  diffusion->removeFluxBoxesLevel(cmp_pcnp1);
  diffusion->removeFluxBoxesLevel(cmp_pcnp1_dp);
	   
  diffusion->removeFluxBoxesLevel(fluxSCn);
  diffusion->removeFluxBoxesLevel(fluxSCnp1);
    
  if (show_selected_runtimes > 0)
    {
      const int IOProc   = ParallelDescriptor::IOProcessorNumber();
      Real      run_time = ParallelDescriptor::second() - strt_time;

      ParallelDescriptor::ReduceRealMax(run_time,IOProc);

      if (ParallelDescriptor::IOProcessor())
        std::cout << "PorousMedia::scalar_CPL_update(): time: " << run_time << '\n';
    }
  
  //
  // Write out the min and max of each component of the new state.
  //
  if (verbose > 3) check_minmax();

}

//
// Compute capillary update.  This assumes there are only 2 phases and
// incompressible.  We only solve for component 1, and solution to 
// component 2 are deduced from component 1.
//
void
PorousMedia::diff_capillary_update (Real      dt,
				    int       corrector,
				    MultiFab* u_mac)
  
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::diff_capillary_update()");

  BL_ASSERT(nphases == 2);
  BL_ASSERT(have_capillary == 1);

  const Real strt_time = ParallelDescriptor::second();

  // Build single component edge-centered array of MultiFabs for fluxes
  MultiFab** fluxSCn;
  MultiFab** fluxSCnp1;
  const int nGrow = 0;
  const int nComp = 1;
  diffusion->allocFluxBoxesLevel(fluxSCn,  nGrow,nComp);
  diffusion->allocFluxBoxesLevel(fluxSCnp1,nGrow,nComp);

  int nc = 0; 
  int nd = 1;
  MultiFab*  delta_rhs    = 0;
  MultiFab*  alpha        = 0;
  MultiFab** tmp          = 0;
  MultiFab** cmp_pcnp1_dp = 0;
  MultiFab*  S_nwt = 0;
  MultiFab&  S_new = get_new_data(State_Type);
  diffusion->allocFluxBoxesLevel(cmp_pcnp1_dp,0,1);

  MultiFab* sat_res_mf = new MultiFab(grids,1,1);
  sat_res_mf->setVal(1.);
  for (MFIter mfi(*sat_res_mf); mfi.isValid();++mfi)
    {
      const Box& box = (*sat_res_mf)[mfi].box();
      (*sat_res_mf)[mfi].minus((*cpl_coef)[mfi],box,3,0,1);
    }
  sat_res_mf->mult(density[nc]);
  diffusion->set_rho(sat_res_mf); 

  S_nwt = new MultiFab(grids,1,1);
  MultiFab::Copy(*S_nwt,S_new,nc,0,nComp,1);

  alpha = new MultiFab(grids, 1, 1);
  MultiFab::Copy(*alpha,*rock_phi,0,0,1,alpha->nGrow());

  tmp = new MultiFab* [BL_SPACEDIM];
  for (int d=0; d<BL_SPACEDIM; d++)
    tmp[d] = &lambda[d];
  
  MultiFab* Stmp = new MultiFab(grids,1,1);
  MultiFab::Copy(*Stmp,*pcn_cc,0,0,1,1);
  MultiFab::Add(*Stmp,*pcnp1_cc,0,0,1,1);
  (*Stmp).mult(0.5);
  
  // Newton method.
  // initialization
  diffusion->diffuse_init_CPL(dt,nc,-be_cn_theta,
			      fluxSCn,0,delta_rhs,
			      alpha,tmp,Stmp,S_nwt);

  Real pcTime = state[State_Type].prevTime();
  
  Stmp->setVal(0);
  calcCapillary(pcTime);
  calcLambda(pcTime);
  calcDiffusivity_CPL(tmp,lambda_cc);

  diffusion->diffuse_init_CPL(dt,nc,be_cn_theta,
			      fluxSCnp1,0,delta_rhs,
			      alpha,tmp,pcn_cc,Stmp);

  MultiFab::Add(*S_nwt,*Stmp,0,0,1,0);
  delete Stmp;

  for (int d=0; d<BL_SPACEDIM; d++)
    MultiFab::Add(*fluxSCn[d],*fluxSCnp1[d],0,0,1,0);

  pcTime = state[State_Type].curTime();
  calcCapillary(pcTime);
  calcLambda(pcTime);
  calcDiffusivity_CPL(tmp,lambdap1_cc);
  calcDiffusivity_CPL_dp(cmp_pcnp1_dp,lambdap1_cc,pcTime,1);

  int  max_itr_nwt = 20;
#if (BL_SPACEDIM == 3)
  Real max_err_nwt = 1e-8;
#else
  Real max_err_nwt = 1e-8;
#endif
  int  itr_nwt = 0;
  Real err_nwt = 1e10;
  Real be_theta = be_cn_theta;

  while ((itr_nwt < max_itr_nwt) && (err_nwt > max_err_nwt)) 
    {
      diffusion->diffuse_iter_CPL(dt,nc,ncomps,be_theta,
				  0,alpha,tmp,cmp_pcnp1_dp,
				  pcnp1_cc,S_nwt,&err_nwt);

      if (verbose > 3 && ParallelDescriptor::IOProcessor())
	std::cout << "Newton iteration " << itr_nwt 
	          << " : Error = "       << err_nwt << "\n"; 

      //scalar_adjust_constraint(0,ncomps-1);
      //FillStateBndry(pcTime,State_Type,0,ncomps);
      //calcCapillary(pcTime);
      //calcLambda(pcTime);
      calcDiffusivity_CPL(tmp,lambdap1_cc);
      calcDiffusivity_CPL_dp(cmp_pcnp1_dp,lambdap1_cc,pcTime,1);
      itr_nwt += 1;

      if (verbose > 3)
	check_minmax();
    }

  diffusion->compute_flux(nc,dt,be_cn_theta,fluxSCnp1,pcnp1_cc,tmp);
    
  if (verbose > 3 && ParallelDescriptor::IOProcessor())
    {
      if (itr_nwt < max_itr_nwt)
	std::cout << "Newton converged at iteration " << itr_nwt
		  << " with error " << err_nwt << '\n';
      else
	std::cout << "Newton failed to converged: termination error is "
		  <<  err_nwt << '\n'; 
    }

  //
  // add to phase velocity
  //
  if (u_mac != 0) {
      
    FArrayBox fluxtot;

    for (int d = 0; d < BL_SPACEDIM; d++) 
      {
	for (MFIter fmfi(*fluxSCn[d]); fmfi.isValid(); ++fmfi) 
	  {
	    const Box& ebox = (*fluxSCn[d])[fmfi].box();
	    fluxtot.resize(ebox,nComp);
	    fluxtot.copy((*fluxSCn[d])[fmfi],ebox,0,ebox,0,nComp);
	    if (no_corrector == 1)
	      fluxtot.mult(2.0);
	    else
	      fluxtot.plus((*fluxSCnp1[d])[fmfi],ebox,0,0,nComp);

	    fluxtot.mult(-1.0/density[nc]);
	    fluxtot.divide(area[d][fmfi],0,0,1);
	    u_mac[d][fmfi].plus(fluxtot,ebox,0,0,nComp);
	  }
	u_mac[d].FillBoundary();
      }
  }

  //
  // Increment the viscous flux registers
  // The fluxes are - beta \nabla p_c. We accummulate flux assuming 
  // it is on the LHS.  Thus, we need to multiply by -dt due to the sign change.
  // 

  if (do_reflux && corrector) {

    FArrayBox fluxtot;
	  
    for (int d = 0; d < BL_SPACEDIM; d++) 
      {
	MultiFab fluxes;
	
	if (level < parent->finestLevel())
	  fluxes.define((*fluxSCn[d]).boxArray(), ncomps, 0, Fab_allocate);
	
	for (MFIter fmfi(*fluxSCn[d]); fmfi.isValid(); ++fmfi)
	  {
	    // for component nc
	    const Box& ebox = (*fluxSCn[d])[fmfi].box();
	    
	    fluxtot.resize(ebox,ncomps);
	    fluxtot.copy((*fluxSCn[d])[fmfi],ebox,0,ebox,nc,1);
	    fluxtot.plus((*fluxSCnp1[d])[fmfi],ebox,0,nc,1);

	    (*fluxSCn[d])[fmfi].mult(-density[nd]/density[nc]);
	    (*fluxSCnp1[d])[fmfi].mult(-density[nd]/density[nc]);
	    fluxtot.copy((*fluxSCn[d])[fmfi],ebox,0,ebox,nd,1);
	    fluxtot.plus((*fluxSCnp1[d])[fmfi],ebox,0,nd,1);

	    if (level < parent->finestLevel())
	      fluxes[fmfi].copy(fluxtot);

	    if (level > 0)
	      getViscFluxReg().FineAdd(fluxtot,d,fmfi.index(),0,0,ncomps,-dt);
	  }

	if (level < parent->finestLevel())
	  getLevel(level+1).getViscFluxReg().CrseInit(fluxes,d,0,0,ncomps,dt);
		  
      }
  }

  delete delta_rhs;
  delete S_nwt;
  delete alpha;
  delete sat_res_mf;
  delete [] tmp;

  diffusion->removeFluxBoxesLevel(cmp_pcnp1_dp);
  diffusion->removeFluxBoxesLevel(fluxSCn);
  diffusion->removeFluxBoxesLevel(fluxSCnp1);
    
  if (show_selected_runtimes > 0)
    {
      const int IOProc   = ParallelDescriptor::IOProcessorNumber();
      Real      run_time = ParallelDescriptor::second() - strt_time;

      ParallelDescriptor::ReduceRealMax(run_time,IOProc);

      if (ParallelDescriptor::IOProcessor())
        std::cout << "PorousMedia::diff_CPL_update(): time: " << run_time << '\n';
    }

  //
  // Write out the min and max of each component of the new state.
  //
  if (verbose > 3) check_minmax();
}

#ifdef MG_USE_FBOXLIB
//
// Richard equation: Equilibrium solver
//
void
PorousMedia::richard_eqb_update (MultiFab* u_mac)

{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::richards_update()");
  BL_ASSERT(nphases == 1);
  BL_ASSERT(have_capillary == 1);

  const Real strt_time = ParallelDescriptor::second();

  // Build single component edge-centered array of MultiFabs for fluxes
  MultiFab** fluxSC;
  const int nGrow = 0;
  const int nComp = 1;
  diffusion->allocFluxBoxesLevel(fluxSC,nGrow,nComp);

  int nc = 0; 
  MultiFab** cmp_pcp1    = 0;
  MultiFab** cmp_pcp1_dp = 0;
  MultiFab sat_res_mf(grids,1,1);
  sat_res_mf.setVal(1.);
  for (MFIter mfi(sat_res_mf); mfi.isValid();++mfi)
    {
      const Box& box = sat_res_mf[mfi].box();
      sat_res_mf[mfi].minus((*cpl_coef)[mfi],box,3,0,1);
    }
  //sat_res_mf.mult(density[nc]);
  diffusion->set_rho(&sat_res_mf); 

  // Compute first res_fix = \nabla v_inflow.  
  // Its value does not change.
  MultiFab res_fix(grids,1,0);
  res_fix.setVal(0.);
  calc_richard_velbc(res_fix,u_mac);

  // Newton method.
  // initialization
  int do_upwind = 1;
  int  max_itr_nwt = 10;
  Real max_err_nwt = 1e-8;
  int  itr_nwt = 0;
  Real err_nwt = 1e10;
  Real pcTime = state[State_Type].curTime();
  MultiFab& P_new = get_new_data(Press_Type);
  FillStateBndry(pcTime,State_Type,0,ncomps);
  diffusion->allocFluxBoxesLevel(cmp_pcp1,0,1);
  diffusion->allocFluxBoxesLevel(cmp_pcp1_dp,0,3);
  calcCapillary(pcTime);
  calcLambda(pcTime);
  MultiFab::Copy(P_new,*pcnp1_cc,0,0,1,1);
  P_new.mult(-1.0,1);
  P_new.plus(atmospheric_pressure_atm,1);
  calc_richard_coef(cmp_pcp1,lambdap1_cc,u_mac,0,do_upwind,pcTime);
  MultiFab* dalpha=0;
  bool do_n = true;
  Real dt=-1;
  calc_richard_jac (cmp_pcp1_dp,dalpha,lambdap1_cc,u_mac,pcTime,dt,0,do_upwind,do_n);
  while ((itr_nwt < max_itr_nwt) && (err_nwt > max_err_nwt)) 
    {
      diffusion->richard_iter_eqb(nc,gravity,gravity_dir,density,res_fix,
				  cmp_pcp1,cmp_pcp1_dp,
				  u_mac,do_upwind,&err_nwt);      
      if (verbose > 3 && ParallelDescriptor::IOProcessor())
	std::cout << "Newton iteration " << itr_nwt 
	          << " : Error = "       << err_nwt << "\n"; 
      scalar_adjust_constraint(0,ncomps-1);
      FillStateBndry(pcTime,State_Type,0,ncomps);
      calcCapillary(pcTime);
      calcLambda(pcTime);
      MultiFab::Copy(P_new,*pcnp1_cc,0,0,1,1);
      P_new.mult(-1.0,1);
      P_new.plus(atmospheric_pressure_atm,1);
      compute_vel_phase(u_mac,0,pcTime);
      calc_richard_coef(cmp_pcp1,lambdap1_cc,u_mac,0,do_upwind,pcTime);
      calc_richard_jac (cmp_pcp1_dp,dalpha,lambdap1_cc,u_mac,pcTime,dt,0,do_upwind,do_n);
      itr_nwt += 1;

      if (verbose > 3)	check_minmax();
    }
    
  diffusion->compute_flux(nc,1.0,1.0,fluxSC,pcnp1_cc,cmp_pcp1);

  if (verbose > 3 && ParallelDescriptor::IOProcessor())
    {
      if (itr_nwt < max_itr_nwt)
	std::cout << "Newton converged at iteration " << itr_nwt
		  << " with error " << err_nwt << '\n';
      else
	std::cout << "Newton failed to converged: termination error is "
		  <<  err_nwt << '\n'; 
    }

  /*
  //
  // Increment the viscous flux registers
  // The fluxes are - beta \nabla p_c. We accummulate flux assuming 
  // it is on the LHS.  Thus, we need to multiply by -dt due to the sign change.
  // 

  if (do_reflux && corrector) {

      FArrayBox fluxtot;
      for (int d = 0; d < BL_SPACEDIM; d++) 
	{
	  MultiFab fluxes;
	  
	  if (level < parent->finestLevel())
	    fluxes.define((*fluxSC[d]).boxArray(), nComp, 0, Fab_allocate);
	  
	  for (MFIter fmfi(*fluxSC[d]); fmfi.isValid(); ++fmfi)
	    {
	      // for component nc
	      const Box& ebox = (*fluxSC[d])[fmfi].box();
	      
	      fluxtot.resize(ebox,nComp);
	      fluxtot.copy((*fluxSC[d])[fmfi],ebox,0,ebox,0,1);

	      if (level < parent->finestLevel())
		fluxes[fmfi].copy(fluxtot);
	      
	      if (level > 0)
		getViscFluxReg().FineAdd(fluxtot,d,fmfi.index(),0,0,nComp,-dt);
	    }
	  
	  if (level < parent->finestLevel())
	    getLevel(level+1).getViscFluxReg().CrseInit(fluxes,d,0,0,nComp,dt);
      }
  }
  */

  diffusion->removeFluxBoxesLevel(cmp_pcp1);
  diffusion->removeFluxBoxesLevel(cmp_pcp1_dp);
  diffusion->removeFluxBoxesLevel(fluxSC);
    
  if (show_selected_runtimes > 0)
    {
      const int IOProc   = ParallelDescriptor::IOProcessorNumber();
      Real      run_time = ParallelDescriptor::second() - strt_time;

      ParallelDescriptor::ReduceRealMax(run_time,IOProc);

      

      if (ParallelDescriptor::IOProcessor())
        std::cout << "PorousMedia::richard_update(): time: " 
		  << run_time << '\n';
    }
  //
  // Write out the min and max of each component of the new state.
  //
  if (verbose > 3) check_minmax();

}

//
// Richard equation: Time-dependent solver.  Only doing a first-order implicit scheme
//
NLSstatus
PorousMedia::richard_scalar_update (Real dt, int& total_nwt_iter, MultiFab* u_mac)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::richards_update()");
  BL_ASSERT(have_capillary == 1);

  const Real strt_time = ParallelDescriptor::second();

  const int nGrow = 0;
  int nc = 0; 
  MultiFab** cmp_pcp1    = 0;
  MultiFab** cmp_pcp1_dp = 0;
  MultiFab sat_res_mf(grids,1,1);
  sat_res_mf.setVal(1.);
  for (MFIter mfi(sat_res_mf); mfi.isValid();++mfi)
    {
      const Box& box = sat_res_mf[mfi].box();
      sat_res_mf[mfi].minus((*cpl_coef)[mfi],box,3,0,1);
    }
  diffusion->set_rho(&sat_res_mf);

  MultiFab& S_new = get_new_data(State_Type);
  MultiFab& P_new = get_new_data(Press_Type);
  MultiFab* alpha = new MultiFab(grids,1,1);
  MultiFab::Copy(*alpha,*rock_phi,0,0,1,alpha->nGrow());

  // Compute first res_fix = -\phi * n^k + dt*\nabla v_inflow.  
  // Its value does not change.
  Real pcTime = state[State_Type].curTime();
  MultiFab res_fix(grids,1,0);
  MultiFab::Copy(res_fix,S_new,nc,0,1,0);
  for (MFIter mfi(res_fix); mfi.isValid(); ++mfi)
    res_fix[mfi].mult((*alpha)[mfi],mfi.validbox(),0,0,1);
  res_fix.mult(-1.0);
  compute_vel_phase(u_mac,0,pcTime);
  calc_richard_velbc(res_fix,u_mac,dt*density[0]);

  // Newton method.
  // initialization
  int do_upwind = 1;
  int  max_itr_nwt = total_nwt_iter;
  Real max_err_nwt = 1e-6;
  int  itr_nwt = 0;
  Real err_nwt = 1e10;
  FillStateBndry(pcTime,State_Type,0,ncomps);
  FillStateBndry(pcTime,Press_Type,0,1);
  diffusion->allocFluxBoxesLevel(cmp_pcp1,0,1);
  diffusion->allocFluxBoxesLevel(cmp_pcp1_dp,0,3);

  calcLambda(pcTime);
  calc_richard_coef(cmp_pcp1,lambdap1_cc,u_mac,0,do_upwind,pcTime);

  MultiFab* dalpha = 0;
  if (!do_richard_sat_solve) {
      dalpha = new MultiFab(grids,1,1,Fab_allocate); // Note: requires a delete
  }
  calc_richard_jac (cmp_pcp1_dp,dalpha,lambdap1_cc,u_mac,pcTime,dt,0,do_upwind,do_richard_sat_solve);

  Diffusion::NewtonStepInfo linear_status;
  linear_status.success = true;
  linear_status.max_ls_iterations = richard_max_ls_iterations;
  linear_status.min_ls_factor = richard_min_ls_factor;
  linear_status.ls_acceptance_factor = richard_ls_acceptance_factor;
  linear_status.ls_reduction_factor = richard_ls_reduction_factor;
  linear_status.monitor_linear_solve = richard_monitor_linear_solve;
  linear_status.monitor_line_search = richard_monitor_line_search;

  if (do_richard_sat_solve)
    {
      calcCapillary(pcTime);
      MultiFab::Copy(P_new,*pcnp1_cc,0,0,1,1);
      P_new.mult(-1.0,1);
      P_new.plus(atmospheric_pressure_atm,1);
      while ((itr_nwt < max_itr_nwt) && (err_nwt > max_err_nwt) && (linear_status.success)) 
	{
            itr_nwt++;
            diffusion->richard_iter(dt,nc,gravity,gravity_dir,density,res_fix,
                                    alpha,cmp_pcp1,cmp_pcp1_dp,
                                    u_mac,do_upwind,linear_status);
            
            err_nwt = linear_status.residual_norm_post_ls;

            if (linear_status.success) {
                if (richard_solver_verbose>0 && ParallelDescriptor::IOProcessor()) {
                    std::cout << "     Iteration (n) " << itr_nwt 
                              << " : Error = "       << err_nwt << " (tol: " << max_err_nwt << ")\n";
                }
                if (model != PM_RICHARDS)
                    scalar_adjust_constraint(0,ncomps-1);
                FillStateBndry(pcTime,State_Type,0,ncomps);
                calcCapillary(pcTime);
                calcLambda(pcTime);
                MultiFab::Copy(P_new,*pcnp1_cc,0,0,1,1);
                P_new.mult(-1.0,1);
                P_new.plus(atmospheric_pressure_atm,1);
                compute_vel_phase(u_mac,0,pcTime);
                calc_richard_coef(cmp_pcp1,lambdap1_cc,u_mac,0,do_upwind,pcTime);
                calc_richard_jac (cmp_pcp1_dp,dalpha,lambdap1_cc,u_mac,pcTime,dt,0,do_upwind,do_richard_sat_solve);
                if (verbose > 3) check_minmax();
            }
	}
    }
  else
    {
        //MultiFab dalpha(grids,1,1);
        //calc_richard_alpha(&dalpha,pcTime);

      while ((itr_nwt < max_itr_nwt) && (err_nwt > max_err_nwt)  && (linear_status.success)) 
	{
            itr_nwt++;
            diffusion->richard_iter_p(dt,nc,gravity,gravity_dir,density,res_fix,
                                      alpha,dalpha,cmp_pcp1,cmp_pcp1_dp,
                                      u_mac,do_upwind,linear_status);

	    err_nwt = linear_status.residual_norm_post_ls;

            if (linear_status.success) {
                if (richard_solver_verbose>0 && ParallelDescriptor::IOProcessor()) {
                    std::cout << "     Iteration (p) " << itr_nwt 
                              << " : Error = "       << err_nwt << " (tol: " << max_err_nwt << ")\n";
                }
                calcInvPressure (S_new,P_new,pcTime,0,0,1); 
                calcLambda(pcTime);
                compute_vel_phase(u_mac,0,pcTime);
                calc_richard_coef(cmp_pcp1,lambdap1_cc,u_mac,0,do_upwind,pcTime);
                calc_richard_jac (cmp_pcp1_dp,dalpha,lambdap1_cc,u_mac,pcTime,dt,0,do_upwind,do_richard_sat_solve);
                //calc_richard_alpha(&dalpha,pcTime);
                if (verbose > 3)  check_minmax();
            }
	}
    }

  NLSstatus retVal = NLSstatus::NLS_SUCCESS;
  if (!linear_status.success) {
      retVal = NLSstatus::NLS_LINEAR_FAIL;
  }
  if (itr_nwt >= max_itr_nwt) {
      retVal = NLSstatus::NLS_NONLINEAR_FAIL;
      if (richard_solver_verbose>1 && ParallelDescriptor::IOProcessor())
          std::cout << "     **************** Newton failed in richard_scalar_update: too many iterations (max = " << max_itr_nwt << '\n'; 
  }

  MultiFab** fluxSC;
  const int nComp = 1;
  if (retVal == NLSstatus::NLS_SUCCESS) {
      diffusion->allocFluxBoxesLevel(fluxSC,nGrow,nComp);
      diffusion->richard_flux(nc,-1.0,gravity,gravity_dir,density,fluxSC,pcnp1_cc,cmp_pcp1);
  }

  delete alpha;
  if (!do_richard_sat_solve) {
      delete dalpha;
  }
  diffusion->removeFluxBoxesLevel(cmp_pcp1);
  diffusion->removeFluxBoxesLevel(cmp_pcp1_dp);

  if (retVal != NLSstatus::NLS_SUCCESS) {
      return retVal;
  }

  if (richard_solver_verbose>1 && ParallelDescriptor::IOProcessor() && retVal == NLSstatus::NLS_SUCCESS) {
      std::cout << "     Newton converged in " << itr_nwt << " iterations (max = "
                << total_nwt_iter << ") with err: " 
                << err_nwt << " (tol = " << max_err_nwt << ")\n";
  }
  total_nwt_iter = itr_nwt;
  
  //
  // Increment the viscous flux registers
  // The fluxes are - beta \nabla p_c. We accummulate flux assuming 
  // it is on the LHS.  Thus, we need to multiply by -dt.
  // 
  if (do_reflux) 
    {
      FArrayBox fluxtot;
      for (int d = 0; d < BL_SPACEDIM; d++) 
	{
	  MultiFab fluxes;
	  
	  if (level < parent->finestLevel())
	    fluxes.define((*fluxSC[d]).boxArray(), nComp, 0, Fab_allocate);
	  
	  for (MFIter fmfi(*fluxSC[d]); fmfi.isValid(); ++fmfi)
	    {
	      // for component nc
	      const Box& ebox = (*fluxSC[d])[fmfi].box();
	      
	      fluxtot.resize(ebox,nComp);
	      fluxtot.copy((*fluxSC[d])[fmfi],ebox,0,ebox,0,1);
	      
	      if (level < parent->finestLevel())
		fluxes[fmfi].copy(fluxtot);
	      
	      if (level > 0)
		getViscFluxReg().FineAdd(fluxtot,d,fmfi.index(),0,0,nComp,-dt);
	    }
	  
	  if (level < parent->finestLevel())
	    getLevel(level+1).getViscFluxReg().CrseInit(fluxes,d,0,0,nComp,dt);
      }
  }
      
  diffusion->removeFluxBoxesLevel(fluxSC);    

 
  Real run_time = ParallelDescriptor::second() - strt_time;
  richard_time = run_time;
  ParallelDescriptor::ReduceRealMax(richard_time);
  richard_time_min = std::min(richard_time_min,richard_time);
  
  if (show_selected_runtimes > 0)
    { 
      const int IOProc   = ParallelDescriptor::IOProcessorNumber();
      ParallelDescriptor::ReduceRealMax(run_time,IOProc);


      if (ParallelDescriptor::IOProcessor())
        std::cout << "PorousMedia::richard_update(): time: " 
		  << run_time << '\n';
    }
  //
  // Write out the min and max of each component of the new state.
  //
  if (verbose > 3) check_minmax();

  return retVal;
}

//
// Richard equation: Time-dependent solver.  Only doing a first-order implicit scheme
//
NLSstatus
PorousMedia::richard_composite_update (Real dt, NLScontrol& nl_data)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::richards_composite_update()");
  BL_ASSERT(have_capillary == 1);

  if (ParallelDescriptor::IOProcessor()) {
    std::cout << "Non-PETSc composite Richard solve not currently supported" << std::endl;
  }
  return NLSstatus::NLS_CATASTROPHIC_FAIL;

#if 0
  const Real strt_time = ParallelDescriptor::second();

  int nlevs = parent->finestLevel() - level + 1;
  int nc = 0;

  // FIXME: Memory for the following needs to be explicitly managed if this option is called
  Array < PArray <MultiFab> > Hcoeffs;
  Array < PArray <MultiFab> > Jacobian;
  PArray <MultiFab>  DAlpha;
  Array <MultiFab*> velPhase;

  int nLevs = pm_amr->finestLevel() + 1;
  velPhase.resize(nLevs);
  for (int lev = 0; lev <nLevs; lev++) {
    BoxArray grids = pm_amr->getLevel(start_level+lev).boxArray();
    velPhase[lev] = dynamic_cast<PorousMedia*>(&pm_amr->getLevel(start_level+lev))->UMac_Curr();
  }

  Hcoeffs.resize(BL_SPACEDIM);
  Jacobian.resize(BL_SPACEDIM);
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      Hcoeffs[dir].resize(nLevs,PArrayManage);
      Jacobian[dir].resize(nLevs,PArrayManage);
      for (int lev = 0; lev < nLevs; lev++)
	{
	  BoxArray grids = BoxArray(pm_amr->getLevel(start_level+lev).boxArray()).surroundingNodes(dir);
	  Hcoeffs[dir].set(lev, new MultiFab(grids,1,0));
	  Jacobian[dir].set(lev, new MultiFab(grids,3,0));
	}
    }
  DAlpha.resize(nLevs,PArrayManage);
  for (int lev = 0; lev <nLevs; lev++) {
    BoxArray grids = pm_amr->getLevel(start_level+lev).boxArray();
    DAlpha.set(lev, new MultiFab(grids,1,1)); // Why is this grow cell required/filled?
  }



  Array<MultiFab*>& u_mac_local = nl_data.velPhase;
  Array < PArray <MultiFab> >& cmp_pcp1 = nl_data.Hcoeffs;
  Array < PArray <MultiFab> >& cmp_pcp1_dp = nl_data.Jacobian;
  PArray <MultiFab>& dalpha = nl_data.DAlpha;

  // Create a nlevs-level array for the coefficients
  PArray <MultiFab> alpha(nlevs,PArrayManage);
  PArray <MultiFab> res_fix(nlevs,PArrayManage);
    
  int do_upwind = 1;
  int  max_itr_nwt = nl_data.MaxNLIterations();
  Real max_err_nwt = 1e-6;
  Real err_nwt = 1e10;
  Real pcTime = state[State_Type].curTime();

  MFTower& IC = *(nl_data.InitialState);
  for (int lev=0; lev<nlevs; lev++)
  {
      PorousMedia&    fine_lev   = getLevel(lev);
      const BoxArray& fine_grids = fine_lev.boxArray();      
      MultiFab& S_lev = fine_lev.get_new_data(State_Type);
      MultiFab& P_lev = fine_lev.get_new_data(Press_Type);

      alpha.set(lev,new MultiFab(fine_grids,1,1));
      MultiFab::Copy(alpha[lev],*(fine_lev.rock_phi),0,0,1,1);

      res_fix.set(lev,new MultiFab(fine_grids,1,1));
      if (do_richard_sat_solve) {
	MultiFab::Copy(res_fix[lev],IC[lev],nc,0,1,0);
      }
      else {
	fine_lev.FillStateBndry(pcTime,Press_Type,0,1);
	fine_lev.calcInvPressure(res_fix[lev],IC[lev]);
      }

      for (MFIter mfi(res_fix[lev]); mfi.isValid(); ++mfi)
	res_fix[lev][mfi].mult(alpha[lev][mfi],mfi.validbox(),0,0,1);
      res_fix[lev].mult(-1.0);

      fine_lev.compute_vel_phase(u_mac_local[lev],0,pcTime);
      fine_lev.calc_richard_velbc(res_fix[lev],u_mac_local[lev],dt*density[0]);

      if (do_richard_sat_solve) 
	{
            // FIXME: pulled from above after calcLambda
	  fine_lev.calcCapillary(pcTime);
            // FIXME: in the scalar version, this is done inside the do_richard_sat_solve loop below
            //fine_lev.calcLambda(pcTime);
	  MultiFab::Copy(P_lev,*(fine_lev.pcnp1_cc),0,0,1,1);
	  P_lev.mult(-1.0,1);
          P_lev.plus(atmospheric_pressure_atm,1);
	}
  }

  Diffusion::NewtonStepInfo linear_status;
  linear_status.success = true;
  linear_status.max_ls_iterations = richard_max_ls_iterations;
  linear_status.min_ls_factor = richard_min_ls_factor;
  linear_status.ls_acceptance_factor = richard_ls_acceptance_factor;
  linear_status.ls_reduction_factor = richard_ls_reduction_factor;
  linear_status.monitor_linear_solve = richard_monitor_linear_solve;
  linear_status.monitor_line_search = richard_monitor_line_search;

  Real solve_time = 0;

  if (do_richard_sat_solve)
    {
        while ((nl_data.NLIterationsTaken() < nl_data.MaxNLIterations()) && (err_nwt > max_err_nwt) && (linear_status.success)) 
	{
	  nl_data.SetNLIterationsTaken(nl_data.NLIterationsTaken()+1);
	  diffusion->richard_composite_iter(dt,nlevs,nc,gravity,gravity_dir,density,res_fix,
					    alpha,cmp_pcp1,cmp_pcp1_dp,u_mac_local,
					    do_upwind,linear_status); 

          err_nwt = linear_status.residual_norm_post_ls;

	  for (int lev=0; lev<nlevs; lev++)
          {
	      PorousMedia&  fine_lev = getLevel(lev);   
	      MultiFab& P_lev        = fine_lev.get_new_data(Press_Type);

	      fine_lev.FillStateBndry(pcTime,State_Type,0,ncomps);
	      fine_lev.calcCapillary(pcTime);
	      fine_lev.calcLambda(pcTime);
	      MultiFab::Copy(P_lev,*(fine_lev.pcnp1_cc),0,0,1,1);
	      P_lev.mult(-1.0,1);
              P_lev.plus(atmospheric_pressure_atm,1);

	      MultiFab* tmp_cmp_pcp1[BL_SPACEDIM];
	      MultiFab* tmp_cmp_pcp1_dp[BL_SPACEDIM];
	      for (int dir=0;dir<BL_SPACEDIM;dir++)
		{
		  tmp_cmp_pcp1[dir] = &cmp_pcp1[dir][lev];
		  tmp_cmp_pcp1_dp[dir] = &cmp_pcp1_dp[dir][lev];
		}

	      fine_lev.compute_vel_phase(u_mac_local[lev],0,pcTime);
	      fine_lev.calc_richard_coef(tmp_cmp_pcp1,fine_lev.lambdap1_cc,
					 u_mac_local[lev],0,do_upwind,pcTime);
              if (nl_data.UpdateJacobian(lev)) {
                  fine_lev.calc_richard_jac(tmp_cmp_pcp1_dp,0,fine_lev.lambdap1_cc,
                                            u_mac_local[lev],pcTime,dt,0,do_upwind,do_richard_sat_solve);
              }
 	    }
        }
    }

  else
    {
      while ((nl_data.NLIterationsTaken() < nl_data.MaxNLIterations()) && (err_nwt > max_err_nwt) && (linear_status.success)) 
	{
	  nl_data.SetNLIterationsTaken(nl_data.NLIterationsTaken()+1);

	  bool update_jac = false;
	  for (int lev=0;lev<nlevs;lev++) update_jac = nl_data.UpdateJacobian(lev) || update_jac;
	  const Real tmp_time = ParallelDescriptor::second();
	  diffusion->richard_composite_iter_p(dt,nlevs,nc,gravity,gravity_dir,density,res_fix,
                                              alpha,dalpha,cmp_pcp1,cmp_pcp1_dp,
                                              u_mac_local,update_jac,do_upwind,linear_status); 
	  solve_time+= ParallelDescriptor::second() - tmp_time;
          err_nwt = linear_status.residual_norm_post_ls;

          if (richard_solver_verbose>1 && ParallelDescriptor::IOProcessor()) {
              std::cout << "  " << nl_data.NLIterationsTaken() << "  Amanzi-S Newton Function norm " << err_nwt << "\n"; 
          }
	}
    }

  NLSstatus retVal = NLSstatus::NLS_SUCCESS;
  if (!linear_status.success) {
      retVal = NLSstatus::NLS_LINEAR_FAIL;
  }
  if (nl_data.NLIterationsTaken() >= nl_data.MaxNLIterations()) {
      retVal = NLSstatus::NLS_NONLINEAR_FAIL;
      if (richard_solver_verbose>1 && ParallelDescriptor::IOProcessor())
          std::cout << "     **************** Newton failed in richard_composite_update: too many iterations (max = "
                    << nl_data.MaxNLIterations() << ")\n"; 
  }

  if (retVal != NLSstatus::NLS_SUCCESS) {
      return retVal;
  }

  if (richard_solver_verbose>1 && ParallelDescriptor::IOProcessor() && retVal == NLSstatus::NLS_SUCCESS) {
      std::cout << "     Newton converged in " << nl_data.NLIterationsTaken() << " iterations (max = "
                << nl_data.MaxNLIterations() << ") with err: " 
                << err_nwt << " (tol = " << max_err_nwt << ")\n";
  }


  Real run_time = ParallelDescriptor::second() - strt_time;
  richard_time = run_time;
  ParallelDescriptor::ReduceRealMax(richard_time);
  ParallelDescriptor::ReduceRealMax(solve_time);
  richard_time_min = std::min(richard_time_min,richard_time);
  
  if (show_selected_runtimes > 0)
    { 
      const int IOProc   = ParallelDescriptor::IOProcessorNumber();
      ParallelDescriptor::ReduceRealMax(run_time,IOProc);


      if (ParallelDescriptor::IOProcessor())
        std::cout << "PorousMedia::richard_update(): time: " 
		  << run_time << ' ' << solve_time  << '\n';
    }
  //
  // Write out the min and max of each component of the new state.
  //
  if (verbose > 3) check_minmax();

  return retVal;
#endif
}
#endif

//
// Enforce the constraint sum_i s_i = 1.  This is achieved by adjusting 
// the saturation of the dominant component specified in the input.
//
void
PorousMedia::scalar_adjust_constraint (int  first_scalar,
				       int  last_scalar)
{
  if (idx_dominant > -1) {
      MultiFab&  S_new = get_new_data(State_Type);
      
      MultiFab S_adj(grids,1,S_new.nGrow());
      MultiFab S_div(grids,1,S_new.nGrow());
      S_adj.setVal(1.0);
      
      for (int kk=first_scalar; kk<=last_scalar; kk++)
      {
          if (solid.compare(pNames[pType[kk]]) != 0 && 
              kk != idx_dominant) 
          {
              MultiFab::Copy(S_div,S_new,kk,0,1,S_div.nGrow());
              S_div.mult(1.0/density[kk],S_div.nGrow());
              S_adj.minus(S_div,0,1,S_adj.nGrow());
          }
      }
      S_adj.mult(density[idx_dominant],S_div.nGrow());
  MultiFab::Copy(S_new,S_adj,0,idx_dominant,1,S_new.nGrow());
  S_new.FillBoundary();
  geom.FillPeriodicBoundary(S_new,true);
  }
}

void
coarsenMask(FArrayBox& crse, const FArrayBox& fine, const IntVect& ratio)
{
    const Box& fbox = fine.box();
    const Box cbox = BoxLib::coarsen(fbox,ratio);
    crse.resize(cbox,1); crse.setVal(0);

    Box b1(BoxLib::refine(cbox,ratio));

    const int* flo      = fbox.loVect();
    const int* fhi      = fbox.hiVect();
    IntVect    d_length = fbox.size();
    const int* flen     = d_length.getVect();
    const int* clo      = cbox.loVect();
    IntVect    cbox_len = cbox.size();
    const int* clen     = cbox_len.getVect();
    const int* lo       = b1.loVect();
    int        longlen  = b1.longside();

    const Real* fdat = fine.dataPtr();
    Real* cdat = crse.dataPtr();

    Array<Real> t(longlen,0);

    int klo = 0, khi = 0, jlo = 0, jhi = 0, ilo, ihi;
    D_TERM(ilo=flo[0]; ihi=fhi[0]; ,
           jlo=flo[1]; jhi=fhi[1]; ,
           klo=flo[2]; khi=fhi[2];)

#define IXPROJ(i,r) (((i)+(r)*std::abs(i))/(r) - std::abs(i))
#define IOFF(j,k,lo,len) D_TERM(0, +(j-lo[1])*len[0], +(k-lo[2])*len[0]*len[1])
   
   int ratiox = 1, ratioy = 1, ratioz = 1;
   D_TERM(ratiox = ratio[0];,
          ratioy = ratio[1];,
          ratioz = ratio[2];)

   for (int k = klo; k <= khi; k++)
   {
       const int kc = IXPROJ(k,ratioz);
       for (int j = jlo; j <= jhi; j++)
       {
           const int   jc = IXPROJ(j,ratioy);
           Real*       c = cdat + IOFF(jc,kc,clo,clen);
           const Real* f = fdat + IOFF(j,k,flo,flen);
           //
           // Copy fine grid row of values into tmp array.
           //
           for (int i = ilo; i <= ihi; i++)
               t[i-lo[0]] = f[i-ilo];

           for (int off = 0; off < ratiox; off++)
           {
               for (int ic = 0; ic < clen[0]; ic++)
               {
                   const int i = ic*ratiox + off;
                   c[ic] = std::max(c[ic],t[i]);
               }
           }
       }
   }

#undef IXPROJ
#undef IOFF
}





//
// Tag cells for refinement
//
void
PorousMedia::errorEst (TagBoxArray& tags,
		       int         clearval,
		       int         tagval,
		       Real        time,
		       int         n_error_buf, 
		       int         ngrow)
{
  const int*  domain_lo = geom.Domain().loVect();
  const int*  domain_hi = geom.Domain().hiVect();
  const Real* dx        = geom.CellSize();
  const Real* prob_lo   = geom.ProbLo();
  const Real* prob_hi   = geom.ProbHi();
  Array<int>  itags;

  //
  // Tag cells for refinement
  //
  for (int j = 0; j < err_list.size(); j++)
  {
      const ErrorRec::ErrorFunc& efunc = err_list[j].errFunc();
      const PM_Error_Value* pmfunc = dynamic_cast<const PM_Error_Value*>(&efunc);
      if (pmfunc==0) 
      {
          MultiFab* mf = derive(err_list[j].name(), time, err_list[j].nGrow());

          for (MFIter mfi(*mf); mfi.isValid(); ++mfi)
          {
              RealBox     gridloc = RealBox(grids[mfi.index()],geom.CellSize(),geom.ProbLo());
              itags               = tags[mfi.index()].tags();
              int*        tptr    = itags.dataPtr();
              const int*  tlo     = tags[mfi.index()].box().loVect();
              const int*  thi     = tags[mfi.index()].box().hiVect();
              const int*  lo      = mfi.validbox().loVect();
              const int*  hi      = mfi.validbox().hiVect();
              const Real* xlo     = gridloc.lo();
              Real*       dat     = (*mf)[mfi].dataPtr();
              const int*  dlo     = (*mf)[mfi].box().loVect();
              const int*  dhi     = (*mf)[mfi].box().hiVect();
              const int   ncomp   = (*mf)[mfi].nComp();
              
              err_list[j].errFunc()(tptr, ARLIM(tlo), ARLIM(thi), &tagval,
                                    &clearval, dat, ARLIM(dlo), ARLIM(dhi),
                                    lo,hi, &ncomp, domain_lo, domain_hi,
                                    dx, xlo, prob_lo, &time, &level);
                      
              //
              // Don't forget to set the tags in the TagBox.
              //
              tags[mfi.index()].tags(itags);
          }
          delete mf;
      }
      else {

          Real min_time = pmfunc->MinTime();
          Real max_time = pmfunc->MaxTime();
          int max_level = pmfunc->MaxLevel();

          if ( (max_level<0) || (max_level>parent->maxLevel()) ) {
              max_level = parent->maxLevel();
          }

          if ( ( (min_time>=max_time) || (min_time<=time) && (max_time>=time) )
               && (level<max_level) )
          {
              IntVect cumRatio = IntVect(D_DECL(1,1,1));
              for (int i=level; i<max_level; ++i) {
                  cumRatio *= parent->refRatio()[i];
              }
              
              const Geometry& fgeom = parent->Geom(max_level);
              const Real* dx_fine = fgeom.CellSize();
              const Real* plo = fgeom.ProbLo();

              const Array<const Region*>& my_regions = pmfunc->Regions();

              MultiFab* mf = 0;
              const std::string& name = err_list[j].name();

              if (!pmfunc->regionOnly())
                mf = derive(err_list[j].name(), time, err_list[j].nGrow());

              FArrayBox mask, cmask;
              for (MFIter mfi(tags); mfi.isValid(); ++mfi)
              {
                  TagBox& tagbox = tags[mfi];
                  const Box fine_box = Box(tagbox.box()).refine(cumRatio);
                  
                  mask.resize(fine_box,1); mask.setVal(0);                  
                  for (int j=0; j<my_regions.size(); ++j) {
                      my_regions[j]->setVal(mask,1,0,dx_fine,0);
                  }                  
                  coarsenMask(cmask,mask,cumRatio);

                  if (cmask.max()>0)
                  {
                      itags               = tags[mfi.index()].tags();
                      int*        tptr    = itags.dataPtr();
                      const int*  tlo     = tags[mfi.index()].box().loVect();
                      const int*  thi     = tags[mfi.index()].box().hiVect();
                      const Real* mdat    = cmask.dataPtr();
                      const int*  mlo     = cmask.box().loVect();
                      const int*  mhi     = cmask.box().hiVect();

                      if (pmfunc->regionOnly())
                      {
                          const Box& crse_box = cmask.box();
                          BL_ASSERT(crse_box == tagbox.box());
                          int numPts = crse_box.numPts();
                          for (int i=0; i<numPts; ++i) {
                              if (mdat[i]==1) {
                                  tptr[i] = tagval;
                              }
                          }
                      }
                      else {

                        const Box& vbox = grids[mfi.index()];
                          RealBox     gridloc = RealBox(vbox,geom.CellSize(),geom.ProbLo());
                          const int*  lo      = vbox.loVect();
                          const int*  hi      = vbox.hiVect();
                          const Real* xlo     = gridloc.lo();
                          Real*       dat     = (*mf)[mfi].dataPtr();
                          const int*  dlo     = (*mf)[mfi].box().loVect();
                          const int*  dhi     = (*mf)[mfi].box().hiVect();
                          const int   ncomp   = (*mf)[mfi].nComp();
                          
                          Real value = pmfunc->Value();

                          pmfunc->tagCells(tptr,ARLIM(tlo),ARLIM(thi),
                                           &tagval, &clearval, dat, ARLIM(dlo), ARLIM(dhi),
                                           mdat, ARLIM(mlo), ARLIM(mhi),
                                           lo,hi, &ncomp, domain_lo, domain_hi,
                                           dx, xlo, prob_lo, &time, &level);
                      }
                          
                      //
                      // Don't forget to set the tags in the TagBox.
                      //
                      tags[mfi.index()].tags(itags);
                  }
              }

              delete mf;
          }
      }
  }
#if 0
  //
  // Tag cells for refinement based on permeability values
  //
  if (do_kappa_refine == 1)
    { 
      Real kpset = 1.e-6;
      Array<int> itags;
      
      for (MFIter mfi(*kappa); mfi.isValid(); ++mfi)
	{
	  const int* k_lo  = (*kappa)[mfi].loVect();
	  const int* k_hi  = (*kappa)[mfi].hiVect();
	  const Real* kdat = (*kappa)[mfi].dataPtr();

	  itags            = tags[mfi.index()].tags();
	  const int* t_lo  = tags[mfi.index()].box().loVect();
	  const int* t_hi  = tags[mfi.index()].box().hiVect();
	  const int* tdat  = itags.dataPtr();

	  const int*  lo   = mfi.validbox().loVect();
	  const int*  hi   = mfi.validbox().hiVect();
	
	  FORT_KPERROR(tdat,ARLIM(t_lo),ARLIM(t_hi),
		       kdat,ARLIM(k_lo),ARLIM(k_hi),
		       &tagval, &kpset, dx, prob_lo, prob_hi,
		       lo, hi, domain_lo, domain_hi, &level);
	
	  tags[mfi.index()].tags(itags);
	}
    }
#endif
}

Real
PorousMedia::sumDerive (const std::string& name, Real time)
{
    Real      sum = 0.0;
    MultiFab* mf  = derive(name,time,0);

    BL_ASSERT(!(mf == 0));

    BoxArray baf;

    if (level < parent->finestLevel())
    {
        baf = parent->boxArray(level+1);
        baf.coarsen(fine_ratio);
    }

    for (MFIter mfi(*mf); mfi.isValid(); ++mfi)
    {
        FArrayBox& fab = mf->get(mfi);

        if (level < parent->finestLevel())
        {
            std::vector< std::pair<int,Box> > isects = baf.intersections(grids[mfi.index()]);

            for (int ii = 0, N = isects.size(); ii < N; ii++)
            {
                fab.setVal(0,isects[ii].second,0,fab.nComp());
            }
        }

        sum += fab.sum(0);
    }

    delete mf;

    ParallelDescriptor::ReduceRealSum(sum);

    return sum;
}

Real
PorousMedia::volWgtSum (const std::string& name,
			Real           time)
{
  Real        sum     = 0;
  const Real* dx      = geom.CellSize();
  MultiFab*   mf      = derive(name,time,0);

  BoxArray baf;

  if (level < parent->finestLevel())
  {
      baf = parent->boxArray(level+1);
      baf.coarsen(fine_ratio);
  }

  for (MFIter mfi(*mf); mfi.isValid(); ++mfi)
    {
      FArrayBox& fab = (*mf)[mfi];

      if (level < parent->finestLevel())
        {
            if (level < parent->finestLevel())
            {
                std::vector< std::pair<int,Box> > isects = baf.intersections(grids[mfi.index()]);

                for (int ii = 0, N = isects.size(); ii < N; ii++)
                {
                    fab.setVal(0,isects[ii].second,0,fab.nComp());
                }
            }
        }
      Real        s;
      const Real* dat = fab.dataPtr();
      const int*  dlo = fab.loVect();
      const int*  dhi = fab.hiVect();
      const int*  lo  = grids[mfi.index()].loVect();
      const int*  hi  = grids[mfi.index()].hiVect();

      FORT_SUMMASS(dat,ARLIM(dlo),ARLIM(dhi),ARLIM(lo),ARLIM(hi),dx,&s);

      sum += s;
    }

  delete mf;

  ParallelDescriptor::ReduceRealSum(sum);

  return sum;
}

void
PorousMedia::sum_integrated_quantities ()
{
  const int finest_level = parent->finestLevel();

  Real time = state[State_Type].curTime();
  Real mass = 0.0;
  Array<Real> tmoles(ntracers,0);

  for (int lev = 0; lev <= finest_level; lev++) {
    PorousMedia& ns_level = getLevel(lev);
    mass += ns_level.volWgtSum("Volumetric_Water_Content",time);
    for (int n=0; n<ntracers; ++n) {
      std::string VSC = "Volumetric_" + tNames[n] + "_Content";
      tmoles[n] += ns_level.volWgtSum(VSC,time);
    }
  }

  if (ParallelDescriptor::IOProcessor()) {

    std::ios_base::fmtflags oldflags = std::cout.flags(); std::cout << std::scientific << std::setprecision(10);
    std::cout << "  Volume integrated diagnostics:" << '\n';
    std::cout << "    TIME=" << time << "[s]  Inventory(Water)=" << mass*density[0] << "[kg]\n";
    for (int n=0; n<ntracers; ++n) {
      std::cout << "    TIME=" << time << "[s]  Inventory(" << tNames[n] << ")=" << tmoles[n] << "[moles]\n";
    }
    std::cout.flags(oldflags);
  }
}

void
PorousMedia::setPlotVariables()
{
    ParmParse pp("amr");

    // By default, do not add state variables
    if (pp.contains("plot_vars"))
    {
        std::string nm;
      
        int nPltVars = pp.countval("plot_vars");
      
        for (int i = 0; i < nPltVars; i++)
        {
            pp.get("plot_vars", nm, i);

            if (nm == "ALL") 
                parent->fillStatePlotVarList();
            else if (nm == "NONE")
                parent->clearStatePlotVarList();
            else
                parent->addStatePlotVar(nm);
        }
    }

    // Search for "ALL" in list
    bool has_all = false;
    Array<std::string> names_to_derive;

    if (pp.contains("derive_plot_vars"))
    {
        std::string nm;
      
        int nDrvPltVars = pp.countval("derive_plot_vars");
        names_to_derive.resize(nDrvPltVars);
        pp.getarr("derive_plot_vars",names_to_derive,0,nDrvPltVars);
      
        for (int i = 0; i < nDrvPltVars; i++)
        {
            if (names_to_derive[i] == "ALL") 
                has_all = true;
        }
    }

    if (has_all || names_to_derive.size()==0) {
        names_to_derive = UserDerives();
    }

    for (int i=0; i<names_to_derive.size(); ++i) {

        const std::string name = names_to_derive[i];

        if (derive_lst.canDerive(name)) {

            if (derive_lst.get(name)->deriveType() == IndexType::TheCellType())
            {
                parent->addDerivePlotVar(name);
            }
        }
    }
}

std::string
PorousMedia::thePlotFileType () const
{
  //
  // Increment this whenever the writePlotFile() format changes.
  //
  static const std::string the_plot_file_type("PorousMedia-V1.1");

  return the_plot_file_type;
}

void
PorousMedia::writePlotFile (const std::string& dir,
			    std::ostream&  os,
			    VisMF::How     how)
{
  if ( ! Amr::Plot_Files_Output() ) return;
  int i, n;
  //
  // The list of indices of State to write to plotfile.
  // first component of pair is state_type,
  // second component of pair is component # within the state_type
  //
  std::vector<std::pair<int,int> > plot_var_map;

  int noutput = desc_lst.size();
  for (int typ = 0; typ < noutput; typ++)
    for (int comp = 0; comp < desc_lst[typ].nComp();comp++)
      if (parent->isStatePlotVar(desc_lst[typ].name(comp)) &&
	  desc_lst[typ].getType() == IndexType::TheCellType())
	{
	  plot_var_map.push_back(std::pair<int,int>(typ,comp));
	}

  int num_derive = 0;
  std::list<std::string> derive_names;
  const std::list<DeriveRec>& dlist = derive_lst.dlist();

  for (std::list<DeriveRec>::const_iterator it = dlist.begin();
       it != dlist.end();
       ++it)
    {
      if (parent->isDerivePlotVar(it->name()))
	{
	  derive_names.push_back(it->name());
	  num_derive += it->numDerive();
	}
    }

  int n_data_items = plot_var_map.size() + num_derive;
  Real cur_time = state[State_Type].curTime();

  if (level == 0 && ParallelDescriptor::IOProcessor())
    {
      //
      // The first thing we write out is the plotfile type.
      //
      os << thePlotFileType() << '\n';

      if (n_data_items == 0)
	BoxLib::Error("Must specify at least one valid data item to plot");

      os << n_data_items << '\n';

      //
      // Names of variables -- first state, then derived
      //
      for (i =0; i < plot_var_map.size(); i++)
        {
	  int typ  = plot_var_map[i].first;
	  int comp = plot_var_map[i].second;
	  os << desc_lst[typ].name(comp) << '\n';
        }

      for (std::list<std::string>::const_iterator it = derive_names.begin();
	   it != derive_names.end();
	   ++it)
        {
	  const DeriveRec* rec = derive_lst.get(*it);
	  for (i = 0; i < rec->numDerive(); i++)
	    os << rec->variableName(i) << '\n';
        }
      os << BL_SPACEDIM << '\n';
      os << parent->cumTime() << '\n';
      int f_lev = parent->finestLevel();
      os << f_lev << '\n';
      for (i = 0; i < BL_SPACEDIM; i++)
	os << Geometry::ProbLo(i) << ' ';
      os << '\n';
      for (i = 0; i < BL_SPACEDIM; i++)
	os << Geometry::ProbHi(i) << ' ';
      os << '\n';
      for (i = 0; i < f_lev; i++)
	os << parent->refRatio(i)[0] << ' ';
      os << '\n';
      for (i = 0; i <= f_lev; i++)
	os << parent->Geom(i).Domain() << ' ';
      os << '\n';
      for (i = 0; i <= f_lev; i++)
	os << parent->levelSteps(i) << ' ';
      os << '\n';
      for (i = 0; i <= f_lev; i++)
        {
	  for (int k = 0; k < BL_SPACEDIM; k++)
	    os << parent->Geom(i).CellSize()[k] << ' ';
	  os << '\n';
        }
      os << (int) Geometry::Coord() << '\n';
      os << "0\n"; // Write bndry data.
    }
  // Build the directory to hold the MultiFab at this level.
  // The name is relative to the directory containing the Header file.
  //
  static const std::string BaseName = "/Cell";

  std::string Level = BoxLib::Concatenate("Level_", level, 1);
  //
  // Now for the full pathname of that directory.
  //
  std::string FullPath = dir;
  if (!FullPath.empty() && FullPath[FullPath.length()-1] != '/')
    FullPath += '/';
  FullPath += Level;
  //
  // Only the I/O processor makes the directory if it doesn't already exist.
  //
  if (ParallelDescriptor::IOProcessor())
    if (!BoxLib::UtilCreateDirectory(FullPath, 0755))
      BoxLib::CreateDirectoryFailed(FullPath);
  //
  // Force other processors to wait till directory is built.
  //
  ParallelDescriptor::Barrier();

  if (ParallelDescriptor::IOProcessor())
    {
      os << level << ' ' << grids.size() << ' ' << cur_time << '\n';
      os << parent->levelSteps(level) << '\n';

      for (i = 0; i < grids.size(); ++i)
        {
	  RealBox gridloc = RealBox(grids[i],geom.CellSize(),geom.ProbLo());
	  for (n = 0; n < BL_SPACEDIM; n++)
	    os << gridloc.lo(n) << ' ' << gridloc.hi(n) << '\n';
        }
      //
      // The full relative pathname of the MultiFabs at this level.
      // The name is relative to the Header file containing this name.
      // It's the name that gets written into the Header.
      //
      if (n_data_items > 0)
        {
	  std::string PathNameInHeader = Level;
	  PathNameInHeader += BaseName;
	  os << PathNameInHeader << '\n';
        }
    }

  //
  // We combine all of the multifabs -- state, derived, etc -- into one
  // multifab -- plotMF.
  // NOTE: we are assuming that each state variable has one component,
  // but a derived variable is allowed to have multiple components.
  int       cnt   = 0;
  int       ncomp = 1;
  const int nGrow = 0;
  MultiFab  plotMF(grids,n_data_items,nGrow);
  MultiFab* this_dat = 0;
  //
  // Cull data from state variables -- use no ghost cells.
  //
  for (i = 0; i < plot_var_map.size(); i++)
    {
      int typ  = plot_var_map[i].first;
      int comp = plot_var_map[i].second;
      this_dat = &state[typ].newData();
      MultiFab::Copy(plotMF,*this_dat,comp,cnt,ncomp,nGrow);
      cnt+= ncomp;
    }
  //
  // Cull data from derived variables.
  // 
  Real plot_time;

  if (derive_names.size() > 0)
    {
      for (std::list<std::string>::const_iterator it = derive_names.begin();
	   it != derive_names.end();
	   ++it) 
	{
	  plot_time = cur_time;
	  const DeriveRec* rec = derive_lst.get(*it);
	  ncomp = rec->numDerive();
	  MultiFab* derive_dat = derive(*it,plot_time,nGrow);
	  MultiFab::Copy(plotMF,*derive_dat,0,cnt,ncomp,nGrow);
	  delete derive_dat;
	  cnt += ncomp;
	}
    }
  //
  // Use the Full pathname when naming the MultiFab.
  //
  std::string TheFullPath = FullPath;
  TheFullPath += BaseName;
  VisMF::Write(plotMF,TheFullPath,how,true);
}

Real
PorousMedia::estTimeStep (MultiFab* u_mac)
{
  if (fixed_dt > 0.0)
    {
      Real factor = 1.0;

      if (!(level == 0))
        {
	  int ratio = 1;
	  for (int lev = 1; lev <= level; lev++)
            {
	      ratio *= parent->nCycle(lev);
            }
	  factor = 1.0/double(ratio);
        }

      return factor*fixed_dt;
    }

  Real estdt        = 1.0e+20; // FIXME: need more robust
  const Real cur_time = state[State_Type].curTime();

  if (solute_transport_limits_dt) {

      if (dt_eig != 0.0)
      {
          if (cfl>0) {
              estdt = cfl * dt_eig;
          }
          else
              estdt = dt_eig;
      } 
      else 
      {
          int making_new_umac = 0;
          
          // Need to define the MAC velocities in order to define the initial dt 
          if (u_mac == 0) 
          {
              making_new_umac = 1;
              
              u_mac = new MultiFab[BL_SPACEDIM];
              for (int dir = 0; dir < BL_SPACEDIM; dir++)
              {
                  BoxArray edge_grids(grids);
                  edge_grids.surroundingNodes(dir);
                  u_mac[dir].define(edge_grids,1,0,Fab_allocate);
                  u_mac[dir].setVal(0.);
              }
#ifdef MG_USE_FBOXLIB
              if (model == PM_RICHARDS) {
                  if (!steady_use_PETSc_snes) {
                      compute_vel_phase(u_mac,0,cur_time);
                  }
              }
              else
#endif
		if ( (model == PM_STEADY_SATURATED)
                     || (model == PM_SATURATED) ) {
		  set_vel_from_bcs(PMParent()->startTime(),u_mac);
		} else 
              {
                  MultiFab* RhoD;
                  RhoD  = new MultiFab[BL_SPACEDIM];
                  for (int dir = 0; dir < BL_SPACEDIM; dir++)
                  {
                      BoxArray edge_grids(grids);
                      edge_grids.surroundingNodes(dir);
                      RhoD[dir].define(edge_grids,1,0,Fab_allocate);
                      RhoD[dir].setVal(0.);
                  }
                  
                  initial_mac_project(u_mac,RhoD,cur_time);
                  delete [] RhoD;
              }
          }
          
          
          predictDT(u_mac,cur_time);
          if (diffuse_tracers && be_cn_theta_trac==0) {
            Real dt_diff = predictDT_diffusion_explicit(cur_time);
            dt_eig = std::min(dt_diff, dt_eig);
          }
          
	  estdt = (cfl > 0  ?  cfl  :  1) *dt_eig;

          if (making_new_umac)
              delete [] u_mac;
      }
  }

  // 
  // Limit by max_dt
  //
#ifdef MG_USE_FBOXLIB
  if (model == PM_RICHARDS) {
      Real richard_max_dt = (initial_iter  ?  steady_richard_max_dt  :  transient_richard_max_dt);
      if (richard_max_dt>0) {
          estdt = std::min(richard_max_dt,estdt);
      }
  }  
#endif

  return estdt;
}

Real
PorousMedia::initialTimeStep (MultiFab* u_mac)
{
    Real dt_0;

    if (dt_init>0) {
        dt_0 = dt_init;
    }
    else {
        dt_0 = estTimeStep(u_mac);
    }

    const Real cur_time = state[State_Type].curTime();
    Real stop_time = PMParent()->StopTime();
    if (stop_time > cur_time) {
        dt_0 = std::min(dt_0, stop_time - cur_time);
    }
    
    return dt_0;
}

void
PorousMedia::predictDT (MultiFab* u_macG, Real t_eval)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::predictDT()");

  const Real* dx       = geom.CellSize();

  dt_eig = 1.e20; // FIXME: Need more robust
  
  Real eigmax[BL_SPACEDIM] = { D_DECL(0,0,0) };

  MultiFab RhoSat(grids,ncomps,nGrowEIGEST);
  get_fillpatched_rhosat(t_eval,RhoSat,nGrowEIGEST);

  // Find Ung
  int Ung = -1;
  for (int i=0; i<10; ++i) {
    if (BoxLib::surroundingNodes(BoxLib::grow(grids[0],i),0) == u_macG[0][0].box()) {
      Ung = i;
    }
  }
  BL_ASSERT(Ung>=0);
  int Uidx = 0;

  for (MFIter mfi(RhoSat); mfi.isValid(); ++mfi) {
    const int i = mfi.index();

    Array<int> state_bc;
    state_bc = getBCArray(State_Type,i,0,1);
    
    Real eigmax_m[BL_SPACEDIM] = {D_DECL(0,0,0)};
    
    if (model == PM_SINGLE_PHASE) {
      godunov->esteig_lin (grids[i], D_DECL(u_macG[0][i],u_macG[1][i],u_macG[2][i]),(*rock_phi)[i], eigmax_m);
    }
    else if (model == PM_TWO_PHASE) {

      const int n_kr_coef = kr_coef->nComp();
      if (do_cpl_advect) {
	godunov->esteig_cpl (grids[i], dx, 
			     u_macG[0][i], kpedge[0][i],
			     u_macG[1][i], kpedge[1][i],
#if BL_SPACEDIM == 3
			     u_macG[2][i], kpedge[2][i],
#endif
			     RhoSat[mfi], (*pcnp1_cc)[i],
			     (*rock_phi)[i], 
			     (*kr_coef)[i], n_kr_coef,
			     state_bc.dataPtr(),eigmax_m);
      } else {
	godunov->esteig (grids[i], dx, 
			 u_macG[0][i], kpedge[0][i],
			 u_macG[1][i], kpedge[1][i],
#if BL_SPACEDIM==3
			 u_macG[2][i],kpedge[2][i],
#endif
			 RhoSat[mfi],(*rock_phi)[i], 
			 (*kr_coef)[i], n_kr_coef,
			 state_bc.dataPtr(),eigmax_m);
      }
    }

    if (advect_tracers > 0) {
#if BL_SPACEDIM==2
      RhoSat[mfi].mult(1/density[0],0,ncomps);
      Advection::EstimateMaxEigenvalues(RhoSat[mfi],0,RhoSat.nGrow(),
                                        D_DECL(u_macG[0][i],u_macG[1][i],u_macG[2][i]),0,Ung,
                                        (*rock_phi)[i],rock_phi->nGrow(),
                                        grids[i],eigmax_m);
      RhoSat[mfi].mult(density[0],0,ncomps);
#else
      godunov->esteig_trc (grids[i], D_DECL(u_macG[0][i],
					    u_macG[1][i],
					    u_macG[2][i]),
			   RhoSat[mfi],1,(*rock_phi)[i] ,eigmax_m);
#endif
    }

    for (int dir = 0; dir < BL_SPACEDIM; dir++) {
      eigmax[dir] = std::max(eigmax[dir],eigmax_m[dir]);
      if (eigmax_m[dir] > 1.e-15) {
	dt_eig = std::min(dt_eig,dx[dir]/eigmax_m[dir]);
      }
    }
  }
  ParallelDescriptor::ReduceRealMin(dt_eig);

  if (ParallelDescriptor::IOProcessor() && verbose>0) {
    std::cout << "  TRANSPORT: Level: " << level << " CFL dt limit = " << dt_eig << '\n';
  }

  if (verbose > 3) {
    const int IOProc   = ParallelDescriptor::IOProcessorNumber();
    ParallelDescriptor::ReduceRealMax(&eigmax[0], BL_SPACEDIM, IOProc);

    if (ParallelDescriptor::IOProcessor()) {
      for (int dir = 0; dir < BL_SPACEDIM; dir++)
	std::cout << "Max Eig in dir " << dir << " = " << eigmax[dir] << '\n';
    }
  }
}

Real
PorousMedia::predictDT_diffusion_explicit (Real      t_eval,
                                           MultiFab* saturation)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::predictDT_diffusion_explicit()");
  BL_ASSERT(diffuse_tracers);

  int first_tracer = ncomps;
  calcDiffusivity(t_eval,first_tracer,ntracers);
  
  MultiFab** diff_edge  = 0;
  diffusion->allocFluxBoxesLevel(diff_edge,0,1);

  MultiFab* sptr = saturation;
  if (sptr == 0) {
    MultiFab Slocal(grids,ncomps,nGrowEIGEST);
    // FIXME: Note only one component (water) assumed here
    int wComp = 0;
    for (FillPatchIterator S_fpi(*this,get_new_data(State_Type),nGrowEIGEST,
                                 t_eval,State_Type,wComp,1); S_fpi.isValid(); ++S_fpi) {
      FArrayBox& psv = Slocal[S_fpi];
      int i = S_fpi.index();
      const FArrayBox& phi = (*rock_phi)[i];
      const FArrayBox& vol = volume[i];
      psv.copy(S_fpi(),0,0,1);
      psv.mult(1/density[wComp]);
      psv.mult(phi,0,0,1);
      psv.mult(vol,0,0,1);
    }
    sptr = &Slocal;
  }

  const Real* dx = geom.CellSize();
  Real dt_diff = 1.e20; // FIXME: Need more robust
  FArrayBox psad[BL_SPACEDIM];

  for (int n=0; n<ntracers; ++n) {
  
    getDiffusivity(diff_edge, t_eval, first_tracer+n, 0, 1);

    for (MFIter mfi(*sptr); mfi.isValid(); ++mfi) {

      const Box& box = mfi.validbox();
      int i = mfi.index();
      const FArrayBox& psv = (*sptr)[mfi];

      for (int d=0; d<BL_SPACEDIM; ++d) {
        const Box ebox = BoxLib::surroundingNodes(box,d);
        psad[d].resize(ebox,1);
        psad[d].copy(area[d][i]);
        psad[d].mult((*diff_edge[d])[i],0,0,1);
      }

      FORT_MAX_TRACDIFF_DT(box.loVect(), box.hiVect(),
                           psv.dataPtr(),     ARLIM(psv.loVect()),    ARLIM(psv.hiVect()),
                           psad[0].dataPtr(), ARLIM(psad[0].loVect()),ARLIM(psad[0].hiVect()),
                           psad[1].dataPtr(), ARLIM(psad[1].loVect()),ARLIM(psad[1].hiVect()),
#if (BL_SPACEDIM==3)
                           psad[2].dataPtr(), ARLIM(psad[2].loVect()),ARLIM(psad[2].hiVect()),
#endif
                           dx, &dt_diff);
    }
  }
  ParallelDescriptor::ReduceRealMin(dt_diff);
  diffusion->removeFluxBoxesLevel(diff_edge);
  return dt_diff;
}

Real
PorousMedia::GetUserInputInitDt()
{
    Real user_input_dt_init = -1;

    if (execution_mode==INIT_TO_STEADY)
    {
        Real cum_time = parent->cumTime(); // Time evolved to so far
        Real start_time = parent->startTime(); // Time simulation started from
        if (switch_time <= start_time  || cum_time > switch_time ) {
          user_input_dt_init = dt_init;
        } else {
          user_input_dt_init = steady_init_time_step;
        }
    } 
    else 
    {
        user_input_dt_init = execution_mode==TRANSIENT  ?  dt_init  :  steady_init_time_step;
    }
    return user_input_dt_init;        
}

void
PorousMedia::computeNewDt (int                   finest_level,
                           int                   sub_cycle,
                           Array<int>&           n_cycle,
                           const Array<IntVect>& ref_ratio,
                           Array<Real>&          dt_min,
                           Array<Real>&          dt_level,
                           Real                  stop_time,
                           int                   post_regrid_flag)
{
  //
  // We are at the end of a coarse grid timecycle.
  // Compute the timesteps for the next iteration.
  //
  if (level > 0) return;

  // Time step possibly affected/controlled by:
  // 1) CFL stability of solute transport - Relevant only if advect_tracers is true
  //      
  // 2) Solver for flow - In this case, we can extract nothing useful from the state, the dt 
  //          is set by the dynamics of the solver.  Thus, we record the dt last time we left 
  //          the solver.  If nothing else is at play, we use this
  // 
  // 3) User input dt_fixed
  //
  // 4) If regrided, assume that we computed a dt_min prior (in dt_min) and enforce
  //
  // 5) Bounded growth/decrease via user input
  //

  bool start_with_previously_suggested_dt = true;
  bool check_for_dt_cut_by_event = true;
  bool in_transient_period = false;

  int  tpc_interval = -1;
  Real dt_event = -1;
  Real dt_prev_suggest = -1;
  Real dt_eig_local = -1;
  Real dt_init_local = -1;

  Real dt_0;
  int max_level = parent->maxLevel();

  Real cum_time = parent->cumTime(); // Time evolved to so far
  Real start_time = parent->startTime(); // Time simulation started from
 
  if (fixed_dt > 0) {
      dt_0 = fixed_dt;
  }
  else
  {
      if (post_regrid_flag != 1)
      {
          if (start_with_previously_suggested_dt) {
              dt_prev_suggest = PMParent()->Dt0FromPreviousAdvance();
          }
          
          if (check_for_dt_cut_by_event) {
              dt_event = PMParent()->Dt0BeforeEventCut();
          }
      }
          
      // Compute CFL stability for solutes
      if (solute_transport_limits_dt && ntracers>0 && do_tracer_advection)
      {
          if (execution_mode!=INIT_TO_STEADY || (state[State_Type].curTime() >= switch_time)) {
              PorousMedia* pm0 = dynamic_cast<PorousMedia*>(&parent->getLevel(0));
              dt_eig_local = pm0->estTimeStep(pm0->u_mac_curr);
              int n_factor = 1;
              for (int i = 1; i <= finest_level; i++)
              {
                  n_factor *= n_cycle[i];
                  PorousMedia* pm = dynamic_cast<PorousMedia*>(&parent->getLevel(i));
                  dt_eig_local = std::min(dt_eig_local,pm->estTimeStep(pm->u_mac_curr) * n_factor);
              }
          }
      }

      Real transient_start = (execution_mode==INIT_TO_STEADY ? switch_time : start_time);
      in_transient_period = cum_time >= transient_start;
      if (cum_time == transient_start) {
	dt_init_local = GetUserInputInitDt();
      }
  }

  // Now implement rules to pick dt
  if (dt_init_local > 0) 
  {
      dt_0 = dt_init_local;
  }
  else 
  {
      Real dt_previously_taken = dt_level[0];
      if (start_with_previously_suggested_dt && dt_prev_suggest>0) 
      {
          dt_0 = dt_prev_suggest;
      }
      else {
	dt_0 = dt_previously_taken;
      }
      
      if (check_for_dt_cut_by_event && dt_event>0) {
	dt_0 = dt_event;
      }

      if (in_transient_period) 
      {
          if (dt_grow_max >= 1) {
              dt_0 = std::min(dt_0, dt_grow_max * dt_previously_taken);
          }
          if (dt_shrink_max >0  && dt_shrink_max <= 1) {
              dt_0 = std::max(dt_0, dt_shrink_max * dt_previously_taken);
          }
          if (model == PM_RICHARDS && transient_richard_max_dt > 0) {
              dt_0 = std::min(transient_richard_max_dt,dt_0);
          }
      }
      else {
          if (model == PM_RICHARDS && steady_richard_max_dt > 0) {
              dt_0 = std::min(steady_richard_max_dt,dt_0);
          }
      }

      if (solute_transport_limits_dt && dt_eig_local>0) {
          dt_0 = std::min(dt_0, dt_eig_local);
      }
  }

  int n_factor = 1;
  for (int i = 0; i <= max_level; i++)
  {
      n_factor   *= n_cycle[i];
      dt_level[i] = dt_0/( (Real)n_factor );
  }
}

void
PorousMedia::computeInitialDt (int                   finest_level,
                               int                   sub_cycle,
                               Array<int>&           n_cycle,
                               const Array<IntVect>& ref_ratio,
                               Array<Real>&          dt_level, 
                               Real                  stop_time)
{
  //
  // Grids have been constructed, compute dt for all levels.
  //
  if (level > 0)
    return;

  if (verbose>3 && ParallelDescriptor::IOProcessor())
    std::cout << "... computing dt at level 0 only in computeInitialDt\n";

  const int max_level = parent->maxLevel();

  n_cycle[0] = 1;
  for (int i = 1; i <= max_level; i++)
    {
      n_cycle[i] = sub_cycle ? parent->MaxRefRatio(i-1) : 1;
    }

  Real cum_time = parent->cumTime(); // Time evolved to so far
  Real start_time = parent->startTime(); // Time simulation started from
  
  Real dt_0 = GetUserInputInitDt();

  if (dt_0 < 0) {
      dt_0 = 1.0e100;
      int n_factor = 1;
      for (int i = 0; i <= finest_level; i++)
      {
          
          const PorousMedia* pm = dynamic_cast<const PorousMedia*>(&parent->getLevel(i));
          dt_level[i] = getLevel(i).initialTimeStep(pm->u_mac_curr);
          n_factor   *= n_cycle[i];
          dt_0        = std::min(dt_0,n_factor*dt_level[i]);
      }
  }

  if (init_shrink>0) 
      dt_0 *= init_shrink;

  int n_factor = 1;
  for (int i = 0; i <= max_level; i++)
    {
      n_factor   *= n_cycle[i];
      dt_level[i] = dt_0/( (Real)n_factor );
    }
}

//
// This function estimates the initial timesteping used by the model.
//

void
PorousMedia::post_init_estDT (Real&        dt_init_local,
                              Array<int>&  nc_save,
                              Array<Real>& dt_save,
                              Real         stop_time)
{
  const Real strt_time    = parent->startTime();
  const int  finest_level = parent->finestLevel();

  if (verbose>3 && ParallelDescriptor::IOProcessor())
    std::cout << "... computing dt at all levels in post_init_estDT\n";

  dt_init_local = GetUserInputInitDt();

  if (dt_init_local < 0) {

    // Start with entire interval
    dt_init_local = stop_time - parent->startTime();

    int n_factor_tot = 1;
    for (int k = 0; k <= finest_level; k++) {
      PorousMedia* pm_lev = dynamic_cast<PorousMedia*>(&parent->getLevel(k));
      dt_save[k] = getLevel(k).initialTimeStep(pm_lev->UMac_Curr());      
      n_factor_tot *= parent->nCycle(k);

      int n_factor = 1;
      for (int m = finest_level; m > k; m--) {
	n_factor *= parent->nCycle(m);
      }
      dt_init_local = std::min( dt_init_local, dt_save[k]/((Real) n_factor) );
    }

    dt_init_local *= n_factor_tot;
  }

  // Make something workable if stop>=start
  if (stop_time <= parent->startTime())
  {
      dt_init_local = std::abs(dt_init);
  }

  BL_ASSERT(dt_init_local != 0);

  int n_factor = 1;
  dt_save[0] = dt_init_local;
  for (int k = 0; k <= finest_level; k++)
  {
      nc_save[k] = parent->nCycle(k);
      n_factor  *= nc_save[k];
      dt_save[k] = dt_init_local/( (Real) n_factor);
  }

  parent->setDtLevel(dt_save);
  parent->setNCycle(nc_save);
  for (int k = 0; k <= finest_level; k++)
    {
      getLevel(k).setTimeLevel(strt_time,dt_init_local,dt_init_local);
    }
}

//
// Fills in amrLevel okToContinue.
//

int
PorousMedia::okToContinue ()
{
  bool ret = true;
  std::string reason_for_stopping = "n/a";
  bool successfully_completed = false;

  if (level == 0) {
    if (parent->dtLevel(0) <= dt_cutoff) {
      ret = false; reason_for_stopping = "Dt at level 0 too small";
    }

    int max_step = PMParent()->MaxStep();
    if (parent->levelSteps(0) >= max_step) {
      ret = false; reason_for_stopping = "Hit maximum allowed time steps";
      successfully_completed = true;
    }

    Real stop_time = PMParent()->StopTime();
    if (parent->cumTime() >= stop_time) {
      ret = false; reason_for_stopping = "Hit maximum allowed time";
      successfully_completed = true;
    }

    if (!ret) {
      //
      // Print final solutions
      //
      if (verbose > 3) {      
        for (int lev = 0; lev <= parent->finestLevel(); lev++) {
          if (ParallelDescriptor::IOProcessor()) {
            std::cout << "Final solutions at level = " << lev << '\n';
          }
          getLevel(lev).check_minmax();                     
        }
      }
            
      //
      // Compute observations
      //
      Observation::setPMAmrPtr(PMParent());
      PArray<Observation>& observations = PMParent()->TheObservations();
      if (successfully_completed  &&  ParallelDescriptor::IOProcessor()) {
        if (observations.size()) {
          std::cout << "Computed observations:\n";
          for (int i=0; i<observations.size(); ++i) {
            const std::map<int,Real> vals = observations[i].vals;
            for (std::map<int,Real>::const_iterator it=vals.begin();it!=vals.end(); ++it) {
              int j = it->first;
              std::string& name = AMR_to_Amanzi_label_map[observations[i].name];
              std::cout << i << ", " << name << ", " 
                        << j << ", " << observations[i].times[j] << ", "
                        << it->second << std::endl;
            }
          }
          std::cout << "\n";
        }
      }
    }
    
    if (!ret && verbose > 1 && ParallelDescriptor::IOProcessor()) {
      std::cout << "Stopping simulation: " << reason_for_stopping << std::endl;
    }
  }
  return ret;
}

//
// THE MAIN HOOKS INTO AMR AND AMRLEVEL
//

//
// Integration cycle on fine level grids is complete .
// post_timestep() is responsible for syncing levels together.
//
// The registers used for level syncing are initialized in the
// coarse level advance and incremented in the fine level advance.
// These quantities are described in comments above advance_setup.
//

void
PorousMedia::post_timestep (int crse_iteration)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::post_timestep()");

#ifdef MG_USE_FBOXLIB
  if (model == PM_RICHARDS && !do_multilevel_full) {
    richard_sync();
  }
  else 
#endif
  {
    if (do_reflux) {
      
      if (level < parent->finestLevel()) {
        
        if (Ssync==0) {
          Ssync = new MultiFab(grids,NUM_SCALARS,1);
	}
        Ssync->setVal(0);
	reflux();
        avgDown();
	mac_sync();
      }
    }
  }

  old_intersect_new          = grids;
  is_first_step_after_regrid = false;
}

PMAmr*
PorousMedia::PMParent()
{
  PMAmr* pm_parent = dynamic_cast<PMAmr*>(parent);
  if (!pm_parent) {
    BoxLib::Abort("Bad cast");
  }
  return pm_parent;
}

const PMAmr*
PorousMedia::PMParent() const
{
  const PMAmr* pm_parent = dynamic_cast<const PMAmr*>(parent);
  if (!pm_parent) {
    BoxLib::Abort("Bad cast");
  }
  return pm_parent;
}

//
// Build any additional data structures after restart.
//
void PorousMedia::post_restart()
{
  if (level==0)
  {
      PMParent()->GetLayout().Build();
  }

  init_rock_properties();

  if (level == 0)
    {
      Observation::setPMAmrPtr(PMParent());
      Real prev_time = state[State_Type].prevTime();
      Real curr_time = state[State_Type].curTime();
      PArray<Observation>& observations = PMParent()->TheObservations();
      for (int i=0; i<observations.size(); ++i)
          observations[i].process(prev_time, curr_time, parent->levelSteps(0));
    }
}

//
// Build any additional data structures after regrid.
//
void
PorousMedia::post_regrid (int lbase,
                          int new_finest)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::post_regrid()");

  if (level == lbase  && steady_use_PETSc_snes) {

    // NOTE: If grids change at any level, the layout (and RS) is no longer valid
    Layout& layout = PMParent()->GetLayout();
    PMAmr* pm_parent = PMParent();
    int new_nLevs = new_finest - lbase + 1;

    if (!(layout.IsCompatible(pm_parent,new_nLevs))) {
      bool rebuild_rs = false;
      if (richard_solver != 0) {
        delete richard_solver;
        rebuild_rs = true;
      }

      bool rebuild_rsc = false;
      if (richard_solver_control != 0) {
        delete richard_solver_control;
        rebuild_rsc = true;
      }

      bool rebuild_rsd = false;
      if (richard_solver_data != 0) {
        delete richard_solver_data;
        rebuild_rsd = true;
      }

      layout.Build(); // Internally destroys itself on rebuild
      if (rebuild_rsc) {richard_solver_control = new NLScontrol();}
      if (rebuild_rsd) {
        richard_solver_data = new RSAMRdata(0,new_nLevs,layout,pm_parent,*richard_solver_control,rock_manager);
      }
      if (rebuild_rs) {
        BuildNLScontrolData(*richard_solver_control,*richard_solver_data,"Flow_PK");
        richard_solver = new RichardSolver(*richard_solver_data,*richard_solver_control);
      }
    }
  }
}

void 
PorousMedia::init_rock_properties ()
{
  int nGrow = materialID->nGrow();
  bool ignore_mixed = true;
  rock_manager->GetMaterialID(level,*materialID,nGrow,ignore_mixed);

  const Real* dx = geom.CellSize();
  const int max_level = parent->maxLevel();
  Real cur_time = state[State_Type].curTime();

  MultiFab kappatmp(grids,BL_SPACEDIM,nGrowHYP);
  bool ret = rock_manager->GetProperty(cur_time,level,kappatmp,"permeability",0,kappatmp.nGrow());
  if (!ret) BoxLib::Abort("Failed to build permeability");
  for (MFIter mfi(kappatmp); mfi.isValid(); ++mfi) {
    const Box& cbox = mfi.validbox();
    const FArrayBox& cdat = kappatmp[mfi];
    for (int d=0; d<BL_SPACEDIM; ++d) {
      Box ebox = Box(cbox).surroundingNodes(d);
      FArrayBox& edat = kpedge[d][mfi];
      BL_ASSERT(edat.box().contains(ebox));
      FORT_INITKEDGE(cdat.dataPtr(),ARLIM(cdat.loVect()),ARLIM(cdat.hiVect()),
                     edat.dataPtr(),ARLIM(edat.loVect()),ARLIM(edat.hiVect()),
                     cbox.loVect(),cbox.hiVect(),&d);
    }
  }
      
  kappa->setVal(0.);
  for (int d=0; d<BL_SPACEDIM; d++) {
    MultiFab::Add(*kappa,kappatmp,d,0,1,kappa->nGrow());
  }
  kappa->mult(1.0/BL_SPACEDIM);

  rock_manager->Porosity(cur_time,level,*rock_phi,0,rock_phi->nGrow());

  if ( (model != PM_SINGLE_PHASE)
       && (model != PM_SINGLE_PHASE_SOLID)
       && (model != PM_STEADY_SATURATED)
       && (model != PM_SATURATED) ) {

    // FIXME: Fix up covered cells, averaged kr params make no sense
    MultiFab pcParams(grids,rock_manager->NComp("capillary_pressure"),kr_coef->nGrow());
    bool ignore_mixed = true;
    bool retKr = rock_manager->GetProperty(state[State_Type].curTime(),level,pcParams,
                                      "capillary_pressure",0,kr_coef->nGrow(),0,ignore_mixed);
    if (!retKr) BoxLib::Abort("capillary_pressure");
    kr_coef->setVal(3.0,0,1,kr_coef->nGrow());
    MultiFab::Copy(*kr_coef,pcParams,1,1,1,kr_coef->nGrow()); // "m"
    MultiFab::Copy(*kr_coef,pcParams,3,2,1,kr_coef->nGrow()); // "Sr"

    // FIXME: Fix up covered cells, averaged cpl params make no sense
    //bool retCpl = rock_manager->GetProperty(state[State_Type].curTime(),level,*cpl_coef,
    //                                   "capillary_pressure",0,cpl_coef->nGrow(),0,ignore_mixed);
    //if (!retCpl) BoxLib::Abort("Failed to build capillary_pressure");
    cpl_coef->setVal(3.0,0,1,kr_coef->nGrow());
    MultiFab::Copy(*cpl_coef,pcParams,1,1,1,kr_coef->nGrow()); // "m"
    MultiFab::Copy(*cpl_coef,pcParams,2,2,1,kr_coef->nGrow()); // "sigma" ("alpha")
    MultiFab::Copy(*cpl_coef,pcParams,3,3,1,kr_coef->nGrow()); // "Sr"
  }

  if (model == PM_SATURATED) {
    if (rock_manager->CanDerive("specific_storage")) {
      bool retSs = rock_manager->GetProperty(state[State_Type].curTime(),level,*specific_storage,
                                        "specific_storage",0,specific_storage->nGrow());
      if (!retSs) BoxLib::Abort("Failed to build specific_storage");
    }
    else {
      specific_storage->setVal(0);
    }
  }
}

//
// Ensure state, and pressure are consistent.
//

void
PorousMedia::post_init (Real stop_time)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::post_init()");

  if (level > 0)
    //
    // Nothing to sync up at level > 0.
    //
    return;

  const int   finest_level = parent->finestLevel();
  Real        dt_init_local = 0.;
  Array<Real> dt_save(finest_level+1);
  Array<int>  nc_save(finest_level+1);

  //
  // Ensure state is consistent, i.e. velocity field is non-divergent,
  // Coarse levels are fine level averages, pressure is zero.
  // Call initial_mac_project in order to get a good initial dt.
  //
  post_init_state();

  post_init_estDT(dt_init_local, nc_save, dt_save, stop_time);

  const Real strt_time = parent->startTime();

  for (int k = 0; k <= finest_level; k++)
    getLevel(k).setTimeLevel(strt_time,dt_save[k],dt_save[k]);

  parent->setDtLevel(dt_save);
  parent->setNCycle(nc_save);

  //
  // Compute the initial estimate of conservation.
  //
  if (sum_interval > 0)
    sum_integrated_quantities();

  if (level == 0)
    {
      Observation::setPMAmrPtr(PMParent());
      Real prev_time = state[State_Type].prevTime();
      Real curr_time = state[State_Type].curTime();
      PArray<Observation>& observations = PMParent()->TheObservations();
      for (int i=0; i<observations.size(); ++i)
          observations[i].process(prev_time, curr_time, parent->levelSteps(0));
    }
}

//
// MULTILEVEL SYNC FUNCTIONS
//


//
// This function ensures that the state is initially consistent
// with respect to the divergence condition and fields are initially consistent
//

void
PorousMedia::post_init_state ()
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::post_init_state()");

  int  finest_level = parent->finestLevel();
  for (int lev=0;lev<= finest_level;lev++)
  {
    PorousMedia& pm = getLevel(lev);
    for (int i = 0; i < num_state_type; i++)
    {
      pm.state[i].allocOldData();
      MultiFab& od = pm.get_old_data(i);
      MultiFab& nd = pm.get_new_data(i);
      MultiFab::Copy(od,nd,0,0,nd.nComp(),0);
    }
  }

  // Multilevel pressure/velocity initialization for Richards or for saturated
  if ( (model == PM_RICHARDS)
       || (model == PM_STEADY_SATURATED && !do_constant_vel)
       || (model == PM_SATURATED) ) {
    
    PMAmr* pmamr = PMParent();
    int  finest_level = parent->finestLevel();

    if (do_richard_init_to_steady) {
      // Solve for p,v
      richard_init_to_steady();        
    }
    else 
    {
      if (steady_use_PETSc_snes) {
        // Compute initial velocity field based on given p field
        NLScontrol nlsc_init;
	RSAMRdata rs_data(0,pmamr->finestLevel(),pmamr->GetLayout(),pmamr,nlsc_init,rock_manager);
        BuildNLScontrolData(nlsc_init,rs_data,"Init_Velocity");
        RichardSolver rs(rs_data,nlsc_init);
        rs.ResetRhoSat();
        rs.ComputeDarcyVelocity(rs.GetPressureNp1(),pmamr->startTime());
      }
    }

    // Velocity currently in u_mac_curr, form u_macG_curr
    for (int lev = 0; lev <= finest_level; lev++) {
      PorousMedia* pm = dynamic_cast<PorousMedia*>(&parent->getLevel(lev));
      BL_ASSERT(pm);
      
      if (pm->u_macG_curr == 0) {
        pm->u_macG_curr = pm->AllocateUMacG();
      }
      
      if (lev == 0) {
        pm->create_umac_grown(pm->u_mac_curr,pm->u_macG_curr);
      } else {
        PArray<MultiFab> u_macG_crse(BL_SPACEDIM,PArrayManage);
        pm->GetCrseUmac(u_macG_crse,pmamr->startTime());
        pm->create_umac_grown(pm->u_mac_curr,u_macG_crse,pm->u_macG_curr); 
      }

      // Copy u_macG_curr to u_macG_prev and u_macG_trac
      if (pm->u_macG_prev == 0) {
        pm->u_macG_prev = pm->AllocateUMacG();
      }
      if (pm->u_macG_trac == 0) {
        pm->u_macG_trac = pm->AllocateUMacG();
      }
      
      // Initialize u_mac_prev and u_macG_trac
      for (int d=0; d<BL_SPACEDIM; ++d) {
        MultiFab::Copy(pm->u_macG_prev[d],pm->u_macG_curr[d],0,0,1,pm->u_macG_curr[d].nGrow());
        MultiFab::Copy(pm->u_macG_trac[d],pm->u_macG_curr[d],0,0,1,pm->u_macG_curr[d].nGrow());
      }
      
    }

    // Build a RS for the Flow_PK
    richard_solver_control = new NLScontrol();
    Layout& layout = PMParent()->GetLayout();
    PMAmr* pm_parent = PMParent();
    richard_solver_data = new RSAMRdata(0,layout.NumLevels(),layout,pm_parent,*richard_solver_control,rock_manager);
    BuildNLScontrolData(*richard_solver_control,*richard_solver_data,"Flow_PK");
    richard_solver = new RichardSolver(*richard_solver_data,*richard_solver_control);
  }

  PorousMedia::initial_step = true;

  //
  // Average scalar and pressure data down from finer levels
  // so that conserved data is consistant between levels.
  //
  for (int k = finest_level-1; k>= 0; k--)
  {
    getLevel(k).avgDown();
  }
}

//
// Compute an initial MAC velocity in order to get a good first dt
//
void
PorousMedia::initial_mac_project (MultiFab* u_mac, MultiFab* RhoD, Real time)
{
  BL_ASSERT(model != PM_RICHARDS);
  BL_ASSERT(model != PM_STEADY_SATURATED);
  BL_ASSERT(model != PM_SATURATED);
  mac_project(u_mac,RhoD,time);
}

//
// Helper function for PorousMedia::SyncInterp().
//

static
void
set_bc_new (int*            bc_new,
            int             n,
            int             src_comp,
            const int*      clo,
            const int*      chi,
            const int*      cdomlo,
            const int*      cdomhi,
            const BoxArray& cgrids,
            int**           bc_orig_qty)
            
{
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      int bc_index = (n+src_comp)*(2*BL_SPACEDIM) + dir;
      bc_new[bc_index]             = INT_DIR;
      bc_new[bc_index+BL_SPACEDIM] = INT_DIR;
 
      if (clo[dir] < cdomlo[dir] || chi[dir] > cdomhi[dir])
        {
	  for (int crse = 0; crse < cgrids.size(); crse++)
            {
	      const int* c_lo = cgrids[crse].loVect();
	      const int* c_hi = cgrids[crse].hiVect();

	      if (clo[dir] < cdomlo[dir] && c_lo[dir] == cdomlo[dir])
		bc_new[bc_index] = bc_orig_qty[crse][bc_index];
	      if (chi[dir] > cdomhi[dir] && c_hi[dir] == cdomhi[dir])
		bc_new[bc_index+BL_SPACEDIM] = bc_orig_qty[crse][bc_index+BL_SPACEDIM]; 
            }
        }
    }
}

//
// Interpolate A cell centered Sync correction from a
// coarse level (c_lev) to a fine level (f_lev).
//
// This routine interpolates the num_comp components of CrseSync
// (starting at src_comp) and either increments or puts the result into
// the num_comp components of FineSync (starting at dest_comp)
// The components of bc_orig_qty corespond to the quantities of CrseSync.
//

void
PorousMedia::SyncInterp (MultiFab&      CrseSync,
			 int            c_lev,
			 MultiFab&      FineSync,
			 int            f_lev,
			 IntVect&       ratio,
			 int            src_comp,
			 int            dest_comp,
			 int            num_comp,
			 int            increment,
			 Real           dt_clev, 
			 int**          bc_orig_qty,
			 SyncInterpType which_interp,
			 int            state_comp)
{
  BL_ASSERT(which_interp >= 0 && which_interp <= 5);

  Interpolater* interpolater = 0;

  switch (which_interp)
    {
    case PC_T:           interpolater = &pc_interp;           break;
    case CellCons_T:     interpolater = &cell_cons_interp;    break;
    case CellConsLin_T:  interpolater = &lincc_interp;        break;
    case CellConsProt_T: interpolater = &protected_interp;    break;
    default:
      BoxLib::Abort("PorousMedia::SyncInterp(): how did this happen");
    }

  PorousMedia&   fine_level  = getLevel(f_lev);
  const BoxArray& fgrids     = fine_level.boxArray();
  const Geometry& fgeom      = parent->Geom(f_lev);
  const BoxArray& cgrids     = getLevel(c_lev).boxArray();
  const Geometry& cgeom      = parent->Geom(c_lev);
  const Real*     dx_crse    = cgeom.CellSize();
  Box             cdomain    = BoxLib::coarsen(fgeom.Domain(),ratio);
  const int*      cdomlo     = cdomain.loVect();
  const int*      cdomhi     = cdomain.hiVect();
  int*            bc_new     = new int[2*BL_SPACEDIM*(src_comp+num_comp)];

  BoxArray cdataBA(fgrids.size());

  for (int i = 0; i < fgrids.size(); i++)
    cdataBA.set(i,interpolater->CoarseBox(fgrids[i],ratio));
  //
  // Note: The boxes in cdataBA may NOT be disjoint !!!
  //
  MultiFab cdataMF(cdataBA,num_comp,0);

  cdataMF.setVal(0);

  cdataMF.copy(CrseSync, src_comp, 0, num_comp);
  //
  // Set physical boundary conditions in cdataMF.
  //
  for (MFIter mfi(cdataMF); mfi.isValid(); ++mfi)
    {
      int         i       = mfi.index();
      RealBox     gridloc = RealBox(fine_level.boxArray()[i],
				    fine_level.Geom().CellSize(),
				    fine_level.Geom().ProbLo());
      FArrayBox&  cdata   = cdataMF[mfi];
      const int*  clo     = cdata.loVect();
      const int*  chi     = cdata.hiVect();
      const Real* xlo     = gridloc.lo();

      for (int n = 0; n < num_comp; n++)
        {
	  set_bc_new(bc_new,n,src_comp,clo,chi,cdomlo,cdomhi,cgrids,bc_orig_qty);

	  FORT_FILCC(cdata.dataPtr(n), ARLIM(clo), ARLIM(chi),
		     cdomlo, cdomhi, dx_crse, xlo,
		     &(bc_new[2*BL_SPACEDIM*(n+src_comp)]));
        }
    }
  cgeom.FillPeriodicBoundary(cdataMF, 0, num_comp);
  //
  // Interpolate from cdataMF to fdata and update FineSync.
  // Note that FineSync and cdataMF will have the same distribution
  // since the length of their BoxArrays are equal.
  //
  FArrayBox    fdata;
  Array<BCRec> bc_interp(num_comp);

  MultiFab* fine_stateMF = 0;
  if (interpolater == &protected_interp)
    fine_stateMF = &(getLevel(f_lev).get_new_data(State_Type));

  for (MFIter mfi(cdataMF); mfi.isValid(); ++mfi)
    {
      int        i     = mfi.index();
      FArrayBox& cdata = cdataMF[mfi];
      const int* clo   = cdata.loVect();
      const int* chi   = cdata.hiVect();

      fdata.resize(fgrids[i], num_comp);
      //
      // Set the boundary condition array for interpolation.
      //
      for (int n = 0; n < num_comp; n++)
        {
	  set_bc_new(bc_new,n,src_comp,clo,chi,cdomlo,cdomhi,cgrids,bc_orig_qty);
        }

      for (int n = 0; n < num_comp; n++)
        {
	  for (int dir = 0; dir < BL_SPACEDIM; dir++)
            {
	      int bc_index = (n+src_comp)*(2*BL_SPACEDIM) + dir;
	      bc_interp[n].setLo(dir,bc_new[bc_index]);
	      bc_interp[n].setHi(dir,bc_new[bc_index+BL_SPACEDIM]);
            }
        }

      interpolater->interp(cdata,0,fdata,0,num_comp,fgrids[i],ratio,
			   cgeom,fgeom,bc_interp,src_comp,State_Type);

      if (increment)
        {
	  fdata.mult(dt_clev);

	  if (interpolater == &protected_interp) {

	    cdata.mult(dt_clev);
	    FArrayBox& fine_state = (*fine_stateMF)[i];
	    interpolater->protect(cdata,0,fdata,0,fine_state,state_comp,
				  num_comp,fgrids[i],ratio,
				  cgeom,fgeom,bc_interp);
	    Real dt_clev_inv = 1./dt_clev;
	    cdata.mult(dt_clev_inv);

	  }
            
	  FineSync[i].plus(fdata,0,dest_comp,num_comp);
        }
      else
        {
	  FineSync[i].copy(fdata,0,dest_comp,num_comp);
        }
    }

  delete [] bc_new;
}

//
// Interpolate sync pressure correction to a finer level.
//

void
PorousMedia::SyncProjInterp (MultiFab& phi,
			     int       c_lev,
			     MultiFab& P_new,
			     MultiFab& P_old,
			     int       f_lev,
			     IntVect&  ratio,
			     bool      first_crse_step_after_initial_iters,
			     Real      cur_crse_pres_time,
			     Real      prev_crse_pres_time)
{
  const Geometry& fgeom   = parent->Geom(f_lev);
  const BoxArray& P_grids = P_new.boxArray();
  const Geometry& cgeom   = parent->Geom(c_lev);

  BoxArray crse_ba(P_grids.size());

  for (int i = 0; i < P_grids.size(); i++)
    {
      crse_ba.set(i,node_bilinear_interp.CoarseBox(P_grids[i],ratio));
    }

  Array<BCRec> bc(BL_SPACEDIM);
  MultiFab     crse_phi(crse_ba,1,0);

  crse_phi.setVal(1.e200);
  crse_phi.copy(phi,0,0,1);

  FArrayBox     fine_phi;
  PorousMedia& fine_lev            = getLevel(f_lev);
  const Real    cur_fine_pres_time  = fine_lev.state[Press_Type].curTime();
  const Real    prev_fine_pres_time = fine_lev.state[Press_Type].prevTime();

  if (state[Press_Type].descriptor()->timeType() == 
      StateDescriptor::Point && first_crse_step_after_initial_iters)
    {
      const Real time_since_zero  = cur_crse_pres_time - prev_crse_pres_time;
      const Real dt_to_prev_time  = prev_fine_pres_time - prev_crse_pres_time;
      const Real dt_to_cur_time   = cur_fine_pres_time - prev_crse_pres_time;
      const Real cur_mult_factor  = dt_to_cur_time / time_since_zero;
      const Real prev_mult_factor = dt_to_prev_time / dt_to_cur_time;

      for (MFIter mfi(crse_phi); mfi.isValid(); ++mfi)
        {
	  fine_phi.resize(P_grids[mfi.index()],1);
	  fine_phi.setVal(1.e200);
	  node_bilinear_interp.interp(crse_phi[mfi],0,fine_phi,0,1,
				      fine_phi.box(),ratio,cgeom,fgeom,bc,
				      0,Press_Type);
	  fine_phi.mult(cur_mult_factor);
	  P_new[mfi.index()].plus(fine_phi);
	  fine_phi.mult(prev_mult_factor);
	  P_old[mfi.index()].plus(fine_phi);
        }
    }
  else 
    {
      for (MFIter mfi(crse_phi); mfi.isValid(); ++mfi)
        {
	  fine_phi.resize(P_grids[mfi.index()],1);
	  fine_phi.setVal(1.e200);
	  node_bilinear_interp.interp(crse_phi[mfi],0,fine_phi,0,1,
				      fine_phi.box(),ratio,cgeom,fgeom,bc,
				      0,Press_Type);
	  P_new[mfi.index()].plus(fine_phi);
	  P_old[mfi.index()].plus(fine_phi);

        }
    }
}

//
// Averages a multifab of fine data down onto a multifab of coarse data.
//
// This should be an Amrlevel or Multifab function
//
void
PorousMedia::avgDown (MultiFab* s_crse,
		      int c_lev,
		      MultiFab* s_fine, 
		      int f_lev) 
{
    PorousMedia&   fine_lev = getLevel(f_lev);
    PorousMedia&   crse_lev = getLevel(c_lev);
    const BoxArray& fgrids  = fine_lev.grids;
    MultiFab&       fvolume = fine_lev.volume;
    const BoxArray& cgrids  = crse_lev.grids;
    MultiFab&       cvolume = crse_lev.volume;
    IntVect         ratio   = parent->refRatio(c_lev);

    int nc = (*s_crse).nComp();
    avgDown(cgrids,fgrids,*s_crse,*s_fine,cvolume,fvolume,c_lev,f_lev,0,nc,ratio);
}

void
PorousMedia::avgDown (const BoxArray& cgrids,
		      const BoxArray& fgrids,
		      MultiFab&       S_crse,
		      MultiFab&       S_fine,
		      MultiFab&       cvolume,
		      MultiFab&       fvolume,
		      int             c_level,
		      int             f_level,
		      int             scomp,
		      int             ncomp,
		      const IntVect&  fratio)
{
  BL_ASSERT(cgrids == S_crse.boxArray());
  BL_ASSERT(fgrids == S_fine.boxArray());
  BL_ASSERT(cvolume.boxArray() == cgrids);
  BL_ASSERT(fvolume.boxArray() == fgrids);
  BL_ASSERT(S_crse.nComp() == S_fine.nComp());
  BL_ASSERT(fvolume.nComp() == 1 && cvolume.nComp() == 1);

  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::avgDown()");
  //
  // Coarsen() the fine stuff on processors owning the fine data.
  //
  BoxArray crse_S_fine_BA(fgrids.size());

  for (int i = 0; i < fgrids.size(); ++i)
    {
      crse_S_fine_BA.set(i,BoxLib::coarsen(fgrids[i],fratio));
    }

  MultiFab crse_S_fine(crse_S_fine_BA,ncomp,0);
  MultiFab crse_fvolume(crse_S_fine_BA,1,0);

  crse_fvolume.copy(cvolume);
  for (MFIter mfi(S_fine); mfi.isValid(); ++mfi)
    {
      const int i = mfi.index();

      avgDown(S_fine[i],crse_S_fine[i],fvolume[i],crse_fvolume[i],
	      f_level,c_level,crse_S_fine_BA[i],scomp,ncomp,fratio);
    }
  
  S_crse.copy(crse_S_fine,0,scomp,ncomp);
}

//
// Average fine down to coarse in the ovlp intersection.
//

void
PorousMedia::avgDown (const FArrayBox& fine_fab,
		      const FArrayBox& crse_fab, 
		      const FArrayBox& fine_vol,
		      const FArrayBox& crse_vol,
		      int              f_level,
		      int              c_level,
		      const Box&       ovlp,
		      int              scomp,
		      int              ncomp,
		      const IntVect&   fratio)
{
  avgDown_doit(fine_fab,crse_fab,fine_vol,crse_vol,
	       f_level,c_level,ovlp,scomp,ncomp,fratio);
}



//
// Actually average the data down (this is static)
//

void
PorousMedia::avgDown_doit (const FArrayBox& fine_fab,
			   const FArrayBox& crse_fab, 
			   const FArrayBox& fine_vol,
			   const FArrayBox& crse_vol,
			   int              f_level,
			   int              c_level,
			   const Box&       ovlp,
			   int              scomp,
			   int              ncomp,
			   const IntVect&   fratio)
{
  //
  //  NOTE: We copy from component scomp of the fine fab into component 0 of the crse fab
  //        because the crse fab is a temporary which was made starting at comp 0, it is
  //        not the actual state data.
  //
  const int*  ovlo   = ovlp.loVect();
  const int*  ovhi   = ovlp.hiVect();
  const int*  flo    = fine_fab.loVect();
  const int*  fhi    = fine_fab.hiVect();
  const Real* f_dat  = fine_fab.dataPtr(scomp);
  const int*  fvlo   = fine_vol.loVect();
  const int*  fvhi   = fine_vol.hiVect();
  const Real* fv_dat = fine_vol.dataPtr();
  const int*  clo    = crse_fab.loVect();
  const int*  chi    = crse_fab.hiVect();
  const Real* c_dat  = crse_fab.dataPtr();
  const int*  cvlo   = crse_vol.loVect();
  const int*  cvhi   = crse_vol.hiVect();
  const Real* cv_dat = crse_vol.dataPtr();

  FORT_AVGDOWN(c_dat,ARLIM(clo),ARLIM(chi),&ncomp,
	       f_dat,ARLIM(flo),ARLIM(fhi),
	       cv_dat,ARLIM(cvlo),ARLIM(cvhi),
	       fv_dat,ARLIM(fvlo),ARLIM(fvhi),
	       ovlo,ovhi,fratio.getVect());
}

static
void
SyncMacAcrossPeriodicEdges (MultiFab&       u_mac_crse_in_dir,
                            const MultiFab& crse_src,
                            const Geometry& cgeom,
                            int             dir,
                            int             nc)
{
  if (cgeom.isPeriodic(dir))
    {
      const Box cdmn = BoxLib::surroundingNodes(cgeom.Domain(),dir);

      const int N = 2;
      const int L = cdmn.length(dir) - 1;

      Box sides[N] = {cdmn,cdmn};

      sides[0].shift(dir, +L); // The hi end.
      sides[1].shift(dir, -L); // The lo end.

      const IntVect ZeroVector(D_DECL(0,0,0));

      IntVect shifts[N] = {ZeroVector,ZeroVector};

      shifts[0][dir] = -L; // How to shift hi -> lo
      shifts[1][dir] = +L; // How to shift lo -> hi

      for (int which = 0; which < N; ++which)
        {
	  Array<int> pmap;

	  BoxList bl(cdmn.ixType());

	  std::vector< std::pair<int,Box> > isects;

	  isects = crse_src.boxArray().intersections(sides[which]);

	  for (int i = 0; i < isects.size(); i++)
            {
	      const Box bx = crse_src.boxArray()[isects[i].first] & cdmn;

	      if (bx.ok())
                {
		  bl.push_back(bx);
		  pmap.push_back(crse_src.DistributionMap()[isects[i].first]);
                }
            }

	  if (!bl.isEmpty())
            {
	      pmap.push_back(ParallelDescriptor::MyProc()); // The sentinel.
	      MultiFab mf;
	      mf.define(BoxArray(bl), nc, 0, DistributionMapping(pmap), Fab_allocate);
	      mf.copy(crse_src);
	      mf.shift(shifts[which]);
	      u_mac_crse_in_dir.copy(mf);
            }
        }
    }
}

//
// Average edged values down a level
//
void
PorousMedia::SyncEAvgDown (PArray<MultiFab> u_mac_crse,
			   PArray<MultiFab> u_mac_fine,
			   int c_lev)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::SyncEAvgDown()");

  const Geometry& cgeom = parent->Geom(c_lev);
  IntVect         ratio = parent->refRatio(c_lev);
  int             nc    = u_mac_fine[0].nComp();
  
  for (int n = 0; n < u_mac_fine.size(); ++n)
    {
      //
      // crse_src & fine_src must have same parallel distribution.
      // We'll use the KnapSack distribution for the fine_src_ba.
      // Since fine_src_ba should contain more points, this'll lead
      // to a better distribution.
      //
      BoxArray fine_src_ba = u_mac_fine[n].boxArray();
      BoxArray crse_src_ba = BoxArray(fine_src_ba.size());

      for (int i=0; i<fine_src_ba.size();++i)
	{
	  crse_src_ba.set(i,Box(fine_src_ba[i]).coarsen(ratio));
	  fine_src_ba.set(i,Box(crse_src_ba[i]).refine(ratio));
	}

      std::vector<long> wgts(fine_src_ba.size());
    
      for (unsigned int i = 0; i < wgts.size(); i++)
	wgts[i] = fine_src_ba[i].numPts();
	
      DistributionMapping dm;
      dm.KnapSackProcessorMap(wgts,ParallelDescriptor::NProcs());

      MultiFab crse_src,  fine_src; 
    
      crse_src.define(crse_src_ba, nc, 0, dm, Fab_allocate);
      fine_src.define(fine_src_ba, nc, 0, dm, Fab_allocate);
    
      crse_src.setVal(1.e200);
      fine_src.setVal(1.e200);
	
      fine_src.copy(u_mac_fine[n]);
    
      for (MFIter mfi(crse_src); mfi.isValid(); ++mfi)
	{
	  const int  nComp = nc;
	  const Box& box   = crse_src[mfi].box();
	  const int* rat   = ratio.getVect();
	  FORT_EDGE_AVGDOWN(box.loVect(), box.hiVect(), &nComp, rat, &n,
			    crse_src[mfi].dataPtr(),
			    ARLIM(crse_src[mfi].loVect()),
			    ARLIM(crse_src[mfi].hiVect()),
			    fine_src[mfi].dataPtr(),
			    ARLIM(fine_src[mfi].loVect()),
			    ARLIM(fine_src[mfi].hiVect()));
	}

      fine_src.clear();
    
      u_mac_crse[n].copy(crse_src);

      SyncMacAcrossPeriodicEdges(u_mac_crse[n], crse_src, cgeom, n, nc);

    }
}

void
PorousMedia::SyncEAvgDown (MultiFab* u_mac_crse,
			   int c_lev,
			   MultiFab* u_mac_fine, 
			   int f_lev) 
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::SyncEAvgDown()");

  BL_ASSERT(f_lev > 0);

  const Geometry& cgeom      = parent->Geom(c_lev);
  const BoxArray& fgrids     = getLevel(f_lev).grids;
  IntVect    ratio           = IntVect::TheUnitVector();
  ratio                     *= parent->refRatio(c_lev);
  BoxArray f_bnd_ba = fgrids;
  BoxArray c_bnd_ba = BoxArray(f_bnd_ba.size());

  int nc = u_mac_fine[0].nComp();

  for (int i = 0; i < f_bnd_ba.size(); ++i)
    {
      c_bnd_ba.set(i,Box(f_bnd_ba[i]).coarsen(ratio));
      f_bnd_ba.set(i,Box(c_bnd_ba[i]).refine(ratio));
    }

  for (int n = 0; n < BL_SPACEDIM; ++n)
    {
      //
      // crse_src & fine_src must have same parallel distribution.
      // We'll use the KnapSack distribution for the fine_src_ba.
      // Since fine_src_ba should contain more points, this'll lead
      // to a better distribution.
      //
      BoxArray crse_src_ba(c_bnd_ba);
      BoxArray fine_src_ba(f_bnd_ba);

      crse_src_ba.surroundingNodes(n);
      fine_src_ba.surroundingNodes(n);

      std::vector<long> wgts(fine_src_ba.size());

      for (unsigned int i = 0; i < wgts.size(); i++)
	{
	  wgts[i] = fine_src_ba[i].numPts();
	}
      DistributionMapping dm;
      //
      // This call doesn't invoke the MinimizeCommCosts() stuff.
      // There's very little to gain with these types of coverings
      // of trying to use SFC or anything else.
      // This also guarantees that these DMs won't be put into the
      // cache, as it's not representative of that used for more
      // usual MultiFabs.
      //
      dm.KnapSackProcessorMap(wgts,ParallelDescriptor::NProcs());

      MultiFab crse_src,  fine_src; 

      crse_src.define(crse_src_ba, nc, 0, dm, Fab_allocate);
      fine_src.define(fine_src_ba, nc, 0, dm, Fab_allocate);
	    
      crse_src.setVal(1.e200);
      fine_src.setVal(1.e200);
	
      fine_src.copy(u_mac_fine[n]);
        
      for (MFIter mfi(crse_src); mfi.isValid(); ++mfi)
	{
	  const int  nComp = nc;
	  const Box& box   = crse_src[mfi].box();
	  const int* rat   = ratio.getVect();
	  FORT_EDGE_AVGDOWN(box.loVect(), box.hiVect(), &nComp, rat, &n,
			    crse_src[mfi].dataPtr(),
			    ARLIM(crse_src[mfi].loVect()),
			    ARLIM(crse_src[mfi].hiVect()),
			    fine_src[mfi].dataPtr(),
			    ARLIM(fine_src[mfi].loVect()),
			    ARLIM(fine_src[mfi].hiVect()));
	}
      fine_src.clear();

      u_mac_crse[n].copy(crse_src);

      SyncMacAcrossPeriodicEdges(u_mac_crse[n], crse_src, cgeom, n, nc);

    }
}

void
PorousMedia::SyncEAvgDown (MultiFab* u_mac_crse[],
			   int c_lev,
			   MultiFab* u_mac_fine[], 
			   int f_lev) 
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::SyncEAvgDown()");

  BL_ASSERT(f_lev > 0);

  const Geometry& cgeom      = parent->Geom(c_lev);
  const BoxArray& fgrids     = getLevel(f_lev).grids;
  IntVect    ratio           = IntVect::TheUnitVector();
  ratio                     *= parent->refRatio(c_lev);
  BoxArray f_bnd_ba = fgrids;
  BoxArray c_bnd_ba = BoxArray(f_bnd_ba.size());

  int nc = (*u_mac_fine[0]).nComp();

  for (int i = 0; i < f_bnd_ba.size(); ++i)
    {
      c_bnd_ba.set(i,Box(f_bnd_ba[i]).coarsen(ratio));
      f_bnd_ba.set(i,Box(c_bnd_ba[i]).refine(ratio));
    }

  for (int n = 0; n < BL_SPACEDIM; ++n)
    {
      //
      // crse_src & fine_src must have same parallel distribution.
      // We'll use the KnapSack distribution for the fine_src_ba.
      // Since fine_src_ba should contain more points, this'll lead
      // to a better distribution.
      //
      BoxArray crse_src_ba(c_bnd_ba);
      BoxArray fine_src_ba(f_bnd_ba);

      crse_src_ba.surroundingNodes(n);
      fine_src_ba.surroundingNodes(n);

      std::vector<long> wgts(fine_src_ba.size());

      for (unsigned int i = 0; i < wgts.size(); i++)
	{
	  wgts[i] = fine_src_ba[i].numPts();
	}
      DistributionMapping dm;
      //
      // This call doesn't invoke the MinimizeCommCosts() stuff.
      // There's very little to gain with these types of coverings
      // of trying to use SFC or anything else.
      // This also guarantees that these DMs won't be put into the
      // cache, as it's not representative of that used for more
      // usual MultiFabs.
      //
      dm.KnapSackProcessorMap(wgts,ParallelDescriptor::NProcs());

      MultiFab crse_src,  fine_src; 

      crse_src.define(crse_src_ba, nc, 0, dm, Fab_allocate);
      fine_src.define(fine_src_ba, nc, 0, dm, Fab_allocate);
	    
      crse_src.setVal(1.e200);
      fine_src.setVal(1.e200);
	
      fine_src.copy(*u_mac_fine[n]);
        
      for (MFIter mfi(crse_src); mfi.isValid(); ++mfi)
	{
	  const int  nComp = nc;
	  const Box& box   = crse_src[mfi].box();
	  const int* rat   = ratio.getVect();
	  FORT_EDGE_AVGDOWN(box.loVect(), box.hiVect(), &nComp, rat, &n,
			    crse_src[mfi].dataPtr(),
			    ARLIM(crse_src[mfi].loVect()),
			    ARLIM(crse_src[mfi].hiVect()),
			    fine_src[mfi].dataPtr(),
			    ARLIM(fine_src[mfi].loVect()),
			    ARLIM(fine_src[mfi].hiVect()));
	}

      fine_src.clear();

      u_mac_crse[n]->copy(crse_src);

      SyncMacAcrossPeriodicEdges(*u_mac_crse[n], crse_src, cgeom, n, nc);

    }
}


//
// The Mac Sync correction function
//
void
PorousMedia::mac_sync ()
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::mac_sync()");

  bool do_sl_richard_sync = ( model == PM_RICHARDS  && !do_multilevel_full);
  BL_ASSERT(!do_sl_richard_sync);

  bool do_explicit_tracer_sync_only = 
    ( (model == PM_STEADY_SATURATED)
      || (model == PM_SATURATED)
      || (model == PM_RICHARDS && do_multilevel_full) )
    && (diffuse_tracers && be_cn_theta_trac==0);

  const int  numscal   = ncomps; 
  const Real prev_time = state[State_Type].prevTime();
  const Real curr_time = state[State_Type].curTime();
  const Real dt        = parent->dtLevel(level);
  MultiFab& S_new = get_new_data(State_Type);

  bool any_diffusive = false;
  if (do_explicit_tracer_sync_only) {
    //
    // Here, the ONLY sync to compute is the time-explicit corrections from 
    // advection and/or diffusion of the tracers.  There are no syncs for the
    // components because they are either solved multi-level or are not active
    //
    Ssync->mult(dt,0);
    int first_tracer = ncomps;
    int last_tracer = first_tracer + ntracers - 1;
    for (MFIter mfi(*Ssync); mfi.isValid(); ++mfi) {
      for (int n=first_tracer; n <= last_tracer; n++) {
	(*Ssync)[mfi].divide((*rock_phi)[mfi],mfi.validbox(),0,n,1);
      }
      S_new[mfi].plus((*Ssync)[mfi],mfi.validbox(),first_tracer,first_tracer,ntracers);
    }
  } else {

    bool compute_Ucorr = 
      (model != PM_STEADY_SATURATED)
      && (model != PM_SATURATED)
      && !( model == PM_RICHARDS && do_multilevel_full && use_PETSc_snes_for_evolution );

    if (compute_Ucorr) {
      //
      // Assign rock_phi to alpha
      //
      MultiFab* alpha = new MultiFab(grids, 1, 1);
      MultiFab::Copy(*alpha,*rock_phi,0,0,1,alpha->nGrow());
      
      //
      // Compute the u_mac for the correction.
      //
      MultiFab* p_corr = new MultiFab(grids,1,1);
      for (int i=0; i < BL_SPACEDIM; i++) {
        u_corr[i].setVal(0.);
      }
    
      create_lambda(curr_time); 
      const BCRec& p_bc    = desc_lst[Press_Type].getBC(0);
      mac_projector->mac_sync_solve(level,p_bc,lambda,p_corr,u_corr,fine_ratio);
      //
      // Update coarse grid state by adding correction from mac_sync solve
      // the correction is the advective tendency of the new velocities.
      //
      mac_projector->mac_sync_compute(level,u_macG_curr,u_corr,
                                      Ssync,lambda,rock_phi,kappa,
                                      lambda_cc,dlambda_cc,kr_coef,
                                      kpedge,p_corr,
                                      level > 0 ? &getAdvFluxReg(level) : 0,
                                      advectionType, prev_time, dt,
                                      ncomps,be_cn_theta);
      //
      // The following used to be done in mac_sync_compute.  Ssync is
      //   the source for a rate of change to rock_phi*S over the time step, so
      //   Ssync*dt is the source to the actual sync amount.
      //
      Ssync->mult(-dt,Ssync->nGrow());
      MultiFab::Copy(S_new,*Ssync,0,ncomps+ntracers+1,1,1);
    
      //
      // Diffusion solve for Ssync
      //    
      for (int kk  = 0; kk < ncomps; kk++)
        if (is_diffusive[kk])
          any_diffusive = true;
    
      if (any_diffusive)
      {
	MultiFab tmp(grids,1,1);
	MultiFab** fluxSC  = 0;
	diffusion->allocFluxBoxesLevel(fluxSC,0,1);
	
	tmp.setVal(0.);
	for (int i=0; i < BL_SPACEDIM; ++i){
	  (*fluxSC[i]).setVal(0.);
	}
	
	//
	// Set up rho function for diffusive solve
	//
	MultiFab* rho = new MultiFab(grids,1,1);
	MultiFab::Copy(*rho,S_new,0,0,1,1);
	for (int kk = 1; kk<ncomps; kk++)
        {
          if (solid.compare(pNames[pType[kk]]) != 0) 
            MultiFab::Add(*rho,S_new,kk,0,1,1);
        }
	diffusion->set_rho(rho);
	delete rho;
	
	for (int kk = 0; kk<ncomps; kk++)
        {
          if (is_diffusive[kk])
          {
            MultiFab** cmp_diffn=0;
		
            if (variable_scal_diff)
            {
              Real diffTime = state[State_Type].curTime();
              diffusion->allocFluxBoxesLevel(cmp_diffn, 0, 1);
              getDiffusivity(cmp_diffn, diffTime,kk,0,1);
            }
            diffusion->diffuse_Ssync(Ssync,kk,dt,be_cn_theta,
                                     fluxSC,0,cmp_diffn,alpha);
            if (variable_scal_diff)
              diffusion->removeFluxBoxesLevel(cmp_diffn);
		
            if (level > 0)
            {
              for (int d = 0; d < BL_SPACEDIM; d++)
              {
                Real mult = dt;
                MultiFab& fluxSCd = *fluxSC[d];
                for (MFIter fmfi(fluxSCd); fmfi.isValid(); ++fmfi)
                  getViscFluxReg().FineAdd(fluxSCd[fmfi],d,
                                           fmfi.index(),
                                           0,kk,1,mult);
              }
            }
          }
        }
	diffusion->removeFluxBoxesLevel(fluxSC);
      }
    
      // 
      // Capillary-solve.  Since capillary function is nonlinear, we cannot
      // do a simple capillary-diffuse solve for Ssync.  A full nonlinear
      // parabolic solve is needed to determine the new solution at the end of 
      // coarse timestep.  
      //
      if  (have_capillary)
      {
	const int nGrow = 0;
	const int nComp = 1;
	MultiFab** fluxSC    = 0;
	MultiFab** fluxSCp1  = 0;
	diffusion->allocFluxBoxesLevel(fluxSC,  nGrow,nComp);
	diffusion->allocFluxBoxesLevel(fluxSCp1,nGrow,nComp);
      
	int nc = 0; 
	int nd = 1;
	MultiFab*  delta_rhs = 0;
	MultiFab** cmp_pcn   = 0;
	MultiFab** cmp_pcnp1 = 0;
	MultiFab** cmp_pcnp1_dp = 0;
	MultiFab*  S_nwt = 0;
	MultiFab&  S_new = get_new_data(State_Type);

	MultiFab* sat_res_mf = new MultiFab(grids,1,1);
	sat_res_mf->setVal(1.);
	for (MFIter mfi(*sat_res_mf); mfi.isValid();++mfi)
        {
          const Box& box = (*sat_res_mf)[mfi].box();
          (*sat_res_mf)[mfi].minus((*cpl_coef)[mfi],box,3,0,1);
        }
	sat_res_mf->mult(density[nc]);
	diffusion->set_rho(sat_res_mf); 

	MultiFab S_tmp(grids,ncomps,1);
	MultiFab::Copy(S_tmp,S_new,0,0,ncomps,1);

	S_nwt = new MultiFab(grids,1,1);
	MultiFab::Copy(*S_nwt,S_new,nc,0,nComp,1);
      
	delta_rhs = new MultiFab(grids,1,1);
	MultiFab::Copy(*delta_rhs,*Ssync,nc,0,nComp,1);

	//
	// Newton iteration
	//

	// initialization
	Real pcTime = state[State_Type].prevTime();
	diffusion->allocFluxBoxesLevel(cmp_pcn,0,1);
	calcCapillary(pcTime);
	calcDiffusivity_CPL(cmp_pcn,lambda_cc); 
 
	pcTime = state[State_Type].curTime();
	FillStateBndry (pcTime,State_Type,0,ncomps);
	diffusion->allocFluxBoxesLevel(cmp_pcnp1,0,1);
	diffusion->allocFluxBoxesLevel(cmp_pcnp1_dp,0,1);
	calcCapillary(pcTime);
	calcLambda(pcTime);
	calcDiffusivity_CPL(cmp_pcnp1,lambdap1_cc);
	calcDiffusivity_CPL_dp(cmp_pcnp1_dp,lambdap1_cc,pcTime,1);
      
	int  max_itr_nwt = 20;
#if (BL_SPACEDIM == 3)
	Real max_err_nwt = 1e-8;
#else
	Real max_err_nwt = 1e-8;
#endif
 
	int  itr_nwt = 0;
	Real err_nwt = 1e10;
	diffusion->diffuse_init_CPL(dt,nc,be_cn_theta,
				    fluxSC,0,delta_rhs,
				    alpha,cmp_pcn,pcn_cc,S_nwt);
	while ((itr_nwt < max_itr_nwt) && (err_nwt > max_err_nwt)) 
        {
          diffusion->diffuse_iter_CPL(dt,nc,ncomps,be_cn_theta,
                                      0,alpha,cmp_pcnp1,cmp_pcnp1_dp,
                                      pcnp1_cc,S_nwt,&err_nwt);

          if (verbose > 3 && ParallelDescriptor::IOProcessor())
            std::cout << "Newton iteration " << itr_nwt 
                      << " : Error = "       << err_nwt << "\n"; 

          scalar_adjust_constraint(0,ncomps-1);
          FillStateBndry (pcTime,State_Type,0,ncomps);
          calcCapillary(pcTime);
          calcLambda(pcTime);
          calcDiffusivity_CPL(cmp_pcnp1,lambdap1_cc);
          calcDiffusivity_CPL_dp(cmp_pcnp1_dp,lambdap1_cc,pcTime,1);
          itr_nwt += 1;	  

          if (verbose > 3) 
            check_minmax();
        }

	diffusion->compute_flux(nc,dt,be_cn_theta,fluxSCp1,pcnp1_cc,cmp_pcnp1);

	if (verbose > 3 && ParallelDescriptor::IOProcessor())
        {
          if (itr_nwt < max_itr_nwt)
            std::cout << "Newton converged at iteration " << itr_nwt
                      << " with error " << err_nwt << '\n';
          else
            std::cout << "Newton failed to converged: termination error is "
                      <<  err_nwt << '\n'; 
        }
      
	if (level > 0)
        {
          for (int d = 0; d < BL_SPACEDIM; d++)
          {
            Real mult = -dt;
            MultiFab& fluxSCd = *fluxSCp1[d];
            for (MFIter fmfi(fluxSCd); fmfi.isValid(); ++fmfi)
              getViscFluxReg().FineAdd(fluxSCd[fmfi],d,
                                       fmfi.index(),
                                       0,nc,1,mult);
	  
            fluxSCd.mult(-density[nd]/density[nc]);
            for (MFIter fmfi(fluxSCd); fmfi.isValid(); ++fmfi)
              getViscFluxReg().FineAdd(fluxSCd[fmfi],d,
                                       fmfi.index(),
                                       0,nd,1,mult);
          }
        }
      
	// Determine the corrector after capillary-solve
	for (MFIter mfi(*S_nwt); mfi.isValid();++mfi)
        {
          const Box& box = mfi.validbox();
          (*Ssync)[mfi].copy(S_new[mfi],box,0,box,0,ncomps);
          (*Ssync)[mfi].minus(S_tmp[mfi],box,0,0,ncomps);
        }
	
      
  
	delete delta_rhs;
	delete S_nwt;
	delete sat_res_mf;
      
	diffusion->removeFluxBoxesLevel(fluxSC);
	diffusion->removeFluxBoxesLevel(fluxSCp1);
	diffusion->removeFluxBoxesLevel(cmp_pcn);
	diffusion->removeFluxBoxesLevel(cmp_pcnp1);
	diffusion->removeFluxBoxesLevel(cmp_pcnp1_dp);
      }
    
      delete p_corr;
     delete alpha;
      //
      // Add the sync correction to the state.
      //
     if ( ( (model != PM_STEADY_SATURATED)
	    && (model != PM_SATURATED) )
	  && have_capillary == 0 ) {
        if (!any_diffusive) {
          for (MFIter mfi(*Ssync); mfi.isValid(); ++mfi) {
            for (int nc = 0; nc < ncomps; nc++)
              (*Ssync)[mfi].divide((*rock_phi)[mfi],0,nc,1);
          }
        }

        for (MFIter mfi(S_new); mfi.isValid(); ++mfi) {
          S_new[mfi].plus((*Ssync)[mfi],mfi.validbox(),0,0,numscal);
        }
      }
    } else { // No Ucorr needed

      //
      // tracer sync must be diffused time-implicitly
      //
      const Real  prev_time    = state[State_Type].prevTime();
      const Real  cur_time     = state[State_Type].curTime();
      const Real  dt           = cur_time - prev_time;

      MultiFab sat_new(grids,ncomps,nGrowHYP);
      Real t_sat_new = cur_time;
      get_fillpatched_rhosat(t_sat_new,sat_new,nGrowHYP);
      for (int n=0; n<ncomps; ++n) {
        sat_new.mult(1/density[n],n,1,nGrowHYP);
      }

      MultiFab *betanp1[BL_SPACEDIM], *beta1np1[BL_SPACEDIM];
      for (int d = 0; d < BL_SPACEDIM; d++) {
        const BoxArray eba = BoxArray(grids).surroundingNodes(d);
        betanp1[d] = new MultiFab(eba,1,0);
        if (tensor_tracer_diffusion) {
          beta1np1[d] = new MultiFab(eba,1,0);
        }
      }

      //  Solve for increment to C:
      //
      //  phi.sat.delc.Vol + theta.dt.Sum(DF.Area) = Rhs.dt.Vol
      //
      //  Here, Rhs = refluxed flux registers = -Div(DF_cf)/(dt.Vol)
      //
      int first_tracer = ncomps;
      MFVector phi(*rock_phi);
      MFVector sphi_new(sat_new,0,1,1); sphi_new.MULTAY(phi,1);
      MFVector Volume(volume);
      
      int Wflag = 2;
      MultiFab* Whalf = 0;
      MultiFab* alpha = 0;
      int op_maxOrder = 3;
  
      Real a_new = 1;
      Real b_new = be_cn_theta_trac*dt;
      IntVect rat = level == 0 ? IntVect(D_DECL(1,1,1)) : crse_ratio;
      Array<BCRec> tracer_bc(1, get_desc_lst()[State_Type].getBC(first_tracer));

      Diffuser<MFVector,ABecHelper>* scalar_diffuser = 0;
      Diffuser<MFVector,TensorOp>* tensor_diffuser = 0;
      ABecHelper *scalar_linop = 0;
      TensorOp *tensor_linop = 0;

      calcDiffusivity(cur_time,first_tracer,1,&sat_new);
      if (tensor_tracer_diffusion) {
        int rati = rat[0];
        for (int d=1; d<BL_SPACEDIM; ++d) {
          BL_ASSERT(rat[d] == rati);
        }
        TensorDiffusionBndry tbd(grids,1,geom);
        tbd.setHomogValues(tracer_bc,rati);
        getTensorDiffusivity(betanp1, beta1np1, cur_time);
        tensor_linop = getOp(a_new,b_new,tbd,0,1,&(sphi_new.multiFab()),0,1,Whalf,0,
                             Wflag,betanp1,0,1,beta1np1,0,1,volume,area,alpha,0);
        tensor_linop->maxOrder(op_maxOrder);
        tensor_diffuser = new Diffuser<MFVector,TensorOp>(0,tensor_linop,Volume,0,&sphi_new);
      }
      else {
        ViscBndry vbd(grids,1,geom);
        vbd.setHomogValues(tracer_bc[0],rat);
        getDiffusivity(betanp1, cur_time, first_tracer, 0, 1); // Get just one component, all same
        scalar_linop = getOp(a_new,b_new,vbd,0,&(sphi_new.multiFab()),0,1,Whalf,0,
                             Wflag,betanp1,0,1,volume,area,alpha,0,ntracers);
        scalar_linop->maxOrder(op_maxOrder);
        scalar_diffuser = new Diffuser<MFVector,ABecHelper>(0,scalar_linop,Volume,0,&sphi_new);
      }

      MFVector new_state(*Ssync,first_tracer,1,1);
      MFVector Rhs(*Ssync,first_tracer,1,1);
      MultiFab& S_new = get_new_data(State_Type);
      MultiFab& S_old = get_old_data(State_Type);

      for (int n=0; n<ntracers; ++n) {
        new_state.setVal(0);
        MultiFab::Copy(Rhs,*Ssync,first_tracer+n,0,1,0);

        if (tensor_tracer_diffusion) {
          tensor_diffuser->Diffuse(0,new_state,Rhs,prev_time,cur_time,visc_abs_tol,visc_tol);
        }
        else {
          scalar_diffuser->Diffuse(0,new_state,Rhs,prev_time,cur_time,visc_abs_tol,visc_tol);
        }

        if (tensor_tracer_diffusion) {
          tensor_linop->compFlux(D_DECL(*betanp1[0],*betanp1[1],*betanp1[2]),new_state,
                                 MCHomogeneous_BC,0,0,1,0);
        }
        else {
          scalar_linop->compFlux(D_DECL(*betanp1[0],*betanp1[1],*betanp1[2]),new_state,
                                 LinOp::Homogeneous_BC,0,0,1,0);
        }
        
        for (int d = 0; d < BL_SPACEDIM; d++) {
          betanp1[d]->mult(be_cn_theta_trac/geom.CellSize()[d]);
          if (level > 0) {
            for (MFIter mfi(*betanp1[d]); mfi.isValid(); ++mfi) {
              getViscFluxReg().FineAdd((*betanp1[d])[mfi],d,mfi.index(),0,first_tracer+n,1,dt);
            }
          }
        }

        MultiFab::Copy(*Ssync,new_state,0,first_tracer+n,1,0);
        MultiFab::Add(S_new,*Ssync,first_tracer+n,first_tracer+n,1,0);

        // Form phi.sat.C in order to interpolate to finer levels
        MultiFab::Multiply(*Ssync,sphi_new,0,first_tracer+n,1,0);
      }

      delete tensor_diffuser;
      delete scalar_diffuser;
      delete scalar_linop;
      delete tensor_linop;
      for (int d = 0; d < BL_SPACEDIM; d++) {
        delete betanp1[d];
        if (tensor_tracer_diffusion) {
          delete beta1np1[d];
        }
      }

      //
      // Get boundary conditions.
      //
      Array<int*>         sync_bc(grids.size());
      Array< Array<int> > sync_bc_array(grids.size());
      
      for (int i = 0; i < grids.size(); i++) {
        sync_bc_array[i] = getBCArray(State_Type,i,ncomps,ntracers);
        sync_bc[i]       = sync_bc_array[i].dataPtr();
      }

      //
      // Interpolate the sync correction to the finer levels.
      //
      IntVect    ratio = IntVect::TheUnitVector();
      const Real mult  = 1.0;
      for (int lev = level+1; lev <= parent->finestLevel(); lev++) {
        ratio                     *= parent->refRatio(lev-1);
        PorousMedia&     fine_lev  = getLevel(lev);
        const BoxArray& fine_grids = fine_lev.boxArray();
        MultiFab sync_incr(fine_grids,ntracers,0);
        
        SyncInterp(*Ssync,level,sync_incr,lev,ratio,first_tracer,0,
                   ntracers,0,mult,sync_bc.dataPtr());

        MultiFab& S_new_fine = fine_lev.get_new_data(State_Type);
        const MultiFab& fine_phi = *fine_lev.rock_phi;
        for (int n=0; n<ntracers; ++n) {
          MultiFab::Divide(sync_incr,fine_phi,0,n,1,0);
          MultiFab::Divide(sync_incr,S_new_fine,0,n,1,0);
	      sync_incr.mult(density[0],n,1);
        }
        MultiFab::Add(S_new_fine,sync_incr,0,first_tracer,ntracers,0);
      }
    }
  }

  if (idx_dominant > -1)
    scalar_adjust_constraint(0,ncomps-1);
  //
  // Get boundary conditions.
  //
  Array<int*>         sync_bc(grids.size());
  Array< Array<int> > sync_bc_array(grids.size());
      
  for (int i = 0; i < grids.size(); i++)
    {
      sync_bc_array[i] = getBCArray(State_Type,i,0,numscal);
      sync_bc[i]       = sync_bc_array[i].dataPtr();
    }

  //
  // Interpolate the sync correction to the finer levels.
  //
  IntVect    ratio = IntVect::TheUnitVector();
  const Real mult  = 1.0;
  for (int lev = level+1; lev <= parent->finestLevel(); lev++)
    {
      ratio                     *= parent->refRatio(lev-1);
      PorousMedia&     fine_lev  = getLevel(lev);
      const BoxArray& fine_grids = fine_lev.boxArray();
      MultiFab sync_incr(fine_grids,numscal,0);
      sync_incr.setVal(0.0);
      
      SyncInterp(*Ssync,level,sync_incr,lev,ratio,0,0,
		 numscal,1,mult,sync_bc.dataPtr());
      
      MultiFab& S_new = fine_lev.get_new_data(State_Type);
      for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
	S_new[mfi].plus(sync_incr[mfi],fine_grids[mfi.index()],
			0,0,numscal);
    }
}

//
// The Mac Sync correction function
//
#ifdef MG_USE_FBOXLIB
void
PorousMedia::richard_sync ()
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::richard_sync()");

  BL_ASSERT(have_capillary);

  const Real dt = parent->dtLevel(level);
       
  //
  //   Ssync is the source for a rate of change to rock_phi*S over the time step, so
  //   Ssync*dt*density[0] is the source to the actual sync amount.
  //

  if (verbose > 3)
    {
      Real tmp = (*Ssync).norm2(0);
      if (ParallelDescriptor::IOProcessor())
	std::cout << "SSYNC NORM  AFTER = " << tmp << '\n';
    }

  // 
  // Capillary-solve.  Since capillary function is nonlinear, we cannot
  // do a simple capillary-diffuse solve for Ssync.  A full nonlinear
  // parabolic solve is needed to determine the new solution at the end of 
  // coarse timestep.  
  //

  // Build single component edge-centered array of MultiFabs for fluxes
  MultiFab** fluxSC;
  const int nGrow = 0;
  const int nComp = 1;
  diffusion->allocFluxBoxesLevel(fluxSC,nGrow,nComp);
  
  int nc = 0; 
  MultiFab** cmp_pcp1    = 0;
  MultiFab** cmp_pcp1_dp = 0;
  MultiFab sat_res_mf(grids,1,1);
  sat_res_mf.setVal(1.);
  for (MFIter mfi(sat_res_mf); mfi.isValid();++mfi)
    {
      const Box& box = sat_res_mf[mfi].box();
      sat_res_mf[mfi].minus((*cpl_coef)[mfi],box,3,0,1);
    }
  diffusion->set_rho(&sat_res_mf); 

  bool sync_n = true;

  MultiFab& S_new  = get_new_data(State_Type);
  MultiFab& S_old  = get_old_data(State_Type);
  MultiFab& P_new  = get_new_data(Press_Type);
  MultiFab* alpha  = new MultiFab(grids,1,1);
  MultiFab* dalpha = 0;
  MultiFab Tmp(grids,1,1);
  
  if (sync_n)
    MultiFab::Copy(Tmp,S_new,0,0,1,1);
  else
    MultiFab::Copy(Tmp,P_new,0,0,1,1);
  MultiFab::Copy(*alpha,*rock_phi,0,0,1,alpha->nGrow());
  
  if (!do_richard_sat_solve) dalpha = new MultiFab(grids,1,1);
      
  // Compute first res_fix = -\phi * n^k + dt*\nabla v_inflow.  
  // Its value does not change.
  MultiFab res_fix(grids,1,0);
  MultiFab::Copy(res_fix,S_old,nc,0,1,0);
  for (MFIter mfi(res_fix); mfi.isValid(); ++mfi)
    res_fix[mfi].mult((*alpha)[mfi],mfi.validbox(),0,0,1);
  res_fix.mult(-1.0);
  Ssync->mult(-dt*density[0]);
  MultiFab::Add(res_fix,*Ssync,nc,0,1,0);
  calc_richard_velbc(res_fix,u_mac_curr,dt*density[0]);
  // Newton method.
  // initialization
  int do_upwind = 1;
  int  max_itr_nwt = 20;
  Real max_err_nwt = 1e-8;
  int  itr_nwt = 0;
  Real err_nwt = 1e10;
  Real pcTime = state[State_Type].curTime();
  FillStateBndry(pcTime,State_Type,0,ncomps);
  diffusion->allocFluxBoxesLevel(cmp_pcp1,0,1);
  diffusion->allocFluxBoxesLevel(cmp_pcp1_dp,0,3);
  
  calcLambda(pcTime);
  calc_richard_coef(cmp_pcp1,lambdap1_cc,u_mac_curr,0,do_upwind,pcTime);
  calc_richard_jac (cmp_pcp1_dp,dalpha,lambdap1_cc,u_mac_curr,pcTime,dt,0,do_upwind,do_richard_sat_solve);

  
  //if (!do_richard_sat_solve) calc_richard_alpha(dalpha,pcTime);

  Diffusion::NewtonStepInfo linear_status;
  linear_status.status = "";
  linear_status.success = true;
  linear_status.reason = "";
  linear_status.ls_iterations = -1;
  linear_status.max_ls_iterations = 10;
  linear_status.min_ls_factor = 1.e-8;
  linear_status.ls_factor = -1;
  linear_status.ls_acceptance_factor = 1.;
  linear_status.ls_reduction_factor = 0.1;
  linear_status.residual_norm_pre_ls = -1; 
  linear_status.residual_norm_post_ls = -1;
  linear_status.initial_residual_norm = -1;
  linear_status.initial_solution_norm = -1;
  linear_status.monitor_linear_solve = false;
  linear_status.monitor_line_search = false;

  if (do_richard_sat_solve)
    {
      calcCapillary(pcTime);
      MultiFab::Copy(P_new,*pcnp1_cc,0,0,1,1);
      P_new.mult(-1.0,1);
      P_new.plus(atmospheric_pressure_atm,1);
      while ((itr_nwt < max_itr_nwt) && (err_nwt > max_err_nwt) && (linear_status.status!="Finished")) 
	{
          diffusion->richard_iter(dt,nc,gravity,gravity_dir,density,res_fix,
                                    alpha,cmp_pcp1,cmp_pcp1_dp,
                                    u_mac_curr,do_upwind,linear_status);    
            
            err_nwt = linear_status.residual_norm_post_ls;

            if (linear_status.success) {
                if (verbose > 3 && ParallelDescriptor::IOProcessor())
                    std::cout << "Newton iteration " << itr_nwt 
                              << " : Error = "       << err_nwt << "\n"; 
                if (model != PM_RICHARDS)
                    scalar_adjust_constraint(0,ncomps-1);
                FillStateBndry(pcTime,State_Type,0,ncomps);
                calcCapillary(pcTime);
                calcLambda(pcTime);
                MultiFab::Copy(P_new,*pcnp1_cc,0,0,1,1);
                P_new.mult(-1.0,1);
                P_new.plus(atmospheric_pressure_atm,1);
                compute_vel_phase(u_mac_curr,0,pcTime);
                calc_richard_coef(cmp_pcp1,lambdap1_cc,u_mac_curr,0,do_upwind,pcTime);
                calc_richard_jac (cmp_pcp1_dp,0,lambdap1_cc,u_mac_curr,pcTime,dt,0,do_upwind,do_richard_sat_solve);
                itr_nwt += 1;
                if (verbose > 3)  check_minmax();
            }
	}
    }
  else
    {
        //MultiFab dalpha(grids,1,1);
        //calc_richard_alpha(&dalpha,pcTime);

      while ((itr_nwt < max_itr_nwt) && (err_nwt > max_err_nwt) && (linear_status.status!="Finished")) 
	{
          diffusion->richard_iter_p(dt,nc,gravity,gravity_dir,density,res_fix,
                                      alpha,dalpha,cmp_pcp1,cmp_pcp1_dp,
                                      u_mac_curr,do_upwind,linear_status);

	    err_nwt = linear_status.residual_norm_post_ls;

            if (linear_status.success) {
                linear_status.success = true;
                if (verbose > 3 && ParallelDescriptor::IOProcessor())
                    std::cout << "Newton iteration " << itr_nwt 
                              << " : Error = "       << err_nwt << "\n"; 
                calcInvPressure (S_new,P_new,pcTime,0,0,1); 
                if (model != PM_RICHARDS)
                    scalar_adjust_constraint(0,ncomps-1);
                calcLambda(pcTime);
                compute_vel_phase(u_mac_curr,0,pcTime);
                calc_richard_coef(cmp_pcp1,lambdap1_cc,u_mac_curr,0,do_upwind,pcTime);
                calc_richard_jac (cmp_pcp1_dp,dalpha,lambdap1_cc,u_mac_curr,pcTime,dt,0,do_upwind,do_richard_sat_solve);
                //calc_richard_alpha(&dalpha,pcTime);
                itr_nwt += 1;
                if (verbose > 3)  check_minmax();
            }
        }
    }

  NLSstatus retVal = NLSstatus::NLS_SUCCESS;
  if (!linear_status.success) {
      retVal = NLSstatus::NLS_LINEAR_FAIL;
  }

  if (err_nwt > max_err_nwt) {
      retVal = NLSstatus::NLS_NONLINEAR_FAIL;
      if (ParallelDescriptor::IOProcessor()) {
	std::cout << "     **************** Newton failed in richard_sync: too many iterations\n"; 
      }
  }

  if (retVal == NLSstatus::NLS_SUCCESS)
  {  
    diffusion->richard_flux(nc,-1.0,gravity,gravity_dir,density,fluxSC,pcnp1_cc,cmp_pcp1);

      if (verbose > 3 && ParallelDescriptor::IOProcessor())
      {
          std::cout << "Newton converged at iteration " << itr_nwt
                    << " with error " << err_nwt << '\n';
      }
  
      if (level > 0) 
      {
          for (int d = 0; d < BL_SPACEDIM; d++) 
          {
              for (MFIter fmfi(*fluxSC[d]); fmfi.isValid(); ++fmfi)
                  getViscFluxReg().FineAdd((*fluxSC[d])[fmfi],d,fmfi.index(),0,nc,nComp,-dt);
          }
      }
  
      // Determine the corrector after capillary-solve
      for (MFIter mfi(*Ssync); mfi.isValid();++mfi)
      {
          const Box& box = mfi.validbox();
          if (sync_n)
          {
              (*Ssync)[mfi].copy(S_new[mfi],box,0,box,0,ncomps);
              (*Ssync)[mfi].minus(Tmp[mfi],box,0,0,ncomps);
          }
          else
          {
              (*Ssync)[mfi].copy(P_new[mfi],box,0,box,0,1);
              (*Ssync)[mfi].minus(Tmp[mfi],box,0,0,1);
          }
      }

      MultiFab::Copy(S_new,*Ssync,0,ncomps+ntracers,1,0);
  
      //
      // Get boundary conditions.
      //
      Array<int*>         sync_bc(grids.size());
      Array< Array<int> > sync_bc_array(grids.size());
      
      for (int i = 0; i < grids.size(); i++)
      {
          sync_bc_array[i] = getBCArray(Press_Type,i,0,1);
          sync_bc[i]       = sync_bc_array[i].dataPtr();
      }

      //
      // Interpolate the sync correction to the finer levels.
      //
      IntVect    ratio = IntVect::TheUnitVector();
      const Real mult  = 1.0;
      for (int lev = level+1; lev <= parent->finestLevel(); lev++)
      {
          ratio                     *= parent->refRatio(lev-1);
          PorousMedia&     fine_lev  = getLevel(lev);
          const BoxArray& fine_grids = fine_lev.boxArray();
          MultiFab sync_incr(fine_grids,1,0);
          sync_incr.setVal(0.0);
      
          SyncInterp(*Ssync,level,sync_incr,lev,ratio,0,0,
                     1,1,mult,sync_bc.dataPtr());

          MultiFab& S_new = fine_lev.get_new_data(Press_Type);
          if (sync_n)
          {
              for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
                  S_new[mfi].plus(sync_incr[mfi],fine_grids[mfi.index()],
                                  0,0,1);
          }
          else
          {	    
              MultiFab& P_new = fine_lev.get_new_data(Press_Type);
              for (MFIter mfi(P_new); mfi.isValid(); ++mfi)
                  P_new[mfi].plus(sync_incr[mfi],fine_grids[mfi.index()],
                                  0,0,1);
              MultiFab P_tmp(fine_grids,1,0);
              MultiFab::Copy(P_tmp,P_new,0,0,1,0);
              P_tmp.mult(-1.0);
              P_tmp.plus(atmospheric_pressure_atm,0);
              const Real curr_time = state[State_Type].curTime();
              fine_lev.calcInvCapillary(S_new,P_tmp,curr_time,0,0,0);
          }
      }
  }

  delete alpha;
  if (dalpha) delete dalpha;
  diffusion->removeFluxBoxesLevel(cmp_pcp1);
  diffusion->removeFluxBoxesLevel(cmp_pcp1_dp);
  diffusion->removeFluxBoxesLevel(fluxSC);

  if (diffuse_tracers || advect_tracers) {
    BoxLib::Abort("No sync operation installed for richard_sync");
  }
}
#endif

//
// The reflux function
//
void
PorousMedia::reflux ()
{
  bool do_tracer_advection_reflux = 
    advect_tracers &&
    ( (model == PM_STEADY_SATURATED)
      || (model == PM_SATURATED)
      || (model == PM_RICHARDS) );

  if (do_tracer_advection_reflux) {
    reflux(getAdvFluxReg(level+1),ncomps,ntracers);
  }

  bool do_component_advection_reflux =
    (model != PM_STEADY_SATURATED)
    && (model != PM_SATURATED)
    && ( ! (model == PM_RICHARDS && do_multilevel_full) );

  if (do_component_advection_reflux) {
    reflux(getAdvFluxReg(level+1),0,ncomps);
  }

  bool do_tracer_visc_reflux = 
    diffuse_tracers &&
    (model == PM_STEADY_SATURATED)
    || (model == PM_SATURATED)
    || (model == PM_RICHARDS);

  if (do_tracer_visc_reflux) {
    reflux(getViscFluxReg(level+1),ncomps,ntracers);
  }

  bool do_component_visc_reflux =
    (model != PM_STEADY_SATURATED)
    && (model != PM_SATURATED)
    && ( ! (model == PM_RICHARDS && do_multilevel_full) );

  if (do_component_visc_reflux) {
    reflux(getAdvFluxReg(level+1),0,ncomps);
  }
}

void
PorousMedia::reflux (FluxRegister& fr, int sComp, int nComp)
{
  if (level == parent->finestLevel())
    return;

  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::reflux()");

  BL_ASSERT(do_reflux);
  //
  // First do refluxing step.
  //
  Real          dt_crse = parent->dtLevel(level);
  Real          scale   = 1.0/dt_crse;

  fr.Reflux(*Ssync,volume,scale,sComp,sComp,nComp,geom);
  //
  // This is necessary in order to zero out the contribution to any
  // coarse grid cells which underlie fine grid cells.
  //
  BoxArray baf = getLevel(level+1).boxArray();

  baf.coarsen(fine_ratio);

  for (MFIter mfi(*Ssync); mfi.isValid(); ++mfi)
  {
      BL_ASSERT(grids[mfi.index()] == mfi.validbox());

      std::vector< std::pair<int,Box> > isects = baf.intersections(mfi.validbox());

      for (int i = 0, N = isects.size(); i < N; i++)
      {
          (*Ssync)[mfi.index()].setVal(0,isects[i].second,sComp,nComp);
      }
  }
}

//
// Average fine information from the complete set of state types to coarse.
//

void
PorousMedia::avgDown ()
{
  if (level == parent->finestLevel())
    return;

  PorousMedia&   fine_lev = getLevel(level+1);
  const BoxArray& fgrids  = fine_lev.grids;
  MultiFab&       fvolume = fine_lev.volume;
  //
  // Average down the state at the new time.
  //
  
  MultiFab& S_crse = get_new_data(State_Type);
  MultiFab& S_fine = fine_lev.get_new_data(State_Type);
  avgDown(grids,fgrids,S_crse,S_fine,volume,fvolume,level,level+1,0,S_crse.nComp(),fine_ratio);

  //
  // Average down the pressure at the new time.
  //
  MultiFab& P_crse = get_new_data(Press_Type);
  MultiFab& P_fine = fine_lev.get_new_data(Press_Type);
  avgDown(grids,fgrids,P_crse,P_fine,volume,fvolume,level,level+1,0,1,fine_ratio);
  if (do_reflux && u_macG_curr != 0 && fine_lev.u_macG_curr !=0)
    SyncEAvgDown(u_macG_curr,level,fine_lev.u_macG_curr,level+1);
 
  if (do_tracer_chemistry>0) {
    MultiFab& Aux_crse = get_new_data(Aux_Chem_Type);
    MultiFab& Aux_fine = fine_lev.get_new_data(Aux_Chem_Type);
    avgDown(grids,fgrids,Aux_crse,Aux_fine,volume,fvolume,
            level,level+1,0,Aux_crse.nComp(),fine_ratio);
    
    MultiFab& FC_crse = get_new_data(FuncCount_Type);
    MultiFab& FC_fine = fine_lev.get_new_data(FuncCount_Type);
    avgDown(grids,fgrids,FC_crse,FC_fine,volume,fvolume,
            level,level+1,0,1,fine_ratio);
  }
}

//
// ACCESS FUNCTIONS FOLLOW
//

//
// Virtual access function for getting the advective flux out of the
// advection routines for diagnostics and refluxing.
//

void
PorousMedia::pullFluxes (int        i,
                         int        start_ind,
                         int        ncomp,
                         FArrayBox& xflux,
                         FArrayBox& yflux,
#if (BL_SPACEDIM == 3)
                         FArrayBox& zflux,
#endif
                         Real       dt)
{
  //
  // Add fluxes into the refluxing counters.
  //
  if (do_reflux)
    {
      if (level < parent->finestLevel())
        {
	  FluxRegister& fr = getAdvFluxReg(level+1);
	  fr.CrseInit(xflux,xflux.box(),0,0,start_ind,ncomp,-dt);
	  fr.CrseInit(yflux,yflux.box(),1,0,start_ind,ncomp,-dt);
#if (BL_SPACEDIM == 3)                              
	  fr.CrseInit(zflux,zflux.box(),2,0,start_ind,ncomp,-dt);
#endif
        }
      if (level > 0)
        {
	  advflux_reg->FineAdd(xflux,0,i,0,start_ind,ncomp,dt);
	  advflux_reg->FineAdd(yflux,1,i,0,start_ind,ncomp,dt);
#if (BL_SPACEDIM == 3)                                
	  advflux_reg->FineAdd(zflux,2,i,0,start_ind,ncomp,dt);
#endif
        }
    }
}

void
PorousMedia::getForce (MultiFab& force,
		       int       nGrow,
		       int       strt_comp,
		       int       num_comp,
		       Real      time,
		       bool      do_rho_scale)
{
  BL_ASSERT(strt_comp+num_comp <= ncomps + ntracers);
  BL_ASSERT(force.nGrow()>=nGrow);
  BL_ASSERT(force.boxArray()==grids);

  force.setVal(0);
  if (do_source_term) {
    const Real* dx = geom.CellSize();
    MultiFab mask(grids,num_comp,0);
    MultiFab tmp(grids,num_comp,0); tmp.setVal(0);

    for (int i=0; i<source_array.size(); ++i) {
      mask.setVal(0);
      int snum_comp = std::min(num_comp-strt_comp,ncomps);
      for (MFIter mfi(force); mfi.isValid(); ++mfi) {
	source_array[i].apply(tmp[mfi],dx,0,snum_comp,time);	
	const Array<const Region*>& regions = source_array[i].Regions();
	for (int j=0; j<regions.size(); ++j) {
	  if (snum_comp > 0) {
	    source_array[i].apply(tmp[mfi],dx,0,snum_comp,time);
	  }
	  regions[j]->setVal(mask[mfi],1,0,dx,0);
	}
      }

      const std::string& stype = source_array[i].Type();
      Real total_volume_this_level = 1;

      Real cellVol = 1;
      for (int d=0; d<BL_SPACEDIM; ++d) {
        cellVol *= dx[d];
      }
      Real num_cells=0;
      for (MFIter mfi(mask); mfi.isValid(); ++mfi) {
        num_cells += mask[mfi].sum(mfi.validbox(),0,1);
      }
      ParallelDescriptor::ReduceRealSum(num_cells);

      total_volume_this_level = num_cells * cellVol;

      // Scale all values set by this source function so they sum to 
      // user specified value
      if (stype == "volume_weighted" || stype == "point") {
        mask.mult(1/total_volume_this_level,0,1,0);
      }
      else {
	if (stype != "uniform") {
	  BoxLib::Abort(std::string("Unsupported Source function type: \""+stype+"\"").c_str());
	}
      }

      if (strt_comp+num_comp > ncomps) {
	int tstrt_comp = std::max(ncomps,strt_comp);
	int tnum_comp = num_comp - snum_comp;

	for (int it=0; it<tnum_comp; ++it) {
	  const RegionData& tsource = tsource_array[i][it];
	  if (tsource.Type()=="uniform" || tsource.Type()=="point") {
	    for (MFIter mfi(force); mfi.isValid(); ++mfi) {
	      tsource.apply(tmp[mfi],dx,it+snum_comp,1,time);
	    }
	  }
	  else {
	    BoxLib::Abort(std::string("Tracer source type \""+tsource.Type()+"\" not yet implemented").c_str());
	  }
	}
      }

      for (MFIter mfi(mask); mfi.isValid(); ++mfi) {
	const FArrayBox& maskfab = mask[mfi];
	FArrayBox& f = force[mfi];
	const FArrayBox& t = tmp[mfi];
	const Box& box = mfi.validbox();
	for (IntVect iv=box.smallEnd(), End=box.bigEnd(); iv<=End; box.next(iv)) {
	  Real m = maskfab(iv,0);
	  if (m > 0) {
	    for (int n=0; n<num_comp; ++n) {
	      f(iv,n) = m * t(iv,n); // NOTE: volume-weighting of component source impacts solute sources
	    }
	  }
	}
      }

      force.FillBoundary(0,num_comp);
      geom.FillPeriodicBoundary(force,0,num_comp);
	
      if (do_rho_scale) {
	for (int i=0; i<snum_comp; ++i) {
	  force.mult(density[strt_comp+i],i,1);
	}
      }
    }
  }
}

//
// Fills ghost cells of states.
//
void
PorousMedia::FillStateBndry (Real time,
                             int  state_idx,
                             int  src_comp, 
                             int  ncomp) 
{
  MultiFab& S = get_data(state_idx,time);

  if (S.nGrow() == 0)
    return;

  for (FillPatchIterator fpi(*this,S,S.nGrow(),time,state_idx,src_comp,ncomp);
       fpi.isValid();
       ++fpi)
    {
      //
      // Fill all ghost cells interior & exterior to valid region.
      //
      BoxList boxes = BoxLib::boxDiff(fpi().box(),grids[fpi.index()]);
      for (BoxList::iterator bli = boxes.begin(); bli != boxes.end(); ++bli)
        {
	  S[fpi.index()].copy(fpi(),*bli,0,*bli,src_comp,ncomp);
        }
    }
}


void 
PorousMedia::getViscTerms (MultiFab& visc_terms,
                           int       src_comp, 
                           int       ncomp,
                           Real      time)
{
  // 
  // Initialize all viscous terms to zero
  //
  const int nGrow = visc_terms.nGrow();
  visc_terms.setVal(0,0,ncomp,nGrow);

  //
  // Get Scalar Diffusive Terms
  //
  const int first_scal = src_comp;
  const int num_scal   = ncomp;

  if (num_scal > 0)

    {
      for (int icomp = first_scal; icomp < first_scal+num_scal; icomp++)
	{
	  if (is_diffusive[icomp])
	    {
	      MultiFab** cmp_diffn = 0;
	  
	      if (variable_scal_diff)
		{
		  diffusion->allocFluxBoxesLevel(cmp_diffn, 0, 1);
		  getDiffusivity(cmp_diffn, time, icomp, 0, 1);
		}
	      diffusion->getViscTerms(visc_terms,src_comp,icomp,
				      time,0,cmp_diffn);
	      if (variable_scal_diff)
		{
		  diffusion->removeFluxBoxesLevel(cmp_diffn);
		}	  
	    }
	}

      //
      // Get Capillary Diffusive Terms at time n
      //
      if (have_capillary)
	{
	  int nc = 0;
	  MultiFab** cmp_pcn = 0;
	  diffusion->allocFluxBoxesLevel(cmp_pcn,0,1);

	  calcCapillary(time);
	  calcDiffusivity_CPL(cmp_pcn,lambda_cc);

	  // multiply by kedge
	  for (int dir = 0; dir < BL_SPACEDIM; dir++)
	    {
	      for (MFIter mfi(*cmp_pcn[dir]); mfi.isValid(); ++mfi)
		(*cmp_pcn[dir])[mfi].mult(kpedge[dir][mfi],0,0,1);
	      (*cmp_pcn[dir]).FillBoundary();
	    }

	  diffusion->getCplViscTerms(visc_terms,nc,time,density.dataPtr(),0,
				     cmp_pcn,pcn_cc);
	  diffusion->removeFluxBoxesLevel(cmp_pcn);
	}	
    }

  //
  // Ensure consistent grow cells
  //    
  if (nGrow > 0)
    {
      for (MFIter mfi(visc_terms); mfi.isValid(); ++mfi)
        {
	  FArrayBox& vt  = visc_terms[mfi];
	  const Box& box = mfi.validbox();
	  FORT_VISCEXTRAP(vt.dataPtr(),ARLIM(vt.loVect()),ARLIM(vt.hiVect()),
			  box.loVect(),box.hiVect(),&ncomp);
        }
      visc_terms.FillBoundary(0,ncomp);
      //
      // Note: this is a special periodic fill in that we want to
      // preserve the extrapolated grow values when periodic --
      // usually we preserve only valid data.  The scheme relies on
      // the fact that there is good data in the "non-periodic" grow cells.
      // ("good" data produced via VISCEXTRAP above)
      //
      geom.FillPeriodicBoundary(visc_terms,0,ncomp,true);
    }
}

//
// Functions for calculating the variable viscosity and diffusivity.
// These default to setting the variable viscosity and diffusivity arrays
// to the values in visc_coef and diff_coef.  These functions would
// need to be replaced in any class derived from PorousMedia that
// wants variable coefficients.
//

void 
PorousMedia::calcDiffusivity (const Real time, 
			      const int  src_comp, 
			      const int  ncomp,
                              MultiFab*  saturation)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::calcDiffusivity()");

  if (ncomp == 0) return;
  if (nphases>1) {
    BoxLib::Abort("Need to extend calcDiffusivity to support nphases>1");
  }

  // How many of the ones asked for are associated with component saturations
  int num_comps = std::max(0, std::min(ncomps,src_comp+ncomp)-src_comp );

  int num_tracs = diffuse_tracers ? ncomp - num_comps : 0;
  int num_coeffs = num_comps + num_tracs;

  if (num_coeffs > 0) {

    int num_const_coeffs = num_coeffs;
    Array<Real> const_diff_coef(num_const_coeffs,0);

    for (int i=0; i<num_comps; ++i) {
      const_diff_coef[i] = visc_coef[src_comp+i];
    }

    const TimeLevel whichTime = which_time(State_Type,time);
    BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);

    MultiFab* diff_cc = (whichTime == AmrOldTime) ? diffn_cc : diffnp1_cc;
    BL_ASSERT(diff_cc != 0);
    const int nGrow   = 1;

    MultiFab* satp = saturation;

    MultiFab Slocal;
    if (saturation==0) {
      Slocal.define(grids,1,nGrow,Fab_allocate);
      for (FillPatchIterator fpi(*this,Slocal,nGrow,time,State_Type,0,ncomps);
           fpi.isValid();
           ++fpi)
      {
        // Compute phase saturation
        FArrayBox&  Sfab  = fpi();
        Slocal[fpi].copy(Sfab,0,0,1);
        for (int n=1; n<ncomps; ++n) {
          Slocal[fpi].plus(Sfab,n,0,1);
        }
        Slocal[fpi].mult(1/density[0],0,1);
      }
      satp = &Slocal;
    }

    if (num_comps>0) {
      // Compute sat.phi.D
      for (MFIter mfi(*satp); mfi.isValid(); ++mfi) {
        FArrayBox& spD = (*diff_cc)[mfi];
        const FArrayBox& Sfab = (*satp)[mfi];
        const FArrayBox& pfab = (*rock_phi)[mfi];

        for (int i=0; i<num_comps; ++i) {
          spD.setVal(const_diff_coef[i],src_comp+i);
          spD.mult(Sfab,0,src_comp+i,1);
          spD.mult(pfab,0,src_comp+i,1);
        }
      }
    }

    if (num_tracs>0) {
      const Real* dx = geom.CellSize();

      int first_tracer = ncomps;
      int dComp_tracs = std::max(0,src_comp-ncomps) + first_tracer;

      BL_ASSERT(dComp_tracs + num_tracs <= diff_cc->nComp());

      // FIXME: D and tau are n-dimensional because they may have come from averaging down, and if so 
      // should use harmonic/arith formulas.  However, at the moment, the cell-centered diffusion coefficient has
      // only a single component per species. As a HACK we will take just the first component of these vectors
      // but this should be fixed by having an n-dim vector of these things.

      MultiFab tmp(grids,BL_SPACEDIM,nGrow);
      bool retD = rock_manager->GetProperty(time,level,tmp,"molecular_diffusion_coefficient",0,nGrow);
      MultiFab::Copy(*diff_cc,tmp,0,dComp_tracs,1,nGrow);

      bool retT = rock_manager->GetProperty(time,level,tmp,"tortuosity",0,nGrow);
      MultiFab tau(grids,1,nGrow);
      MultiFab::Copy(tau,tmp,0,0,1,nGrow);

      if (!retD || !retT) {
        diff_cc->setVal(0,dComp_tracs,num_tracs,nGrow);
      }
      else {
        // Set D_eff <- D * tau * sat * phi / rho, copy out to all tracers
        diff_cc->mult(1/density[0],dComp_tracs,1,nGrow);
        for (MFIter mfi(*satp); mfi.isValid(); ++mfi) {
          const Box& box = (*satp)[mfi].box();
          FArrayBox& fab = (*diff_cc)[mfi];
          fab.mult((*satp)[mfi],0,dComp_tracs,1);
          fab.mult((*rock_phi)[mfi],0,dComp_tracs,1);
          fab.mult(tau[mfi],0,dComp_tracs,1);
          for (int n=1; n<num_tracs; ++n) {
            fab.copy(fab,dComp_tracs,dComp_tracs+n,1);
          }
        }
      }
    }
  }
}

void 
PorousMedia::getDiffusivity (MultiFab*  diffusivity[BL_SPACEDIM],
			     const Real time,
			     const int  state_comp,
			     const int  dst_comp,
			     const int  ncomp)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::getDiffusivity()");

  //
  // Pick correct diffusivity component
  //
  int diff_comp = state_comp;

  //
  // Select time level to work with (N or N+1)
  //   
  const TimeLevel whichTime = which_time(State_Type,time);
    
  BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);

  MultiFab* diff_cc  = (whichTime == AmrOldTime) ? diffn_cc : diffnp1_cc;
  //
  // Fill edge-centered diffusivities based on diffn_cc or diffnp1_cc
  //
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      for (MFIter ecMfi(*diffusivity[dir]); ecMfi.isValid(); ++ecMfi)
        {
	  center_to_edge_plain((*diff_cc)[ecMfi],(*diffusivity[dir])[ecMfi],
			       diff_comp,dst_comp,ncomp);
        }
    }
}

void 
PorousMedia::getTensorDiffusivity (MultiFab*  diagonal_diffusivity[BL_SPACEDIM],
                                   MultiFab*  off_diagonal_diffusivity[BL_SPACEDIM],
                                   const Real time)
{
  const TimeLevel whichTime = which_time(State_Type,time);    
  BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);

  MultiFab* diff_cc  = (whichTime == AmrOldTime) ? diffn_cc : diffnp1_cc;
  int first_tracer = ncomps;
  getDiffusivity(diagonal_diffusivity,time,first_tracer,0,1);

  std::string pName = "dispersivity";
  int nCompAlpha = rock_manager->NComp(pName);
  if (nCompAlpha < 1) {
    for (int d=0; d<BL_SPACEDIM; ++d) {
      (*off_diagonal_diffusivity[d]).setVal(0);
    }
    return;
  }
  MultiFab alpha(grids,nCompAlpha,1); // FIXME: This actually never changes
  bool ret = rock_manager->GetProperty(state[State_Type].curTime(),level,alpha,
                                  pName,0,alpha.nGrow());
  BL_ASSERT(ret);

  const MultiFab* u_macG = (whichTime == AmrOldTime) ? u_macG_prev : u_macG_curr;
  BL_ASSERT(u_macG != 0);
 
  for (MFIter mfi(alpha); mfi.isValid(); ++mfi) {
    int idx = mfi.index();
    const Box& box = mfi.validbox();
    const FArrayBox& a = alpha[mfi];
    const FArrayBox& Dcc = (*diff_cc)[mfi];

    const FArrayBox& u = u_macG[0][idx];
    const FArrayBox& bx  = (*diagonal_diffusivity[0])[idx];
    const FArrayBox& b1x = (*off_diagonal_diffusivity[0])[idx];

    const FArrayBox& v = u_macG[1][idx];
    const FArrayBox& by  = (*diagonal_diffusivity[1])[idx];
    const FArrayBox& b1y = (*off_diagonal_diffusivity[1])[idx];

#if (BL_SPACEDIM > 2)
    const FArrayBox& w = u_macG[2][idx];
    const FArrayBox& bz  = (*diagonal_diffusivity[2])[idx];
    const FArrayBox& b1z = (*off_diagonal_diffusivity[2])[idx];
#endif

    Array<int> bc;
    bc = getBCArray(Press_Type,idx,0,1);

    FORT_TENSORDIFF(
      box.loVect(), box.hiVect(),
      a.dataPtr(), ARLIM(a.loVect()),ARLIM(a.hiVect()),
      Dcc.dataPtr(first_tracer), ARLIM(Dcc.loVect()),ARLIM(Dcc.hiVect()),

      u.dataPtr(),   ARLIM(u.loVect()),   ARLIM(u.hiVect()),
      bx.dataPtr(),  ARLIM(bx.loVect()),  ARLIM(bx.hiVect()),
      b1x.dataPtr(), ARLIM(b1x.loVect()), ARLIM(b1x.hiVect()),

      v.dataPtr(),   ARLIM(v.loVect()),   ARLIM(v.hiVect()),
      by.dataPtr(),  ARLIM(by.loVect()),  ARLIM(by.hiVect()),
      b1y.dataPtr(), ARLIM(b1y.loVect()), ARLIM(b1y.hiVect()),

#if BL_SPACEDIM>2
      w.dataPtr(),   ARLIM(w.loVect()),   ARLIM(w.hiVect()),
      bz.dataPtr(),  ARLIM(bz.loVect()),  ARLIM(bz.hiVect()),
      b1z.dataPtr(), ARLIM(b1z.loVect()), ARLIM(b1z.hiVect()),
#endif
      bc.dataPtr());
  }
}

void 
PorousMedia::calcDiffusivity_CPL (MultiFab*  diffusivity[BL_SPACEDIM],
				  const Real time)
{

  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::calcDiffusivity_CPL()");
  const TimeLevel whichTime = which_time(State_Type,time);
  BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);    
  MultiFab* lcc = (whichTime == AmrOldTime) ? lambda_cc : lambdap1_cc;
  calcDiffusivity_CPL(diffusivity,lcc);
}

void 
PorousMedia::calcDiffusivity_CPL (MultiFab*        diffusivity[BL_SPACEDIM],
				  const MultiFab*  lbd_cc)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::calcDiffusivity_CPL()");   

  const int*  domlo    = geom.Domain().loVect();
  const int*  domhi    = geom.Domain().hiVect();
  const int   ncomp    = (*diffusivity[0]).nComp();
  for (MFIter mfi(*lbd_cc); mfi.isValid(); ++mfi)
    {
      const int idx   = mfi.index();
      const int* lo   = mfi.validbox().loVect();
      const int* hi   = mfi.validbox().hiVect();

      const int* lbd_lo  = (*lbd_cc)[idx].loVect();
      const int* lbd_hi  = (*lbd_cc)[idx].hiVect();
      const Real* lbddat = (*lbd_cc)[idx].dataPtr();

      const int* dfx_lo  = (*diffusivity[0])[idx].loVect();
      const int* dfx_hi  = (*diffusivity[0])[idx].hiVect();
      const Real* dfxdat = (*diffusivity[0])[idx].dataPtr();

      const int* dfy_lo  = (*diffusivity[1])[idx].loVect();
      const int* dfy_hi  = (*diffusivity[1])[idx].hiVect();
      const Real* dfydat = (*diffusivity[1])[idx].dataPtr();

#if(BL_SPACEDIM==3)
      const int* dfz_lo  = (*diffusivity[2])[idx].loVect();
      const int* dfz_hi  = (*diffusivity[2])[idx].hiVect();
      const Real* dfzdat = (*diffusivity[2])[idx].dataPtr();
#endif
      Array<int> bc;
      bc = getBCArray(State_Type,idx,0,1);
      FORT_GETDIFFUSE_CPL(lbddat, ARLIM(lbd_lo), ARLIM(lbd_hi),
			  dfxdat, ARLIM(dfx_lo), ARLIM(dfx_hi),
			  dfydat, ARLIM(dfy_lo), ARLIM(dfy_hi),
#if(BL_SPACEDIM==3)
			  dfzdat, ARLIM(dfz_lo), ARLIM(dfz_hi),
#endif
			  lo,hi,domlo,domhi,bc.dataPtr(),&ncomp);
    }
  // multiply by kedge
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      for (MFIter ecMfi(*diffusivity[dir]); ecMfi.isValid(); ++ecMfi)
        {
	  (*diffusivity[dir])[ecMfi].mult(kpedge[dir][ecMfi],0,0,1);
        }
      (*diffusivity[dir]).FillBoundary();
    }  
}

void 
PorousMedia::calcDiffusivity_CPL_dp (MultiFab* diffusivity[BL_SPACEDIM],
				     const MultiFab* lbd_cc,
				     const Real time,
				     const int ncomp)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::calcDiffusivity_CPL_dp()");

  MultiFab& S = get_data(State_Type,time);
  const int nGrow = 1;    

  const int*  domlo    = geom.Domain().loVect();
  const int*  domhi    = geom.Domain().hiVect();
  const int n_cpl_coef = cpl_coef->nComp(); 

  // Calculate diffusivity with the dp/ds term.
  for (FillPatchIterator fpi(*this,S,nGrow,time,State_Type,0,ncomps);
       fpi.isValid();
       ++fpi)
    {
      const int idx   = fpi.index();
      const Box box   = BoxLib::grow(grids[idx],nGrow);

      BL_ASSERT(box == fpi().box());

      FArrayBox Htmp(box,1);
      Htmp.setVal(0.);
      const Real* hdat = Htmp.dataPtr();

      const Real* ndat = fpi().dataPtr(); 
      const int*  n_lo = fpi().loVect();
      const int*  n_hi = fpi().hiVect();

      const Real* lbddat = (*lbd_cc)[fpi].dataPtr();
      const int* lbd_lo  = (*lbd_cc)[fpi].loVect();
      const int* lbd_hi  = (*lbd_cc)[fpi].hiVect();	

      const Real* pdat   = (*rock_phi)[fpi].dataPtr();
      const int* p_lo    = (*rock_phi)[fpi].loVect();
      const int* p_hi    = (*rock_phi)[fpi].hiVect();

      const Real* kdat   = (*kappa)[fpi].dataPtr();
      const int* k_lo    = (*kappa)[fpi].loVect();
      const int* k_hi    = (*kappa)[fpi].hiVect();

      const int* lo      = fpi.validbox().loVect();
      const int* hi      = fpi.validbox().hiVect();

      const int* dfx_lo  = (*diffusivity[0])[idx].loVect();
      const int* dfx_hi  = (*diffusivity[0])[idx].hiVect();
      const Real* dfxdat = (*diffusivity[0])[idx].dataPtr();

      const int* dfy_lo  = (*diffusivity[1])[idx].loVect();
      const int* dfy_hi  = (*diffusivity[1])[idx].hiVect();
      const Real* dfydat = (*diffusivity[1])[idx].dataPtr();

#if(BL_SPACEDIM==3)
      const int* dfz_lo  = (*diffusivity[2])[idx].loVect();
      const int* dfz_hi  = (*diffusivity[2])[idx].hiVect();
      const Real* dfzdat = (*diffusivity[2])[idx].dataPtr();
#endif

      const Real* cpdat  = (*cpl_coef)[fpi].dataPtr(); 
      const int*  cp_lo  = (*cpl_coef)[fpi].loVect();
      const int*  cp_hi  = (*cpl_coef)[fpi].hiVect();

      Array<int> bc;
      bc = getBCArray(State_Type,idx,0,1);

      FORT_GETDIFFUSE_CPL_dp(ndat, hdat, ARLIM(n_lo), ARLIM(n_hi),
			     lbddat, ARLIM(lbd_lo), ARLIM(lbd_hi),
			     dfxdat, ARLIM(dfx_lo), ARLIM(dfx_hi),
			     dfydat, ARLIM(dfy_lo), ARLIM(dfy_hi),
#if(BL_SPACEDIM==3)
			     dfzdat, ARLIM(dfz_lo), ARLIM(dfz_hi),
#endif
			     pdat, ARLIM(p_lo), ARLIM(p_hi),
			     kdat, ARLIM(k_lo), ARLIM(k_hi),
			     cpdat, ARLIM(cp_lo), ARLIM(cp_hi),
			     &n_cpl_coef,
			     lo, hi, domlo, domhi,
			     bc.dataPtr(), &ncomp);
    }
    
  // multiply by kedge
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      for (MFIter ecMfi(*diffusivity[dir]); ecMfi.isValid(); ++ecMfi)
        {
	  (*diffusivity[dir])[ecMfi].mult(kpedge[dir][ecMfi],0,0,1);
        }
      (*diffusivity[dir]).FillBoundary();
    }
}

#ifdef MG_USE_FBOXLIB
void 
PorousMedia::calc_richard_coef (MultiFab*        diffusivity[BL_SPACEDIM],
				const MultiFab*  lbd_cc,
				const MultiFab*  umac,
				const int        nc,
				const int        do_upwind,
				double           time)
{

  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::calc_richard_coef()");

  const int*  domlo    = geom.Domain().loVect();
  const int*  domhi    = geom.Domain().hiVect();
  const int ncp1 = nc + 1;

  // Calculate diffusivity for the richard's equation
  for (MFIter mfi(*lbd_cc); mfi.isValid(); ++mfi)
    {

      const int idx      = mfi.index();
      const int* lo      = mfi.validbox().loVect();
      const int* hi      = mfi.validbox().hiVect();
      
      const int* lbd_lo  = (*lbd_cc)[idx].loVect();
      const int* lbd_hi  = (*lbd_cc)[idx].hiVect();
      const Real* lbddat = (*lbd_cc)[idx].dataPtr();

      const int* ux_lo   = umac[0][idx].loVect();
      const int* ux_hi   = umac[0][idx].hiVect();
      const Real* uxdat  = umac[0][idx].dataPtr();

      const int* uy_lo   = umac[1][idx].loVect();
      const int* uy_hi   = umac[1][idx].hiVect();
      const Real* uydat  = umac[1][idx].dataPtr();

      const int* dfx_lo  = (*diffusivity[0])[idx].loVect();
      const int* dfx_hi  = (*diffusivity[0])[idx].hiVect();
      const Real* dfxdat = (*diffusivity[0])[idx].dataPtr();

      const int* dfy_lo  = (*diffusivity[1])[idx].loVect();
      const int* dfy_hi  = (*diffusivity[1])[idx].hiVect();
      const Real* dfydat = (*diffusivity[1])[idx].dataPtr();

#if(BL_SPACEDIM==3)      
      const int* uz_lo   = umac[2][idx].loVect();
      const int* uz_hi   = umac[2][idx].hiVect();
      const Real* uzdat  = umac[2][idx].dataPtr();

      const int* dfz_lo  = (*diffusivity[2])[idx].loVect();
      const int* dfz_hi  = (*diffusivity[2])[idx].hiVect();
      const Real* dfzdat = (*diffusivity[2])[idx].dataPtr();
#endif

      Array<int> bc;
      bc = getBCArray(State_Type,idx,0,1);

      FORT_RICHARD_COEF(lbddat, ARLIM(lbd_lo), ARLIM(lbd_hi),
			dfxdat, ARLIM(dfx_lo), ARLIM(dfx_hi),
			dfydat, ARLIM(dfy_lo), ARLIM(dfy_hi),
#if(BL_SPACEDIM==3)
			dfzdat, ARLIM(dfz_lo), ARLIM(dfz_hi),
#endif
			uxdat, ARLIM(ux_lo), ARLIM(ux_hi),
			uydat, ARLIM(uy_lo), ARLIM(uy_hi),
#if(BL_SPACEDIM==3)
			uzdat, ARLIM(uz_lo), ARLIM(uz_hi),
#endif
			lo,hi,domlo,domhi,bc.dataPtr(),
			&ncp1,&do_upwind);

    }

  // multiply by kedge
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      for (MFIter ecMfi(*diffusivity[dir]); ecMfi.isValid(); ++ecMfi)
	  (*diffusivity[dir])[ecMfi].mult(kpedge[dir][ecMfi],0,0,1);
      (*diffusivity[dir]).FillBoundary();
    }

  FArrayBox inflow;
  for (OrientationIter oitr; oitr; ++oitr) {
      Orientation face = oitr();
      FArrayBox mask;
      if (get_inflow_velocity(face,inflow,mask,time)) {
	int shift = ( face.isHigh() ? -1 : +1 );
	inflow.setVal(0.);
          inflow.shiftHalf(face.coordDir(),shift);
          for (MFIter mfi(*diffusivity[face.coordDir()]); mfi.isValid(); ++mfi) {
	    FArrayBox& u = (*diffusivity[face.coordDir()])[mfi];
              Box ovlp = inflow.box() & u.box();
              if (ovlp.ok()) {
                for (IntVect iv=ovlp.smallEnd(), End=ovlp.bigEnd(); iv<=End; ovlp.next(iv)) {
                  if (mask(iv,0) != 0) {
                    u(iv,0) = inflow(iv,0);
                  }
                }
              }
          }
      }
  }
}

void 
PorousMedia::calc_richard_jac (MultiFab*       diffusivity[BL_SPACEDIM],
                               MultiFab*       dalpha,
			       const MultiFab* lbd_cc,                                
			       const MultiFab* umac,
			       Real            time,
			       Real            dt,
			       int             nc,
			       int             do_upwind,
			       bool            do_richard_sat_solve)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::calc_richard_jac()");

  MultiFab& S = get_data(State_Type,time);
  MultiFab& P = get_data(Press_Type,time);
  const int nGrow = 1;    
  //
  // Select time level to work with (N or N+1)
  //
  const TimeLevel whichTime = which_time(State_Type,time);
  BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);
  MultiFab* pc_cc = (whichTime == AmrOldTime) ? pcn_cc : pcnp1_cc;

  const Real* dx       = geom.CellSize();
  const int*  domlo    = geom.Domain().loVect();
  const int*  domhi    = geom.Domain().hiVect();
  const int n_cpl_coef = cpl_coef->nComp(); 
  const int n_kr_coef  = kr_coef->nComp(); 

  //FIXME: analytical jacobian is not working
  bool do_analytic_jac = false;//true;

#ifdef BL_USE_PETSC
  PetscErrorCode ierr;
  Layout& layout = PMParent()->GetLayout();
  Mat& J = layout.Jacobian();
  Vec& JRowScale = layout.JRowScale();
  BaseFab<int> nodeNums;
  const BCRec& theBC = AmrLevel::desc_lst[Press_Type].getBC(0);
#endif

  for (FillPatchIterator fpi(*this,S,nGrow,time,State_Type,0,ncomps);
       fpi.isValid();
       ++fpi)
    {
      const int idx   = fpi.index();
      const Box box   = BoxLib::grow(grids[idx],nGrow);

      BL_ASSERT(box == fpi().box());

      const Real* ndat = fpi().dataPtr(); 
      const int*  n_lo = fpi().loVect();
      const int*  n_hi = fpi().hiVect();

      const Real* prdat  = P[fpi].dataPtr(); 
      const int*  pr_lo  = P[fpi].loVect();
      const int*  pr_hi  = P[fpi].hiVect();

      const Real* lbddat = (*lbd_cc)[fpi].dataPtr();
      const int* lbd_lo  = (*lbd_cc)[fpi].loVect();
      const int* lbd_hi  = (*lbd_cc)[fpi].hiVect();

      const Real* pcdat  = (*pc_cc)[fpi].dataPtr(); 
      const int*  pc_lo  = (*pc_cc)[fpi].loVect();
      const int*  pc_hi  = (*pc_cc)[fpi].hiVect();

      const Real* pdat   = (*rock_phi)[fpi].dataPtr();
      const int* p_lo    = (*rock_phi)[fpi].loVect();
      const int* p_hi    = (*rock_phi)[fpi].hiVect();

      const Real* kdat   = (*kappa)[fpi].dataPtr();
      const int* k_lo    = (*kappa)[fpi].loVect();
      const int* k_hi    = (*kappa)[fpi].hiVect();

      const int* lo      = fpi.validbox().loVect();
      const int* hi      = fpi.validbox().hiVect();

      const int* ux_lo   = umac[0][idx].loVect();
      const int* ux_hi   = umac[0][idx].hiVect();
      const Real* uxdat  = umac[0][idx].dataPtr();

      const int* uy_lo   = umac[1][idx].loVect();
      const int* uy_hi   = umac[1][idx].hiVect();
      const Real* uydat  = umac[1][idx].dataPtr();

      const int* dfx_lo  = (*diffusivity[0])[idx].loVect();
      const int* dfx_hi  = (*diffusivity[0])[idx].hiVect();
      const Real* dfxdat = (*diffusivity[0])[idx].dataPtr();

      const int* dfy_lo  = (*diffusivity[1])[idx].loVect();
      const int* dfy_hi  = (*diffusivity[1])[idx].hiVect();
      const Real* dfydat = (*diffusivity[1])[idx].dataPtr();

      const int* kpx_lo  = kpedge[0][idx].loVect();
      const int* kpx_hi  = kpedge[0][idx].hiVect();
      const Real* kpxdat = kpedge[0][idx].dataPtr();

      const int* kpy_lo  = kpedge[1][idx].loVect();
      const int* kpy_hi  = kpedge[1][idx].hiVect();
      const Real* kpydat = kpedge[1][idx].dataPtr();

#if(BL_SPACEDIM==3)    
      const int* uz_lo   = umac[2][idx].loVect();
      const int* uz_hi   = umac[2][idx].hiVect();
      const Real* uzdat  = umac[2][idx].dataPtr();

      const int* dfz_lo  = (*diffusivity[2])[idx].loVect();
      const int* dfz_hi  = (*diffusivity[2])[idx].hiVect();
      const Real* dfzdat = (*diffusivity[2])[idx].dataPtr();

      const int* kpz_lo  = kpedge[2][idx].loVect();
      const int* kpz_hi  = kpedge[2][idx].hiVect();
      const Real* kpzdat = kpedge[2][idx].dataPtr();
#endif 
      const Real* krdat  = (*kr_coef)[fpi].dataPtr(); 
      const int*  kr_lo  = (*kr_coef)[fpi].loVect();
      const int*  kr_hi  = (*kr_coef)[fpi].hiVect();
      const Real* cpdat  = (*cpl_coef)[fpi].dataPtr(); 
      const int*  cp_lo  = (*cpl_coef)[fpi].loVect();
      const int*  cp_hi  = (*cpl_coef)[fpi].hiVect();

      if (dalpha) {
          const Real* adat = (*dalpha)[fpi].dataPtr();
          const int*  a_lo = (*dalpha)[fpi].loVect();
          const int*  a_hi = (*dalpha)[fpi].hiVect();

          FORT_RICHARD_ALPHA(adat, ARLIM(a_lo), ARLIM(a_hi),
                             ndat, ARLIM(n_lo), ARLIM(n_hi),
                             pdat, ARLIM(p_lo), ARLIM(p_hi),
                             kdat, ARLIM(k_lo), ARLIM(k_hi),
                             cpdat, ARLIM(cp_lo), ARLIM(cp_hi),
                             &n_cpl_coef, lo, hi);
      }

      Array<int> bc;
      bc = getBCArray(Press_Type,idx,0,1);

      if (do_analytic_jac) 
	FORT_RICHARD_AJAC(ndat, ARLIM(n_lo), ARLIM(n_hi),
			  dfxdat, ARLIM(dfx_lo), ARLIM(dfx_hi),
			  dfydat, ARLIM(dfy_lo), ARLIM(dfy_hi),
#if(BL_SPACEDIM==3)
			  dfzdat, ARLIM(dfz_lo), ARLIM(dfz_hi),
#endif	
			  uxdat, ARLIM(ux_lo), ARLIM(ux_hi),
			  uydat, ARLIM(uy_lo), ARLIM(uy_hi),
#if(BL_SPACEDIM==3)
			  uzdat, ARLIM(uz_lo), ARLIM(uz_hi),
#endif
			  kpxdat, ARLIM(kpx_lo), ARLIM(kpx_hi),
			  kpydat, ARLIM(kpy_lo), ARLIM(kpy_hi),
#if(BL_SPACEDIM==3)
			  kpzdat, ARLIM(kpz_lo), ARLIM(kpz_hi),
#endif
			  lbddat, ARLIM(lbd_lo), ARLIM(lbd_hi),
			  pcdat, ARLIM(pc_lo), ARLIM(pc_hi),
			  pdat, ARLIM(p_lo), ARLIM(p_hi),
			  kdat, ARLIM(k_lo), ARLIM(k_hi),
			  krdat, ARLIM(kr_lo), ARLIM(kr_hi), &n_kr_coef,
			  cpdat, ARLIM(cp_lo), ARLIM(cp_hi), &n_cpl_coef,
			  lo, hi, domlo, domhi, dx, bc.dataPtr(), 
			  rinflow_bc_lo.dataPtr(),rinflow_bc_hi.dataPtr(), 
			  &do_upwind);
      else
	{
	  if (do_richard_sat_solve)
	    FORT_RICHARD_NJAC(ndat,   ARLIM(n_lo), ARLIM(n_hi),
			      dfxdat, ARLIM(dfx_lo), ARLIM(dfx_hi),
			      dfydat, ARLIM(dfy_lo), ARLIM(dfy_hi),
#if(BL_SPACEDIM==3)
			      dfzdat, ARLIM(dfz_lo), ARLIM(dfz_hi),
#endif	
			      uxdat, ARLIM(ux_lo), ARLIM(ux_hi),
			      uydat, ARLIM(uy_lo), ARLIM(uy_hi),
#if(BL_SPACEDIM==3)
			      uzdat, ARLIM(uz_lo), ARLIM(uz_hi),
#endif
			      kpxdat, ARLIM(kpx_lo), ARLIM(kpx_hi),
			      kpydat, ARLIM(kpy_lo), ARLIM(kpy_hi),
#if(BL_SPACEDIM==3)
			      kpzdat, ARLIM(kpz_lo), ARLIM(kpz_hi),
#endif
			      lbddat, ARLIM(lbd_lo), ARLIM(lbd_hi),
			      pcdat, ARLIM(pc_lo), ARLIM(pc_hi),
			      pdat, ARLIM(p_lo), ARLIM(p_hi),
			      kdat, ARLIM(k_lo), ARLIM(k_hi),
			      krdat, ARLIM(kr_lo), ARLIM(kr_hi), &n_kr_coef,
			      cpdat, ARLIM(cp_lo), ARLIM(cp_hi), &n_cpl_coef,
			      lo, hi, domlo, domhi, dx, bc.dataPtr(), 
			      rinflow_bc_lo.dataPtr(),rinflow_bc_hi.dataPtr(), 
			      &richard_perturbation_scale_for_J, &do_upwind);
	  else
	    {
	      FORT_RICHARD_NJAC2(dfxdat, ARLIM(dfx_lo), ARLIM(dfx_hi),
				 dfydat, ARLIM(dfy_lo), ARLIM(dfy_hi),
#if(BL_SPACEDIM==3)
				 dfzdat, ARLIM(dfz_lo), ARLIM(dfz_hi),
#endif	
				 uxdat, ARLIM(ux_lo), ARLIM(ux_hi),
				 uydat, ARLIM(uy_lo), ARLIM(uy_hi),
#if(BL_SPACEDIM==3)
				 uzdat, ARLIM(uz_lo), ARLIM(uz_hi),
#endif
				 kpxdat, ARLIM(kpx_lo), ARLIM(kpx_hi),
				 kpydat, ARLIM(kpy_lo), ARLIM(kpy_hi),
#if(BL_SPACEDIM==3)
				 kpzdat, ARLIM(kpz_lo), ARLIM(kpz_hi),
#endif
				 lbddat, ARLIM(lbd_lo), ARLIM(lbd_hi),
				 prdat, ARLIM(pr_lo), ARLIM(pr_hi),
				 pdat, ARLIM(p_lo), ARLIM(p_hi),
				 kdat, ARLIM(k_lo), ARLIM(k_hi),
				 krdat, ARLIM(kr_lo), ARLIM(kr_hi), &n_kr_coef,
				 cpdat, ARLIM(cp_lo), ARLIM(cp_hi), &n_cpl_coef,
				 lo, hi, domlo, domhi, dx, bc.dataPtr(), 
				 rinflow_bc_lo.dataPtr(),rinflow_bc_hi.dataPtr(), 
				 &richard_perturbation_scale_for_J, &do_upwind);
	    }
	}

#ifdef BL_USE_PETSC
      const Box& vbox = grids[idx];
      Box gbox = Box(vbox).grow(1);
      nodeNums.resize(gbox,1);
      layout.SetNodeIds(nodeNums,level,idx);

      Array<int> cols(1+2*BL_SPACEDIM);
      Array<int> rows(1);
      Array<Real> vals(cols.size());
      FArrayBox* wrk[BL_SPACEDIM];
      for (int d=0; d<BL_SPACEDIM; ++d) {
          wrk[d] = &((*diffusivity[d])[idx]);
      }

      for (IntVect iv(vbox.smallEnd()), iEnd=vbox.bigEnd(); iv<=iEnd; vbox.next(iv))
      {
          cols[0] = nodeNums(iv);
          if (cols[0]>=0) {
              rows[0] = cols[0];
              vals[0] = (dalpha ? (*dalpha)[fpi](iv,0)  :  0);
              Real rdt = (dt>0  ?  density[nc]*dt : 1); // The "b" factor
              int cnt = 1;
              for (int d=0; d<BL_SPACEDIM; ++d) {
                  vals[0] -= rdt * (*wrk[d])(iv,2);
                  IntVect ivp = iv + BoxLib::BASISV(d);
                  int np = nodeNums(ivp,0);
                  if (np>=0) {
                      cols[cnt]  = np; 
                      vals[cnt]  = -rdt * (*wrk[d])(iv,0);
                      cnt++;
                  }
                  else {
                      if (theBC.hi()[d]==FOEXTRAP) {
                          vals[0] -= rdt * (*wrk[d])(iv,0);
                      }
                  }
                  
                  IntVect ivn = iv - BoxLib::BASISV(d);
                  int nn = nodeNums(ivn,0);
                  if (nn>=0) {
                      cols[cnt]  = nn; 
                      vals[cnt]  = -rdt * (*wrk[d])(iv,1);
                      cnt++;
                  }
                  else {
                      if (theBC.lo()[d]==FOEXTRAP) {
                          vals[0] -= rdt * (*wrk[d])(iv,1);
                      }
                  }
              }
              
              // Normalize matrix entries
#if 1
              Real max_abs = 1;
#else
              Real max_abs = 0;
              for (int n=0; n<cnt; ++n) {
                  max_abs = std::max(max_abs,std::abs(vals[n]));
              }
              max_abs = 1/max_abs;
              for (int n=0; n<cnt; ++n) {
                  vals[n] *= max_abs;
              }
#endif
              ierr = MatSetValues(J,rows.size(),rows.dataPtr(),cnt,cols.dataPtr(),vals.dataPtr(),INSERT_VALUES); CHKPETSC(ierr);
              ierr = VecSetValues(JRowScale,1,&(rows[0]),&max_abs,INSERT_VALUES);
          }
      }
#endif

    }

#ifdef BL_USE_PETSC
  ierr = MatAssemblyBegin(J,MAT_FINAL_ASSEMBLY); CHKPETSC(ierr);
  ierr = MatAssemblyEnd(J,MAT_FINAL_ASSEMBLY); CHKPETSC(ierr);
  ierr = VecAssemblyBegin(JRowScale); CHKPETSC(ierr);  
  ierr = VecAssemblyEnd(JRowScale); CHKPETSC(ierr);  
#endif
}


void 
PorousMedia::calc_richard_alpha (MultiFab&       alpha,
                                 const MultiFab& N,
                                 Real            time,
                                 int             sComp,
                                 int             dComp,
                                 int             nGrow) const
{
  BL_ASSERT(N.nGrow() >= nGrow); // Assumes that boundary cells have been properly filled
  BL_ASSERT(alpha.nGrow() >= nGrow); // Fill boundary cells (in F)
  BL_ASSERT(N.nComp()>=sComp+ncomps && alpha.nComp()>=dComp+ncomps);

  MultiFab sat(grids,1,nGrow);
  MultiFab::Copy(sat,N,0,0,1,nGrow);
  sat.mult(1/density[0],0,1,nGrow);
  rock_manager->DInverseCapillaryPressure(sat,*materialID,time,alpha,sComp,dComp,nGrow);
  alpha.mult(-density[0],dComp,1,nGrow);
  MultiFab::Multiply(alpha,*rock_phi,0,dComp,1,nGrow);
  if (nGrow > 0) {
    alpha.FillBoundary(dComp);
    geom.FillPeriodicBoundary(alpha,dComp,ncomps);
  }
}

void 
PorousMedia::calc_richard_velbc (MultiFab& res, 
				 MultiFab* u_phase,
				 const Real dt)  
{ 
  //
  // Add boundary condition to residual
  //
  const int* domlo = geom.Domain().loVect(); 
  const int* domhi = geom.Domain().hiVect();
  const Real* dx   = geom.CellSize();

  for (MFIter mfi(res); mfi.isValid(); ++mfi)
    {
      const int* lo = mfi.validbox().loVect();
      const int* hi = mfi.validbox().hiVect();
	
      FArrayBox& rg       = res[mfi];  
      FArrayBox& ux       = u_phase[0][mfi];
      FArrayBox& uy       = u_phase[1][mfi];
      DEF_LIMITS (rg,rg_dat,rglo,rghi);
      DEF_LIMITS (ux,ux_dat,uxlo,uxhi);
      DEF_LIMITS (uy,uy_dat,uylo,uyhi);

#if (BL_SPACEDIM == 3)
      FArrayBox& uz       = u_phase[2][mfi];
      DEF_LIMITS (uz,uz_dat,uzlo,uzhi);
#endif
      FORT_RICHARD_VELBC (rg_dat, ARLIM(rglo), ARLIM(rghi),
			  ux_dat, ARLIM(uxlo), ARLIM(uxhi),
			  uy_dat, ARLIM(uylo), ARLIM(uyhi),
#if (BL_SPACEDIM == 3)
			  uz_dat, ARLIM(uzlo), ARLIM(uzhi),
#endif
			  lo,hi,domlo,domhi,dx,
			  rinflow_bc_lo.dataPtr(),
			  rinflow_bc_hi.dataPtr(), 
			  &dt);
    }
}
#endif

void 
PorousMedia::calcCapillary (Real time)
{
  const TimeLevel whichTime = which_time(State_Type,time);
  BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);
  MultiFab* pc = (whichTime == AmrOldTime) ? pcn_cc : pcnp1_cc;
  calcCapillary(*pc,get_data(State_Type,time),time,0,0,0);
}

void 
PorousMedia::calcCapillary (MultiFab&       pc,
			    const MultiFab& N,
                            Real            time,
                            int             sComp,
                            int             dComp,
			    int             nGrow) const
{
  BL_ASSERT(N.nGrow() >= nGrow); // Assumes that boundary cells have been properly filled
  BL_ASSERT(pc.nGrow() >= nGrow); // Fill boundary cells (in F)
  BL_ASSERT(N.nComp()>=ncomps && pc.nComp()>=ncomps);

  MultiFab sat(grids,1,nGrow);
  MultiFab::Copy(sat,N,0,0,1,nGrow);
  sat.mult(1/density[0],0,1,nGrow);
  rock_manager->CapillaryPressure(sat,*materialID,time,pc,sComp,dComp,nGrow);
  if (nGrow > 0) {
    pc.FillBoundary(dComp);
    geom.FillPeriodicBoundary(pc,dComp,ncomps);
  }
}

void 
PorousMedia::calcInvCapillary (Real time)
{
  const TimeLevel whichTime = which_time(State_Type,time);
  BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);
  MultiFab* pc = (whichTime == AmrOldTime) ? pcn_cc : pcnp1_cc;
  calcInvCapillary(get_data(State_Type,time),*pc,time,0,0,0);
}

void 
PorousMedia::calcInvCapillary (MultiFab&       N,
			       const MultiFab& pc,
                               Real            time,
                               int             sComp,
                               int             dComp,
                               int             nGrow) const
{
  BL_ASSERT(N.nGrow() >= nGrow); // Assumes that boundary cells have been properly filled
  BL_ASSERT(pc.nGrow() >= nGrow); // Fill boundary cells (in F)
  BL_ASSERT(N.nComp() >= dComp+ncomps && pc.nComp() >= sComp+ncomps);

  rock_manager->InverseCapillaryPressure(pc,*materialID,time,N,sComp,dComp,nGrow);
  N.mult(density[0],dComp,1,nGrow);

  if (nGrow > 0) {
    N.FillBoundary(dComp,ncomps);
    geom.FillPeriodicBoundary(N,dComp,ncomps);
  }
}

void
PorousMedia::calcInvPressure (MultiFab&       N,
                              const MultiFab& P,
                              Real            time,
                              int             sComp,
                              int             dComp,
                              int             nGrow) const
{
  //
  // Pcap = Pgas - Pwater, then get N=s.rho from Pcap(s)^{-1}
  //
  BL_ASSERT(N.nGrow() >= nGrow  && P.nGrow() >= nGrow);
  BL_ASSERT(N.nComp() >= dComp+ncomps && P.nComp() >= sComp+ncomps);
  BL_ASSERT(N.boxArray() == P.boxArray());
  MultiFab pc(P.boxArray(),1,nGrow);
  pc.setVal(atmospheric_pressure_atm,0,1,nGrow);
  MultiFab::Subtract(pc,P,0,0,1,nGrow);
  calcInvCapillary(N,pc,time,0,dComp,nGrow);
}

void 
PorousMedia::smooth_pc (MultiFab* pc)
{
  //
  // Calculate the capillary pressure for a given state.  
  //
  const int n_cpl_coef = cpl_coef->nComp();
  for (MFIter mfi(*pc); mfi.isValid(); ++mfi) 
    {
      const int idx  = mfi.index();
      const int* lo  = grids[idx].loVect();
      const int* hi  = grids[idx].hiVect();

      const Real* ddat  = (*pc)[mfi].dataPtr(); 
      const int*  d_lo  = (*pc)[mfi].loVect();
      const int*  d_hi  = (*pc)[mfi].hiVect();

      const Real* cpdat  = (*cpl_coef)[mfi].dataPtr(); 
      const int*  cp_lo  = (*cpl_coef)[mfi].loVect();
      const int*  cp_hi  = (*cpl_coef)[mfi].hiVect();

      FORT_SMOOTH_CPL( ddat, ARLIM(d_lo), ARLIM(d_hi),
		       cpdat, ARLIM(cp_lo), ARLIM(cp_hi),
		       &n_cpl_coef, lo, hi);
    }
  pc->FillBoundary();
}

void 
PorousMedia::calcLambda (MultiFab&       lambda,
                         const MultiFab& N,
                         Real            time,
                         int             sComp,
                         int             dComp,
                         int             nGrow) const
{
  BL_ASSERT(N.nGrow() >= nGrow); // Assumes that boundary cells have been properly filled
  BL_ASSERT(lambda.nGrow() >= nGrow); // Fill boundary cells (in F)
  BL_ASSERT(N.nComp()>=ncomps && lambda.nComp()>=ncomps);

  MultiFab sat(grids,1,nGrow);
  MultiFab::Copy(sat,N,0,0,1,nGrow);
  sat.mult(1/density[0],0,1,nGrow);
  rock_manager->RelativePermeability(sat,*materialID,time,lambda,sComp,dComp,nGrow);
  BL_ASSERT(ncomps==1);
  lambda.mult(1/muval[0],0,1,nGrow);
  if (nGrow > 0) {
    lambda.FillBoundary(dComp);
    geom.FillPeriodicBoundary(lambda,dComp,ncomps);
  }
}

void 
PorousMedia::calcLambda (const Real time)
{
  MultiFab& N = get_data(State_Type,time);
  FillStateBndry(time,State_Type,0,ncomps);
  const TimeLevel whichTime = which_time(State_Type,time);
  BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);
  MultiFab*lcc = (whichTime == AmrOldTime) ? lambda_cc : lambdap1_cc;
  calcLambda(*lcc,N,time,0,0,0);
}

void 
PorousMedia::calcDLambda (const Real time, MultiFab* dlbd_cc)
{
  //
  // Calculate the lambda values at cell-center. 
  //

  MultiFab& S = get_data(State_Type,time);

  MultiFab* dlcc;
  if (dlbd_cc == 0)
    dlcc = dlambda_cc;
  else
    dlcc = dlbd_cc;

  const int nGrow = 1;    
  const int n_kr_coef = kr_coef->nComp();
  for (FillPatchIterator fpi(*this,S,nGrow,time,State_Type,0,ncomps);
       fpi.isValid();
       ++fpi)
    {
      const int idx   = fpi.index();
      const Box box   = BoxLib::grow(grids[idx],nGrow);

      BL_ASSERT(box == fpi().box());

      FArrayBox& Sfab   = fpi();
      const Real* ndat  = Sfab.dataPtr(); 
      const int*  n_lo  = Sfab.loVect();
      const int*  n_hi  = Sfab.hiVect();

      const Real* ddat  = (*dlcc)[fpi].dataPtr(); 
      const int*  d_lo  = (*dlcc)[fpi].loVect();
      const int*  d_hi  = (*dlcc)[fpi].hiVect();

      const Real* krdat  = (*kr_coef)[fpi].dataPtr(); 
      const int*  kr_lo  = (*kr_coef)[fpi].loVect();
      const int*  kr_hi  = (*kr_coef)[fpi].hiVect();

      FORT_MK_DLAMBDA( ddat, ARLIM(d_lo), ARLIM(d_hi),
		       ndat, ARLIM(n_lo), ARLIM(n_hi), 
		       krdat, ARLIM(kr_lo),ARLIM(kr_hi),
		       &n_kr_coef);
    }

  (*dlcc).FillBoundary();
    
}

void
PorousMedia::set_overdetermined_boundary_cells (Real time)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::set_overdetermined_boundary_cells()");

}


void
PorousMedia::center_to_edge_plain (const FArrayBox& ccfab,
				   FArrayBox&       ecfab,
				   int              sComp,
				   int              dComp,
				   int              nComp)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::center_to_edge_plain()");

  //
  // This routine fills an edge-centered FAB from a cell-centered FAB.
  // It assumes that the data in all cells of the cell-centered FAB is
  // valid and totally ignores any concept of boundary conditions.  
  // It is assummed that the cell-centered FAB fully contains the 
  // edge-centered FAB.  If anything special needs to be done at boundaries, 
  // a varient of this routine needs to be written.  See 
  // HeatTransfer::center_to_edge_fancy().
  //
  const Box&      ccbox = ccfab.box();
  const Box&      ecbox = ecfab.box();
  const IndexType ixt   = ecbox.ixType();
  //
  // Get direction for interpolation to edges
  //
  int dir = -1;
  for (int d = 0; d < BL_SPACEDIM; d++)
    if (ixt.test(d))
      dir = d;
  //
  // Miscellanious checks
  //
  BL_ASSERT(!(ixt.cellCentered()) && !(ixt.nodeCentered()));
  BL_ASSERT(BoxLib::grow(ccbox,-BoxLib::BASISV(dir)).contains(BoxLib::enclosedCells(ecbox)));
  BL_ASSERT(sComp+nComp <= ccfab.nComp() && dComp+nComp <= ecfab.nComp());
  //
  // Shift cell-centered data to edges
  //
  Box fillBox = ccbox; 
  for (int d = 0; d < BL_SPACEDIM; d++)
    if (d != dir)
      fillBox.setRange(d, ecbox.smallEnd(d), ecbox.length(d));
    
  const int isharm = def_harm_avg_cen2edge;
  FORT_CEN2EDG(fillBox.loVect(), fillBox.hiVect(),
	       ARLIM(ccfab.loVect()), ARLIM(ccfab.hiVect()),
	       ccfab.dataPtr(sComp),
	       ARLIM(ecfab.loVect()), ARLIM(ecfab.hiVect()),
	       ecfab.dataPtr(dComp),
	       &nComp, &dir, &isharm);
}

// ===================
// Boundary Conditions
// ===================

void
PorousMedia::setPhysBoundaryValues (FArrayBox& dest,
                                    int        state_indx,
                                    Real       time,
                                    int        dest_comp,
                                    int        src_comp,
                                    int        num_comp)
{
    // The default behavior of an AmrLevel, for reference:
    state[state_indx].FillBoundary(dest,time,geom.CellSize(),
                                   geom.ProbDomain(),dest_comp,src_comp,num_comp);

    if (state_indx==State_Type) {
      int last_comp = src_comp + num_comp - 1;
      int n_t = 0;
      int n_c = 0;
      int s_t = -1;

      if (src_comp >= 0 && src_comp < ncomps) {
	n_c = std::min(last_comp, ncomps-1) - src_comp + 1;
      }

      if (last_comp >= ncomps) {
	s_t = std::max(ncomps, src_comp);
	n_t = std::min(ncomps+ntracers-1,last_comp) - s_t + 1;
      }

      if (n_c > 0) {
	dirichletStateBC(dest,time,src_comp,dest_comp,n_c);
      }
      if (n_t > 0) {
	dirichletTracerBC(dest,time,s_t,dest_comp+n_c,n_t);
      }
    }
    else if (state_indx==Press_Type) {
      dirichletPressBC(dest,time);
    }
}

void
PorousMedia::getDirichletFaces (Array<Orientation>& Faces,
				const int           comp_Type,
				const BCRec&        _bc)
{
  Faces.resize(0);
  for (int idir = 0; idir < BL_SPACEDIM; idir++)
    {
      //if (1 || (comp_Type == Press_Type && _bc.lo(idir) == EXT_DIR) ||
      if ((comp_Type == Press_Type && _bc.lo(idir) == EXT_DIR) ||
	  (comp_Type == State_Type && _bc.lo(idir) == EXT_DIR))
        {
	  const int len = Faces.size();
	  Faces.resize(len+1);
	  Faces.set(len,Orientation(idir,Orientation::low));
        }
      //if (1 || (comp_Type == Press_Type && _bc.hi(idir) == EXT_DIR) ||
      if ((comp_Type == Press_Type && _bc.hi(idir) == EXT_DIR) ||
	  (comp_Type == State_Type && _bc.hi(idir) == EXT_DIR))
        {
	  const int len = Faces.size();
	  Faces.resize(len+1);
	  Faces.set(len,Orientation(idir,Orientation::high));
        }
    }
}

bool
PorousMedia::grids_on_side_of_domain (const BoxArray&    _grids,
				      const Box&         _domain,
				      const Orientation& _Face) 
{
  // FIXME: this should use the intersections code
    const int idir = _Face.coordDir();

    if (_Face.isLow())
      {
        for (int igrid = 0; igrid < _grids.size(); igrid++)
	  { 
            if (_grids[igrid].smallEnd(idir) == _domain.smallEnd(idir))
	      return true;
	  }
      }
  
    if (_Face.isHigh())
      {
        for (int igrid = 0; igrid < _grids.size(); igrid++)
	  {
            if (_grids[igrid].bigEnd(idir) == _domain.bigEnd(idir))
	      return true;
	  }
      }

    return false;
}

Real
PorousMedia::AdjustBCevalTime(int  state_idx,
                              Real time,
                              bool tadj_verbose)
{                              
    // HACK
    // If exec_mode is INIT_TO_STEADY, then build an adjusted eval time such that
    // if t^n+1 = switch_time, we are approaching switch_time, eval bcs just prior
    // if t^n = switch time, we are leaving switch_time, eval exactly at that time
    Real t_eval = time;
    Real prev_time = state[state_idx].prevTime();

    const Array<Real>& tpc_start_times = PMParent()->TPCStartTimes();
    for (int i=0; i<tpc_start_times.size(); ++i) {
        Real curr_time = state[state_idx].curTime();
        Real teps = (curr_time - prev_time)*1.e-6;
        
        if (std::abs(curr_time - tpc_start_times[i]) < teps) {
            t_eval = std::min(t_eval, std::max(prev_time, tpc_start_times[i] - teps));
        }
        
        if (std::abs(prev_time - tpc_start_times[i]) < teps) {
            t_eval = std::max(t_eval, std::min(curr_time, tpc_start_times[i] + teps));
        }

        if (tadj_verbose && ParallelDescriptor::IOProcessor() && t_eval != time) {
            const int old_prec = std::cout.precision(18);
            std::cout << "NOTE: Adjusting eval time for saturation to avoid straddling tpc" << std::endl;
            std::cout << "    prev_time, curr_time, tpc_start_time: " 
                      << prev_time << ", " << curr_time << ", " << tpc_start_times[i] << std::endl;
            std::cout << "    cum_time, strt_time, time, t_eval: " << parent->cumTime() << ", " 
                      << parent->startTime() << ", " << time << ", " << t_eval << std::endl;
            std::cout.precision(old_prec);
        }
    }

    return t_eval;
}

void
PorousMedia::dirichletStateBC (FArrayBox& fab, Real time,int sComp, int dComp, int nComp)
{
  if (model == PM_RICHARDS) { // FIXME: Support solving Richards in saturation form?
    if (! (geom.Domain().contains(fab.box())) ) {
      if (ParallelDescriptor::IOProcessor()) {
	BoxLib::Abort("Should not be fillPatching rhosat directly with Richards when solving pressure form");
      }
    }
  }

#if 0
  // 
  // Note: If this is required, it will have to be moved, perhaps to 
  // set_preferred_boundary_values, where boundary cells are filled
  // in parallel, since we can only get material properties in parallel
  // (fills multifabs not fabs)
  //
  Array<int> bc(BL_SPACEDIM*2,0); // FIXME: Never set, why do we need this
  if (bc_descriptor_map.size()) 
  {
    BL_ASSERT(sComp+nComp<=ncomps);
    BL_ASSERT(dComp+nComp<=fab.nComp());
    const Box domain = geom.Domain();
    const Real* dx   = geom.CellSize();

    Real t_eval = AdjustBCevalTime(State_Type,time,false);
    FArrayBox bndFab, cplbnd, mask;
    FArrayBox cpldat, phidat, kpdat, ktdat;

    for (std::map<Orientation,BCDesc>::const_iterator
           it=bc_descriptor_map.begin(); it!=bc_descriptor_map.end(); ++it) 
    {
      const BCDesc& bc_desc = it->second;
      const Box bndBox = bc_desc.first;
      Box ovlp = bndBox & fab.box();
      if (ovlp.ok()) {
        const Array<int>& face_bc_idxs = bc_desc.second;
        bndFab.resize(ovlp,nComp);
        mask.resize(ovlp,1); 

        for (int i=0; i<face_bc_idxs.size(); ++i) {
          const RegionData& face_bc = bc_array[face_bc_idxs[i]]; 

          mask.setVal(0);
          const Array<const Region*>& regions = face_bc.Regions();
                    
          if (face_bc.Type() == "zero_total_velocity"  || face_bc.Type() == "noflow") {
            get_inflow_density(it->first,face_bc,bndFab,bndBox,t_eval);
          }
          else {
            face_bc.apply(bndFab,dx,0,nComp,t_eval);
          }
        }
        fab.copy(bndFab,0,dComp,nComp);
      }
    }
  }
#endif
}  

void
PorousMedia::dirichletTracerBC (FArrayBox& fab, Real time, int sComp, int dComp, int nComp)
{
  BL_ASSERT(setup_tracer_transport > 0);

  Real t_eval = AdjustBCevalTime(State_Type,time,false);

  FArrayBox bndFab;
  for (int n=0; n<nComp; ++n) 
  {
    int tracer_idx = sComp+n-ncomps;
    if (tbc_descriptor_map[tracer_idx].size()) 
    {
      const Box domain = geom.Domain();
      const Real* dx   = geom.CellSize();
            
      for (std::map<Orientation,BCDesc>::const_iterator
             it=tbc_descriptor_map[tracer_idx].begin(); it!=tbc_descriptor_map[tracer_idx].end(); ++it) 
      {
        const Box bndBox = Box(it->second.first) & fab.box();
        if (bndBox.ok()) {
          bndFab.resize(bndBox,1);
          bndFab.copy(fab,dComp+n,0,1);
          const Array<int>& face_bc_idxs = it->second.second;
          for (int i=0; i<face_bc_idxs.size(); ++i) {
            const RegionData& face_tbc = tbc_array[tracer_idx][face_bc_idxs[i]];
            face_tbc.apply(bndFab,dx,0,1,t_eval);
          }
          fab.copy(bndFab,0,dComp+n,1);

          // if (it->first == Orientation(1,Orientation::low)) {
          //   Box cbox = BoxLib::adjCellLo(domain,1,1) & bndBox;
          //   FArrayBox tc(cbox,1);
          //   tc.copy(bndFab);
          //   std::cout << "TBC " << std::endl;
          //   std::cout << tc << std::endl;

          //   BoxLib::Abort();

          // }
        }
      }
    }    
  }
}

void
PorousMedia::dirichletPressBC (FArrayBox& fab, Real time)
{
  Array<int> bc(BL_SPACEDIM*2,0); // FIXME: Never set, why do we need this
  if (pbc_descriptor_map.size()) 
  {
    const Box domain = geom.Domain();
    const int* domhi = domain.hiVect();
    const int* domlo = domain.loVect();
    const Real* dx   = geom.CellSize();
    Real t_eval = AdjustBCevalTime(Press_Type,time,false);
    
    FArrayBox sdat, prdat, mask;
    for (std::map<Orientation,BCDesc>::const_iterator
           it=pbc_descriptor_map.begin(); it!=pbc_descriptor_map.end(); ++it) 
    {
      const BCDesc& bc_desc = it->second;
      const Box bndBox = bc_desc.first;
      const Array<int>& face_bc_idxs = bc_desc.second;
      Box subbox = bndBox & fab.box();
      if (subbox.ok()) {

        // Set the pressure boundary condition based on the saturation
        if (model==PM_RICHARDS && do_richard_sat_solve) {
          //
          // NOTE: This has been disabled
          //
          // If the boundary condition is specified as a Dirichlet condition
          // on saturation, then the material properties are required in order
          // to compute the corresponding capillary pressure.  One would like to
          // use the level-stored materialID iMultiFab to inform this function 
          // about which material properties to use, but the iMultiFab is a 
          // parallel data structure, and so is not generally available for 
          // arbitrary boxes in space.  Alternatively, we can use the RockManager
          // directly, however that too fills material ids in parallel.  So
          // some new functionality is required either way.
          //
          if (ParallelDescriptor::IOProcessor()) {
            std::cout << "do_richard_sat_solve=true not currently supported" << std::endl;
          }
          BoxLib::Abort();

          sdat.resize(subbox,ncomps); 
          prdat.resize(subbox,ncomps); prdat.setVal(0);

          for (int i=0; i<face_bc_idxs.size(); ++i) {
            const RegionData& face_bc = bc_array[face_bc_idxs[i]]; 

            sdat.setVal(0);
            face_bc.apply(sdat,dx,0,ncomps,t_eval);
            mask.resize(subbox,1); mask.setVal(-1);
            const Array<const Region*>& regions = face_bc.Regions();
            for (int j=0; j<regions.size(); ++j)
            { 
              regions[j]->setVal(mask,1,0,dx,0);
            }
            IArrayBox matID; // FIXME: THIS IS NOT YET PROPERLY FILLED SO THE NEXT CALL WILL FAIL!!!
            rock_manager->CapillaryPressure(sdat.dataPtr(),matID.dataPtr(),time,prdat.dataPtr(),subbox.numPts());

            for (IntVect iv=subbox.smallEnd(); iv<=subbox.bigEnd(); subbox.next(iv)) {
              if (mask(iv,0) > 0) {
                for (int n=0; n<ncomps; ++n) {
                  fab(iv,n) = - prdat(iv,n);
                }
              }
            }
          }
        }
        else {

          prdat.resize(subbox,ncomps); prdat.setVal(0);
          mask.resize(subbox,1);

          for (int i=0; i<face_bc_idxs.size(); ++i) {
            const RegionData& face_bc = bc_array[face_bc_idxs[i]]; 
            mask.setVal(0);
            const Array<const Region*>& regions = face_bc.Regions();

            if (face_bc.Type() == "pressure") {
              for (int j=0; j<regions.size(); ++j) {
                regions[j]->setVal(mask,1,0,dx,0);
              }
              face_bc.apply(prdat,dx,0,ncomps,t_eval);
            }
            else if (face_bc.Type() == "pressure_head") {
              for (int j=0; j<regions.size(); ++j) {
                regions[j]->setVal(mask,1,0,dx,0);
              }
              Real head_val = face_bc(t_eval)[0];
	      if (BL_SPACEDIM<3 && gravity_dir>BL_SPACEDIM-1) {
		head_val -= z_location;
	      }
	      head_val = head_val * density[0] * gravity + atmospheric_pressure_atm; // gravity=g/101325

              Array<Real> gradp(3,0);
              gradp[gravity_dir] = - density[0] * gravity;// gravity=g/101325
              const Real* problo = geom.ProbLo();
              const Real* probhi = geom.ProbHi();

              Array<Real> glo(BL_SPACEDIM), ghi(BL_SPACEDIM);
              if (use_gauge_pressure[face_bc.Label()]) {
                glo.resize(BL_SPACEDIM,0);
                for (int j=0; j<BL_SPACEDIM; ++j) {
                  ghi[j] = probhi[j] - problo[j];
                }
              }
              else {
                for (int j=0; j<BL_SPACEDIM; ++j) {
                  glo[j] = problo[j];
                  ghi[j] = probhi[j];
                }
              }

              const Real* ref_loc = problo;
              Real ref_val = head_val;
              Real* p_ptr = prdat.dataPtr();
              const int* p_lo = prdat.loVect();
              const int* p_hi = prdat.hiVect();
              FORT_LINEAR_PRESSURE(p_lo, p_hi, p_ptr, ARLIM(p_lo),ARLIM(p_hi), &ncomps,
                                   dx, glo.dataPtr(), ghi.dataPtr(), &ref_val, ref_loc, gradp.dataPtr());

            }
            else if (face_bc.Type() == "linear_pressure") {
              for (int j=0; j<regions.size(); ++j) {
                regions[j]->setVal(mask,1,0,dx,0);
              }
	      Array<Real> vals = face_bc(t_eval);
	      BL_ASSERT(vals.size()>=2*BL_SPACEDIM+1);
	      const Real* gradp = &(vals[1]);
	      const Real* loc = &(vals[1+BL_SPACEDIM]);
              Real* p_ptr = prdat.dataPtr();
              const int* p_lo = prdat.loVect();
              const int* p_hi = prdat.hiVect();
              const Real* problo = geom.ProbLo();
              const Real* probhi = geom.ProbHi();
              FORT_LINEAR_PRESSURE(p_lo, p_hi, p_ptr, ARLIM(p_lo),ARLIM(p_hi), &ncomps,
                                   dx, problo, probhi, &(vals[0]), loc, gradp);
            }

            for (IntVect iv=subbox.smallEnd(); iv<=subbox.bigEnd(); subbox.next(iv)) {
              if (mask(iv,0) > 0) {
                for (int n=0; n<ncomps; ++n) {
                  fab(iv,n) = prdat(iv,n);
                }
              }
            }
          }
        }
      }
    }
  }
}

void
PorousMedia::dirichletDefaultBC (FArrayBox& fab, Real time)
{
    int nComp = fab.nComp();
    FArrayBox bndFab;

    if (tbc_descriptor_map.size()) 
      {
	const Box domain = geom.Domain();
	const Real* dx   = geom.CellSize();
            
	for (std::map<Orientation,BCDesc>::const_iterator
	       it=tbc_descriptor_map[0].begin(); it!=tbc_descriptor_map[0].end(); ++it) 
            {
	      const Box bndBox = Box(it->second.first) & fab.box();
                if (bndBox.ok()) {
                    bndFab.resize(bndBox,nComp);
		    bndFab.setVal(0.);
                    fab.copy(bndFab,0,0,nComp);
                }
            }
      }    
}

void
PorousMedia::derive_Material_ID(Real      time,
                                MultiFab& mf,
                                int       dcomp)
{
  BL_ASSERT(dcomp < mf.nComp());  
  const int ngrow = mf.nGrow();
  for (MFIter mfi(mf); mfi.isValid(); ++mfi) {
    FArrayBox& fab = mf[mfi];
    const IArrayBox& ifab = (*materialID)[mfi];
    Box box=Box(mfi.validbox()).grow(ngrow);
    for (IntVect iv=box.smallEnd(), End=box.bigEnd(); iv<=End; box.next(iv)) {
      fab(iv,dcomp) = ifab(iv,0);
    }
  }
}

void
PorousMedia::derive_Grid_ID(Real      time,
                            MultiFab& mf,
                            int       dcomp)
{
  BL_ASSERT(dcomp < mf.nComp());
  const int ngrow = mf.nGrow();

  BoxArray dstBA(mf.boxArray());
  mf.setVal(-1,dcomp,1,ngrow);
  for (MFIter mfi(mf); mfi.isValid(); ++mfi) {
    mf[mfi].setVal(mfi.index());
  }
}

void
PorousMedia::derive_Core_ID(Real      time,
                            MultiFab& mf,
                            int       dcomp)
{
  BL_ASSERT(dcomp < mf.nComp());
  const int ngrow = mf.nGrow();

  BoxArray dstBA(mf.boxArray());
  mf.setVal(-1,dcomp,1,ngrow);
  for (MFIter mfi(mf); mfi.isValid(); ++mfi) {
    mf[mfi].setVal(ParallelDescriptor::MyProc());
  }
}

void
PorousMedia::derive_Cell_ID(Real      time,
                            MultiFab& mf,
                            int       dcomp)
{
  BL_ASSERT(dcomp < mf.nComp());
  const int ngrow = mf.nGrow();

  BoxArray dstBA(mf.boxArray());
  mf.setVal(-1,dcomp,1,ngrow);
  Layout& layout = PMParent()->GetLayout();
  Layout::IntFab ifab;
  for (MFIter mfi(mf); mfi.isValid(); ++mfi) {
    Box gbox = mf[mfi].box();
    ifab.resize(gbox,1);
    layout.SetNodeIds(ifab,level,mfi.index());
    const int* idat = ifab.dataPtr();
    Real* rdat = mf[mfi].dataPtr();
    int numpts = gbox.numPts();
    for (int i=0; i<numpts; ++i) {
      rdat[i] = Real(idat[i]);
    }
  }
}

void
PorousMedia::derive_Volumetric_Water_Content(Real      time,
                                             MultiFab& mf,
                                             int       dcomp,
                                             int       ntrac)
{
  // Note, assumes one comp per phase
  int scomp = -1;
  for (int i=0; i<cNames.size(); ++i) {
    if (cNames[i] == "Water") {
      if (pNames[i] != "Aqueous") {
        BoxLib::Abort("No Water in the Aqueous phase");
      }
      scomp = i;
    }
  }

  if (scomp>=0) {
    const BoxArray& BA = mf.boxArray();
    int ngrow = mf.nGrow();
    BL_ASSERT(mf.nGrow()<=rock_phi->nGrow());

    int ncomp = 1; // Just water
    FillPatchIterator fpi(*this,mf,ngrow,time,State_Type,scomp,ncomp);
    for ( ; fpi.isValid(); ++fpi) {
      mf[fpi].copy(fpi(),0,dcomp,ncomp);
      mf[fpi].mult((*rock_phi)[fpi],0,dcomp,ncomp);
      mf[fpi].mult(1/density[scomp],dcomp,ncomp);
    }

    if (ntrac >= 0) {
      BL_ASSERT(ntrac < ntracers);
      int ncompt = 1;
      int scompt = ncomps + ntrac;
      FillPatchIterator fpi(*this,mf,ngrow,time,State_Type,scompt,ncompt);
      for ( ; fpi.isValid(); ++fpi) {
        mf[fpi].mult(fpi(),0,dcomp,ncompt);
      }
    }
  }            
  else {
    BoxLib::Abort("PorousMedia: cannot derive Volumetric_Water_Content");
  }
}

void
PorousMedia::derive_Aqueous_Saturation(Real      time,
                                       MultiFab& mf,
                                       int       dcomp)
{
  // Sum all components in the Aqueous phase
  // FIXME: Assumes one comp per phase
  int scomp = -1;
  int naq = 0;
  for (int ip=0; ip<pNames.size(); ++ip) {
    if (pNames[ip] == "Aqueous") {
      scomp = ip;
      naq++;
    }
  }

  if (naq==1) {
    const BoxArray& BA = mf.boxArray();
    int ngrow = mf.nGrow();
    BL_ASSERT(mf.nGrow()<=1); // state only has this many
    int ncomp = 1; // Just aqueous
    FillPatchIterator fpi(*this,mf,ngrow,time,State_Type,scomp,ncomp);
    for ( ; fpi.isValid(); ++fpi)
    {
      mf[fpi].copy(fpi(),0,dcomp,ncomp);
      mf[fpi].mult(1/density[scomp],dcomp,ncomp);
    }
    BL_ASSERT(scomp>=0 && scomp<ncomps);        }            
  else {
    BoxLib::Abort("PorousMedia:: no support for more than one Aqueous component");
  }
}

void
PorousMedia::derive_Aqueous_Pressure(Real      time,
                                     MultiFab& mf,
                                     int       dcomp)
{
  Real t_new = state[Press_Type].curTime(); 
  int ncomp = 1;
  int ngrow = mf.nGrow();
  AmrLevel::derive("pressure",time,mf,dcomp);
  if ( (model == PM_RICHARDS)
       || (model == PM_STEADY_SATURATED)
       || (model == PM_SATURATED) ) {
    mf.mult(BL_ONEATM,dcomp,ncomp,ngrow);
  }
  else {
    BoxLib::Abort(std::string("PorousMedia:: Aqueous_Pressure not yet implemented for " + model).c_str());
  }
}

void
PorousMedia::derive_Hydraulic_Head(Real      time,
                                   MultiFab& mf,
                                   int       dcomp)
{
  Real t_new = state[Press_Type].curTime();
  int ncomp = 1;
  int ngrow = mf.nGrow();
  const Real* plo = geom.ProbLo();
  const Real* dx = geom.CellSize();
  if ( (model == PM_RICHARDS)
       || (model == PM_STEADY_SATURATED)
       || (model == PM_SATURATED) ) {
    if (gravity==0) {
      BoxLib::Abort("PorousMedia::derive_Hydraulic_Head: cannot derived hydraulic head since gravity = 0");
    }
    AmrLevel::derive("pressure",time,mf,dcomp);
    mf.plus(-atmospheric_pressure_atm,dcomp,ncomps,ngrow);
    Array<Real> rhog(ncomps);
    for (int i=0; i<ncomps; ++i) {
      rhog[i] = density[i] * gravity;
    }
    for (MFIter mfi(mf); mfi.isValid(); ++mfi) {
      FArrayBox& fab = mf[mfi];
      const Box box = BoxLib::grow(mfi.validbox(),ngrow);
      FORT_HYD_HEAD(box.loVect(), box.hiVect(),
                    fab.dataPtr(), ARLIM(fab.loVect()), ARLIM(fab.hiVect()),
                    rhog.dataPtr(), &gravity_dir, dx, plo, &ncomps);
    }
    if (BL_SPACEDIM<3 && gravity_dir>BL_SPACEDIM-1) {
      mf.plus(z_location,dcomp,ncomps,ngrow);
    }
  }
  else {
    BoxLib::Abort(std::string("PorousMedia:: Hydraulic_Head not yet implemented for " + model).c_str());
  }
}

void
PorousMedia::derive_Aqueous_Volumetric_Flux(Real      time,
                                            MultiFab& mf,
                                            int       dcomp,
                                            int       dir)
{
  BL_ASSERT(dir < BL_SPACEDIM);
  if ( (model == PM_RICHARDS)
       || (model == PM_STEADY_SATURATED)
       || (model == PM_SATURATED) ) {
    MultiFab tmf(grids,BL_SPACEDIM,0);
    // FIXME: Input parameter?
    bool do_upwind = false;
    umac_edge_to_cen(u_mac_curr,tmf,do_upwind); 
    MultiFab::Copy(mf,tmf,dir,dcomp,1,0);
  }
  else {
    BoxLib::Abort(std::string("PorousMedia::derive: Aqueous_Volumetric_Flux not yet implemented for "+model).c_str());
  }
}

void
PorousMedia::derive_Porosity(Real      time,
                             MultiFab& mf,
                             int       dcomp)
{
  const BoxArray& BA = mf.boxArray();
  int ngrow = mf.nGrow();
  int ncomp = 1;
  BL_ASSERT(mf.nGrow()<=rock_phi->nGrow());
  MultiFab::Copy(mf,*rock_phi,0,dcomp,ncomp,ngrow);
}

void
PorousMedia::derive_Intrinsic_Permeability(Real      time,
                                           MultiFab& mf,
                                           int       dcomp,
                                           int       dir)
{
  MultiFab kappatmp(grids,BL_SPACEDIM,0);
  bool ret = rock_manager->GetProperty(state[State_Type].curTime(),level,kappatmp,
                                  "permeability",0,mf.nGrow());
  if (!ret) BoxLib::Abort("Failed to build permeability");
  MultiFab::Copy(mf,kappatmp,dir,dcomp,1,0);
  // Return values in mks
  mf.mult(1/BL_ONEATM,dcomp,1,0);
}

void
PorousMedia::derive_Molecular_Diffusion_Coefficient(Real      time,
                                                    MultiFab& mf,
                                                    int       dcomp,
                                                    int       dir)
{
  MultiFab Dtmp(grids,BL_SPACEDIM,0);
  bool ret = rock_manager->GetProperty(state[State_Type].curTime(),level,Dtmp,
                                  "molecular_diffusion_coefficient",dcomp,mf.nGrow());
  if (!ret) {
    // Assume one component, return def
    Real molecular_diffusion_coefficient_DEF = 0;
    mf.setVal(molecular_diffusion_coefficient_DEF,dcomp,1);
  }
  else {
    MultiFab::Copy(mf,Dtmp,dir,dcomp,1,0);
  }
}

void
PorousMedia::derive_Tortuosity(Real      time,
                               MultiFab& mf,
                               int       dcomp,
                               int       dir)
{
  MultiFab Ttmp(grids,BL_SPACEDIM,0);
  bool ret = rock_manager->GetProperty(state[State_Type].curTime(),level,Ttmp,
                                  "tortuosity",dcomp,mf.nGrow());
  if (!ret) {
    // Assume one component, return def
    Real tortuosity_DEF = 1;
    mf.setVal(tortuosity_DEF,dcomp,1);
  }
  else {
    MultiFab::Copy(mf,Ttmp,dir,dcomp,1,0);
  }
}

void
PorousMedia::derive_SpecificStorage(Real      time,
                                    MultiFab& mf,
                                    int       dcomp)
{
  bool ret = rock_manager->GetProperty(state[State_Type].curTime(),level,mf,
                                  "specific_storage",dcomp,mf.nGrow());
  if (!ret) {
    // Assume one component, return def
    Real specific_storage_DEF = 0;
    mf.setVal(specific_storage_DEF,dcomp,1);
  }
}

void
PorousMedia::derive_CationExchangeCapacity(Real      time,
                                           MultiFab& mf,
                                           int       dcomp)
{
  bool set_default = true;
  if (chemistry_helper!=0) {
    const std::map<std::string,int>& acvm = chemistry_helper->AuxChemVariablesMap();
    //const std::string str (chemistry_model_name=="Amanzi" ? "Ion_Exchange_Site_Density_0" : "Cation_Exchange_Capacity");
    const std::string str = "Ion_Exchange_Site_Density_0";
    std::map<std::string,int>::const_iterator it = acvm.find(str);
    if (it!=acvm.end()) {
      MultiFab::Copy(mf,get_data(Aux_Chem_Type,time),it->second,dcomp,1,0);
      set_default = false;
    }
  }

  if (set_default) {    
    // Assume one component, return def
    Real cation_exchange_capacity_DEF = 0;
    mf.setVal(cation_exchange_capacity_DEF,dcomp,1);
  }
}

void
PorousMedia::derive_Dispersivity(Real      time,
                                 MultiFab& mf,
                                 int       dcomp,
                                 int       dir)
{
  std::string name = (dir == 0  ? "Dispersivity_L" : "Dispersivity_T" );
  std::string pName = "dispersivity";
  int nComp = rock_manager->NComp(pName);
  if (nComp > 0) {
    MultiFab dtmp(grids,nComp,0);
    bool ret = rock_manager->GetProperty(state[State_Type].curTime(),level,dtmp,
                                    "dispersivity",0,mf.nGrow());
    MultiFab::Copy(mf,dtmp,dir,dcomp,1,0);
  } else {
    // Assume one component, return def
    Real dispersivity_DEF = 0;
    mf.setVal(dispersivity_DEF,dcomp,1);
  }
}

MultiFab*
PorousMedia::derive (const std::string& name,
                     Real               time,
                     int                ngrow)
{
  BL_ASSERT(ngrow >= 0);
  
  MultiFab* mf = 0;
  const DeriveRec* rec = derive_lst.get(name);
  if (rec) {
    BoxArray dstBA(grids);
    mf = new MultiFab(dstBA, rec->numDerive(), ngrow);
    int dcomp = 0;
    derive(name,time,*mf,dcomp);
  }
  else {
    //
    // If we got here, cannot derive given name.
    //
    std::string msg("PorousMedia::derive(): unknown variable: ");
    msg += name;
    BoxLib::Error(msg.c_str());
  }
  return mf;
}

void
PorousMedia::derive (const std::string& name,
                     Real               time,
                     MultiFab&          mf,
                     int                dcomp)
{
  const DeriveRec* rec = derive_lst.get(name);

  bool not_found_yet = false;

  if (name == "Material_ID") {
    derive_Material_ID(time,mf,dcomp);
  }
  else if (name == "Grid_ID") {
    derive_Grid_ID(time,mf,dcomp);
  }
  else if (name == "Core_ID") {
    derive_Core_ID(time,mf,dcomp);
  }
  else if (name == "Cell_ID") {
    derive_Cell_ID(time,mf,dcomp);
  }
  else if (name == "Volumetric_Water_Content") {
    derive_Volumetric_Water_Content(time,mf,dcomp);
  }
  else if (name == "Aqueous_Saturation") {
    derive_Aqueous_Saturation(time,mf,dcomp);
  }
  else if (name == "Aqueous_Pressure") {
    derive_Aqueous_Pressure(time,mf,dcomp);
  }
  else if (name == "Hydraulic_Head") {
    derive_Hydraulic_Head(time,mf,dcomp);
  }
  else if (name == "Aqueous_Volumetric_Flux_X" ||
           name == "Aqueous_Volumetric_Flux_Y" ||
           name == "Aqueous_Volumetric_Flux_Z") {
    int dir = ( name == "Aqueous_Volumetric_Flux_X"  ?  0  :
                name == "Aqueous_Volumetric_Flux_Y" ? 1 : 2);
    derive_Aqueous_Volumetric_Flux(time,mf,dcomp,dir);
  }
  else if (name=="Porosity") {
    derive_Porosity(time,mf,dcomp);
  }
  else if (rock_manager)
  {
    if (name == "Intrinsic_Permeability_X" ||
        name == "Intrinsic_Permeability_Y" ||
        name == "Intrinsic_Permeability_Z") {
      int dir = ( name == "Intrinsic_Permeability_X"  ?  0  :
                  name == "Intrinsic_Permeability_Y" ? 1 : 2);
      derive_Intrinsic_Permeability(time,mf,dcomp,dir);
    }
    else if (name == "Molecular_Diffusion_Coefficient_X" ||
        name == "Molecular_Diffusion_Coefficient_Y" ||
        name == "Molecular_Diffusion_Coefficient_Z") {
      int dir = ( name == "Molecular_Diffusion_Coefficient_X"  ?  0  :
                  name == "Molecular_Diffusion_Coefficient_Y" ? 1 : 2);
      derive_Molecular_Diffusion_Coefficient(time,mf,dcomp,dir);
    }
    else if (name == "Dispersivity_L" ||
             name == "Dispersivity_T") {
      int dir = ( name == "Dispersivity_L"  ?  0  : 1);
      derive_Dispersivity(time,mf,dcomp,dir);
    }
    else if (name == "Tortuosity_X" ||
             name == "Tortuosity_Y" ||
             name == "Tortuosity_Z") {
      int dir = ( name == "Tortuosity_X"  ?  0  :
                  name == "Tortuosity_Y" ? 1 : 2);
      derive_Tortuosity(time,mf,dcomp,dir);
    }
    else if (name == "Specific_Storage") {
      derive_SpecificStorage(time,mf,dcomp);
    }
    else if (name == "Cation_Exchange_Capacity") {
      derive_CationExchangeCapacity(time,mf,dcomp);
    }
    else {
      not_found_yet = true;
    }
  } else {
    not_found_yet = true;
  }

  if (not_found_yet) {
    for (int n=0; n<ntracers && not_found_yet; ++n) {
      std::string tname = tNames[n] + "_Aqueous_Concentration";
      std::string VSC = "Volumetric_" + tNames[n] + "_Content";
      if (name==tname) {
        AmrLevel::derive(tNames[n],time,mf,dcomp);
        not_found_yet = false;
      }
      else if (name == VSC) {
        derive_Volumetric_Water_Content(time,mf,dcomp,n);
        not_found_yet = false;
      }
    }

    if (not_found_yet && chemistry_helper!=0) {
      const std::map<std::string,int>& aux_chem_variables_map = chemistry_helper->AuxChemVariablesMap();
      std::map<std::string,int>::const_iterator it = aux_chem_variables_map.find(name);
      if (it != aux_chem_variables_map.end()) {
        int ncompt = 1;
        int scompt = it->second;
        FillPatchIterator fpi(*this,mf,mf.nGrow(),time,Aux_Chem_Type,scompt,ncompt);
        for ( ; fpi.isValid(); ++fpi) {
          mf[fpi].copy(fpi(),0,dcomp,ncompt);
        }
        not_found_yet = false;        
      }
    }
  }

  if (not_found_yet) {
    AmrLevel::derive(name,time,mf,dcomp);
  }
}

void
PorousMedia::manual_tags_placement (TagBoxArray&    tags,
				    Array<IntVect>& bf_lev)
{
  //
  // Tag inflow and outflow faces for refinement
  // 
  Array<Orientation> Faces;
  const BCRec& p_bc = desc_lst[Press_Type].getBC(0);
  getDirichletFaces(Faces,Press_Type,p_bc);

  if (Faces.size()>0)
    {
      for (int j =0; j<4; ++j)
	{
	  for (int i=0; i<Faces.size(); ++i)
	    {
	      const Orientation& Face = Faces[i];
	      const int oDir = Face.coordDir();
	      const Box& crse_domain = BoxLib::coarsen(geom.Domain(),bf_lev[level]);
	      const int mult = (Face.isLow() ? +1 : -1);

	      
	      // Refine entire boundary if new boxes within grid_tol
	      // from outflow
        
	      const int grid_tol = 2;
	      Box flowBox = Box(BoxLib::adjCell(crse_domain,Face,grid_tol));
	      flowBox.shift(oDir,mult*grid_tol);
	      

	      // Only refine if there are already tagged cells in the region
	      
	      bool hasTags = false;
	      for (MFIter tbi(tags); !hasTags && tbi.isValid(); ++tbi)
		if (tags[tbi].numTags(flowBox) > 0) hasTags = true;

	      ParallelDescriptor::ReduceBoolOr(hasTags);
	      
	      // hack to make sure inlet is always refined.
	      if (hasTags)
		tags.setVal(BoxArray(&flowBox,1),TagBox::SET);
	    }
	}
    }	
}

void
PorousMedia::create_umac_grown (MultiFab* u_mac, MultiFab* u_macG)
{

  // This complicated copy handles the periodic boundary condition properly.

  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::create_umac_grown1()");
  BL_ASSERT(level==0);
	    
  for (int n = 0; n < BL_SPACEDIM; ++n)
    {
      MultiFab u_ghost(u_mac[n].boxArray(),1,1);
      u_ghost.setVal(1.e40);
      u_ghost.copy(u_mac[n]);
      u_ghost.FillBoundary();
      geom.FillPeriodicBoundary(u_ghost);
      for (MFIter mfi(u_macG[n]); mfi.isValid(); ++mfi)
	{
	  u_macG[n][mfi].copy(u_ghost[mfi]);
	}
    }
}

void
PorousMedia::create_umac_grown (MultiFab* u_mac, 
				PArray<MultiFab>& u_mac_crse, 
				MultiFab* u_macG) 
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::create_umac_grown2()");

  BL_ASSERT(level>0);

  const BoxArray& fgrids = grids;
  BoxList         bl     = BoxLib::GetBndryCells(fgrids,1);

  BoxArray f_bnd_ba(bl);

  bl.clear();

  BoxArray c_bnd_ba = BoxArray(f_bnd_ba.size());

  for (int i = 0; i < f_bnd_ba.size(); ++i)
    {
      c_bnd_ba.set(i,Box(f_bnd_ba[i]).coarsen(crse_ratio));
      f_bnd_ba.set(i,Box(c_bnd_ba[i]).refine(crse_ratio));
    }

  for (int n = 0; n < BL_SPACEDIM; ++n)
    {
      //
      // crse_src & fine_src must have same parallel distribution.
      // We'll use the KnapSack distribution for the fine_src_ba.
      // Since fine_src_ba should contain more points, this'll lead
      // to a better distribution.
      //
      BoxArray crse_src_ba(c_bnd_ba);
      BoxArray fine_src_ba(f_bnd_ba);

      crse_src_ba.surroundingNodes(n);
      fine_src_ba.surroundingNodes(n);

      std::vector<long> wgts(fine_src_ba.size());

      for (unsigned int i = 0; i < wgts.size(); i++)
	{
	  wgts[i] = fine_src_ba[i].numPts();
	}
      DistributionMapping dm;
      //
      // This call doesn't invoke the MinimizeCommCosts() stuff.
      // There's very little to gain with these types of coverings
      // of trying to use SFC or anything else.
      // This also guarantees that these DMs won't be put into the
      // cache, as it's not representative of that used for more
      // usual MultiFabs.
      //
      dm.KnapSackProcessorMap(wgts,ParallelDescriptor::NProcs());

      MultiFab crse_src,  fine_src; 

      crse_src.define(crse_src_ba, 1, 0, dm, Fab_allocate);
      fine_src.define(fine_src_ba, 1, 0, dm, Fab_allocate);
	    
      crse_src.setVal(1.e200);
      fine_src.setVal(1.e200);
	
      //
      // We want to fill crse_src from lower level u_mac including u_mac's grow cells.
      // Gotta do it in steps since parallel copy only does valid region.
      //
      const MultiFab& u_macLL = u_mac_crse[n];
	  
      BoxArray edge_grids = u_macLL.boxArray();
      edge_grids.grow(1);
      
      MultiFab u_macC(edge_grids,1,0);
      
      for (MFIter mfi(u_macLL); mfi.isValid(); ++mfi)
	u_macC[mfi].copy(u_macLL[mfi]);

      crse_src.copy(u_macC);
      
      for (MFIter mfi(crse_src); mfi.isValid(); ++mfi)
	{
	  const int  nComp = 1;
	  const Box& box   = crse_src[mfi].box();
	  const int* rat   = crse_ratio.getVect();
	  FORT_PC_CF_EDGE_INTERP(box.loVect(), box.hiVect(), &nComp, rat, &n,
				 crse_src[mfi].dataPtr(),
				 ARLIM(crse_src[mfi].loVect()),
				 ARLIM(crse_src[mfi].hiVect()),
				 fine_src[mfi].dataPtr(),
				 ARLIM(fine_src[mfi].loVect()),
				 ARLIM(fine_src[mfi].hiVect()));
	}
      crse_src.clear();
      //
      // Replace pc-interpd fine data with preferred u_mac data at
      // this level u_mac valid only on surrounding faces of valid
      // region - this op will not fill grow region.
      //
      if (u_mac) {
        fine_src.copy(u_mac[n]);
      }

      for (MFIter mfi(fine_src); mfi.isValid(); ++mfi)
	{
	  //
	  // Interpolate unfilled grow cells using best data from
	  // surrounding faces of valid region, and pc-interpd data
	  // on fine edges overlaying coarse edges.
	  //
	  const int  nComp = 1;
	  const Box& fbox  = fine_src[mfi.index()].box(); 
	  const int* rat   = crse_ratio.getVect();
	  FORT_EDGE_INTERP(fbox.loVect(), fbox.hiVect(), &nComp, rat, &n,
			   fine_src[mfi].dataPtr(),
			   ARLIM(fine_src[mfi].loVect()),
			   ARLIM(fine_src[mfi].hiVect()));
	  
	}

      // This complicated copy handles the periodic boundary condition properly.
      if (u_mac) {
        MultiFab u_ghost(u_mac[n].boxArray(),1,1);
        u_ghost.setVal(1.e40);
        u_ghost.copy(u_mac[n]);
        u_ghost.FillBoundary();
        geom.FillPeriodicBoundary(u_ghost);
        for (MFIter mfi(u_macG[n]); mfi.isValid(); ++mfi) {
	  u_macG[n][mfi].copy(u_ghost[mfi]);
	}
      }
      u_macG[n].copy(fine_src);
    }
}

void
PorousMedia::GetCrseUmac(PArray<MultiFab>& u_mac_crse,
                         Real              time          ) const
{
  BL_ASSERT(level>0);
  BL_ASSERT(u_mac_crse.size() == BL_SPACEDIM);

  const PorousMedia* pm = dynamic_cast<const PorousMedia*>(&parent->getLevel(level-1));

  Real t_old = pm->state[State_Type].prevTime();
  Real t_new = pm->state[State_Type].curTime(); 
  Real alpha = (time - t_old)/(t_new - t_old);
  Real teps = 1.e-6;
  const Geometry& cgeom  = parent->Geom(level-1);
  for (int i=0; i<BL_SPACEDIM; ++i)
    {
      BL_ASSERT(!u_mac_crse.defined(i));
      const BoxArray eba = BoxArray(pm->boxArray()).surroundingNodes(i);

      int nGrow = 1;
      u_mac_crse.set(i,new MultiFab(eba, 1, nGrow));

      // This complicated copy is to ensure we copy the boundary
      // data of the coarse grid to ensure periodic boundary
      // condition is correct.
      BoxArray ebaG = BoxArray(eba).grow(nGrow);
      MultiFab emfG(ebaG,1,0);
      FArrayBox tmp;
      for (MFIter mfi(u_mac_crse[i]); mfi.isValid(); ++mfi) {
	if (alpha < teps) {
	  emfG[mfi].copy(pm->u_macG_prev[i][mfi]);
	} else if (1-alpha < teps) {
	  emfG[mfi].copy(pm->u_macG_curr[i][mfi]);
	} else {
	  emfG[mfi].copy(pm->u_macG_prev[i][mfi]);
	  emfG[mfi].mult(1-alpha);
	  tmp.resize(emfG[mfi].box(),1);
	  tmp.copy(pm->u_macG_curr[i][mfi]);
	  tmp.mult(alpha);
	  emfG[mfi].plus(tmp);
	}
      }
      for (MFIter mfi(emfG); mfi.isValid(); ++mfi) {
	u_mac_crse[i][mfi].copy(emfG[mfi]);
      }
      bool do_corners = true;
      u_mac_crse[i].FillBoundary();
      cgeom.FillPeriodicBoundary(u_mac_crse[i],do_corners);
    }
}

void
PorousMedia::GetCrsePressure (MultiFab& phi_crse,
                              Real      time      ) const
{
  if (level==0) return;

  const PorousMedia* pm = dynamic_cast<const PorousMedia*>(&parent->getLevel(level-1));
    
  Real t_old = pm->state[Press_Type].prevTime();
  Real t_new = pm->state[Press_Type].curTime();
  Real alpha = (time - t_old)/(t_new - t_old);
  const Geometry& cgeom  = parent->Geom(level-1);
    
  phi_crse.clear();
  phi_crse.define(pm->boxArray(), 1, 1, Fab_allocate); 

  // BUT NOTE we don't trust phi's ghost cells.
  FArrayBox PhiCrseTemp;

  if (std::fabs(time-t_new)<1.e-10 ) {
    const MultiFab& P_crse_new = pm->get_new_data(Press_Type);
    //MultiFab::Copy(phi_crse,P_crse_new,0,0,1,1);
    for (MFIter mfi(phi_crse); mfi.isValid(); ++mfi)
      phi_crse[mfi].copy(P_crse_new[mfi]);
      
  } 
  else if (std::fabs(time- t_old)<1.e-10) 
    {
      const MultiFab& P_crse_old = pm->get_old_data(Press_Type);
      //MultiFab::Copy(phi_crse,P_crse_old,0,0,1,1);
      for (MFIter mfi(phi_crse); mfi.isValid(); ++mfi)
	phi_crse[mfi].copy(P_crse_old[mfi]);
    
    } 
  else 
    {
      const MultiFab& P_crse_old = pm->get_old_data(Press_Type);
      const MultiFab& P_crse_new = pm->get_new_data(Press_Type);
      for (MFIter mfi(phi_crse); mfi.isValid(); ++mfi)
	{
	  PhiCrseTemp.resize(phi_crse[mfi].box(),1);

	  PhiCrseTemp.copy(P_crse_old[mfi]);
	  Real omalpha = 1.0 - alpha;
	  PhiCrseTemp.mult(omalpha);

	  phi_crse[mfi].copy(P_crse_new[mfi]);
	  phi_crse[mfi].mult(alpha);
	  phi_crse[mfi].plus(PhiCrseTemp);
	 
	}
    }

  phi_crse.FillBoundary();
  cgeom.FillPeriodicBoundary(phi_crse,true);
}

// ============
// IO Functions
// ============

void
PorousMedia::fill_from_plotfile (MultiFab&          mf,
                                 int                dcomp,
                                 const std::string& pltfile,
                                 const std::string& varname)
{
  const Real strt_time = ParallelDescriptor::second();

  if (pltfile.empty())
    BoxLib::Abort("fill_from_plotfile(): pltfile not specified");

  if (varname.empty())
    BoxLib::Abort("fill_from_plotfile(): varname not specified");

  if (verbose>1 && ParallelDescriptor::IOProcessor())
    std::cout << "fill_from_plotfile(): reading data from: " << pltfile << '\n';

  DataServices::SetBatchMode();
  Amrvis::FileType fileType(Amrvis::NEWPLT);
  DataServices dataServices(pltfile, fileType);

  if (!dataServices.AmrDataOk())
    //
    // This calls ParallelDescriptor::EndParallel() and exit()
    //
    DataServices::Dispatch(DataServices::ExitRequest, NULL);
    
  AmrData&           amrData   = dataServices.AmrDataRef();
  Array<std::string> plotnames = amrData.PlotVarNames();

  if (amrData.FinestLevel() < level)
    BoxLib::Abort("fill_from_plotfile(): not enough levels in plotfile");

  if (amrData.ProbDomain()[level] != Domain())
    BoxLib::Abort("fill_from_plotfile(): problem domains do not match");

  int idx = -1;
  for (int i = 0; i < plotnames.size(); ++i)
    if (plotnames[i] == varname) idx = i;

  if (idx == -1)
    {
      std::string msg = "fill_from_plotfile(): could not find '";
      msg += varname;
      msg += "' in the plotfile";
      BoxLib::Abort(msg.c_str());
    }

  amrData.FillVar(mf, level, varname, dcomp);
  amrData.FlushGrids(idx);

  if (verbose>1 && ParallelDescriptor::IOProcessor())
    std::cout << "fill_from_plotfile(): finished init from plotfile" << '\n';

  if (show_selected_runtimes > 0)
  {
    const int IOProc   = ParallelDescriptor::IOProcessorNumber();
    Real      run_time = ParallelDescriptor::second() - strt_time;

    ParallelDescriptor::ReduceRealMax(run_time,IOProc);

    if (ParallelDescriptor::IOProcessor())
        std::cout << "PorousMedia::fill_from_plotfile(): lev: "
                  << level
                  << ", time: " << run_time << '\n';
  }
}

void
PorousMedia::checkPoint (const std::string& dir,
                         std::ostream&  os,
                         VisMF::How     how,
                         bool           dump_old)
{
  for (int i=0; i<num_state_type; ++i) {
    if (state[i].hasOldData()) {
      get_old_data(i).setBndry(0);
    }
    if (state[i].hasNewData()) {
      get_new_data(i).setBndry(0);
    }
  }

  AmrLevel::checkPoint(dir,os,how,dump_old);
  std::string Level = BoxLib::Concatenate("Level_", level, 1);
  std::string uxfile = "/umac_x";
  std::string uyfile = "/umac_y";
  std::string FullPath = dir;
  if (!FullPath.empty() && FullPath[FullPath.length()-1] != '/')
    {
      FullPath += '/';
    }
  FullPath += Level;
  uxfile = FullPath + uxfile;
  uyfile = FullPath + uyfile;
  VisMF::Write(u_mac_curr[0], uxfile);
  VisMF::Write(u_mac_curr[1], uyfile);

  std::string utxfile = "/umact_x";
  std::string utyfile = "/umact_y";
  utxfile = FullPath + utxfile;
  utyfile = FullPath + utyfile;
  VisMF::Write(u_macG_trac[0], utxfile);
  VisMF::Write(u_macG_trac[1], utyfile);

#if (BL_SPACEDIM == 3)
  std::string uzfile = "/umac_z";
  uzfile = FullPath + uzfile;
  VisMF::Write(u_mac_curr[2], uzfile);
  std::string utzfile = "/umact_z";
  utzfile = FullPath + utzfile;
  VisMF::Write(u_macG_trac[2], utzfile);
#endif 

#ifdef MG_USE_FBOXLIB
  if ( (model != PM_RICHARDS)
       && (model != PM_STEADY_SATURATED)
       && (model != PM_SATURATED) )
    {
      std::string rxfile = "/rhs_RhoD_x";
      std::string ryfile = "/rhs_RhoD_y";
      rxfile = FullPath + rxfile;
      ryfile = FullPath + ryfile;
      VisMF::Write(rhs_RhoD[0], rxfile);
      VisMF::Write(rhs_RhoD[1], ryfile);
#if (BL_SPACEDIM == 3)
      std::string rzfile = "/rhs_RhoD_z";
      rzfile = FullPath + rzfile;
      VisMF::Write(rhs_RhoD[2], rzfile);
#endif 
    }
#endif

  os << dt_eig << '\n';
}

// =================
// Utility functions
// =================

void 
PorousMedia::check_sum()
{
  // gathering some statistics of the solutions.

  Real minmax[2] = {1,1};

  MultiFab& S_new = get_new_data(State_Type);
  FArrayBox tmp,tmp2;

  for (MFIter mfi(S_new);mfi.isValid();++mfi) 
    {
      tmp.resize(mfi.validbox(),1);
      tmp2.resize(mfi.validbox(),1);
      tmp.setVal(0);
      tmp2.setVal(0);
    
      for (int kk=0; kk < ncomps; kk++)
	{
	  if (solid.compare(pNames[pType[kk]]) != 0) {
	    tmp2.copy(S_new[mfi],mfi.validbox(),kk,mfi.validbox(),0,1);
	    tmp2.mult(1.0/density[kk]);
	    tmp.plus(tmp2,mfi.validbox(),0,0,1);
	  }
	}
      minmax[0] = std::min(minmax[0],tmp.min(mfi.validbox(),0));
      minmax[1] = std::max(minmax[1],tmp.max(mfi.validbox(),0));
    }
    
  const int IOProc = ParallelDescriptor::IOProcessorNumber();

  ParallelDescriptor::ReduceRealMax(&minmax[0],2,IOProc);

  if (verbose>3 && ParallelDescriptor::IOProcessor())
    {
      std::cout << "   SUM SATURATION MAX/MIN = " 
		<< minmax[1] << ' ' << minmax[0] << '\n';
    }
}

void 
PorousMedia::check_minmax()
{
  MultiFab* rho;
  MultiFab& S_new = get_new_data(State_Type);
  
  rho = new MultiFab(grids,1,0);
  MultiFab::Copy(*rho,S_new,0,0,1,0);

  for (int kk = 1; kk<ncomps; kk++)
    {
      if (solid.compare(pNames[pType[kk]]) != 0) 
	MultiFab::Add(*rho,S_new,kk,0,1,0);
    }
 
  Array<Real> smin(ncomps,1.e20), smax(ncomps,-1.e20);

  for (int kk = 0; kk < ncomps; kk++)
    {
      for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
	{
	  smax[kk] = std::max(smax[kk],S_new[mfi].max(mfi.validbox(),kk));
	  smin[kk] = std::min(smin[kk],S_new[mfi].min(mfi.validbox(),kk));
	}
    }
  const int IOProc = ParallelDescriptor::IOProcessorNumber();

  ParallelDescriptor::ReduceRealMax(smax.dataPtr(), ncomps, IOProc);
  ParallelDescriptor::ReduceRealMin(smin.dataPtr(), ncomps, IOProc);
  
  if (verbose > 3 && ParallelDescriptor::IOProcessor())
    {
      for (int kk = 0; kk < ncomps; kk++)
	{
	  std::cout << "   SNEW MAX/MIN OF COMP " << kk
		    << ' ' << smax[kk] << "  " << smin[kk] << '\n';
	}
    }

  Real rhomaxmin[2] = {-1.e20,+1.e20};
  for (MFIter mfi(*rho); mfi.isValid(); ++mfi)
    {
      rhomaxmin[0] = std::max(rhomaxmin[0],(*rho)[mfi].max(mfi.validbox(),0));
      rhomaxmin[1] = std::min(rhomaxmin[1],(*rho)[mfi].min(mfi.validbox(),0));
    }

  ParallelDescriptor::ReduceRealMax(&rhomaxmin[0], 1, IOProc);
  ParallelDescriptor::ReduceRealMin(&rhomaxmin[1], 1, IOProc);

  if (verbose > 3 && ParallelDescriptor::IOProcessor())
    {  
      std::cout << "   RHO MAX/MIN "
		<< ' ' << rhomaxmin[0] << "  " << rhomaxmin[1] << '\n';
    }

  delete rho;
}

void 
PorousMedia::check_minmax(int fscalar, int lscalar)
{
  MultiFab& S_new = get_new_data(State_Type);
  
  const int nscal = lscalar - fscalar + 1;

  Array<Real> smin(nscal,1.e20), smax(nscal,-1.e20);

  for (int kk = 0; kk < nscal; kk++)
    {
      for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
	{
            smax[kk] = std::max(smax[kk], S_new[mfi].max(mfi.validbox(),fscalar+kk));
            smin[kk] = std::min(smin[kk], S_new[mfi].min(mfi.validbox(),fscalar+kk));
	}
    }
  const int IOProc = ParallelDescriptor::IOProcessorNumber();
  ParallelDescriptor::ReduceRealMax(smax.dataPtr(), nscal, IOProc);
  ParallelDescriptor::ReduceRealMin(smin.dataPtr(), nscal, IOProc);
  
  if (verbose>3 && ParallelDescriptor::IOProcessor())
    {
        for (int kk = 0; kk < nscal; kk++)
	{
	  std::cout << "   SNEW MAX/MIN OF COMP "
                    << fscalar+kk
		    << ' ' << smax[kk] 
		    << ' ' << smin[kk] << '\n';
	}
    }
}

void 
PorousMedia::check_minmax(MultiFab& mf)
{
  const int ncomp = mf.nComp();
  Array<Real> smin(ncomp,1.e20), smax(ncomp,-1.e20);

  for (int kk = 0; kk < ncomp; kk++)
    {
      for (MFIter mfi(mf); mfi.isValid(); ++mfi)
	{
	  smax[kk] = std::max(smax[kk],mf[mfi].max(mfi.validbox(),kk));
	  smin[kk] = std::min(smin[kk],mf[mfi].min(mfi.validbox(),kk));
	}
    }
  const int IOProc = ParallelDescriptor::IOProcessorNumber();

  ParallelDescriptor::ReduceRealMax(smax.dataPtr(), ncomp, IOProc);
  ParallelDescriptor::ReduceRealMin(smin.dataPtr(), ncomp, IOProc);
  
  if (verbose>3 && ParallelDescriptor::IOProcessor())
    {
      for (int kk = 0; kk < ncomp; kk++)
	{
	  std::cout << " MAX/MIN OF MF " << kk
		    << ' ' << smax[kk] << "  " << smin[kk] << '\n';
	}
    }
}

void 
PorousMedia::check_minmax(MultiFab* u_mac)
{
  //
  // Write out the min and max of the MAC velocities.
  //
  Real umax[BL_SPACEDIM] = {D_DECL(-1.e20,-1.e20,-1.e20)};
  Real umin[BL_SPACEDIM] = {D_DECL(+1.e20,+1.e20,+1.e20)};

  for (MFIter mfi(u_mac[0]); mfi.isValid(); ++mfi)
    {
      const int i = mfi.index();

      umax[0] = std::max(umax[0],u_mac[0][i].max(u_mac[0].boxArray()[i]));
      umin[0] = std::min(umin[0],u_mac[0][i].min(u_mac[0].boxArray()[i]));
      umax[1] = std::max(umax[1],u_mac[1][i].max(u_mac[1].boxArray()[i]));
      umin[1] = std::min(umin[1],u_mac[1][i].min(u_mac[1].boxArray()[i]));
#if(BL_SPACEDIM == 3)
      umax[2] = std::max(umax[2],u_mac[2][i].max(u_mac[2].boxArray()[i]));
      umin[2] = std::min(umin[2],u_mac[2][i].min(u_mac[2].boxArray()[i]));
#endif
    }

  const int IOProc = ParallelDescriptor::IOProcessorNumber();

  ParallelDescriptor::ReduceRealMax(&umax[0], BL_SPACEDIM, IOProc);
  ParallelDescriptor::ReduceRealMin(&umin[0], BL_SPACEDIM, IOProc);

  if (verbose>3 && ParallelDescriptor::IOProcessor())
  {
      D_TERM(std::cout << "   UMAC MAX/MIN  " << umax[0] << "  " << umin[0] << '\n';,
             std::cout << "   VMAC MAX/MIN  " << umax[1] << "  " << umin[1] << '\n';,
             std::cout << "   WMAC MAX/MIN  " << umax[2] << "  " << umin[2] << '\n';);
  }
}

void
PorousMedia::umac_edge_to_cen(MultiFab* u_mac, MultiFab& U_cc, bool do_upwind)
{
  int upwind_flag = (do_upwind ? 1 : 0);
  // average velocity onto cell center
  for (MFIter mfi(U_cc); mfi.isValid(); ++mfi)
    {
      const int* lo     = mfi.validbox().loVect();
      const int* hi     = mfi.validbox().hiVect();
    
      const int* u_lo   = U_cc[mfi].loVect();
      const int* u_hi   = U_cc[mfi].hiVect();
      const Real* udat  = U_cc[mfi].dataPtr();
	  
      const int* um_lo  = (u_mac[0])[mfi].loVect();
      const int* um_hi  = (u_mac[0])[mfi].hiVect();
      const Real* umdat = (u_mac[0])[mfi].dataPtr();
	
      const int* vm_lo  = (u_mac[1])[mfi].loVect();
      const int* vm_hi  = (u_mac[1])[mfi].hiVect();
      const Real* vmdat = (u_mac[1])[mfi].dataPtr();
	
#if (BL_SPACEDIM == 3)
      const int* wm_lo  = (u_mac[2])[mfi].loVect();
      const int* wm_hi  = (u_mac[2])[mfi].hiVect();
      const Real* wmdat = (u_mac[2])[mfi].dataPtr();
#endif

      FORT_AVG_UMAC(umdat,ARLIM(um_lo),ARLIM(um_hi),
		    vmdat,ARLIM(vm_lo),ARLIM(vm_hi),
#if (BL_SPACEDIM == 3)
		    wmdat,ARLIM(wm_lo),ARLIM(wm_hi),
#endif
		    udat ,ARLIM( u_lo),ARLIM( u_hi),lo,hi,&upwind_flag);
    }
}

void
PorousMedia::umac_cpy_edge_to_cen(MultiFab* u_mac, int idx_type, int ishift)
{
  // average velocity onto cell center
  MultiFab&  U_cor  = get_new_data(idx_type);
  for (MFIter mfi(U_cor); mfi.isValid(); ++mfi)
    {
      const int* lo     = mfi.validbox().loVect();
      const int* hi     = mfi.validbox().hiVect();
    
      const int* u_lo   = U_cor[mfi].loVect();
      const int* u_hi   = U_cor[mfi].hiVect();
      const Real* udat  = U_cor[mfi].dataPtr();
	  
      const int* um_lo  = (u_mac[0])[mfi].loVect();
      const int* um_hi  = (u_mac[0])[mfi].hiVect();
      const Real* umdat = (u_mac[0])[mfi].dataPtr();
	
      const int* vm_lo  = (u_mac[1])[mfi].loVect();
      const int* vm_hi  = (u_mac[1])[mfi].hiVect();
      const Real* vmdat = (u_mac[1])[mfi].dataPtr();
	
#if (BL_SPACEDIM == 3)
      const int* wm_lo  = (u_mac[2])[mfi].loVect();
      const int* wm_hi  = (u_mac[2])[mfi].hiVect();
      const Real* wmdat = (u_mac[2])[mfi].dataPtr();
#endif

      FORT_CPY_UMAC(umdat,ARLIM(um_lo),ARLIM(um_hi),
		    vmdat,ARLIM(vm_lo),ARLIM(vm_hi),
#if (BL_SPACEDIM == 3)
		    wmdat,ARLIM(wm_lo),ARLIM(wm_hi),
#endif
		    udat ,ARLIM( u_lo),ARLIM( u_hi),lo,hi, &ishift); 
    }
}

void
PorousMedia::compute_divu (MultiFab& soln,
			   MultiFab* umac)
{
  //
  // This compute the divergence of umac
  //

  const Real* dx   = geom.CellSize();

  for (MFIter fpi(soln); fpi.isValid(); ++fpi)
    {
      const int i = fpi.index();
      const int* lo = fpi.validbox().loVect();
      const int* hi = fpi.validbox().hiVect();

      const Real* sdat = soln[i].dataPtr();
      const int* s_lo  = soln[i].loVect();
      const int* s_hi  = soln[i].hiVect();
    
      const Real* uxdat = umac[0][i].dataPtr();
      const int*  uxlo  = umac[0][i].loVect();
      const int*  uxhi  = umac[0][i].hiVect();

      const Real* uydat = umac[1][i].dataPtr();
      const int*  uylo  = umac[1][i].loVect();
      const int*  uyhi  = umac[1][i].hiVect();

#if (BL_SPACEDIM == 3)
      const Real* uzdat = umac[2][i].dataPtr();
      const int*  uzlo  = umac[2][i].loVect();
      const int*  uzhi  = umac[2][i].hiVect();
#endif

      FORT_DIV_UMAC (sdat, ARLIM(s_lo),ARLIM(s_hi),
		     uxdat,ARLIM(uxlo),ARLIM(uxhi),
		     uydat,ARLIM(uylo),ARLIM(uyhi),
#if (BL_SPACEDIM == 3)
		     uzdat,ARLIM(uzlo),ARLIM(uzhi),
#endif
		     lo,hi,dx);
    }
}
