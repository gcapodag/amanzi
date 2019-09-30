/*
  WhetStone, Version 2.2
  Release name: naka-to.

  Copyright 2010-201x held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Author: Konstantin Lipnikov (lipnikov@lanl.gov)

  Efficient quadratures on the reference triangle.
*/

#ifndef AMANZI_WHETSTONE_QUADRATURE_2D_HH_
#define AMANZI_WHETSTONE_QUADRATURE_2D_HH_

namespace Amanzi {
namespace WhetStone {

// number of points, start position
const int q2d_order[15][2] = {
  1,  0,   // skip order 0
  1,  0,
  3,  1,   // order 2 
  7,  4,
  7,  4,
  7,  4,   // order 5
  12, 11,  // order 6
  19, 23,  
  19, 23,  
  19, 23,  // order 9
  37, 42,
  37, 42,
  37, 42,
  37, 42,  // order 13
  0,  79
};

const double q2d_weights[79] = {
  1.0,                // order 1 with 1 point
  0.33333333333333,   // order 2 with 3 points
  0.33333333333333,
  0.33333333333333,
  0.225,              // order 5 with 7 points
  0.125939180544827,
  0.125939180544827,
  0.125939180544827,
  0.132394152788506,
  0.132394152788506,
  0.132394152788506,
  0.050844906370207,  // order 6 with 12 points
  0.050844906370207,
  0.050844906370207,
  0.116786275726379,
  0.116786275726379,
  0.116786275726379,
  0.082851075618374,
  0.082851075618374,
  0.082851075618374,
  0.082851075618374,
  0.082851075618374,
  0.082851075618374,
  0.0971357962827961, // order 9 with 19 points
  0.03133470022713983,
  0.03133470022713983,
  0.03133470022713983,
  0.07782754100477543,
  0.07782754100477543,
  0.07782754100477543,
  0.0796477389272091,
  0.0796477389272091,
  0.0796477389272091,
  0.0255776756586981,
  0.0255776756586981,
  0.0255776756586981,
  0.0432835393772894,
  0.0432835393772894,
  0.0432835393772894,
  0.0432835393772894,
  0.0432835393772894,
  0.0432835393772894,
  0.051739766065744133555179145422, // order 13 with 37 points
  0.008007799555564801597804123460,
  0.008007799555564801597804123460,
  0.008007799555564801597804123460,
  0.046868898981821644823226732071,
  0.046868898981821644823226732071,
  0.046868898981821644823226732071,
  0.046590940183976487960361770070,
  0.046590940183976487960361770070,
  0.046590940183976487960361770070,
  0.031016943313796381407646220131,
  0.031016943313796381407646220131,
  0.031016943313796381407646220131,
  0.010791612736631273623178240136,
  0.010791612736631273623178240136,
  0.010791612736631273623178240136,
  0.032195534242431618819414482205,
  0.032195534242431618819414482205,
  0.032195534242431618819414482205,
  0.015445834210701583817692900053,
  0.015445834210701583817692900053,
  0.015445834210701583817692900053,
  0.015445834210701583817692900053,
  0.015445834210701583817692900053,
  0.015445834210701583817692900053,
  0.017822989923178661888748319485,
  0.017822989923178661888748319485,
  0.017822989923178661888748319485,
  0.017822989923178661888748319485,
  0.017822989923178661888748319485,
  0.017822989923178661888748319485,
  0.037038683681384627918546472190,
  0.037038683681384627918546472190,
  0.037038683681384627918546472190,
  0.037038683681384627918546472190,
  0.037038683681384627918546472190,
  0.037038683681384627918546472190
};

const double q2d_points[79][3] = {
  0.333333333333333, 0.333333333333333, 0.333333333333333,  // order 1 with 1 point
  0.5, 0.5, 0.0,                                            // order 2 with 3 points
  0.0, 0.5, 0.5,
  0.5, 0.0, 0.5,
  0.333333333333333, 0.333333333333333, 0.333333333333333,  // order 5 with 7 points
  0.797426985353087, 0.101286507323456, 0.101286507323456,
  0.101286507323456, 0.797426985353087, 0.101286507323456,
  0.101286507323456, 0.101286507323456, 0.797426985353087,
  0.470142064105115, 0.470142064105115, 0.0597158717897698,
  0.0597158717897698,0.470142064105115, 0.470142064105115,
  0.470142064105115, 0.0597158717897698,0.470142064105115,
  0.873821971016996, 0.063089014491502, 0.063089014491502,  // order 6 with 12 points
  0.063089014491502, 0.873821971016996, 0.063089014491502,
  0.063089014491502, 0.063089014491502, 0.873821971016996,
  0.501426509658179, 0.249286745170910, 0.249286745170910,
  0.249286745170910, 0.501426509658179, 0.249286745170910,
  0.249286745170910, 0.249286745170910, 0.501426509658179,
  0.636502499121399, 0.310352451033785, 0.053145049844816,
  0.636502499121399, 0.053145049844816, 0.310352451033785,
  0.310352451033785, 0.636502499121399, 0.053145049844816,
  0.053145049844816, 0.636502499121399, 0.310352451033785,
  0.310352451033785, 0.053145049844816, 0.636502499121399,
  0.053145049844816, 0.310352451033785, 0.636502499121399,
  0.333333333333333, 0.333333333333333, 0.333333333333333,  // order 9 with 19 points
  0.489682519198737, 0.489682519198737, 0.0206349616025259,
  0.0206349616025259,0.489682519198737, 0.489682519198737,
  0.489682519198737, 0.0206349616025259,0.489682519198737,
  0.4370895914929355,0.4370895914929355,0.1258208170141290,
  0.1258208170141290,0.4370895914929355,0.4370895914929355,
  0.4370895914929355,0.1258208170141290,0.4370895914929355,
  0.6235929287619356,0.1882035356190322,0.1882035356190322,
  0.1882035356190322,0.6235929287619356,0.1882035356190322,
  0.1882035356190322,0.1882035356190322,0.6235929287619356,
  0.9105409732110941,0.044729513394453, 0.044729513394453,
  0.044729513394453, 0.9105409732110941,0.044729513394453,
  0.044729513394453, 0.044729513394453, 0.9105409732110941,
  0.741198598784498, 0.22196298916076573,0.0368384120547363,
  0.741198598784498, 0.0368384120547363,0.22196298916076573,
  0.22196298916076573,0.741198598784498,0.0368384120547363,
  0.0368384120547363,0.741198598784498, 0.22196298916076573,
  0.22196298916076573,0.0368384120547363,0.741198598784498,
  0.0368384120547363,0.22196298916076573,0.741198598784498,
  0.33333333333333333, 0.33333333333333333, 0.33333333333333333, // order 13 wih 37 points
  0.95027566292410556, 0.02486216853794721, 0.02486216853794721,
  0.02486216853794721, 0.95027566292410556, 0.02486216853794721,
  0.02486216853794721, 0.0248621685379472,  0.95027566292410556,
  0.41419254253808232, 0.41419254253808232, 0.17161491492383534,
  0.41419254253808232, 0.17161491492383534, 0.41419254253808232,
  0.17161491492383534, 0.41419254253808232, 0.41419254253808232,
  0.53941224367719044, 0.23029387816140477, 0.23029387816140477,
  0.23029387816140477, 0.53941224367719044, 0.23029387816140477,
  0.23029387816140477, 0.23029387816140477, 0.53941224367719044,
  0.77216003667653256, 0.11391998166173371, 0.11391998166173371,
  0.11391998166173371, 0.77216003667653256, 0.11391998166173371,
  0.11391998166173371, 0.11391998166173371, 0.77216003667653256,
  0.49545730002508232, 0.49545730002508232, 0.00908539994983535,
  0.49545730002508232, 0.00908539994983535, 0.49545730002508232,
  0.00908539994983535, 0.49545730002508232, 0.49545730002508232,
  0.46886135484705650, 0.46886135484705650, 0.06227729030588699,
  0.46886135484705650, 0.06227729030588699, 0.46886135484705650,
  0.06227729030588699, 0.46886135484705650, 0.46886135484705650,
  0.85130650417434855, 0.12661720617202709, 0.02207628965362440,
  0.85130650417434855, 0.02207628965362440, 0.12661720617202709,
  0.12661720617202709, 0.85130650417434855, 0.12661720617202709,
  0.02207628965362440, 0.85130650417434855, 0.12661720617202709,
  0.12661720617202709, 0.02207628965362440, 0.85130650417434855,
  0.02207628965362440, 0.12661720617202709, 0.85130650417434855,
  0.68944197072859129, 0.29193750646888777, 0.01862052280252096,
  0.68944197072859129, 0.01862052280252096, 0.29193750646888777,
  0.29193750646888777, 0.68944197072859129, 0.01862052280252096,
  0.01862052280252096, 0.68944197072859129, 0.29193750646888777,
  0.29193750646888777, 0.01862052280252096, 0.68944197072859129, 
  0.01862052280252096, 0.29193750646888777, 0.68944197072859129,
  0.63586785943387276, 0.26762565927396796, 0.09650648129215922,
  0.63586785943387276, 0.09650648129215922, 0.26762565927396796,
  0.26762565927396796, 0.63586785943387276, 0.09650648129215922,
  0.09650648129215922, 0.63586785943387276, 0.26762565927396796,
  0.26762565927396796, 0.09650648129215922, 0.63586785943387276, 
  0.09650648129215922, 0.26762565927396796, 0.63586785943387276
};

}  // namespace WhetStone
}  // namespace Amanzi

#endif

