/*
This is the flow component of the Amanzi code. 
License: BSD
Authors: Neil Carlson (version 1) 
         Konstantin Lipnikov (version 2) (lipnikov@lanl.gov)
*/

#ifndef __INTERFACE_NOX_HPP__
#define __INTERFACE_NOX_HPP__


#include "exceptions.hh"
#include "NOX_Epetra_Interface_Required.H"
#include "NOX_Epetra_Interface_Jacobian.H"
#include "NOX_Epetra_Interface_Preconditioner.H"
#include "BDF2_Dae.hpp"
#include "Epetra_Map.h"
#include "Epetra_Operator.h"
#include "Mesh.hh"



namespace Amanzi {
namespace AmanziFlow {

class Interface_NOX : public NOX::Epetra::Interface::Required,
                      public NOX::Epetra::Interface::Jacobian,
                      public NOX::Epetra::Interface::Preconditioner {
 public:
  Interface_NOX(BDF2::fnBase* FPK_, const Epetra_Vector& uprev, double time_, double dt) : 
		FPK(FPK_), u0(uprev), lag_prec_(3), lag_count_(0) {
			time = time_;
			deltaT = dt;
			fun_eval = 0;
			fun_eval_time = 0;
		};
  ~Interface_NOX() {};

  // required interface members
  bool computeF(const Epetra_Vector& x, Epetra_Vector& f, FillType flag);
  bool computeJacobian(const Epetra_Vector& x, Epetra_Operator& J) { assert(false); }
  bool computePreconditioner(const Epetra_Vector& x, Epetra_Operator& M, Teuchos::ParameterList* params=NULL);
  void printTime();

  inline void setPrecLag(int lag_prec) { lag_prec_ = lag_prec;}
  inline void resetPrecLagCounter() { lag_count_ = 0; }
  inline int getPrecLag() const { return lag_prec_; }
  inline int getPrecLagCounter() const { return lag_count_; }

 private:
  BDF2::fnBase* FPK;
  const Epetra_Vector& u0;	// value at the previous time step

  double deltaT, time;		// time step
  int lag_prec_;  // the preconditioner is lagged this many times before it is recomputed
  int lag_count_; // this counts how many times the preconditioner has been lagged
  int fun_eval;
  double  fun_eval_time;
};


// class Preconditioner_Test : public Matrix_MFD{
class Preconditioner_Test : public Epetra_Operator{
	public:			    
		Preconditioner_Test(BDF2::fnBase* FPK_
                          //  Teuchos::RCP<Amanzi::AmanziMesh::Mesh> mesh_,
                            ):
                           FPK(FPK_) {mesh = FPK->mesh();};
		~Preconditioner_Test(){};
		
		// required methods
	int Apply(const Epetra_MultiVector& X, Epetra_MultiVector& Y) const { Y = X; return 0;};
	int ApplyInverse(const Epetra_MultiVector& X, Epetra_MultiVector& Y) const {return FPK->ApllyPrecInverse(X,Y); };
	bool UseTranspose() const { return false; }
	int SetUseTranspose(bool) { return 1; }
	
	const Epetra_Comm& Comm() const { return *(mesh->get_comm()); }
	const Epetra_Map& OperatorDomainMap() const { return FPK->super_map(); }
	const Epetra_Map& OperatorRangeMap() const { return FPK->super_map(); }

	const char* Label() const { return strdup("Preconditioner Test"); }
	double NormInf() const { return 0.0; }
	bool HasNormInf() const { return false; }
	
	private:
// 		Teuchos::RCP<Flow_State> FS;
                BDF2::fnBase* FPK;
		Teuchos::RCP<AmanziMesh::Mesh> mesh;
//		Epetra_Map map;
};

}  // namespace AmanziFlow
}  // namespace Amanzi

#endif
