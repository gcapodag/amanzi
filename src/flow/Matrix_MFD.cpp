/*
This is the flow component of the Amanzi code. 

Copyright 2010-2012 held jointly by LANS/LANL, LBNL, and PNNL. 
Amanzi is released under the three-clause BSD License. 
The terms of use and "as is" disclaimer for this license are 
provided in the top-level COPYRIGHT file.

Authors: Konstantin Lipnikov (version 2) (lipnikov@lanl.gov)
*/

#include <vector>

#include "Epetra_FECrsGraph.h"

#include "Flow_PK.hpp"
#include "Matrix_MFD.hpp"

namespace Amanzi {
namespace AmanziFlow {

Matrix_MFD::~Matrix_MFD()
{
  // if (MLprec->IsPreconditionerComputed()) {
  //  MLprec->DestroyPreconditioner();
  //  delete MLprec;
  // }
}


/* ******************************************************************
* Calculate elemental inverse mass matrices. 
* WARNING: The original Aff matrices are destroyed.                                            
****************************************************************** */
void Matrix_MFD::CreateMFDmassMatrices(int mfd3d_method, std::vector<WhetStone::Tensor>& K)
{
  int dim = mesh_->space_dimension();
  WhetStone::MFD3D mfd(mesh_);
  AmanziMesh::Entity_ID_List faces;
  std::vector<int> dirs;

  Mff_cells_.clear();

  int ok;
  nokay_ = npassed_ = 0;

  int ncells = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);
  for (int c = 0; c < ncells; c++) {
    mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
    int nfaces = faces.size();

    Teuchos::SerialDenseMatrix<int, double> Mff(nfaces, nfaces);

    if (mfd3d_method == AmanziFlow::FLOW_MFD3D_HEXAHEDRA_MONOTONE) {
      if ((nfaces == 6 && dim == 3) || (nfaces == 4 && dim == 2))
        ok = mfd.darcy_mass_inverse_hex(c, K[c], Mff);
      else
        ok = mfd.darcy_mass_inverse(c, K[c], Mff);
    } else if (mfd3d_method == AmanziFlow::FLOW_MFD3D_TWO_POINT_FLUX) {
      ok = mfd.darcy_mass_inverse_diagonal(c, K[c], Mff);
    } else if (mfd3d_method == AmanziFlow::FLOW_MFD3D_SUPPORT_OPERATOR) {
      ok = mfd.darcy_mass_inverse_SO(c, K[c], Mff);
    } else if (mfd3d_method == AmanziFlow::FLOW_MFD3D_OPTIMIZED) {
      ok = mfd.darcy_mass_inverse_optimized(c, K[c], Mff);
    } else {
      ok = mfd.darcy_mass_inverse(c, K[c], Mff);
    }

    Mff_cells_.push_back(Mff);

    if (ok == WhetStone::WHETSTONE_ELEMENTAL_MATRIX_FAILED) {
      Errors::Message msg("Matrix_MFD: unexpected failure of LAPACK in WhetStone.");
      Exceptions::amanzi_throw(msg);
    }
    if (ok == WhetStone::WHETSTONE_ELEMENTAL_MATRIX_OK) nokay_++;
    if (ok == WhetStone::WHETSTONE_ELEMENTAL_MATRIX_PASSED) npassed_++;
  }

