#include "amanzi_structured_grid_simulation_driver.H"
#include "ParmParse.H"
#include "PMAmr.H"
#include "PMAMR_Labels.H"
#include "PorousMedia.H"

#include "ParmParseHelpers.H"

static std::map<std::string,std::string>& AMR_to_Amanzi_label_map = Amanzi::AmanziInput::AMRToAmanziLabelMap();

static
void
EnsureFolderExists(const std::string& full_path)
{
  // Find folder name first, and ensure folder exists
  // FIXME: Will fail on Windows
  const std::vector<std::string>& tokens = BoxLib::Tokenize(full_path,"/");
  std::string dir = (full_path[0] == '/' ? "/" : "");
  for (int i=0; i<tokens.size()-1; ++i) {
    dir += tokens[i];
    if (i<tokens.size()-2) dir += "/";
  }
  
  if(!BoxLib::FileExists(dir)) {
    if ( ! BoxLib::UtilCreateDirectory(dir, 0755)) {
      BoxLib::CreateDirectoryFailed(dir);
    }
  }
}


void
Structured_observations(const PArray<Observation>& observation_array,
			Amanzi::ObservationData& observation_data)
{
  for (int i=0; i<observation_array.size(); ++i)
    {
      std::string label = AMR_to_Amanzi_label_map[observation_array[i].name];
      int ntimes = observation_array[i].times.size();
      std::vector<Amanzi::ObservationData::DataTriple> dt(ntimes);
      const std::map<int, Real> vals = observation_array[i].vals;
      for (std::map<int, Real>::const_iterator it = vals.begin();
           it != vals.end(); ++it) {
        int j = it->first;
        dt[j].value = it->second;
        dt[j].time = observation_array[i].times[j];
        dt[j].is_valid = true;
      }
      observation_data[label] = dt;
    }	
}  

Amanzi::Simulator::ReturnType
AmanziStructuredGridSimulationDriver::Run (const MPI_Comm&               mpi_comm,
                                           Teuchos::ParameterList&       input_parameter_list,
                                           Amanzi::ObservationData&      output_observations)
{
    int argc=0;
    char** argv;

#ifdef BL_USE_PETSC
    std::string petsc_help = "Amanzi-S passthrough access to PETSc help option\n";
    std::string petsc_file_str = "Petsc Options File";
    std::string petsc_options_file;
    if (input_parameter_list.isParameter(petsc_file_str))
    {
	petsc_options_file = Teuchos::getParameter<std::string>(input_parameter_list, petsc_file_str);
    }
    PetscInitialize(&argc,&argv,petsc_options_file.c_str(),petsc_help.c_str());
#endif

    BoxLib::Initialize(argc,argv,false,mpi_comm);

    bool pause_for_debug = false;
    if (input_parameter_list.isParameter("Pause For Debug"))
      {
          pause_for_debug= Teuchos::getParameter<bool>(input_parameter_list, "Pause For Debug");
      }

    if ( pause_for_debug && ParallelDescriptor::IOProcessor() ) {
        std::string junk;
        std::cout << "Waiting to attach debugger.  Enter any string to continue ";
        std::cin >> junk;
    }
    ParallelDescriptor::Barrier();

    if ( pause_for_debug && ParallelDescriptor::IOProcessor() ) {
        std::cout << "   continuing run..." << std::endl;
    }

    if (input_parameter_list.isParameter("PPfile"))
      {
	const std::string& PPfile = Teuchos::getParameter<std::string>(input_parameter_list, "PPfile");
	ParmParse::Initialize(argc,argv,PPfile.c_str());
      }

    // Determine whether we need to convert to input file to 
    //native structured format
    bool native = input_parameter_list.get<bool>("Native Structured Input",false);
    Teuchos::ParameterList converted_parameter_list;
    if (!native) 
      converted_parameter_list =
	Amanzi::AmanziInput::convert_to_structured(input_parameter_list);
    else
      converted_parameter_list = input_parameter_list;

    if (input_parameter_list.isParameter("EchoXMLfile"))
      {
        const std::string& EchoXMLfile = Teuchos::getParameter<std::string>(input_parameter_list, "EchoXMLfile");
        Teuchos::writeParameterListToXmlFile(converted_parameter_list,EchoXMLfile);
      }

    // Stuff away a static copy of the input parameters
    PorousMedia::SetInputParameterList(converted_parameter_list);

    BoxLib::Initialize_ParmParse(converted_parameter_list);

    if (input_parameter_list.isParameter("Dump ParmParse Table") ) {
      if (ParallelDescriptor::IOProcessor()) {
        const std::string& pp_file = Teuchos::getParameter<std::string>(input_parameter_list, "Dump ParmParse Table");
        EnsureFolderExists(pp_file);
        std::ofstream ofs; ofs.open(pp_file.c_str());
        bool prettyPrint = false;
        ParmParse::dumpTable(ofs,prettyPrint);
        ofs.close();
      }
      ParallelDescriptor::Barrier();
    }

    const Real run_strt = ParallelDescriptor::second();

    int  max_step;
    Real strt_time;
    Real stop_time;

    ParmParse pp;

    max_step  = -1;    
    strt_time =  0.0;  
    stop_time = -1.0;

    pp.get("max_step",max_step);
    pp.get("strt_time",strt_time);
    pp.get("stop_time",stop_time);

    if (strt_time < 0.0)
        BoxLib::Abort("MUST SPECIFY a non-negative strt_time");

    if (max_step < 0 && stop_time < 0.0)
    {
        BoxLib::Abort(
            "Exiting because neither max_step nor stop_time is non-negative.");
    }

    PMAmr* amrptr = new PMAmr;

    amrptr->init(strt_time,stop_time);
    
    // If we set the regrid_on_restart flag and if we are *not* going to take
    //    a time step then we want to go ahead and regrid here.
    if ( amrptr->RegridOnRestart() && 
         ( (amrptr->levelSteps(0) >= max_step) ||
           (amrptr->cumTime() >= stop_time) ) )
    {
        //
        // Regrid only!
        //
        amrptr->RegridOnly(amrptr->cumTime());
    }
    
    while ( amrptr->okToContinue() )
    {
        amrptr->coarseTimeStep(stop_time);
    }

    // Write final checkpoint and plotfile
    if (amrptr->stepOfLastCheckPoint() < amrptr->levelSteps(0)) {
        amrptr->checkPoint();
    }

    if (amrptr->stepOfLastPlotFile() < amrptr->levelSteps(0)) {
        amrptr->writePlotFile();
    }

    // Process the observations
    const PArray<Observation>& observation_array = amrptr->TheObservations();

    Structured_observations(observation_array,output_observations);

    delete amrptr;

    const int IOProc   = ParallelDescriptor::IOProcessorNumber();
    Real      run_stop = ParallelDescriptor::second() - run_strt;

    ParallelDescriptor::ReduceRealMax(run_stop,IOProc);

    if (ParallelDescriptor::IOProcessor())
      {
        std::cout << "Run time = " << run_stop << std::endl;
	std::cout << "SCOMPLETED\n";
      }

    BoxLib::Finalize(false); // Calling routine responsible for MPI_Finalize call
#ifdef BL_USE_PETSC
    PetscFinalize();
#endif

    return Amanzi::Simulator::SUCCESS;
}