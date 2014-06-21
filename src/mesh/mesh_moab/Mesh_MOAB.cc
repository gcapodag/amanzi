#include "Mesh_MOAB.hh"
//#include <Teuchos_RCP.hpp>

#include "dbc.hh"
#include "errors.hh"

using namespace std;
using namespace moab;


namespace Amanzi
{
namespace AmanziMesh
{

  // Constructor - load up mesh from file

  Mesh_MOAB::Mesh_MOAB (const char *filename, const Epetra_MpiComm *comm, 
			const AmanziGeometry::GeometricModelPtr& gm) 
{
  int result, rank;
  
  clear_internals_();

    
  // Core MOAB object
    
  mbcore = new MBCore();

  if (comm) {
    // MOAB's parallel communicator
    int mbcomm_id;
    mbcomm = new ParallelComm(mbcore,comm->GetMpiComm(),&mbcomm_id);

    if (!mbcomm) {
      cerr << "Failed to initialize MOAB communicator\n";
      assert(mbcomm == 0);
    }
  }

  Mesh::set_comm(comm);


  if (!mbcomm || mbcomm->size() == 1) 
    serial_run = true;
  else
    serial_run = false;

  if (!serial_run) {
      
    // Load partitioned mesh - serial read of mesh with partition
    // info, deletion of non-local entities, resolution of
    // interprocessor connections. If we need ghosts we have to add
    // the option "PARALLEL_GHOSTS=A.B.C.D" where A is usually the
    // topological dimension of the mesh cells, B is the bridge
    // dimension or the dimension of entities across which we want
    // ghosts (0 for vertex connected ghost cells) and C indicates
    // the number of layers of ghost cells we want, D indicates if
    // we want edges/faces bounding the ghost cells as well (1 for
    // edges, 2 for faces, 3 for faces and edges)

    // In the specification for the Ghosts we made the assumption 
    // that we are dealing with 3D meshes only

    result = 
      mbcore->load_file(filename,NULL,
			"PARALLEL=READ_DELETE;PARALLEL_RESOLVE_SHARED_ENTS;PARTITION=PARALLEL_PARTITION;PARALLEL_GHOSTS=3.0.1.2",
			NULL,NULL,0);
      
    rank = mbcomm->rank();
      
  }
  else {

    // Load serial mesh

    result =
      mbcore->load_file(filename,NULL,NULL,NULL,NULL,0);

    rank = 0;
  }

  if (result != MB_SUCCESS) {
    std::cerr << "FAILED" << std::endl;
    std::cerr << "Failed to load " << filename << " on processor " << rank << std::endl;
    std::cerr << "MOAB error code " << result << std::endl;
    assert(result == MB_SUCCESS);
  }
      
      

  // Dimension of space, mesh cells, faces etc
    
  result = mbcore->get_dimension(spacedim);
    

  // Highest topological dimension
    
  int nent;
  result = mbcore->get_number_entities_by_dimension(0,3,nent,false);
  if (result != MB_SUCCESS) {
    std::cerr << "Problem getting number of entities of dim 3" << std::endl;
    assert(result == MB_SUCCESS);
  }
  if (nent) {
    celldim = 3;
    facedim = 2;
  }
  else {
    result = mbcore->get_number_entities_by_dimension(0,2,nent,false);
    if (result != MB_SUCCESS) {
      std::cerr << "Problem getting number of entities of dim 2" << std::endl;
      assert(result == MB_SUCCESS);
    }
    if (nent) {
      celldim = 2;
      facedim = 1;
    }
    else {
      std::cerr << "Flow code works only on 2D and 3D meshes" << std::endl;
      assert(nent > 0);
    }
  }
      


  // Set the geometric model that this mesh is related to

  set_geometric_model(gm);



  { // Keep together and in this order 

    init_pvert_lists();
    init_pcell_lists(); // cells MUST be initialized before faces
    init_pface_lists();

    
    // Create maps from local IDs to MOAB entity handles (must be after
    // the various init_p*_list calls)
    
    init_id_handle_maps();


  }
    
    
  init_global_ids();


  init_pface_dirs();


  // Create Epetra_maps
  
  init_cell_map();
  init_face_map();
  init_node_map();
  


  // Initialize some info about the global number of sets, global set
  // IDs and set types

  if (Mesh::geometric_model() != NULL)
    init_set_info();

}

//--------------------------------------
// Constructor - Construct a new mesh from a subset of an existing mesh
//--------------------------------------

Mesh_MOAB::Mesh_MOAB (const Mesh *inmesh, 
                      const std::vector<std::string>& setnames, 
                      const Entity_kind setkind,
                      const bool flatten,
                      const bool extrude)
{  
  Errors::Message mesg("Construction of new mesh from an existing mesh not yet implemented in the MOAB mesh framework\n");
  Exceptions::amanzi_throw(mesg);
}

Mesh_MOAB::Mesh_MOAB (const Mesh_MOAB& inmesh, 
                      const std::vector<std::string>& setnames, 
                      const Entity_kind setkind,
                      const bool flatten,
                      const bool extrude)
{  
  Errors::Message mesg("Construction of new mesh from an existing mesh not yet implemented in the MOAB mesh framework\n");
  Exceptions::amanzi_throw(mesg);
}



Mesh_MOAB::~Mesh_MOAB() {
  delete cell_map_wo_ghosts_;
  delete cell_map_w_ghosts_;
  delete face_map_wo_ghosts_;
  delete face_map_w_ghosts_;
  delete node_map_wo_ghosts_;
  delete node_map_w_ghosts_;
  delete [] setids_;
  delete [] setdims_;
  delete [] faceflip;
  delete mbcore;
}


// Some initializations

void Mesh_MOAB::clear_internals_ () 
{ 
  mbcore = NULL;
  mbcomm = NULL;

  AllVerts.clear();
  OwnedVerts.clear();
  NotOwnedVerts.clear();
  AllFaces.clear();
  OwnedFaces.clear();
  NotOwnedFaces.clear();
  AllCells.clear();
  OwnedCells.clear();
  GhostCells.clear();
    
  lid_tag = 0;
  gid_tag = 0;
  mattag = 0;
  sstag = 0;
  nstag = 0;

  spacedim = 3;
  celldim = -1;
  facedim = -1;

  faceflip = NULL;

  cell_map_w_ghosts_ = cell_map_wo_ghosts_ = NULL;
  face_map_w_ghosts_ = face_map_wo_ghosts_ = NULL;
  node_map_w_ghosts_ = node_map_wo_ghosts_ = NULL;

  nsets = 0;
  setids_ = setdims_ = NULL;

  Mesh::set_geometric_model((Amanzi::AmanziGeometry::GeometricModelPtr) NULL);
}


void Mesh_MOAB::init_id_handle_maps() {
  int i, nv, nf, nc;
  int result;

  // Assign local IDs to entities

  int tagval = 0;
  result = mbcore->tag_get_handle("LOCAL_ID",1,
                                  MB_TYPE_INTEGER,lid_tag,
                                  MB_TAG_CREAT|MB_TAG_DENSE,&tagval);
  if (result != MB_SUCCESS) {
    std::cerr << "Problem getting tag handle for LOCAL_ID" << std::endl;
    assert(result == MB_SUCCESS);
  }
      


  nv = AllVerts.size();

  vtx_id_to_handle.reserve(nv);

  i = 0;
  for (MBRange::iterator it = OwnedVerts.begin(); it != OwnedVerts.end(); ++it) {
    MBEntityHandle vtx = *it;
    result = mbcore->tag_set_data(lid_tag,&vtx,1,&i);
    if (result != MB_SUCCESS) {
      std::cerr << "Problem getting local ID for vertex" << std::endl;
      assert(result == MB_SUCCESS);
    }
    vtx_id_to_handle[i++] = vtx;
  }    
  for (MBRange::iterator it = NotOwnedVerts.begin(); it != NotOwnedVerts.end(); ++it) {
    MBEntityHandle vtx = *it;
    result = mbcore->tag_set_data(lid_tag,&vtx,1,&i);
    if (result != MB_SUCCESS) {
      std::cerr << "Problem getting local ID for vertex" << std::endl;
      assert(result == MB_SUCCESS);
    }
    vtx_id_to_handle[i++] = vtx;
  }
    


  nf = AllFaces.size();

  face_id_to_handle.reserve(nf);

  i = 0;
  for (MBRange::iterator it = OwnedFaces.begin(); it != OwnedFaces.end(); ++it) {
    MBEntityHandle face = *it;
    result = mbcore->tag_set_data(lid_tag,&face,1,&i);
    if (result != MB_SUCCESS) {
      std::cerr << "Problem getting local ID for face" << std::endl;
      assert(result == MB_SUCCESS);
    }
    face_id_to_handle[i++] = face;
  }
  for (MBRange::iterator it = NotOwnedFaces.begin(); it != NotOwnedFaces.end(); ++it) {
    MBEntityHandle face = *it;
    result = mbcore->tag_set_data(lid_tag,&face,1,&i);
    if (result != MB_SUCCESS) {
      std::cerr << "Problem getting local ID for face" << std::endl;
      assert(result == MB_SUCCESS);
    }
    face_id_to_handle[i++] = face;
  }
    


  nc = AllCells.size();

  cell_id_to_handle.reserve(nc);

  i = 0;
  for (MBRange::iterator it = OwnedCells.begin(); it != OwnedCells.end(); ++it) {
    MBEntityHandle cell = *it;
    result = mbcore->tag_set_data(lid_tag,&cell,1,&i);
    if (result != MB_SUCCESS) {
      std::cerr << "Problem getting local ID for cell" << std::endl;
      assert(result == MB_SUCCESS);
    }
    cell_id_to_handle[i++] = cell;
  }
  for (MBRange::iterator it = GhostCells.begin(); it != GhostCells.end(); ++it) {
    MBEntityHandle cell = *it;
    result = mbcore->tag_set_data(lid_tag,&cell,1,&i);
    if (result != MB_SUCCESS) {
      std::cerr << "Problem getting local ID for cell" << std::endl;
      assert(result == MB_SUCCESS);
    }
    cell_id_to_handle[i++] = cell;
  }
    

}



void Mesh_MOAB::init_global_ids() {
  int result;
    
  if (!serial_run) {
    // Ask Parallel Communicator to assign global IDs to entities

    bool largest_dim_only=false;
    int start_id=0;
    int largest_dim=celldim;
    result = mbcomm->assign_global_ids(0,largest_dim,start_id,largest_dim_only);
    if (result != MB_SUCCESS) {
      std::cerr << "Problem assigning global IDS" << std::endl;
      assert(result == MB_SUCCESS);
    }
    

    // Exchange global IDs across all processors

    result = mbcore->tag_get_handle("GLOBAL_ID",gid_tag);
    if (result != MB_SUCCESS) {
      std::cerr << "Could not get tag handle for GLOBAL_ID data" << std::endl;
      assert(result == MB_SUCCESS);
    }

    mbcomm->exchange_tags(gid_tag,AllVerts);
    mbcomm->exchange_tags(gid_tag,AllFaces);
    mbcomm->exchange_tags(gid_tag,AllCells);

  }
  else {
    // Serial case - we assign global IDs ourselves

    int tagval = 0;
    result = mbcore->tag_get_handle("GLOBAL_ID",1,
                                    MB_TYPE_INTEGER,gid_tag,
                                    MB_TAG_CREAT|MB_TAG_DENSE,&tagval);
    if (result != MB_SUCCESS) {
      std::cerr << "Problem getting tag handle for GLOBAL_ID" << std::endl;
      assert(result == MB_SUCCESS);
    }
      
    int nent = AllVerts.size();
    int *gids = new int[nent];
    for (int i = 0; i < nent; i++) gids[i] = i;

    result = mbcore->tag_set_data(gid_tag,AllVerts,gids);
    if (result != MB_SUCCESS) {
      std::cerr << "Problem setting global IDs for vertices" << std::endl;
      assert(result == MB_SUCCESS);
    }

    delete [] gids;


    nent = AllFaces.size();
    gids = new int[nent];
    for (int i = 0; i < nent; i++) gids[i] = i;

    result = mbcore->tag_set_data(gid_tag,AllFaces,gids);
    if (result != MB_SUCCESS) {
      std::cerr << "Problem setting global IDs for faces" << std::endl;
      assert(result == MB_SUCCESS);
    }

    delete [] gids;


    nent = AllCells.size();
    gids = new int[nent];
    for (int i = 0; i < nent; i++) gids[i] = i;

    result = mbcore->tag_set_data(gid_tag,AllCells,gids);
    if (result != MB_SUCCESS) {
      std::cerr << "Problem setting global IDs for cells" << std::endl;
      assert(result == MB_SUCCESS);
    }

    delete [] gids;
  }


}


void Mesh_MOAB::init_pvert_lists() {
  int result;

  // Get all vertices on this processor 
    
  result = mbcore->get_entities_by_dimension(0,0,AllVerts,false);
  if (result != MB_SUCCESS) {
    std::cerr << "Could not get vertices" << std::endl;
    assert(result == MB_SUCCESS);
  }    


  // Get not owned vertices

  result = mbcomm->get_pstatus_entities(0,PSTATUS_NOT_OWNED,NotOwnedVerts);
  if (result != MB_SUCCESS) {
    std::cerr << "Could not get NotOwned vertices" << std::endl;
    assert(result == MB_SUCCESS);
  }

  // Subtract from all vertices on processor to get owned vertices only

  OwnedVerts = AllVerts;  // I think we DO want a data copy here
  OwnedVerts -= NotOwnedVerts;

}


// init_pface_lists is more complicated than init_pvert_lists and
// init_pcell_lists because of the way MOAB is setting up shared
// entities and ghost entities. When we ask MOAB to resolve shared
// entities, then MOAB sets up faces on interprocessor boundaries and
// assigns each of them to some processor. Therefore, the pstatus tags
// on these faces are correctly set. On the other hand when we ask for
// ghost cells, MOAB does not automatically create ghost faces. Also,
// when we go through ghost cells and create their faces, MOAB does
// not tag them as ghost faces, tagging them as owned faces
// instead. So we have to process them specially.


void Mesh_MOAB::init_pface_lists() {
  int result;


  // Make MOAB create the missing 'faces' (faces in 3D, edges in
  // 2D). We do this by looping over the cells and asking for their
  // faces with the create_if_missing=true option


  for (MBRange::iterator it = AllCells.begin(); it != AllCells.end(); it++) {
    MBEntityHandle cell = *it;
    MBRange cfaces;
      
    result = mbcore->get_adjacencies(&cell,1,facedim,true,cfaces,MBCore::UNION);
    if (result != MB_SUCCESS) {
      std::cerr << "Could not get faces of cell" << cell << std::endl;
      assert(result == MB_SUCCESS);
    }
  }
    

  // Get all "faces" (edges in 2D, faces in 3D) on this processor
    
  result = mbcore->get_entities_by_dimension(0,facedim,AllFaces,false);
  if (result != MB_SUCCESS) {
    std::cerr << "Could not get 'faces'" << std::endl;
    assert(result == MB_SUCCESS);
  }


  // Get not owned faces 

  result = mbcomm->get_pstatus_entities(facedim,PSTATUS_NOT_OWNED,NotOwnedFaces);
  if (result != MB_SUCCESS) {
    std::cerr << "Could not get NotOwned 'faces'" << std::endl;
    assert(result == MB_SUCCESS);
  }


  // Subtract from all faces on processor to get owned faces only
    
  OwnedFaces = AllFaces;  // I think we DO want a data copy here
  OwnedFaces -= NotOwnedFaces;
    

}


void Mesh_MOAB::init_pface_dirs() {
  int result, zero=0, minus1=-1;
  int face_lid, face_gid, cell_gid;
  int sidenum, offset, facedir;
  MBEntityHandle cell, face;
  int DebugWait=1;

  // Do some additional processing to see if ghost faces and their masters
  // are oriented the same way; if not, turn on flag to flip them


  /* In this code, we increment local values of global IDs by 1 so
     that we can distinguish between the lowest gid and no data */

  MBTag tmp_fc0_tag, tmp_fc1_tag;
  result = mbcore->tag_get_handle("TMP_FC0_TAG",1,
                                  MB_TYPE_INTEGER, tmp_fc0_tag,
                                  MB_TAG_CREAT|MB_TAG_DENSE,&zero);
  if (result != MB_SUCCESS) {
    std::cerr << "Problem getting new tag handle" << std::endl;
    assert(result == MB_SUCCESS);
  }
  result = mbcore->tag_get_handle("TMP_FC1_TAG",1,
                                  MB_TYPE_INTEGER, tmp_fc1_tag,
                                  MB_TAG_CREAT|MB_TAG_DENSE,&zero);
  if (result != MB_SUCCESS) {
    std::cerr << "Problem getting new tag handle" << std::endl;
    assert(result == MB_SUCCESS);
  }

  
  for (MBRange::iterator it = OwnedFaces.begin(); it != OwnedFaces.end(); it++) {
    MBRange fcells;
    face = *it;

    result = mbcore->get_adjacencies(&face,1,celldim,false,fcells,MBCore::UNION);
    if (result != MB_SUCCESS) {
      std::cout << "Could not get cells of face" << std::endl;
      assert(result == MB_SUCCESS);
    }

    result = mbcore->tag_set_data(tmp_fc0_tag,&face,1,&zero);
    if (result != MB_SUCCESS) {
      std::cerr << "Problem setting tag data" << std::endl;
      assert(result == MB_SUCCESS);
    }
    result = mbcore->tag_set_data(tmp_fc1_tag,&face,1,&zero);
    if (result != MB_SUCCESS) {
      std::cerr << "Problem setting tag data" << std::endl;
      assert(result == MB_SUCCESS);
    }


    for (MBRange::iterator jt = fcells.begin(); jt != fcells.end(); ++jt) {

      cell = *jt;
      result = mbcore->side_number(cell,face,sidenum,facedir,offset);
      if (result != MB_SUCCESS) {
	cout << "Could not get face dir w.r.t. cell" << std::endl;
	assert(result == MB_SUCCESS);
      }

      result = mbcore->tag_get_data(gid_tag,&cell,1,&cell_gid);
      if (result != MB_SUCCESS) {
	std::cerr << "Problem getting tag data" << std::endl;
	assert(result == MB_SUCCESS);
      }
      cell_gid += 1; 

      if (facedir == 1) {
	result = mbcore->tag_set_data(tmp_fc0_tag,&face,1,&cell_gid);
	if (result != MB_SUCCESS) {
	  std::cerr << "Problem setting tag data" << std::endl;
	  assert(result == MB_SUCCESS);
	}
      }
      else {
	result = mbcore->tag_set_data(tmp_fc1_tag,&face,1,&cell_gid);
	if (result != MB_SUCCESS) {
	  std::cerr << "Problem setting tag data" << std::endl;
	  assert(result == MB_SUCCESS);
	}
      }
    }

  }



  result = mbcomm->exchange_tags(tmp_fc0_tag,AllFaces);
  if (result != MB_SUCCESS) {
    std::cout << "Could not get exchange tag data successfully" << std::endl;
    assert(result == MB_SUCCESS);
  }

  result = mbcomm->exchange_tags(tmp_fc1_tag,AllFaces);
  if (result != MB_SUCCESS) {
    std::cout << "Could not get exchange tag data successfully" << std::endl;
    assert(result == MB_SUCCESS);
  }



  faceflip = new bool[AllFaces.size()];
  for (int i = 0; i < AllFaces.size(); i++) faceflip[i] = false;

  for (MBRange::iterator it = NotOwnedFaces.begin(); it != NotOwnedFaces.end(); it++) {
    MBRange fcells;
    int ghost_cell0_gid = 0, ghost_cell1_gid = 0;
    int master_cell0_gid = 0, master_cell1_gid = 0;

    face = *it;


    result = mbcore->tag_get_data(tmp_fc0_tag,&face,1,&master_cell0_gid);
    if (result != MB_SUCCESS) {
      std::cout << "Could not get face tag data" << std::endl;
      assert(result == MB_SUCCESS);
    }
    result = mbcore->tag_get_data(tmp_fc1_tag,&face,1,&master_cell1_gid);
    if (result != MB_SUCCESS) {
      std::cout << "Could not get face tag data" << std::endl;
      assert(result == MB_SUCCESS);
    }


    result = mbcore->get_adjacencies(&face,1,celldim,false,fcells,MBCore::UNION);
    if (result != MB_SUCCESS) {
      std::cout << "Could not get cells of face" << std::endl;
      assert(result == MB_SUCCESS);
    }


    for (MBRange::iterator jt = fcells.begin(); jt != fcells.end(); ++jt) {
      cell = *jt;

      result = mbcore->side_number(cell,face,sidenum,facedir,offset);
      if (result != MB_SUCCESS) {
	cout << "Could not get face dir w.r.t. cell" << std::endl;
	assert(result == MB_SUCCESS);
      }

      if (facedir == 1) {
	result = mbcore->tag_get_data(gid_tag,&cell,1,&ghost_cell0_gid);
	if (result != MB_SUCCESS) {
	  std::cerr << "Problem getting tag data" << std::endl;
	  assert(result == MB_SUCCESS);
	}
	ghost_cell0_gid += 1;
      }
      else {
	result = mbcore->tag_get_data(gid_tag,&cell,1,&ghost_cell1_gid);
	if (result != MB_SUCCESS) {
	  std::cerr << "Problem getting tag data" << std::endl;
	  assert(result == MB_SUCCESS);
	}
	ghost_cell1_gid += 1;
      }
    }

    if (ghost_cell0_gid == master_cell1_gid || 
	ghost_cell1_gid == master_cell0_gid) {

      // Both cells don't have to match because a ghost face may 
      // not have the cell on the other side
      
      result = mbcore->tag_get_data(lid_tag,&face,1,&face_lid);
      if (result != MB_SUCCESS) {
	cout << "Could not get face tag data" << std::endl;
	assert(result == MB_SUCCESS);
      }
      faceflip[face_lid] = true;

    }
    else { // Sanity check
      if (ghost_cell0_gid != master_cell0_gid &&
	  ghost_cell1_gid != master_cell1_gid) {

	// Problem if there is no match at all

	//
	result = mbcore->tag_get_data(gid_tag,&face,1,&face_gid);
	if (result != MB_SUCCESS) {
	  std::cerr << "Problem getting tag data" << std::endl;
	  assert(result == MB_SUCCESS);
	}
	//

	cout << "Face cells mismatch between master and ghost (processor " << mbcomm->rank() << ")" << std::endl;
	cout << " Face " << face_gid << std::endl;
	cout << "Master cells " << master_cell0_gid << " " << master_cell1_gid << std::endl;
	cout << "Ghost cells " << ghost_cell0_gid << " " << ghost_cell1_gid << std::endl;
      }
    }
  }

}


void Mesh_MOAB::init_pcell_lists() {
  int result;

  // Get all cells (faces in 2D, regions in 3D) on this processor
    
  result = mbcore->get_entities_by_dimension(0,celldim,AllCells,false);
  if (result != MB_SUCCESS) {
    std::cerr << "Could not get cells" << std::endl;
    assert(result == MB_SUCCESS);
  }
    
  // Get not owned cells (which is the same as ghost cells)

  result = mbcomm->get_pstatus_entities(celldim,PSTATUS_GHOST,GhostCells);
  if (result != MB_SUCCESS) {
    std::cerr << "Could not get ghost cells" << std::endl;
    assert(result == MB_SUCCESS);
  }
    
  // Subtract from all cells on processor to get owned cells only
    
  OwnedCells = AllCells;  // I think we DO want a data copy here
  OwnedCells -= GhostCells;
}



void Mesh_MOAB::init_set_info() {
  int maxnsets, result;
  char setname[256];
  MBTag tag;

  // Get element block, sideset and nodeset tags

  result = mbcore->tag_get_handle(MATERIAL_SET_TAG_NAME,mattag);
  if (result != MB_SUCCESS) {
    std::cerr << "Could not get tag for material sets" << std::endl;
    assert(result == MB_SUCCESS);
  }
  result = mbcore->tag_get_handle(NEUMANN_SET_TAG_NAME,sstag);
  if (result != MB_SUCCESS) {
    std::cerr << "Could not get tag for side sets" << std::endl;
    assert(result == MB_SUCCESS);
  }
  result = mbcore->tag_get_handle(DIRICHLET_SET_TAG_NAME,nstag);
  if (result != MB_SUCCESS) {
    std::cerr << "Could not get tag for node sets" << std::endl;
    assert(result == MB_SUCCESS);
  }


  AmanziGeometry::GeometricModelPtr gm = Mesh::geometric_model();

  if (gm == NULL) { 
    Errors::Message mesg("Need region definitions to initialize sets");
    amanzi_throw(mesg);
  }
    

  unsigned int ngr = gm->Num_Regions();

  for (int i = 0; i < ngr; i++) {
    AmanziGeometry::RegionPtr rgn = gm->Region_i(i);

    if (rgn->type() == AmanziGeometry::LABELEDSET) {

      AmanziGeometry::LabeledSetRegionPtr lsrgn =
        dynamic_cast<AmanziGeometry::LabeledSetRegionPtr> (rgn);

      std::string internal_name;
      std::string label = lsrgn->label();
      std::string entity_type_str = lsrgn->entity_str();

      if (entity_type_str == "CELL")
        internal_name = internal_name_of_set(rgn,CELL);
      else if (entity_type_str == "FACE")
        internal_name = internal_name_of_set(rgn,FACE);
      else if (entity_type_str == "NODE")
        internal_name = internal_name_of_set(rgn,NODE);

      result = mbcore->tag_get_handle(internal_name.c_str(),1,MB_TYPE_INTEGER,
                                      tag,MB_TAG_CREAT|MB_TAG_SPARSE);

      if (result != MB_SUCCESS) {
        std::cerr << "Problem getting labeled set " << std::endl;
        assert(result == MB_SUCCESS);
      }
    }
    //    else { /* General region - we have to account for all kinds of
    //              entities being queried in a set defined by this 
    //              region */
    //      Entity_kind int_to_kind[3] = {NODE,FACE,CELL};
    //
    //      for (int k = 0; k < 3; k++) {
    //        Entity_kind kind = int_to_kind[k];
    //
    //      std::string internal_name = internal_name_of_set(rgn,kind);
    //
    //  result = mbcore->tag_get_handle(internal_name.c_str(),1,MB_TYPE_INTEGER,
    //                                  tag,MB_TAG_CREAT|MB_TAG_SPARSE);
    //    if (result != MB_SUCCESS) {
    //      std::cerr << "Could not create tag with name " << rgn->name() << 
    //      std::endl;
    //      assert(result != MB_SUCCESS);
    //    }
    //  }
    //}
  }
}




// Number of OWNED, GHOST or USED entities of different types

    
unsigned int Mesh_MOAB::num_entities (Entity_kind kind, 
					     Parallel_type ptype) const
{
  const int rank = (int) kind;
  const int index = ((int) ptype) - 1;

  switch (kind) {
  case NODE:

    switch (ptype) {
    case OWNED:
      return !serial_run ? OwnedVerts.size() : AllVerts.size();
      break;
    case GHOST:
      return !serial_run ? NotOwnedVerts.size() : 0;
      break;
    case USED:
      return AllVerts.size();
      break;
    default:
      return 0;
    }
    break;


  case FACE:
    switch (ptype) {
    case OWNED:
      return !serial_run ? OwnedFaces.size() : AllFaces.size();
      break;
    case GHOST:
      return !serial_run ? NotOwnedFaces.size() : 0;
      break;
    case USED:
      return AllFaces.size();
      break;
    default:
      return 0;
    }
    break;


  case CELL:

    switch (ptype) {
    case OWNED:
      return !serial_run ? OwnedCells.size() : AllCells.size();
      break;
    case GHOST:
      return !serial_run ? GhostCells.size() : 0;
      break;
    case USED:
      return AllCells.size();
      break;
    default:
      return 0;
    }

    break;
  default:
    std::cerr << "Count requested for unknown entity type" << std::endl;
  }
}