  // sum up the numbers across processors
  int nokay_tmp = nokay_, npassed_tmp = npassed_;
  mesh_->get_comm()->SumAll(&nokay_tmp, &nokay_, 1);
  mesh_->get_comm()->SumAll(&npassed_tmp, &npassed_, 1);
}


/* ******************************************************************
* Calculate elemental stiffness matrices.                                            
****************************************************************** */
void Matrix_MFD::CreateMFDstiffnessMatrices(Epetra_Vector& Krel_cells,
                                            Epetra_Vector& Krel_faces)
{
  int dim = mesh_->space_dimension();
  WhetStone::MFD3D mfd(mesh_);
  AmanziMesh::Entity_ID_List faces;
  std::vector<int> dirs;

  Aff_cells_.clear();
  Afc_cells_.clear();
  Acf_cells_.clear();
  Acc_cells_.clear();

  int ncells = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);
  for (int c = 0; c < ncells; c++) {
    mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
    int nfaces = faces.size();

    Teuchos::SerialDenseMatrix<int, double>& Mff = Mff_cells_[c];
    Teuchos::SerialDenseMatrix<int, double> Bff(nfaces, nfaces);
    Epetra_SerialDenseVector Bcf(nfaces), Bfc(nfaces);

    for (int n = 0; n < nfaces; n++)
      for (int m = 0; m < nfaces; m++) Bff(m, n) = Mff(m, n) * Krel_cells[c] * Krel_faces[faces[m]];

    double matsum = 0.0;  // elimination of mass matrix
    for (int n = 0; n < nfaces; n++) {
      double rowsum = 0.0, colsum = 0.0;
      for (int m = 0; m < nfaces; m++) {
        colsum += Bff(m, n);
        rowsum += Bff(n, m);
      }
      Bcf(n) = -colsum;
      Bfc(n) = -rowsum;
      matsum += colsum;
    }

    Aff_cells_.push_back(Bff);  // This the only place where memory can be allocated.
    Afc_cells_.push_back(Bfc);
    Acf_cells_.push_back(Bcf);
    Acc_cells_.push_back(matsum);
  }
}


/* ******************************************************************
* May be used in the future.                                            
****************************************************************** */
void Matrix_MFD::RescaleMFDstiffnessMatrices(const Epetra_Vector& old_scale,
                                             const Epetra_Vector& new_scale)
{
  int ncells = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);

  for (int c = 0; c < ncells; c++) {
    Teuchos::SerialDenseMatrix<int, double>& Bff = Aff_cells_[c];
    Epetra_SerialDenseVector& Bcf = Acf_cells_[c];

    int n = Bff.numRows();
    double scale = old_scale[c] / new_scale[c];

    for (int i = 0; i < n; i++) {
      for (int j = 0; j < n; j++) Bff(i, j) *= scale;
      Bcf(i) *= scale;
    }
    Acc_cells_[c] *= scale;
  }
}


/* ******************************************************************
* Simply allocates memory.                                           
****************************************************************** */
void Matrix_MFD::CreateMFDrhsVectors()
{
  Ff_cells_.clear();
  Fc_cells_.clear();

  int ncells = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);
  AmanziMesh::Entity_ID_List faces;
  std::vector<int> dirs;

  for (int c = 0; c < ncells; c++) {
    mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
    int nfaces = faces.size();

    Epetra_SerialDenseVector Ff(nfaces);  // Entries are initilaized to 0.0.
    double Fc = 0.0;

    Ff_cells_.push_back(Ff);
    Fc_cells_.push_back(Fc);
  }
}


/* ******************************************************************
* Applies boundary conditions to elemental stiffness matrices and
* creates elemental rigth-hand-sides.                                           
****************************************************************** */
void Matrix_MFD::ApplyBoundaryConditions(
    std::vector<int>& bc_markers, std::vector<double>& bc_values)
{
  int ncells = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);
  AmanziMesh::Entity_ID_List faces;
  std::vector<int> dirs;

  for (int c = 0; c < ncells; c++) {
    mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
    int nfaces = faces.size();

    Teuchos::SerialDenseMatrix<int, double>& Bff = Aff_cells_[c];  // B means elemental.
    Epetra_SerialDenseVector& Bfc = Afc_cells_[c];
    Epetra_SerialDenseVector& Bcf = Acf_cells_[c];

    Epetra_SerialDenseVector& Ff = Ff_cells_[c];
    double& Fc = Fc_cells_[c];

    for (int n = 0; n < nfaces; n++) {
      int f = faces[n];
      if (bc_markers[f] == FLOW_BC_FACE_PRESSURE ||
          bc_markers[f] == FLOW_BC_FACE_HEAD) {
        for (int m = 0; m < nfaces; m++) {
          Ff[m] -= Bff(m, n) * bc_values[f];
          Bff(n, m) = Bff(m, n) = 0.0;
        }
        Fc -= Bcf(n) * bc_values[f];
        Bcf(n) = Bfc(n) = 0.0;

        Bff(n, n) = 1.0;
        Ff[n] = bc_values[f];
      } else if (bc_markers[f] == FLOW_BC_FACE_FLUX) {
        Ff[n] -= bc_values[f] * mesh_->face_area(f);
      }
    }
  }
}


