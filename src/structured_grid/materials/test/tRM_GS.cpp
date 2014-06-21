#include <iostream>
#include <fstream>
#include <iomanip>
using std::cout;
using std::endl;
#include <ParmParse.H>
#include <VisMF.H>

#include <RockManager.H>

int
main (int   argc,
      char* argv[])
{
  BoxLib::Initialize(argc,argv);

  ParmParse pp;

  int nLevs = 3; pp.query("nLevs",nLevs);
  BL_ASSERT(nLevs>0);
  Array<int> n_cells;
  pp.getarr("n_cells",n_cells,0,BL_SPACEDIM);

  Array<int> rRatio(nLevs-1,4);
  if (nLevs>1) {
    pp.getarr("refine_ratio",rRatio,0,nLevs-1);
  }

  Array<IntVect> refRatio(rRatio.size());
  for (int lev=0; lev<rRatio.size(); ++lev) {
    refRatio[lev] = rRatio[lev] * IntVect::TheUnitVector();
  }

  Array<Geometry> geomArray(nLevs);
  for (int lev=0; lev<nLevs; ++lev) {
    Box domain;
    if (lev==0) {
      IntVect be;
      for (int d=0; d<BL_SPACEDIM; ++d) {
        be[d] = n_cells[d] - 1;
      }
      domain = Box(IntVect(D_DECL(0,0,0)),be);
    }
    else {
      domain = Box(geomArray[lev-1].Domain()).refine(refRatio[lev-1]);
    }
    geomArray[lev] = Geometry(domain);
  }

  Region::domlo.resize(BL_SPACEDIM);
  Region::domhi.resize(BL_SPACEDIM);
  for (int d=0; d<BL_SPACEDIM; ++d) {
    Region::domlo[d] = Geometry::ProbLo()[d];
    Region::domhi[d] = Geometry::ProbHi()[d];
  }

  int nGrow = 3;
  RegionManager rm;
  RockManager rockManager(&rm);
  rockManager.FinalizeBuild(geomArray,refRatio,nGrow);

  int maxSize=32;  pp.query("maxSize",maxSize);
  Real time = 0;
  for (int lev=0; lev<nLevs; ++lev) {
    BoxArray ba(geomArray[lev].Domain());
    ba.maxSize(maxSize);
  
    MultiFab phi(ba,1,nGrow);
    ParallelDescriptor::Barrier();
    rockManager.Porosity(time,lev,phi,0,nGrow);
  }
  BoxLib::Finalize();
  return 0;
}