  // Get faces of a cell and directions in which the cell uses the face 

  // On a distributed mesh, this will return all the faces of the
  // cell, OWNED or GHOST. If ordered = true, the faces will be
  // returned in a standard order according to Exodus II convention
  // for standard cells; in all other situations (ordered = false or
  // non-standard cells), the list of faces will be in arbitrary order

  // In 3D, direction is 1 if face normal points out of cell
  // and -1 if face normal points into cell
  // In 2D, direction is 1 if face/edge is defined in the same
  // direction as the cell polygon, and -1 otherwise

 
void Mesh_MOAB::cell_get_faces_and_dirs_internal (const Entity_ID cellid,
                                                  Entity_ID_List *faceids,
                                                  std::vector<int> *face_dirs,
					          const bool ordered) const
{
  
  MBEntityHandle cell;
  MBRange cell_faces;
  std::vector<MBEntityHandle> cell_nodes, face_nodes;
  int *cell_faceids, *cell_facedirs;
  int nf, result;
  int cfstd[6][4] = {{0,1,5,4},    // Expected cell-face-node pattern
		     {1,2,6,5},
		     {2,3,7,6},
		     {0,4,7,3},
		     {0,3,2,1},
		     {4,5,6,7}};

  cell = cell_id_to_handle[cellid];

  result = mbcore->get_adjacencies(&cell, 1, facedim, true, cell_faces, 
			  MBInterface::INTERSECT);
  if (result != MB_SUCCESS) {
    std::cerr << "Problem getting faces of cell" << std::endl;
    assert(result == MB_SUCCESS);
  }
  nf = cell_faces.size();

  faceids->resize(nf);
  if (face_dirs) face_dirs->resize(nf);

  cell_faceids = new int[nf];			
  if (face_dirs) cell_facedirs = new int[nf];


  // Have to re-sort the faces according a specific template for hexes


  if (ordered && nf == 6) { // Hex

    MBEntityHandle *ordfaces, face;

    ordfaces = new MBEntityHandle[6];

    result = mbcore->get_connectivity(&cell, 1, cell_nodes);
    if (result != MB_SUCCESS) {
      std::cerr << "Problem getting nodes of cell" << std::endl;
      assert(result == MB_SUCCESS);
    }

    for (int i = 0; i < nf; i++) {
  
      // Search for a face that has all the expected nodes

      bool found = false;
      int j;
      for (j = 0; j < nf; j++) {

	face = cell_faces[j];
	result = mbcore->get_connectivity(&face, 1, face_nodes);
	if (result != MB_SUCCESS) {
	  std::cerr << "Problem getting nodes of face" << std::endl;
	  assert(result == MB_SUCCESS);
	}


	// Check if this face has all the expected nodes

	bool all_present = true;

	for (int k = 0; k < 4; k++) {
	  Entity_ID node = cell_nodes[cfstd[i][k]];

	  if (face_nodes[0] != node && face_nodes[1] != node &&
	      face_nodes[2] != node && face_nodes[3] != node) {
	    all_present = false;
	    break;
	  }
	}

	if (all_present) {
	  found = true;
	  break;
	}
      }

      assert(found);

      if (found)
	ordfaces[i] = face;
    }


    result = mbcore->tag_get_data(lid_tag,ordfaces,6,cell_faceids);
    if (result != MB_SUCCESS) {
      std::cerr << "Problem getting tag data" << std::endl;
      assert(result == MB_SUCCESS);
    }

    if (face_dirs) {
      for (int i = 0; i < nf; i++) {
        MBEntityHandle face = ordfaces[i];
        int sidenum, offset;
        
        result = mbcore->side_number(cell,face,sidenum,cell_facedirs[i],offset);
        if (result != MB_SUCCESS) {
          cerr << "Could not find face dir in cell" << std::endl;
          assert(result == MB_SUCCESS);
        }
        
        // If this is a ghost face and the master has the opposite direction
        // we are supposed to flip it
        
        if (faceflip[cell_faceids[i]]) cell_facedirs[i] *= -1;
      }
    }

    delete [] ordfaces;

  }
  else {
    result = mbcore->tag_get_data(lid_tag,cell_faces,cell_faceids);
    if (result != MB_SUCCESS) {
      std::cerr << "Problem getting tag data" << std::endl;
      assert(result == MB_SUCCESS);
    }

    if (face_dirs) {
      for (int i = 0; i < nf; i++) {
        MBEntityHandle face = cell_faces[i];
        int sidenum, offset;
        
        result = mbcore->side_number(cell,face,sidenum,cell_facedirs[i],offset);
        if (result != MB_SUCCESS) {
          cerr << "Could not find face dir in cell" << std::endl;
          assert(result == MB_SUCCESS);
        }
        
        // If this is a ghost face and the master has the opposite direction
        // we are supposed to flip it
        
        if (faceflip[cell_faceids[i]]) cell_facedirs[i] *= -1;
      }
    }
  }

  Entity_ID_List::iterator itf = faceids->begin();
  for (int i = 0; i < nf; i++) {
    *itf = cell_faceids[i];
    ++itf;
  }
  if (face_dirs) {
    std::vector<int>::iterator itd = face_dirs->begin();
    for (int i = 0; i < nf; i++) {
      *itd = cell_facedirs[i];
      ++itd;
    }
  }

  delete [] cell_faceids;
  if (face_dirs) delete [] cell_facedirs;
}


void Mesh_MOAB::cell_get_nodes (Entity_ID cellid, Entity_ID_List *cnodes) const
{
  MBEntityHandle cell;
  std::vector<MBEntityHandle> cell_nodes;
  int *cell_nodeids;
  int nn, result;


  cell = cell_id_to_handle[cellid];
      
  result = mbcore->get_connectivity(&cell, 1, cell_nodes);
  if (result != MB_SUCCESS) {
    std::cerr << "Problem getting nodes of cell" << std::endl;
    assert(result == MB_SUCCESS);
  }

  nn = cell_nodes.size();
  cell_nodeids = new int[nn];

  cnodes->resize(nn);
    
  for (int i = 0; i < nn; i++) {
    result = mbcore->tag_get_data(lid_tag,&(cell_nodes[i]),1,&(cell_nodeids[i]));
    if (result != MB_SUCCESS) {
      std::cerr << "Problem getting tag data" << std::endl;
      assert(result == MB_SUCCESS);
    }
  }

  Entity_ID_List::iterator itn = cnodes->begin();
  for (int i = 0; i < nn; i++) {
    *itn = cell_nodeids[i];
    ++itn;
  }

  delete [] cell_nodeids;
}




void Mesh_MOAB::face_get_nodes (Entity_ID faceid, Entity_ID_List *fnodes) const 
{
  MBEntityHandle face;
  std::vector<MBEntityHandle> face_nodes;
  int *face_nodeids;
  int nn, result;

  face = face_id_to_handle[faceid];


  result = mbcore->get_connectivity(&face, 1, face_nodes, true);
  if (result != MB_SUCCESS) {
    std::cerr << "Problem getting nodes of face" << std::endl;
    assert(result == MB_SUCCESS);
  }

  nn = face_nodes.size();

  face_nodeids = new int[nn];
  if (faceflip[faceid]) {
    for (int i = nn-1; i >= 0; i--) {
      result = mbcore->tag_get_data(lid_tag,&(face_nodes[i]),1,&(face_nodeids[nn-i-1]));
      if (result != MB_SUCCESS) {
	std::cerr << "Problem getting tag data" << std::endl;
	assert(result == MB_SUCCESS);
      }
    }
  }
  else {
    for (int i = 0; i < nn; i++) {
      result = mbcore->tag_get_data(lid_tag,&(face_nodes[i]),1,&(face_nodeids[i]));
      if (result != MB_SUCCESS) {
	std::cerr << "Problem getting tag data" << std::endl;
	assert(result == MB_SUCCESS);
      }
    }
  }

  fnodes->resize(nn);
  Entity_ID_List::iterator itn = fnodes->begin();
  for (int i = 0; i < nn; i++) {
    *itn = face_nodeids[i];
    ++itn;
  }

  delete [] face_nodeids;
}
  


void Mesh_MOAB::node_get_coordinates (Entity_ID node_id, AmanziGeometry::Point *ncoord) const 
{
  MBEntityHandle node;
  double coords[3];

  node = vtx_id_to_handle[node_id];

  int result = mbcore->get_coords(&node, 1, coords);
  if (result != MB_SUCCESS) {
    std::cerr << "Problem getting node coordinates" << std::endl;
    assert(result == MB_SUCCESS);
  }

  ncoord->init(spacedim);
  ncoord->set(coords);

}

// Modify a node's coordinates

void Mesh_MOAB::node_set_coordinates(const AmanziMesh::Entity_ID nodeid, 
                                    const double *coords) {
  MBEntityHandle v = vtx_id_to_handle[nodeid];

  int result = mbcore->set_coords(&v, 1, coords);
  if (result != MB_SUCCESS) {
    std::cerr << "Problem setting node coordinates" << std::endl;
    assert(result == MB_SUCCESS);
  }

}

void Mesh_MOAB::node_set_coordinates(const AmanziMesh::Entity_ID nodeid, 
                                     const AmanziGeometry::Point coords) {
  MBEntityHandle v = vtx_id_to_handle[nodeid];

  double coordarray[3] = {0.0,0.0,0.0};

  for (int i = 0; i < spacedim; i++)
    coordarray[i] = coords[i];

  int result = mbcore->set_coords(&v, 1, coordarray);
  if (result != MB_SUCCESS) {
    std::cerr << "Problem setting node coordinates" << std::endl;
    assert(result == MB_SUCCESS);
  }

}





void Mesh_MOAB::cell_get_coordinates (Entity_ID cellid, std::vector<AmanziGeometry::Point> *ccoords) const
{
  MBEntityHandle cell;
  std::vector<MBEntityHandle> cell_nodes;
  double *coords;
  int nn, result;


  cell = cell_id_to_handle[cellid];

  ccoords->clear();
      
  result = mbcore->get_connectivity(&cell, 1, cell_nodes);
  if (result != MB_SUCCESS) {
    std::cerr << "Problem getting nodes of a cell" << std::endl;
    assert(result == MB_SUCCESS);
  }

  nn = cell_nodes.size();

  coords = new double[spacedim];
  
  ccoords->resize(nn);
  std::vector<AmanziGeometry::Point>::iterator it = ccoords->begin();

  for (int i = 0; i < nn; i++) {
    result = mbcore->get_coords(&(cell_nodes[i]),1,coords);
    if (result != MB_SUCCESS) {
      std::cerr << "Problem getting coordinates of a node" << std::endl;
      assert(result == MB_SUCCESS);
    }

    it->set(spacedim,coords);
    ++it;
  }

  delete [] coords;
}




void Mesh_MOAB::face_get_coordinates (Entity_ID faceid, std::vector<AmanziGeometry::Point> *fcoords) const
{
    MBEntityHandle face;
    std::vector<MBEntityHandle> face_nodes;
    double *coords;
    int nn, result;

    face = face_id_to_handle[faceid];

    fcoords->clear();

    result = mbcore->get_connectivity(&face, 1, face_nodes, true);
    if (result != MB_SUCCESS) {
      std::cerr << "Problem getting nodes of face" << std::endl;
      assert(result == MB_SUCCESS);
    }

    nn = face_nodes.size();

    coords = new double[spacedim];
    
    fcoords->resize(nn);
    std::vector<AmanziGeometry::Point>::iterator it = fcoords->begin();

    if (faceflip[faceid]) {
      for (int i = nn-1; i >=0; i--) {
	result = mbcore->get_coords(&(face_nodes[i]),1,coords);
	if (result != MB_SUCCESS) {
	  std::cerr << "Problem getting coordinates of node" << std::endl;
	  assert(result == MB_SUCCESS);
	}

        it->set(spacedim,coords);
        ++it;
      }
    }
    else {
      for (int i = 0; i < nn; i++) {
	result = mbcore->get_coords(&(face_nodes[i]),1,coords);
	if (result != MB_SUCCESS) {
	  std::cerr << "Problem getting tag data" << std::endl;
	  assert(result == MB_SUCCESS);
	}

        it->set(spacedim,coords);
        ++it;
      }
    }

    delete [] coords;
}
  

MBTag Mesh_MOAB::build_set(const AmanziGeometry::RegionPtr region,
                           const Entity_kind kind) const {

  int celldim = Mesh::cell_dimension();
  int spacedim = Mesh::space_dimension();
  AmanziGeometry::GeometricModelPtr gm = Mesh::geometric_model();
  int one = 1;
  MBTag tag;

  // Modify region/set name by prefixing it with the type of entity requested

  std::string internal_name = internal_name_of_set(region,kind);

  // Create entity set based on the region defintion  

  switch (kind) {      
  case CELL:    // cellsets      

    if (region->type() == AmanziGeometry::BOX ||
        region->type() == AmanziGeometry::COLORFUNCTION) {

      mbcore->tag_get_handle(internal_name.c_str(),1,MB_TYPE_INTEGER,tag,
                             MB_TAG_CREAT|MB_TAG_SPARSE);
      
      int ncell = num_entities(CELL, USED);              

      for (int icell = 0; icell < ncell; icell++)
        if (region->inside(cell_centroid(icell)))
          mbcore->tag_set_data(tag,&(cell_id_to_handle[icell]),1,&one);

    }
    else if (region->type() == AmanziGeometry::POINT) {
      AmanziGeometry::Point vpnt(spacedim);
      AmanziGeometry::Point rgnpnt(spacedim);

      mbcore->tag_get_handle(internal_name.c_str(),1,MB_TYPE_INTEGER,tag,
                           MB_TAG_CREAT|MB_TAG_SPARSE);
      
      rgnpnt = ((AmanziGeometry::PointRegionPtr)region)->point();
        
      int nnode = num_entities(NODE, USED);
      double mindist2 = 1.e+16;
      int minnode = -1;
        
      int inode;
      for (inode = 0; inode < nnode; inode++) {
                  
        node_get_coordinates(inode, &vpnt);                  
        double dist2 = (vpnt-rgnpnt)*(vpnt-rgnpnt);
 
        if (dist2 < mindist2) {
          mindist2 = dist2;
          minnode = inode;
          if (mindist2 <= 1.0e-32)
            break;
        }
      }

      Entity_ID_List cells, cells1;

      node_get_cells(minnode,USED,&cells);
      
      int ncells = cells.size();
      for (int ic = 0; ic < ncells; ic++) {
        Entity_ID icell = cells[ic];
        
        // Check if point is contained in cell            
        if (point_in_cell(rgnpnt,icell))
          mbcore->tag_set_data(tag,&(cell_id_to_handle[icell]),1,&one);
      }

    }
    else if (region->type() == AmanziGeometry::PLANE) {

      mbcore->tag_get_handle(internal_name.c_str(),1,MB_TYPE_INTEGER,tag,
                             MB_TAG_CREAT|MB_TAG_SPARSE);
      
      if (celldim == 2) {

        int ncells = num_entities(CELL, USED);              
        for (int ic = 0; ic < ncells; ic++) {

          std::vector<AmanziGeometry::Point> ccoords(spacedim);

          cell_get_coordinates(ic, &ccoords);

          bool on_plane = true;
          for (int j = 0; j < ccoords.size(); j++) {
            if (!region->inside(ccoords[j])) {
              on_plane = false;
              break;
            }
          }
                  
          if (on_plane)
            mbcore->tag_set_data(tag,&(cell_id_to_handle[ic]),1,&one);
        }
      }

    }
    else if (region->type() == AmanziGeometry::LOGICAL) {
      // will process later in this subroutine
    }
    else if (region->type() == AmanziGeometry::LABELEDSET) {
      // Nothing to do
      tag = mattag;
    }
    else {
      Errors::Message mesg("Region type not applicable/supported for cell sets");
      amanzi_throw(mesg);
    }
      
    break;
      
  case FACE:  // sidesets

    if (region->type() == AmanziGeometry::BOX)  {

      mbcore->tag_get_handle(internal_name.c_str(),1,MB_TYPE_INTEGER,tag,
                             MB_TAG_CREAT|MB_TAG_SPARSE);

      int nface = num_entities(FACE, USED);
        
      for (int iface = 0; iface < nface; iface++) {
        if (region->inside(face_centroid(iface)))
          mbcore->tag_set_data(tag,&(face_id_to_handle[iface]),1,&one);

      }
    }
    else if (region->type() == AmanziGeometry::PLANE ||
             region->type() == AmanziGeometry::POLYGON) {

      mbcore->tag_get_handle(internal_name.c_str(),1,MB_TYPE_INTEGER,tag,
                             MB_TAG_CREAT|MB_TAG_SPARSE);

      int nface = num_entities(FACE, USED);
              
      for (int iface = 0; iface < nface; iface++) {
        std::vector<AmanziGeometry::Point> fcoords(spacedim);
            
        face_get_coordinates(iface, &fcoords);
            
        bool on_plane = true;
        for (int j = 0; j < fcoords.size(); j++) {
          if (!region->inside(fcoords[j])) {
            on_plane = false;
            break;
          }
        }
                  
        if (on_plane)
          mbcore->tag_set_data(tag,&(face_id_to_handle[iface]),1,&one);
      }

    }
    else if (region->type() == AmanziGeometry::LABELEDSET) {
      // Nothing to do

      tag = sstag;
    }
    else if (region->type() == AmanziGeometry::LOGICAL) {
      // Will handle it later in the routine
    }
    else {
      Errors::Message mesg("Region type not applicable/supported for face sets");
      amanzi_throw(mesg);
    }
    break;

  case NODE: // Nodesets

    if (region->type() == AmanziGeometry::BOX ||
        region->type() == AmanziGeometry::PLANE ||
        region->type() == AmanziGeometry::POLYGON ||
        region->type() == AmanziGeometry::POINT) {

      mbcore->tag_get_handle(internal_name.c_str(),1,MB_TYPE_INTEGER,tag,
                             MB_TAG_CREAT|MB_TAG_SPARSE);

      int nnode = num_entities(NODE, USED);

      for (int inode = 0; inode < nnode; inode++) {

        AmanziGeometry::Point vpnt(spacedim);
        node_get_coordinates(inode, &vpnt);
                  
        if (region->inside(vpnt)) {
          mbcore->tag_set_data(tag,&(vtx_id_to_handle[inode]),1,&one);

          // Only one node per point region
          if (region->type() == AmanziGeometry::POINT)
            break;      
        }
      }
    }
    else if (region->type() == AmanziGeometry::LABELEDSET) {
      // Just retrieve and return the set

      tag = nstag;
    }
    else if (region->type() == AmanziGeometry::LOGICAL) {
      // We will handle it later in the routine
    }
    else {
      Errors::Message mesg("Region type not applicable/supported for node sets");
      amanzi_throw(mesg);
    }
      
    break;
  }


  if (region->type() == AmanziGeometry::LOGICAL) {
    std::string new_internal_name;

    AmanziGeometry::LogicalRegionPtr boolregion = (AmanziGeometry::LogicalRegionPtr) region;
    std::vector<std::string> region_names = boolregion->component_regions();
    int nreg = region_names.size();
    
    std::vector<MBTag> tags;
    std::vector<AmanziGeometry::RegionPtr> regions;
    MBTag tag1;
    MBRange entset;
    
    for (int r = 0; r < nreg; r++) {
      AmanziGeometry::RegionPtr rgn1 = gm->FindRegion(region_names[r]);
      regions.push_back(rgn1);

      // Did not find the region
      if (rgn1 == NULL) {
        std::stringstream mesg_stream;
        mesg_stream << "Geometric model has no region named " << 
          region_names[r];
        Errors::Message mesg(mesg_stream.str());
        amanzi_throw(mesg);
      }
        
      internal_name = internal_name_of_set(rgn1,kind);
      mbcore->tag_get_handle(internal_name.c_str(),1,MB_TYPE_INTEGER,tag1,
                             MB_TAG_SPARSE);
      if (!tag1)        
        tag1 = build_set(rgn1,kind);  // Recursive call

      tags.push_back(tag1);
    }

    // Check the entity types of the sets are consistent with the
    // entity type of the requested set

    if (boolregion->operation() == AmanziGeometry::COMPLEMENT) {
      int *values[1] = {&one};
      MBRange entset1, entset2;

      switch (kind) {
      case CELL:
        for (int i = 0; i < tags.size(); i++) {
          mbcore->get_entities_by_type_and_tag(0,MBHEX,&(tags[i]),
                                               (void **)values,1,entset2);
          
          entset1.merge(entset2);
        }
        entset = AllCells;
        entset -= entset1;
        break;
      case FACE:
        for (int i = 0; i < tags.size(); i++) {
          mbcore->get_entities_by_type_and_tag(0,MBQUAD,&(tags[i]),
                                               (void **)values,1,entset2);
          
          entset1.merge(entset2);
        }
        entset = AllFaces;
        entset -= entset1;
        break;
      case NODE:
        for (int i = 0; i < tags.size(); i++) {
          mbcore->get_entities_by_type_and_tag(0,MBVERTEX,&(tags[i]),
                                               (void **)values,1,entset2);
          
          entset1.merge(entset2);
        }
        entset = AllVerts;
        entset -= entset1;
        break;
      }

      for (int r = 0; r < nreg; r++)
        new_internal_name = new_internal_name + "+" + region_names[r];
      new_internal_name = "NOT_" + new_internal_name;

    }
    else if (boolregion->operation() == AmanziGeometry::UNION) {

      MBRange entset, entset1;
      switch (kind) {
      case CELL:
        for (int i = 0; i < tags.size(); i++) {
          int *values[1] = {&one};
          mbcore->get_entities_by_type_and_tag(0,MBHEX,&(tags[i]),
                                               (void **)values,1,entset1);

          entset.merge(entset1);
        }
        break;
      case FACE:
        for (int i = 0; i < tags.size(); i++) {
          int *values[1] = {&one};
          mbcore->get_entities_by_type_and_tag(0,MBQUAD,&(tags[i]),
                                               (void **)values,1,entset1);

          entset.merge(entset1);
        }
        break;
      case NODE:
        for (int i = 0; i < tags.size(); i++) {
          int *values[1] = {&one};
          mbcore->get_entities_by_type_and_tag(0,MBVERTEX,&(tags[i]),
                                               (void **)values,1,entset1);

          entset.merge(entset1);
        }
        break;
      }
      
      std::string new_internal_name;
      for (int r = 0; r < nreg; r++)
        new_internal_name = new_internal_name + "+" + region_names[r];
    }
    else if (boolregion->operation() == AmanziGeometry::SUBTRACT) {
      int *values[1] = {&one};
      MBRange entset1, entset2;

      switch (kind) {
      case CELL:
        mbcore->get_entities_by_type_and_tag(0,MBHEX,&(tags[0]),
                                             (void **)values,1,entset);
        for (int i = 1; i < tags.size(); i++) {
          mbcore->get_entities_by_type_and_tag(0,MBHEX,&(tags[i]),
                                               (void **)values,1,entset1);
          
          entset.merge(entset1);
        }
        break;
      case FACE:
        mbcore->get_entities_by_type_and_tag(0,MBQUAD,&(tags[0]),
                                             (void **)values,1,entset);
        for (int i = 1; i < tags.size(); i++) {
          mbcore->get_entities_by_type_and_tag(0,MBQUAD,&(tags[i]),
                                               (void **)values,1,entset1);
          
          entset.merge(entset1);
        }
        break;
      case NODE:
        mbcore->get_entities_by_type_and_tag(0,MBVERTEX,&(tags[0]),
                                             (void **)values,1,entset);
        for (int i = 1; i < tags.size(); i++) {
          mbcore->get_entities_by_type_and_tag(0,MBVERTEX,&(tags[i]),
                                               (void **)values,1,entset1);
          
          entset.merge(entset1);
        }
        break;
      }
      
      std::string new_internal_name = region_names[0];
      for (int r = 0; r < nreg; r++)
        new_internal_name = new_internal_name + "-" + region_names[r];
    }
    else if (boolregion->operation() == AmanziGeometry::INTERSECT) {
      Errors::Message mesg("INTERSECT region not implemented in MOAB");
      amanzi_throw(mesg);
    }

    mbcore->tag_get_handle(new_internal_name.c_str(),1,MB_TYPE_INTEGER,tag,
                           MB_TAG_CREAT | MB_TAG_SPARSE);

    for (MBRange::iterator it = entset.begin(); it != entset.end(); ++it) {
      MBEntityHandle ent = *it;
      mbcore->tag_set_data(tag,&ent,1,&one);
    }
  }

  return tag;
}


void Mesh_MOAB::get_set_entities (const Set_Name setname, 
                                  const Entity_kind kind, 
                                  const Parallel_type ptype,
                                  Entity_ID_List *setents) const {

  int idx, i, lid, one=1;
  bool found(false);
  int celldim = Mesh::cell_dimension();
  int spacedim = Mesh::space_dimension();
  const Epetra_Comm *epcomm = get_comm();

  assert(setents != NULL);
  
  setents->clear();

  AmanziGeometry::GeometricModelPtr gm = Mesh::geometric_model();

  // Is there an appropriate region by this name?

  AmanziGeometry::RegionPtr rgn = gm->FindRegion(setname);

  // Did not find the region
  
  if (rgn == NULL) {
    std::stringstream mesg_stream;
    mesg_stream << "Geometric model has no region named " << setname;
    Errors::Message mesg(mesg_stream.str());
    amanzi_throw(mesg);
  }


  std::string internal_name = internal_name_of_set(rgn,kind);

  MBRange mset1;

  // If region is of type labeled set and a mesh set should have been
  // initialized from the input file
  
  if (rgn->type() == AmanziGeometry::LABELEDSET)
    {
      AmanziGeometry::LabeledSetRegionPtr lsrgn = dynamic_cast<AmanziGeometry::LabeledSetRegionPtr> (rgn);
      std::string label = lsrgn->label();
      std::stringstream labelstream(label);
      int labelint;
      labelstream >> labelint;
      std::string entity_type = lsrgn->entity_str();

      if ((kind == CELL && entity_type != "CELL") ||
          (kind == FACE && entity_type != "FACE") ||
          (kind == NODE && entity_type != "NODE"))
        {
          std::stringstream mesg_stream;
          mesg_stream << "Found labeled set region named " << setname << " but it contains entities of type " << entity_type << ", not the requested type";
          Errors::Message mesg(mesg_stream.str());
          amanzi_throw(mesg);
        } 

      int *values[1] = {&labelint};
      if (kind == CELL)
        mbcore->get_entities_by_type_and_tag(0,MBENTITYSET,&mattag,
                                             (void **)values,1,mset1);
      else if (kind == FACE)
        mbcore->get_entities_by_type_and_tag(0,MBENTITYSET,&sstag,
                                             (void **)values,1,mset1);
      else if (kind == NODE)
        mbcore->get_entities_by_type_and_tag(0,MBENTITYSET,&nstag,
                                             (void **)values,1,mset1);

    }
  else
    {
      // Modify region/set name by prefixing it with the type of
      // entity requested

      MBTag tag = 0;
      bool created;
      int *values[1] = {&one};

      mbcore->tag_get_handle(internal_name.c_str(),1,MB_TYPE_INTEGER,tag,
                             MB_TAG_SPARSE);

      if (!tag)
        tag = build_set(rgn,kind);

      switch(kind) {
      case CELL:
        mbcore->get_entities_by_type_and_tag(0,MBHEX,&tag,(void **)values,1,mset1);
      break;
      case FACE:
        mbcore->get_entities_by_type_and_tag(0,MBQUAD,&tag,(void **)values,1,mset1);
        break;
      case NODE:
        mbcore->get_entities_by_type_and_tag(0,MBVERTEX,&tag,(void **)values,1,mset1);
        break;
      }
    }

  

  /* Check if no processor got any mesh entities */

  int nent_loc = mset1.size();

#ifdef DEBUG
  int nent_glob;

  epcomm->SumAll(&nent_loc,&nent_glob,1);
  if (nent_glob == 0) {
    std::stringstream mesg_stream;
    mesg_stream << "Could not retrieve any mesh entities for set " << setname << std::endl;
    Errors::Message mesg(mesg_stream.str());
    Exceptions::amanzi_throw(mesg);
  }
#endif
  
  setents->resize(nent_loc);
  if (nent_loc) {
    unsigned char pstatus;
    nent_loc = 0; // reset and count to get the real number

    switch (ptype) {
    case OWNED:
      for (MBRange::iterator it = mset1.begin(); it != mset1.end(); ++it) {
        MBEntityHandle ent = *it;

        mbcomm->get_pstatus(ent,pstatus);
        if ((pstatus & PSTATUS_NOT_OWNED) == 0) {
          mbcore->tag_get_data(lid_tag,&ent,1,&lid);
          (*setents)[nent_loc++] = lid;
        }
      }
      break;
    case GHOST:
      for (MBRange::iterator it = mset1.begin(); it != mset1.end(); ++it) {
        MBEntityHandle ent = *it;

        mbcomm->get_pstatus(ent,pstatus);
        if ((pstatus & PSTATUS_NOT_OWNED) == 1) {
          mbcore->tag_get_data(lid_tag,&ent,1,&lid);
          (*setents)[nent_loc++] = lid;
        }
      }
      break;
    case USED:
      for (MBRange::iterator it = mset1.begin(); it != mset1.end(); ++it) {
        MBEntityHandle ent = *it;

        mbcore->tag_get_data(lid_tag,&ent,1,&lid);
        (*setents)[nent_loc++] = lid;
      }
      break;
    }
    
    setents->resize(nent_loc);
  }
      
    /* Check if there were no entities left on any processor after
       extracting the appropriate category of entities */
    
#ifdef DEBUG
  epcomm->SumAll(&nent_loc,&nent_glob,1);
  
  if (nent_glob == 0) {
    std::stringstream mesg_stream;
    mesg_stream << "Could not retrieve any mesh entities of type " << setkind << " for set " << setname << std::endl;
    Errors::Message mesg(mesg_stream.str());
    Exceptions::amanzi_throw(mesg);
  }
#endif


}

void Mesh_MOAB::get_set_entities (const char *setname, 
                                  const Entity_kind kind, 
                                  const Parallel_type ptype,
                                  Entity_ID_List *setents) const {

  std::string setname_str(setname);
  get_set_entities(setname_str,kind,ptype,setents);

}


void Mesh_MOAB::get_set_entities (const Set_ID set_id, 
                                  const Entity_kind kind, 
                                  const Parallel_type ptype,
                                  Entity_ID_List *setents) const {


  Errors::Message mesg("get_set_entities by ID is deprecated");
  amanzi_throw(mesg);    
}

unsigned int Mesh_MOAB::get_set_size(const Set_Name setname, 
                                     const Entity_kind kind, 
                                     const Parallel_type ptype) const {   
  Entity_ID_List setents;
  get_set_entities(setname,kind,ptype,&setents);
  return setents.size();
}
  
unsigned int Mesh_MOAB::get_set_size(const char *setname, 
                                     const Entity_kind kind, 
                                     const Parallel_type ptype) const {   
  std::string setname_str(setname);
  return get_set_size(setname_str,kind,ptype);
}  

unsigned int Mesh_MOAB::get_set_size(const Set_ID set_id, 
                                     const Entity_kind kind, 
                                     const Parallel_type ptype) const {   
  
  Errors::Message mesg("Get set size by ID is deprecated");
  amanzi_throw(mesg);
}


// Upward adjacencies
//-------------------

// Cells of type 'ptype' connected to a node

void Mesh_MOAB::node_get_cells (const Entity_ID nodeid, 
				const Parallel_type ptype,
				Entity_ID_List *cellids) const 
{
  throw std::exception();
}
    
// Faces of type 'ptype' connected to a node

void Mesh_MOAB::node_get_faces (const Entity_ID nodeid, 
				const Parallel_type ptype,
				Entity_ID_List *faceids) const
{
  throw std::exception();
}
    
// Get faces of ptype of a particular cell that are connected to the
// given node

void Mesh_MOAB::node_get_cell_faces (const Entity_ID nodeid, 
				     const Entity_ID cellid,
				     const Parallel_type ptype,
				     Entity_ID_List *faceids) const
{
  throw std::exception();
}
    
// Cells connected to a face

void Mesh_MOAB::face_get_cells_internal (const Entity_ID faceid, 
                                         const Parallel_type ptype,
                                         Entity_ID_List *cellids) const
{
  int result;
  MBEntityHandle face = face_id_to_handle[faceid];
  MBRange fcells;
  
  result = mbcore->get_adjacencies(&face,1,celldim,true,fcells,MBCore::UNION);
  if (result != MB_SUCCESS) {
    std::cerr << "Could not get cells of face" << faceid << std::endl;
    assert(result == MB_SUCCESS);
  }
  
  int nc = fcells.size();
  int fcellids[2];

  result = mbcore->tag_get_data(lid_tag,fcells,(void *)fcellids);
  if (result != MB_SUCCESS) {
    std::cerr << "Problem getting id tag data" << std::endl;
    assert(result == MB_SUCCESS);
  }

  cellids->resize(2);
  Entity_ID_List::iterator it = cellids->begin();

  unsigned char pstatus;
  int n = 0;
  switch (ptype) {
  case USED:
    for (int i = 0; i < nc; i++) {
      *it = fcellids[i];
      ++it;
      ++n;
    }
    break;
  case OWNED:
    for (int i = 0; i < nc; i++) {
      result = mbcomm->get_pstatus(fcells[i],pstatus);
      if ((pstatus & PSTATUS_NOT_OWNED) == 0) {
        *it = fcellids[i];
        ++it;
        ++n;
      }
    }
    break;
  case GHOST:
    for (int i = 0; i < nc; i++) {
      result = mbcomm->get_pstatus(fcells[i],pstatus);
      if ((pstatus & PSTATUS_NOT_OWNED) == 1) {
        *it = fcellids[i];
        ++it;
        ++n;
      }
    }
    break;
  }

  cellids->resize(n);

}
    


// Same level adjacencies
//-----------------------

// Face connected neighboring cells of given cell of a particular ptype
// (e.g. a hex has 6 face neighbors)

// The order in which the cellids are returned cannot be
// guaranteed in general except when ptype = USED, in which case
// the cellids will correcpond to cells across the respective
// faces given by cell_get_faces

void Mesh_MOAB::cell_get_face_adj_cells(const Entity_ID cellid,
					const Parallel_type ptype,
					Entity_ID_List *fadj_cellids) const
{
  throw std::exception();
}

// Node connected neighboring cells of given cell
// (a hex in a structured mesh has 26 node connected neighbors)
// The cells are returned in no particular order

void Mesh_MOAB::cell_get_node_adj_cells(const Entity_ID cellid,
					const Parallel_type ptype,
					Entity_ID_List *nadj_cellids) const
{
  throw std::exception();
}


// Epetra map for cells - basically a structure specifying the
// global IDs of cells owned or used by this processor

void Mesh_MOAB::init_cell_map ()
{
  int *cell_gids;
  int ncell, result;
  const Epetra_Comm *epcomm = Mesh::get_comm();

  if (!serial_run) {

    // For parallel runs create map without and with ghost cells included
    // Also, put in owned cells before the ghost cells

    
    cell_gids = new int[OwnedCells.size()+GhostCells.size()];
    
    result = mbcore->tag_get_data(gid_tag,OwnedCells,cell_gids);
    if (result != MB_SUCCESS) {
      std::cerr << "Problem getting tag data" << std::endl;
      assert(result == MB_SUCCESS);
    }
    ncell = OwnedCells.size();
    

    cell_map_wo_ghosts_ = new Epetra_Map(-1,ncell,cell_gids,0,*epcomm);
    



    result = mbcore->tag_get_data(gid_tag,GhostCells,&(cell_gids[ncell]));
    if (result != MB_SUCCESS) {
      std::cerr << "Problem getting tag data" << std::endl;
      assert(result == MB_SUCCESS);
    }
    
    ncell += GhostCells.size();

    cell_map_w_ghosts_ = new Epetra_Map(-1,ncell,cell_gids,0,*epcomm);

  }
  else {
    cell_gids = new int[AllCells.size()];

    result = mbcore->tag_get_data(gid_tag,AllCells,cell_gids);
    if (result != MB_SUCCESS) {
      std::cerr << "Problem getting tag data" << std::endl;
      assert(result == MB_SUCCESS);
    }

    ncell = AllCells.size();

    cell_map_wo_ghosts_ = new Epetra_Map(-1,ncell,cell_gids,0,*epcomm);
  }

  delete [] cell_gids;

}




// Epetra map for faces - basically a structure specifying the
// global IDs of cells owned or used by this processor

void Mesh_MOAB::init_face_map ()
{
  int *face_gids;
  int nface, result;
  const Epetra_Comm *epcomm = Mesh::get_comm();

  if (!serial_run) {

    // For parallel runs create map without and with ghost cells included
    // Also, put in owned cells before the ghost cells

    
    face_gids = new int[OwnedFaces.size()+NotOwnedFaces.size()];
    
    result = mbcore->tag_get_data(gid_tag,OwnedFaces,face_gids);
    if (result != MB_SUCCESS) {
      std::cerr << "Problem getting tag data" << std::endl;
      assert(result == MB_SUCCESS);
    }

    nface = OwnedFaces.size();
    
    face_map_wo_ghosts_ = new Epetra_Map(-1,nface,face_gids,0,*epcomm);


    result = mbcore->tag_get_data(gid_tag,NotOwnedFaces,&(face_gids[nface]));
    if (result != MB_SUCCESS) {
      std::cerr << "Problem getting tag data" << std::endl;
      assert(result == MB_SUCCESS);
    }
    
    nface += NotOwnedFaces.size();

    face_map_w_ghosts_ = new Epetra_Map(-1,nface,face_gids,0,*epcomm);

  }
  else {
    face_gids = new int[AllFaces.size()];

    result = mbcore->tag_get_data(gid_tag,AllFaces,face_gids);
    if (result != MB_SUCCESS) {
      std::cerr << "Problem getting tag data" << std::endl;
      assert(result == MB_SUCCESS);
    }

    nface = AllFaces.size();

    face_map_wo_ghosts_ = new Epetra_Map(-1,nface,face_gids,0,*epcomm);
  }

  delete [] face_gids;

}




// Epetra map for nodes - basically a structure specifying the
// global IDs of cells owned or used by this processor

void Mesh_MOAB::init_node_map ()
{
  int *vert_gids;
  int nvert, result;
  const Epetra_Comm *epcomm = Mesh::get_comm();

  if (!serial_run) {

    // For parallel runs create map without and with ghost verts included
    // Also, put in owned cells before the ghost verts

    
    vert_gids = new int[OwnedVerts.size()+NotOwnedVerts.size()];
    
    result = mbcore->tag_get_data(gid_tag,OwnedVerts,vert_gids);
    if (result != MB_SUCCESS) {
      std::cerr << "Problem getting tag data" << std::endl;
      assert(result == MB_SUCCESS);
    }

    nvert = OwnedVerts.size();
    
    node_map_wo_ghosts_ = new Epetra_Map(-1,nvert,vert_gids,0,*epcomm);
    



    result = mbcore->tag_get_data(gid_tag,NotOwnedVerts,&(vert_gids[nvert]));
    if (result != MB_SUCCESS) {
      std::cerr << "Problem getting tag data" << std::endl;
      assert(result == MB_SUCCESS);
    }
    
    nvert += NotOwnedVerts.size();

    node_map_w_ghosts_ = new Epetra_Map(-1,nvert,vert_gids,0,*epcomm);

  }
  else {
    vert_gids = new int[AllVerts.size()];

    result = mbcore->tag_get_data(gid_tag,AllVerts,vert_gids);
    if (result != MB_SUCCESS) {
      std::cerr << "Problem getting tag data" << std::endl;
      assert(result == MB_SUCCESS);
    }

    nvert = AllVerts.size();

    node_map_wo_ghosts_ = new Epetra_Map(-1,nvert,vert_gids,0,*epcomm);
  }

  delete [] vert_gids;

}


Entity_ID Mesh_MOAB::GID(Entity_ID lid, Entity_kind kind) const {
  MBEntityHandle ent;
  Entity_ID gid;

  switch (kind) {
  case NODE:
    ent = vtx_id_to_handle[lid];
    break;

  case FACE:
    ent = face_id_to_handle[lid];
    break;

  case CELL:
    ent = cell_id_to_handle[lid];
    break;
  default:
    std::cerr << "Global ID requested for unknown entity type" << std::endl;
  }

  int result = mbcore->tag_get_data(gid_tag,&ent,1,&gid);
  if (result != MB_SUCCESS) {
    std::cerr << "Problem getting tag data" << std::endl;
    assert(result == MB_SUCCESS);
  }

  return gid;
}



inline const Epetra_Map& Mesh_MOAB::cell_map (bool include_ghost) const {
  if (serial_run)
    return *cell_map_wo_ghosts_;
  else
    return (include_ghost ? *cell_map_w_ghosts_ : *cell_map_wo_ghosts_);
}


inline const Epetra_Map& Mesh_MOAB::face_map (bool include_ghost) const {
  if (serial_run)
    return *face_map_wo_ghosts_;
  else
    return (include_ghost ? *face_map_w_ghosts_ : *face_map_wo_ghosts_);
}

inline const Epetra_Map& Mesh_MOAB::node_map (bool include_ghost) const {
  if (serial_run)
    return *node_map_wo_ghosts_;
  else
    return (include_ghost ? *node_map_w_ghosts_ : *node_map_wo_ghosts_);
}

inline const Epetra_Map& Mesh_MOAB::exterior_face_map (void) const {
  throw std::exception(); // Not implemented
}

// Epetra importer that will allow apps to import values from a Epetra
// vector defined on all owned faces into an Epetra vector defined
// only on exterior faces
  
const Epetra_Import& 
Mesh_MOAB::exterior_face_importer (void) const
{
  Errors::Message mesg("not implemented");
  amanzi_throw(mesg);
}

// Get parallel type of eneity
  
Parallel_type Mesh_MOAB::entity_get_ptype(const Entity_kind kind, 
				 const Entity_ID entid) const
  {
    MBEntityHandle ent;
    unsigned char pstatus;

    switch (kind) {
    case NODE:
      ent = vtx_id_to_handle[entid];
      break;
      
    case FACE:
      ent = face_id_to_handle[entid];
      break;
      
    case CELL:
      ent = cell_id_to_handle[entid];
      break;
    default:
      std::cerr << "Global ID requested for unknown entity type" << std::endl;
    }

    mbcomm->get_pstatus(ent,pstatus);
    return ((pstatus & PSTATUS_NOT_OWNED) == 1) ? GHOST : OWNED;
  }
  
  
  
  
  // Get cell type
  
Cell_type Mesh_MOAB::cell_get_type(const Entity_ID cellid) const
  {
    return HEX;
  }
    


std::string 
Mesh_MOAB::internal_name_of_set(const AmanziGeometry::RegionPtr r,
                                const Entity_kind entity_kind) const {

  std::string internal_name;
  
  if (r->type() == AmanziGeometry::LABELEDSET) {
    
    AmanziGeometry::LabeledSetRegionPtr lsrgn = 
      dynamic_cast<AmanziGeometry::LabeledSetRegionPtr> (r);
    std::string label = lsrgn->label();

    if (entity_kind == CELL)
      internal_name = "matset_" + label;
    else if (entity_kind == FACE)
      internal_name = "sideset_" + label;
    else if (entity_kind == NODE)
      internal_name = "nodeset_" + label;      
  }
  else {
    if (entity_kind == CELL)
      internal_name = "CELLSET_" + r->name();
    else if (entity_kind == FACE)
      internal_name = "FACESET_" + r->name();
    else if (entity_kind == NODE)
      internal_name = "NODESET_" + r->name();
  }

  return internal_name;
}


// Deform a mesh so that cell volumes conform as closely as possible
// to target volumes without dropping below the minimum volumes.  If
// move_vertical = true, nodes will be allowed to move only in the
// vertical direction (right now arbitrary node movement is not allowed)

int Mesh_MOAB::deform(const std::vector<double>& target_cell_volumes_in, 
                      const std::vector<double>& min_cell_volumes_in, 
                      const Entity_ID_List& fixed_nodes,
                      const bool move_vertical) 
{
  Errors::Message mesg("Deformation not implemented for Mesh_MOAB");
  amanzi_throw(mesg);
}

// Miscellaneous

void Mesh_MOAB::write_to_exodus_file(const std::string filename) const {
  throw std::exception();
}

 

} // close namespace AmanziMesh
} // close namespace Amanzi