/* ******************************************************************
* Initialize Trilinos matrices. It must be called only once. 
* If matrix is non-symmetric, we generate transpose of the matrix 
* block Afc to reuse cf_graph; otherwise, pointer Afc = Acf.   
****************************************************************** */
void Matrix_MFD::SymbolicAssembleGlobalMatrices(const Epetra_Map& super_map)
{
  const Epetra_Map& cmap = mesh_->cell_map(false);
  const Epetra_Map& fmap = mesh_->face_map(false);
  const Epetra_Map& fmap_wghost = mesh_->face_map(true);

  int avg_entries_row = (mesh_->space_dimension() == 2) ? FLOW_QUAD_FACES : FLOW_HEX_FACES;
  Epetra_CrsGraph cf_graph(Copy, cmap, fmap_wghost, avg_entries_row, false);  // FIX (lipnikov@lanl.gov)
  Epetra_FECrsGraph ff_graph(Copy, fmap, 2*avg_entries_row);

  AmanziMesh::Entity_ID_List faces;
  std::vector<int> dirs;
  int faces_LID[FLOW_MAX_FACES];  // Contigious memory is required.
  int faces_GID[FLOW_MAX_FACES];

  int ncells = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);
  for (int c = 0; c < ncells; c++) {
    mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
    int nfaces = faces.size();

    for (int n = 0; n < nfaces; n++) {
      faces_LID[n] = faces[n];
      faces_GID[n] = fmap_wghost.GID(faces_LID[n]);
    }
    cf_graph.InsertMyIndices(c, nfaces, faces_LID);
    ff_graph.InsertGlobalIndices(nfaces, faces_GID, nfaces, faces_GID);
  }
  cf_graph.FillComplete(fmap, cmap);
  ff_graph.GlobalAssemble();  // Symbolic graph is complete.

  // create global matrices
  Acc_ = Teuchos::rcp(new Epetra_Vector(cmap));
  Acf_ = Teuchos::rcp(new Epetra_CrsMatrix(Copy, cf_graph));
  Aff_ = Teuchos::rcp(new Epetra_FECrsMatrix(Copy, ff_graph));
  Sff_ = Teuchos::rcp(new Epetra_FECrsMatrix(Copy, ff_graph));
  Aff_->GlobalAssemble();
  Sff_->GlobalAssemble();

  if (flag_symmetry_)
    Afc_ = Acf_;
  else
    Afc_ = Teuchos::rcp(new Epetra_CrsMatrix(Copy, cf_graph));

  rhs_ = Teuchos::rcp(new Epetra_Vector(super_map));
  rhs_cells_ = Teuchos::rcp(FS->CreateCellView(*rhs_));
  rhs_faces_ = Teuchos::rcp(FS->CreateFaceView(*rhs_));
}


/* ******************************************************************
* Convert elemental mass matrices into stiffness matrices and 
* assemble them into four global matrices. 
* We need an auxiliary GHOST-based vector to assemble the RHS.
****************************************************************** */
void Matrix_MFD::AssembleGlobalMatrices()
{
  Aff_->PutScalar(0.0);

  const Epetra_Map& fmap_wghost = mesh_->face_map(true);
  AmanziMesh::Entity_ID_List faces;
  std::vector<int> dirs;
  int faces_LID[FLOW_MAX_FACES];
  int faces_GID[FLOW_MAX_FACES];

  int ncells = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);

  for (int c = 0; c < ncells; c++) {
    mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
    int nfaces = faces.size();

    for (int n = 0; n < nfaces; n++) {
      faces_LID[n] = faces[n];
      faces_GID[n] = fmap_wghost.GID(faces_LID[n]);
    }
    (*Acc_)[c] = Acc_cells_[c];
    (*Acf_).ReplaceMyValues(c, nfaces, Acf_cells_[c].Values(), faces_LID);
    (*Aff_).SumIntoGlobalValues(nfaces, faces_GID, Aff_cells_[c].values());

    if (!flag_symmetry_)
        (*Afc_).ReplaceMyValues(c, nfaces, Afc_cells_[c].Values(), faces_LID);
  }
  (*Aff_).GlobalAssemble();

  // We repeat some of the loops for code clarity.
  Epetra_Vector rhs_faces_wghost(fmap_wghost);

  for (int c = 0; c < ncells; c++) {
    mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
    int nfaces = faces.size();

    (*rhs_cells_)[c] = Fc_cells_[c];
    for (int n = 0; n < nfaces; n++) {
      int f = faces[n];
      rhs_faces_wghost[f] += Ff_cells_[c][n];
    }
  }
  FS->CombineGhostFace2MasterFace(rhs_faces_wghost, Add);

  int nfaces = mesh_->num_entities(AmanziMesh::FACE, AmanziMesh::OWNED);
  for (int f = 0; f < nfaces; f++) (*rhs_faces_)[f] = rhs_faces_wghost[f];
}


