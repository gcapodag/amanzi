#include <iostream>
#include "stdlib.h"
#include <UnitTest++.h>
#include <TestReporterStdout.h>

#include "VerboseObject_objs.hh"
int main(int argc, char *argv[])
{
  Teuchos::GlobalMPISession mpiSession(&argc,&argv);
  return UnitTest::RunAllTests ();
}