/* ******************************************************************
* Compute the face Schur complement of 2x2 block matrix.
****************************************************************** */
void Matrix_MFD::ComputeSchurComplement(
    std::vector<int>& bc_markers, std::vector<double>& bc_values)
{
  Sff_->PutScalar(0.0);

  AmanziMesh::Entity_ID_List faces_LID;
  std::vector<int> dirs;
  int ncells = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);

  for (int c = 0; c < ncells; c++) {
    mesh_->cell_get_faces_and_dirs(c, &faces_LID, &dirs);
    int nfaces = faces_LID.size();
    Epetra_SerialDenseMatrix Schur(nfaces, nfaces);

    Epetra_SerialDenseVector& Bcf = Acf_cells_[c];
    Epetra_SerialDenseVector& Bfc = Afc_cells_[c];

    for (int n = 0; n < nfaces; n++) {
      for (int m = 0; m < nfaces; m++) {
        Schur(n, m) = Aff_cells_[c](n, m) - Bfc[n] * Bcf[m] / (*Acc_)[c];
      }
    }

    for (int n = 0; n < nfaces; n++) {  // Symbolic boundary conditions
      int f = faces_LID[n];
      if (bc_markers[f] == FLOW_BC_FACE_PRESSURE ||
          bc_markers[f] == FLOW_BC_FACE_HEAD) {
        for (int m = 0; m < nfaces; m++) Schur(n, m) = Schur(m, n) = 0.0;
        Schur(n, n) = 1.0;
      }
    }

    Epetra_IntSerialDenseVector faces_GID(nfaces);
    for (int n = 0; n < nfaces; n++) faces_GID[n] = (*Acf_).ColMap().GID(faces_LID[n]);
    (*Sff_).SumIntoGlobalValues(faces_GID, Schur);
  }
  (*Sff_).GlobalAssemble();
}


/* ******************************************************************
* Linear algebra operations with matrices: r = f - A * x                                                 
****************************************************************** */
double Matrix_MFD::ComputeResidual(const Epetra_Vector& solution, Epetra_Vector& residual)
{
  Apply(solution, residual);
  residual.Update(1.0, *rhs_, -1.0);

  double norm_residual;
  residual.Norm2(&norm_residual);
  return norm_residual;
}


/* ******************************************************************
* Linear algebra operations with matrices: r = A * x - f                                                 
****************************************************************** */
double Matrix_MFD::ComputeNegativeResidual(const Epetra_Vector& solution, Epetra_Vector& residual)
{
  Apply(solution, residual);
  residual.Update(-1.0, *rhs_, 1.0);

  double norm_residual;
  residual.Norm2(&norm_residual);
  return norm_residual;
}


/* ******************************************************************
* Initialization of the preconditioner                                                 
****************************************************************** */
void Matrix_MFD::InitML_Preconditioner(Teuchos::ParameterList& ML_list_)
{
  ML_list = ML_list_;
  MLprec = new ML_Epetra::MultiLevelPreconditioner(*Sff_, ML_list, false);
}


/* ******************************************************************
* Rebuild ML preconditioner.                                                 
****************************************************************** */
void Matrix_MFD::UpdateML_Preconditioner()
{
  if (MLprec->IsPreconditionerComputed()) MLprec->DestroyPreconditioner();
  MLprec->SetParameterList(ML_list);
  MLprec->ComputePreconditioner();
}


/* ******************************************************************
* Parallel matvec product Aff_cells * X.                                              
****************************************************************** */
int Matrix_MFD::Apply(const Epetra_MultiVector& X, Epetra_MultiVector& Y) const
{
  int ncells = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);
  int nvectors = X.NumVectors();

  const Epetra_Map& cmap = mesh_->cell_map(false);
  const Epetra_Map& fmap = mesh_->face_map(false);

  // Create views Xc and Xf into the cell and face segments of X.
  double **cvec_ptrs = X.Pointers();
  double **fvec_ptrs = new double*[nvectors];
  for (int i = 0; i < nvectors; i++) fvec_ptrs[i] = cvec_ptrs[i] + ncells;

  Epetra_MultiVector Xc(View, cmap, cvec_ptrs, nvectors);
  Epetra_MultiVector Xf(View, fmap, fvec_ptrs, nvectors);

  // Create views Yc and Yf into the cell and face segments of Y.
  cvec_ptrs = Y.Pointers();
  for (int i = 0; i < nvectors; i++) fvec_ptrs[i] = cvec_ptrs[i] + ncells;

  Epetra_MultiVector Yc(View, cmap, cvec_ptrs, nvectors);
  Epetra_MultiVector Yf(View, fmap, fvec_ptrs, nvectors);

  // Face unknowns:  Yf = Aff * Xf + Afc * Xc
  int ierr;
  Epetra_MultiVector Tf(fmap, nvectors);
  ierr  = (*Aff_).Multiply(false, Xf, Yf);
  ierr |= (*Afc_).Multiply(true, Xc, Tf);  // Afc is kept in the transpose form
  Yf.Update(1.0, Tf, 1.0);

  // Cell unknowns:  Yc = Acf * Xf + Acc * Xc
  ierr |= (*Acf_).Multiply(false, Xf, Yc);  // It performs the required parallel communications.
  ierr |= Yc.Multiply(1.0, *Acc_, Xc, 1.0);

  if (ierr) {
    Errors::Message msg("Matrix_MFD::Apply has failed to calculate y = A*x.");
    Exceptions::amanzi_throw(msg);
  }
  delete [] fvec_ptrs;
  return 0;
}


/* ******************************************************************
* The OWNED cell-based and face-based d.o.f. are packed together into 
* the X and Y Epetra vectors, with the cell-based in the first part.
*
* WARNING: When invoked by AztecOO the arguments X and Y may be 
* aliased: possibly the same object or different views of the same 
* underlying data. Thus, we do not assign to Y until the end.                                              
****************************************************************** */
int Matrix_MFD::ApplyInverse(const Epetra_MultiVector& X, Epetra_MultiVector& Y) const
{
  int ncells = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);
  int nvectors = X.NumVectors();

  const Epetra_Map& cmap = mesh_->cell_map(false);
  const Epetra_Map& fmap = mesh_->face_map(false);

  // Create views Xc and Xf into the cell and face segments of X.
  double **cvec_ptrs = X.Pointers();
  double **fvec_ptrs = new double*[nvectors];
  for (int i = 0; i < nvectors; i++) fvec_ptrs[i] = cvec_ptrs[i] + ncells;

  Epetra_MultiVector Xc(View, cmap, cvec_ptrs, nvectors);
  Epetra_MultiVector Xf(View, fmap, fvec_ptrs, nvectors);

  // Create views Yc and Yf into the cell and face segments of Y.
  cvec_ptrs = Y.Pointers();
  for (int i = 0; i < nvectors; i++) fvec_ptrs[i] = cvec_ptrs[i] + ncells;

  Epetra_MultiVector Yc(View, cmap, cvec_ptrs, nvectors);
  Epetra_MultiVector Yf(View, fmap, fvec_ptrs, nvectors);

  // Temporary cell and face vectors.
  Epetra_MultiVector Tc(cmap, nvectors);
  Epetra_MultiVector Tf(fmap, nvectors);

  // FORWARD ELIMINATION:  Tf = Xf - Afc inv(Acc) Xc
  int ierr;
  ierr  = Tc.ReciprocalMultiply(1.0, *Acc_, Xc, 0.0);
  ierr |= (*Afc_).Multiply(true, Tc, Tf);  // Afc is kept in transpose form
  Tf.Update(1.0, Xf, -1.0);

  // Solve the Schur complement system Sff * Yf = Tf.
  MLprec->ApplyInverse(Tf, Yf);

  // BACKWARD SUBSTITUTION:  Yc = inv(Acc) (Xc - Acf Yf)
  ierr |= (*Acf_).Multiply(false, Yf, Tc);  // It performs the required parallel communications.
  Tc.Update(1.0, Xc, -1.0);
  ierr |= Yc.ReciprocalMultiply(1.0, *Acc_, Tc, 0.0);

  if (ierr) {
    Errors::Message msg("Matrix_MFD::ApplyInverse has failed in calculating y = inv(A)*x.");
    Exceptions::amanzi_throw(msg);
  }
  delete [] fvec_ptrs;
  return 0;
}


/* ******************************************************************
* WARNING: Routines requires original mass matrices (Aff_cells), i.e.
* before boundary conditions were imposed.
*
* WARNING: Since diffusive flux is not continuous, we derive it only
* once (using flag) and in exactly the same manner as in routine
* Flow_PK::addGravityFluxes_DarcyFlux.
****************************************************************** */
void Matrix_MFD::DeriveDarcyMassFlux(const Epetra_Vector& solution,
                                     const Epetra_Import& face_importer,
                                     Epetra_Vector& darcy_mass_flux)
{
  Epetra_Vector* solution_faces = FS->CreateFaceView(solution);
#ifdef HAVE_MPI
  Epetra_Vector solution_faces_wghost(mesh_->face_map(true));
  solution_faces_wghost.Import(*solution_faces, face_importer, Insert);
#else
  Epetra_Vector& solution_faces_wghost = *solution_faces;
#endif

  AmanziMesh::Entity_ID_List faces;
  std::vector<double> dp;
  std::vector<int> dirs;

  int ncells = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);
  int nfaces_owned = mesh_->num_entities(AmanziMesh::FACE, AmanziMesh::OWNED);
  int nfaces_wghost = mesh_->num_entities(AmanziMesh::FACE, AmanziMesh::USED);
  std::vector<int> flag(nfaces_wghost, 0);

  for (int c = 0; c < ncells; c++) {
    mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
    int nfaces = faces.size();

    dp.resize(nfaces);
    for (int n = 0; n < nfaces; n++) {
      int f = faces[n];
      dp[n] = solution[c] - solution_faces_wghost[f];
    }

    for (int n = 0; n < nfaces; n++) {
      int f = faces[n];
      if (f < nfaces_owned && !flag[f]) {
        double s(0.0);
        for (int m = 0; m < nfaces; m++) s += Aff_cells_[c](n, m) * dp[m];
        darcy_mass_flux[f] = s * dirs[n];
        flag[f] = 1;
      }
    }
  }
}


/* ******************************************************************
* Derive Darcy velocity in cells. 
* WARNING: It cannot be consistent with the Darcy flux.                                                 
****************************************************************** */
void Matrix_MFD::DeriveDarcyVelocity(const Epetra_Vector& darcy_flux,
                                     const Epetra_Import& face_importer,
                                     Epetra_MultiVector& darcy_velocity) const
{
#ifdef HAVE_MPI
  Epetra_Vector darcy_flux_wghost(mesh_->face_map(true));
  darcy_flux_wghost.Import(darcy_flux, face_importer, Insert);
#else
  Epetra_Vector& darcy_flux_wghost = darcy_flux;
#endif

  Teuchos::LAPACK<int, double> lapack;

  int dim = mesh_->space_dimension();
  Teuchos::SerialDenseMatrix<int, double> matrix(dim, dim);
  double rhs_cell[dim];

  AmanziMesh::Entity_ID_List faces;
  std::vector<int> dirs;

  int ncells_owned = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);
  for (int c = 0; c < ncells_owned; c++) {
    mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
    int nfaces = faces.size();

    for (int i = 0; i < dim; i++) rhs_cell[i] = 0.0;
    matrix.putScalar(0.0);

    for (int n = 0; n < nfaces; n++) {  // populate least-square matrix
      int f = faces[n];
      const AmanziGeometry::Point& normal = mesh_->face_normal(f);
      double area = mesh_->face_area(f);

      for (int i = 0; i < dim; i++) {
        rhs_cell[i] += normal[i] * darcy_flux_wghost[f];
        matrix(i, i) += normal[i] * normal[i];
        for (int j = i+1; j < dim; j++) {
          matrix(j, i) = matrix(i, j) += normal[i] * normal[j];
        }
      }
    }

    int info;
    lapack.POSV('U', dim, 1, matrix.values(), dim, rhs_cell, dim, &info);

    for (int i = 0; i < dim; i++) darcy_velocity[i][c] = rhs_cell[i];
  }
}

}  // namespace AmanziFlow
}  // namespace Amanzi

