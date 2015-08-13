/*
  This is the input component of the Amanzi code. 

  Copyright 2010-201x held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Authors: Jeffrey Johnson (jnjohnson@lbl.gov)
*/

#include <boost/algorithm/string.hpp>  // For string trimming
#include "BoxLib.H"

#include "errors.hh"
#include "exceptions.hh"
#include "InputConverterS.hh"

namespace Amanzi {
namespace AmanziInput {

// For saving a bit of typing.
using namespace std;
using namespace boost::algorithm;
using namespace xercesc;

// Internally-used stuff.
namespace {

typedef list<ParmParse::PP_entry>::iterator list_iterator;
typedef list<ParmParse::PP_entry>::const_iterator const_list_iterator;

// Trims strings and replaces spaces with underscores in a string.
string MangleString(const string& s)
{
  string s1 = s;
  trim(s1);
  for (int i = 0; i < s.length(); ++i)
  {
    if (s1[i] == ' ')
      s1[i] = '_';
  }
  return s1;
}

// Construct a ParmParse prefix name from sets of strings.
string MakePPPrefix(const string& s1)
{
  return MangleString(s1);
}

string MakePPPrefix(const string& s1, const string& s2)
{
  return MangleString(s1) + string(".") + MangleString(s2);
}

string MakePPPrefix(const string& s1, const string& s2, const string& s3)
{
  return MangleString(s1) + string(".") + MangleString(s2) + string(".") + MangleString(s3);
}

string MakePPPrefix(const string& s1, const string& s2, const string& s3, const string& s4)
{
  return MangleString(s1) + string(".") + MangleString(s2) + string(".") + MangleString(s3) + string(".") + MangleString(s4);
}

string MakePPPrefix(const string& s1, const string& s2, const string& s3, const string& s4, const string& s5)
{
  return MangleString(s1) + string(".") + MangleString(s2) + string(".") + MangleString(s3) + string(".") + MangleString(s4) + string(".") + MangleString(s5);
}

// Construct a ParmParse entry from sets of strings/values.
list<string> MakePPEntry(const string& s1)
{
  list<string> pp;
  pp.push_back(MangleString(s1));
  return pp;
}

list<string> MakePPEntry(double d)
{
  list<string> pp;
  stringstream s;
  s << d;
  pp.push_back(s.str());
  return pp;
}

list<string> MakePPEntry(int i)
{
  list<string> pp;
  stringstream s;
  s << i;
  pp.push_back(s.str());
  return pp;
}

list<string> MakePPEntry(long i)
{
  list<string> pp;
  stringstream s;
  s << i;
  pp.push_back(s.str());
  return pp;
}

list<string> MakePPEntry(const string& s1, const string& s2)
{
  list<string> pp;
  pp.push_back(MangleString(s1));
  pp.push_back(MangleString(s2));
  return pp;
}

list<string> MakePPEntry(const vector<string>& ss)
{
  list<string> pp;
  for (size_t i = 0; i < ss.size(); ++i)
    pp.push_back(MangleString(ss[i]));
  return pp;
}

list<string> MakePPEntry(const vector<double>& ds)
{
  list<string> pp;
  for (size_t i = 0; i < ds.size(); ++i)
  {
    stringstream s;
    s << ds[i];
    pp.push_back(s.str());
  }
  return pp;
}

list<string> MakePPEntry(const vector<int>& is)
{
  list<string> pp;
  for (size_t i = 0; i < is.size(); ++i)
  {
    stringstream s;
    s << is[i];
    pp.push_back(s.str());
  }
  return pp;
}

list<string> MakePPEntry(const vector<long>& is)
{
  list<string> pp;
  for (size_t i = 0; i < is.size(); ++i)
  {
    stringstream s;
    s << is[i];
    pp.push_back(s.str());
  }
  return pp;
}

// Shortcut for putting entries into tables.
void AddToTable(list<ParmParse::PP_entry>& table,
                const string& entry_name,
                list<string> entry)
{
  table.push_back(ParmParse::PP_entry(entry_name, entry));
}

} // end anonymous namespace

// Helper for parsing a mechanical property.
void InputConverterS::ParseMechProperty_(DOMElement* mech_prop_node, 
                                         const string& material_name, 
                                         const string& property_name,
                                         list<ParmParse::PP_entry>& table,
                                         bool required)
{
  bool found;
  DOMElement* property = GetChildByName_(mech_prop_node, property_name, found, required);
  if (found)
  {
    if (property_name == "dispersion_tensor") // Weirdo!
    {
      string type = GetAttributeValueS_(property, "type");
      if (type == "uniform_isotropic")
      {
        string alpha_l = GetAttributeValueS_(property, "alpha_l", true);
        string alpha_t = GetAttributeValueS_(property, "alpha_t", true);
        AddToTable(table, MakePPPrefix("rock", material_name, property_name, "alpha_l"),
                   MakePPEntry(alpha_l));
        AddToTable(table, MakePPPrefix("rock", material_name, property_name, "alpha_t"),
                   MakePPEntry(alpha_t));
      }
      else if (type == "burnett_frind")
      {
        string alpha_l = GetAttributeValueS_(property, "alpha_l", true);
        string alpha_th = GetAttributeValueS_(property, "alpha_th", true);
        string alpha_tv = GetAttributeValueS_(property, "alpha_tv", true);
        AddToTable(table, MakePPPrefix("rock", material_name, property_name, "alpha_l"),
                   MakePPEntry(alpha_l));
        AddToTable(table, MakePPPrefix("rock", material_name, property_name, "alpha_th"),
                   MakePPEntry(alpha_th));
        AddToTable(table, MakePPPrefix("rock", material_name, property_name, "alpha_tv"),
                   MakePPEntry(alpha_tv));
      }
      else if (type == "lichtner_kelkar_robinson")
      {
        string alpha_lh = GetAttributeValueS_(property, "alpha_lh", true);
        string alpha_lv = GetAttributeValueS_(property, "alpha_lv", true);
        string alpha_th = GetAttributeValueS_(property, "alpha_th", true);
        string alpha_tv = GetAttributeValueS_(property, "alpha_tv", true);
        AddToTable(table, MakePPPrefix("rock", material_name, property_name, "alpha_lh"),
                   MakePPEntry(alpha_lh));
        AddToTable(table, MakePPPrefix("rock", material_name, property_name, "alpha_lv"),
                   MakePPEntry(alpha_lv));
        AddToTable(table, MakePPPrefix("rock", material_name, property_name, "alpha_th"),
                   MakePPEntry(alpha_th));
        AddToTable(table, MakePPPrefix("rock", material_name, property_name, "alpha_tv"),
                   MakePPEntry(alpha_tv));
      }
      else if (type == "file")
      {
        string filename = GetAttributeValueS_(property, "filename", true);
        AddToTable(table, MakePPPrefix("rock", material_name, property_name, "filename"),
                   MakePPEntry(filename));
      }
      else
        ThrowErrorIllformed_("materials->mechanical_properties", "type", property_name);
      AddToTable(table, MakePPPrefix("rock", material_name, property_name, "type"),
                 MakePPEntry(type));
    }
    else
    {
      string value = GetAttributeValueS_(property, "value", false);
      string type = GetAttributeValueS_(property, "type", false);
      if (!value.empty() && ((property_name != "porosity") || (type != "gslib")))
      {
        AddToTable(table, MakePPPrefix("rock", material_name, property_name, "vals"),
                   MakePPEntry(value));
        AddToTable(table, MakePPPrefix("rock", material_name, property_name, "distribution_type"),
                   MakePPEntry("uniform"));
      }
      else
      {
        if (!type.empty())
        {
          if (type == "file")
          {
            string filename = GetAttributeValueS_(property, "filename");
            AddToTable(table, MakePPPrefix("rock", material_name, property_name, "filename"),
                       MakePPEntry(filename));
            AddToTable(table, MakePPPrefix("rock", material_name, property_name, "type"),
                       MakePPEntry("file"));
          }
          else if ((property_name == "porosity") && (type == "gslib")) // special considerations!
          {
            string parameter_file = GetAttributeValueS_(property, "parameter_file");
            AddToTable(table, MakePPPrefix("rock", material_name, property_name, "parameter_file"),
                       MakePPEntry(parameter_file));
            AddToTable(table, MakePPPrefix("rock", material_name, property_name, "type"),
                       MakePPEntry("gslib"));

            string data_file = GetAttributeValueS_(property, "data_file", false);
            if (!data_file.empty())
            {
              AddToTable(table, MakePPPrefix("rock", material_name, property_name, "data_file"),
                         MakePPEntry(data_file));
            }
          }
        }
      }
    }
  }
}

InputConverterS::InputConverterS():
  InputConverter()
{
}

InputConverterS::~InputConverterS()
{
}

void InputConverterS::ParseUnits_()
{
  // Units are supposedly not supported yet. I guess we'll find out!
}

void InputConverterS::ParseDefinitions_()
{
  list<ParmParse::PP_entry> table;
  bool found;
  DOMNode* macros = GetUniqueElementByTagsString_("definitions, macros", found);
  if (found)
  {
    bool found;
    vector<DOMNode*> time_macros = GetChildren_(macros, "time_macro", found);
    vector<string> time_macro_names;
    if (found)
    {
      for (size_t i = 0; i < time_macros.size(); ++i)
      {
        DOMElement* time_macro = static_cast<DOMElement*>(time_macros[i]);
        string macro_name = GetAttributeValueS_(time_macro, "name");
        time_macro_names.push_back(macro_name);
        vector<string> times;

        // Before we look for specific times, check for other stuff.
        bool found;
        string start = GetChildValueS_(time_macro, "start", found);
        if (found)
        {
          // We've got one of the interval-based time macros.
          // Get the other required elements.
          string timestep_interval = GetChildValueS_(time_macro, "timestep_interval", found, true);
          string stop = GetChildValueS_(time_macro, "stop", found, true);
          
          char* endptr = const_cast<char*>(start.c_str());
          double t1 = strtod(start.c_str(), &endptr);
          if (endptr == start.c_str()) // Not parsed!
            ThrowErrorIllformed_("definitions->macros", "start", "time_macro");
          endptr = const_cast<char*>(stop.c_str());
          double t2 = strtod(stop.c_str(), &endptr);
          if (endptr == stop.c_str()) // Not parsed!
            ThrowErrorIllformed_("definitions->macros", "stop", "time_macro");
          endptr = const_cast<char*>(timestep_interval.c_str());
          double interval = strtod(timestep_interval.c_str(), &endptr);
          if (endptr == timestep_interval.c_str()) // Not parsed!
            ThrowErrorIllformed_("definitions->macros", "timestep_interval", "time_macro");

          // Shove this macro into our table.
          AddToTable(table, MakePPPrefix("amr", "time_macros", macro_name, "type"),
                                         MakePPEntry("period"));
          AddToTable(table, MakePPPrefix("amr", "time_macros", macro_name, "start"),
                                         MakePPEntry(start));
          AddToTable(table, MakePPPrefix("amr", "time_macros", macro_name, "stop"),
                                         MakePPEntry(stop));
          AddToTable(table, MakePPPrefix("amr", "time_macros", macro_name, "period"),
                                         MakePPEntry(timestep_interval));
        }
        else
        {
          // We're just looking for times.
          bool found;
          vector<DOMNode*> time_nodes = GetChildren_(time_macro, "time", found, true);
          vector<string> times;
          for (size_t j = 0; j < time_nodes.size(); ++j)
            times.push_back(XMLString::transcode(time_nodes[j]->getTextContent()));

          string macro_name = MangleString(macro_name);
          AddToTable(table, MakePPPrefix("amr", "time_macros", macro_name, "type"),
                                         MakePPEntry("times"));
          AddToTable(table, MakePPPrefix("amr", "time_macros", macro_name, "times"),
                                         MakePPEntry(times));
        }
      }
    }
    // List of all time macro names.
    AddToTable(table, MakePPPrefix("amr", "time_macros"), 
                                   MakePPEntry(time_macro_names));

    vector<DOMNode*> cycle_macros = GetChildren_(macros, "cycle_macro", found);
    vector<string> cycle_macro_names;
    if (found)
    {
      for (size_t i = 0; i < cycle_macros.size(); ++i)
      {
        DOMElement* cycle_macro = static_cast<DOMElement*>(cycle_macros[i]);
        string macro_name = GetAttributeValueS_(cycle_macro, "name");
        cycle_macro_names.push_back(macro_name);
        vector<string> cycles;

        bool found;
        string start = GetChildValueS_(cycle_macro, "start", found, true);
        string timestep_interval = GetChildValueS_(cycle_macro, "timestep_interval", found, true);
        string stop = GetChildValueS_(cycle_macro, "stop", found, true);
         
        // Verify the entries.
        char* endptr = const_cast<char*>(start.c_str());
        long c1 = strtol(start.c_str(), &endptr, 10);
        if (endptr == start.c_str()) // Not parsed!
          ThrowErrorIllformed_("definitions->macros", "start", "cycle_macro");
        endptr = const_cast<char*>(stop.c_str());
        long c2 = strtol(stop.c_str(), &endptr, 10);
        if (endptr == stop.c_str()) // Not parsed!
          ThrowErrorIllformed_("definitions->macros", "stop", "cycle_macro");
        endptr = const_cast<char*>(timestep_interval.c_str());
        long interval = strtol(timestep_interval.c_str(), &endptr, 10);
        if (endptr == timestep_interval.c_str()) // Not parsed!
          ThrowErrorIllformed_("definitions->macros", "timestep_interval", "cycle_macro");

        // Shove this macro into our table.
        AddToTable(table, MakePPPrefix("amr", "cycle_macro", macro_name, "type"),
                                       MakePPEntry("period"));
        AddToTable(table, MakePPPrefix("amr", "cycle_macro", macro_name, "start"),
                                       MakePPEntry(start));
        AddToTable(table, MakePPPrefix("amr", "cycle_macro", macro_name, "stop"),
                                       MakePPEntry(stop));
        AddToTable(table, MakePPPrefix("amr", "cycle_macro", macro_name, "period"),
                                       MakePPEntry(timestep_interval));
      }
    }
    // List of all cycle macro names.
    AddToTable(table, MakePPPrefix("amr", "cycle_macros"), 
                                   MakePPEntry(cycle_macro_names));

    // FIXME: variable_macro not yet supported.
  }

  if (!table.empty())
    ParmParse::appendTable(table);
}

void InputConverterS::ParseExecutionControls_()
{
  list<ParmParse::PP_entry> table;
  bool found;

  // Verbosity level(s).
  DOMNode* verbosity = GetUniqueElementByTagsString_("execution_controls, verbosity", found);
  if (found)
  {
    DOMElement* verb = static_cast<DOMElement*>(verbosity);
    string level = GetAttributeValueS_(verb, "level");
    transform(level.begin(), level.end(), level.begin(), ::tolower);

    // Each level of verbosity corresponds to several verbosity parameters 
    // for Amanzi-S.
    int prob_v, mg_v, cg_v, amr_v, diffuse_v, io_v, fab_v;
    if (level == "none") {
      prob_v = 0; mg_v = 0; cg_v = 0; amr_v = 0; diffuse_v = 0; io_v = 0; fab_v = 0;
    }
    else if (level == "low") {
      prob_v = 1; mg_v = 0; cg_v = 0; amr_v = 1;  diffuse_v = 0; io_v = 0; fab_v = 0;
    }
    else if (level == "medium") {
      prob_v = 1; mg_v = 0; cg_v = 0; amr_v = 2;  diffuse_v = 0; io_v = 0; fab_v = 0;
    }
    else if (level == "high") {
      prob_v = 2; mg_v = 0; cg_v = 0; amr_v = 3;  diffuse_v = 0; io_v = 0; fab_v = 0;
    }
    else if (level == "extreme") {
      prob_v = 3; mg_v = 2; cg_v = 2; amr_v = 3;  diffuse_v = 1; io_v = 1; fab_v = 1;
    }

    AddToTable(table, MakePPPrefix("prob", "v"), MakePPEntry(prob_v));
    AddToTable(table, MakePPPrefix("mg", "v"), MakePPEntry(mg_v));
    AddToTable(table, MakePPPrefix("cg", "v"), MakePPEntry(cg_v));
    AddToTable(table, MakePPPrefix("amr", "v"), MakePPEntry(amr_v));
    AddToTable(table, MakePPPrefix("diffuse", "v"), MakePPEntry(diffuse_v));
    AddToTable(table, MakePPPrefix("io", "v"), MakePPEntry(io_v));
    AddToTable(table, MakePPPrefix("fab", "v"), MakePPEntry(fab_v));
  }

  if (!table.empty())
    ParmParse::appendTable(table);
}

void InputConverterS::ParseNumericalControls_()
{
  list<ParmParse::PP_entry> table;
  ParmParse::appendTable(table);
}

void InputConverterS::ParseMesh_()
{
  list<ParmParse::PP_entry> table;
  bool found;

  DOMElement* dimension = static_cast<DOMElement*>(GetUniqueElementByTagsString_("mesh, dimension", found));
  {
    MemoryManager mm;
    string dim = XMLString::transcode(dimension->getTextContent());
    if (dim == "2")
      dim_ = 2;
    else if (dim == "3")
      dim_ = 3;
    else
      ThrowErrorIllformed_("mesh->generate", "integer (2 or 3)", "dimension");
  }
  DOMElement* generate = static_cast<DOMElement*>(GetUniqueElementByTagsString_("mesh, generate", found));
  if (found)
  {
    bool found;
    DOMElement* number_of_cells = GetChildByName_(generate, "number_of_cells", found, true);
    nx_ = GetAttributeValueL_(number_of_cells, "nx", true);
    ny_ = GetAttributeValueL_(number_of_cells, "ny", true);
    vector<int> n(dim_);
    n[0] = nx_;
    n[1] = ny_;
    if (dim_ == 3)
    {
      nz_ = GetAttributeValueL_(number_of_cells, "nz", true);
      n[2] = nz_;
    }
    AddToTable(table, MakePPPrefix("amr", "n_cell"), MakePPEntry(n));

    // Stash min/max coordinates for our own porpoises.
    DOMElement* box = GetChildByName_(generate, "box", found, true);
    vector<double> lo_coords = GetAttributeVector_(box, "low_coordinates", true);
    if (lo_coords.size() != dim_)
      ThrowErrorIllformed_("mesh->generate->box", "coordinate array", "low_coordinates");
    vector<double> hi_coords = GetAttributeVector_(box, "high_coordinates", true);
    if (hi_coords.size() != dim_)
      ThrowErrorIllformed_("mesh->generate->box", "coordinate array", "high_coordinates");
    xmin_ = lo_coords[0]; xmax_ = hi_coords[0];
    ymin_ = lo_coords[1]; ymax_ = hi_coords[1];
    if (dim_ == 3)
      zmin_ = lo_coords[2]; zmax_ = hi_coords[2];

    AddToTable(table, MakePPPrefix("geometry", "prob_lo"), MakePPEntry(lo_coords));
    AddToTable(table, MakePPPrefix("geometry", "prob_hi"), MakePPEntry(hi_coords));

    // Periodic boundaries are not supported.
    vector<int> is_periodic(dim_, 0);
    AddToTable(table, MakePPPrefix("geometry", "is_periodic"), MakePPEntry(is_periodic));

    // Coordinate system is 0, which is probably "Cartesian."
    AddToTable(table, MakePPPrefix("geometry", "coord_sys"), MakePPEntry(0));
  }
  else
    ThrowErrorMisschild_("mesh", "generate", "mesh");

  // This one comes for free.
  AddToTable(table, MakePPPrefix("Mesh", "Framework"), MakePPEntry("Structured"));

  // FIXME: Anything else?

  if (!table.empty())
    ParmParse::appendTable(table);
}

void InputConverterS::ParseRegions_()
{
  list<ParmParse::PP_entry> table;
  bool found;
  
printf("greppin regions\n");
  DOMNode* regions = GetUniqueElementByTagsString_("regions", found);
  if (found)
  {
    bool found;

    // box
    vector<DOMNode*> boxes = GetChildren_(regions, "box", found);
    for (size_t i = 0; i < boxes.size(); ++i)
    {
      bool found;
      DOMElement* box = static_cast<DOMElement*>(boxes[i]);
      string region_name = GetAttributeValueS_(box, "name", true);
      vector<double> lo_coords = GetAttributeVector_(box, "low_coordinates", found);
      vector<double> hi_coords = GetAttributeVector_(box, "high_coordinates", found);
      AddToTable(table, MakePPPrefix("geometry", region_name, "lo_coordinate"), 
                                     MakePPEntry(lo_coords));
      AddToTable(table, MakePPPrefix("geometry", region_name, "hi_coordinate"), 
                                     MakePPEntry(hi_coords));

      // Determine the geometry tolerance geometry_eps. dim_, {x,y,z}{min/max}_
      // should all be available because they are computed in ParseMesh(), 
      // which should precede this method.
      double max_size = max(xmax_-xmin_, ymax_-ymin_);
      if (dim_ == 3)
        max_size = max(max_size, zmax_-zmin_);
      double geometry_eps = 1e-6 * max_size; // FIXME: This factor is fixed.
      AddToTable(table, MakePPPrefix("geometry", region_name, "geometry_eps"), 
                                     MakePPEntry(geometry_eps));

      // Is this region a surface?
      string type = "box";
      for (int d = 0; d < dim_; ++d)
      {
        if (std::abs(hi_coords[d] - lo_coords[d]) < geometry_eps)
          type = "surface";
      }
      AddToTable(table, MakePPPrefix("geometry", region_name, "type"), 
                                     MakePPEntry(type));

      // FIXME: As to the "purpose" of this region: Marc, help!
      string purpose = "all";
      AddToTable(table, MakePPPrefix("geometry", region_name, "purpose"), 
                                     MakePPEntry(purpose));
    }

    // FIXME: color functions (what files do we read from?)
    vector<DOMNode*> colors = GetChildren_(regions, "color", found);
    for (size_t i = 0; i < colors.size(); ++i)
    {
    }

    // point
    vector<DOMNode*> points = GetChildren_(regions, "point", found);
    for (size_t i = 0; i < points.size(); ++i)
    {
      bool found;
      DOMElement* point = static_cast<DOMElement*>(points[i]);
      string region_name = GetAttributeValueS_(point, "name", true);
      vector<double> coords = GetAttributeVector_(point, "coordinate", found);
      AddToTable(table, MakePPPrefix("geometry", region_name, "coordinate"), MakePPEntry(coords));
      AddToTable(table, MakePPPrefix("geometry", region_name, "type"), MakePPEntry("point"));
      AddToTable(table, MakePPPrefix("geometry", region_name, "purpose"), MakePPEntry("all"));
    }

    // plane
    vector<DOMNode*> planes = GetChildren_(regions, "plane", found);
    for (size_t i = 0; i < planes.size(); ++i)
    {
      bool found;
      DOMElement* plane = static_cast<DOMElement*>(planes[i]);
      string region_name = GetAttributeValueS_(plane, "name", true);
      vector<double> location = GetAttributeVector_(plane, "location", found);
      vector<double> normal = GetAttributeVector_(plane, "normal", found);

      // FIXME: We need to redo the orientation logic.
      vector<double> lo_coords;
      vector<double> hi_coords;
      
      AddToTable(table, MakePPPrefix("geometry", region_name, "lo_coordinate"), MakePPEntry(lo_coords));
      AddToTable(table, MakePPPrefix("geometry", region_name, "hi_coordinate"), MakePPEntry(hi_coords));
      AddToTable(table, MakePPPrefix("geometry", region_name, "type"), MakePPEntry("surface"));

      // FIXME: purpose here also needs work.
      string purpose;
      AddToTable(table, MakePPPrefix("geometry", region_name, "purpose"), MakePPEntry(purpose));

      // Optional tolerance.
      string tolerance = GetAttributeValueS_(plane, "tolerance", found);
      if (found)
        AddToTable(table, MakePPPrefix("geometry", region_name, "tolerance"), MakePPEntry("all"));

    }

    // region (?!)
    vector<DOMNode*> my_regions = GetChildren_(regions, "region", found);
    for (size_t i = 0; i < my_regions.size(); ++i)
    {
    }

    // Regions only available in 2D
    if (dim_ == 2)
    {
      // polygon 
      vector<DOMNode*> polygons = GetChildren_(regions, "polygon", found);
      for (size_t i = 0; i < polygons.size(); ++i)
      {
        bool found;
        DOMElement* polygon = static_cast<DOMElement*>(polygons[i]);
        string region_name = GetAttributeValueS_(polygon, "name", true);
        int num_points = GetAttributeValueL_(polygon, "num_points", true);
        vector<DOMNode*> points = GetChildren_(regions, "points", found);
        vector<double> v1, v2, v3;
        for (size_t j = 0; j < points.size(); ++j)
        {
          DOMElement* point = static_cast<DOMElement*>(points[i]);
          string coord_string = XMLString::transcode(point->getTextContent());
          vector<double> coords = MakeCoordinates_(coord_string);
          v1.push_back(coords[0]);
          v2.push_back(coords[1]);
          if (dim_ == 3)
            v3.push_back(coords[2]);
        }
        AddToTable(table, MakePPPrefix("geometry", region_name, "v1"), MakePPEntry(v1));
        AddToTable(table, MakePPPrefix("geometry", region_name, "v2"), MakePPEntry(v2));
        if (!v3.empty())
          AddToTable(table, MakePPPrefix("geometry", region_name, "v3"), MakePPEntry(v3));

        AddToTable(table, MakePPPrefix("geometry", region_name, "type"), MakePPEntry("polygon"));
        AddToTable(table, MakePPPrefix("geometry", region_name, "purpose"), MakePPEntry("all"));
      }

      // ellipse 
      vector<DOMNode*> ellipses = GetChildren_(regions, "ellipse", found);
      for (size_t i = 0; i < ellipses.size(); ++i)
      {
        bool found;
        DOMElement* ellipse = static_cast<DOMElement*>(ellipses[i]);
        string region_name = GetAttributeValueS_(ellipse, "name", true);

        string center_string = GetAttributeValueS_(ellipse, "center", true);
        vector<double> center = MakeCoordinates_(center_string);
        AddToTable(table, MakePPPrefix("geometry", region_name, "center"), MakePPEntry(center));

        string radius_string = GetAttributeValueS_(ellipse, "radius", true);
        vector<double> radius = MakeCoordinates_(radius_string);
        AddToTable(table, MakePPPrefix("geometry", region_name, "radius"), MakePPEntry(radius));

        AddToTable(table, MakePPPrefix("geometry", region_name, "type"), MakePPEntry("ellipse"));
        // FIXME: A sense of purpose, please.
        AddToTable(table, MakePPPrefix("geometry", region_name, "purpose"), MakePPEntry("6"));
      }
    }

    // 3D-only region types.
    else if (dim_ == 3)
    {
      // rotated_polygon 
      // FIXME
      vector<DOMNode*> rotated_polygons = GetChildren_(regions, "rotated_polygon", found);
      for (size_t i = 0; i < rotated_polygons.size(); ++i)
      {
      }

      // swept_polygon
      // FIXME
      vector<DOMNode*> swept_polygons = GetChildren_(regions, "swept_polygon", found);
      for (size_t i = 0; i < swept_polygons.size(); ++i)
      {
      }
    }

    // logical
    vector<DOMNode*> logicals = GetChildren_(regions, "logical", found);
    for (size_t i = 0; i < logicals.size(); ++i)
    {
      // FIXME: Not done yet. v2.x spec claims this isn't supported for structured, BTW.
    }
  }

  if (!table.empty())
    ParmParse::appendTable(table);
}

void InputConverterS::ParseGeochemistry_()
{
#if 0
  list<ParmParse::PP_entry> table;
  bool found;

  DOMNode* geochem = GetUniqueElementByTagsString_(doc_, "geochemistry", found);
  if (found)
  {
    DOMElement* rxn_network = GetChildByName_(geochem, "reaction_network", found, true);
    string file = GetAttributeValueS_(rxn_network, "file", found, true);
    string format = GetAttributeValueS_(rxn_network, "format", found, true);
    vector<DOMNode*> constraints = GetChildren_(materials, "constraint", found);

  }

  if (!table.empty())
    ParmParse::appendTable(table);
#endif
}

void InputConverterS::ParseMaterials_()
{
  list<ParmParse::PP_entry> table;
  bool found;
  vector<string> material_names;

  DOMNode* materials = GetUniqueElementByTagsString_("materials", found);
  if (found)
  {
    bool found;
    vector<DOMNode*> mats = GetChildren_(materials, "material", found);
    for (size_t i = 0; i < mats.size(); ++i)
    {
      DOMElement* mat = static_cast<DOMElement*>(mats[i]);
      string mat_name = GetAttributeValueS_(mat, "name");
      material_names.push_back(mat_name);
      bool found;

      // Mechanical properties.
      DOMElement* mech_prop = GetChildByName_(mat, "mechanical_properties", found);
      if (found)
      {
        ParseMechProperty_(mech_prop, mat_name, "porosity", table, true);
        ParseMechProperty_(mech_prop, mat_name, "particle_density", table, false); // FIXME: Should be true for required!
        ParseMechProperty_(mech_prop, mat_name, "specific_storage", table, false); 
        ParseMechProperty_(mech_prop, mat_name, "specific_yield", table, false); 
        ParseMechProperty_(mech_prop, mat_name, "dispersion_tensor", table, false);
        ParseMechProperty_(mech_prop, mat_name, "tortuosity", table, false); 
      }

      // Assigned regions.
      vector<string> assigned_regions = GetChildVectorS_(mat, "assigned_regions", found, true);
      AddToTable(table, MakePPPrefix("rock", mat_name, "regions"), 
                                     MakePPEntry(assigned_regions));

      // Permeability OR hydraulic conductivity.
      bool k_found, K_found;
      DOMElement* permeability = GetChildByName_(mat, "permeability", k_found, false);
      DOMElement* conductivity = GetChildByName_(mat, "hydraulic_conductivity", K_found, false);
      if (!k_found && !K_found)
      {
        Errors::Message msg;
        msg << "Neither permeability nor hydraulic_conductivity was found for material \"" << mat_name << "\".\n";
        msg << "Please correct and try again.\n";
        Exceptions::amanzi_throw(msg);
      }
      else if (k_found && K_found)
      {
        Errors::Message msg;
        msg << "Both permeability AND hydraulic_conductivity were found for material \"" << mat_name << "\".\n";
        msg << "Only one of these is allowed. Please correct and try again.\n";
        Exceptions::amanzi_throw(msg);
      }
      else if (k_found)
      {
        string x = GetAttributeValueS_(permeability, "x", false);
        if (!x.empty())
        {
          string y = GetAttributeValueS_(permeability, "y", true);
          string z = GetAttributeValueS_(permeability, "z", true);
          AddToTable(table, MakePPPrefix("rock", mat_name, "permeability", "horizontal", "vals"),
                     MakePPEntry(x));
          AddToTable(table, MakePPPrefix("rock", mat_name, "permeability", "horizontal1", "vals"),
                     MakePPEntry(y));
          AddToTable(table, MakePPPrefix("rock", mat_name, "permeability", "vertical", "vals"),
                     MakePPEntry(z));

          // FIXME: Are these two guys needed? When?
          AddToTable(table, MakePPPrefix("rock", mat_name, "permeability", "distribution_type"),
                     MakePPEntry("uniform"));
          AddToTable(table, MakePPPrefix("rock", mat_name, "permeability_dist"),
                     MakePPEntry("uniform"));
        }
        else
        {
          string type = GetAttributeValueS_(permeability, "type", true);
          AddToTable(table, MakePPPrefix("rock", mat_name, "permeability", "type"),
                     MakePPEntry(type));
          if (type == "file")
          {
            string filename = GetAttributeValueS_(permeability, "filename", true);
            string attribute = GetAttributeValueS_(permeability, "attribute", true);
            AddToTable(table, MakePPPrefix("rock", mat_name, "permeability", "filename"),
                       MakePPEntry(filename));
            AddToTable(table, MakePPPrefix("rock", mat_name, "permeability", "attribute"),
                                           MakePPEntry(attribute));
          }
          else if (type == "gslib")
          {
            string parameter_file = GetAttributeValueS_(permeability, "parameter_file", true);
            string value = GetAttributeValueS_(permeability, "value", true);
            string data_file = GetAttributeValueS_(permeability, "data_file", true);
            AddToTable(table, MakePPPrefix("rock", mat_name, "permeability", "parameter_file"),
                       MakePPEntry(parameter_file));
            AddToTable(table, MakePPPrefix("rock", mat_name, "permeability", "value"),
                       MakePPEntry(value));
            AddToTable(table, MakePPPrefix("rock", mat_name, "permeability", "data_file"),
                       MakePPEntry(data_file));
          }
          else
            ThrowErrorIllformed_("materials", "type", "permeability");
        }
      }
      else
      {
        string x = GetAttributeValueS_(conductivity, "x", false);
        if (!x.empty())
        {
          string y = GetAttributeValueS_(conductivity, "y", true);
          string z = GetAttributeValueS_(conductivity, "z", true);
          AddToTable(table, MakePPPrefix("rock", mat_name, "hydraulic_conductivity", "horizontal", "vals"),
                     MakePPEntry(x));
          AddToTable(table, MakePPPrefix("rock", mat_name, "hydraulic_conductivity", "horizontal1", "vals"),
                     MakePPEntry(y));
          AddToTable(table, MakePPPrefix("rock", mat_name, "hydraulic_conductivity", "vertical", "vals"),
                     MakePPEntry(z));
        }
        else
        {
          string type = GetAttributeValueS_(permeability, "type", true);
          AddToTable(table, MakePPPrefix("rock", mat_name, "hydraulic_conductivity", "type"),
                     MakePPEntry(type));
          if (type == "gslib")
          {
            string parameter_file = GetAttributeValueS_(conductivity, "parameter_file", true);
            string value = GetAttributeValueS_(conductivity, "value", true);
            string data_file = GetAttributeValueS_(conductivity, "data_file", true);
            AddToTable(table, MakePPPrefix("rock", mat_name, "hydraulic_conductivity", "parameter_file"),
                       MakePPEntry(parameter_file));
            AddToTable(table, MakePPPrefix("rock", mat_name, "hydraulic_conductivity", "value"),
                       MakePPEntry(value));
            AddToTable(table, MakePPPrefix("rock", mat_name, "hydraulic_conductivity", "data_file"),
                       MakePPEntry(data_file));
          }
          else
            ThrowErrorIllformed_("materials", "type", "hydraulic_conductivity");
        }
      }

      // Capillary pressure model.
      DOMElement* cap_pressure = GetChildByName_(mat, "cap_pressure", found, false);
      if (found)
      {
        bool found;
        string model = GetAttributeValueS_(cap_pressure, "model", true);
        if (model == "van_genuchten")
          AddToTable(table, MakePPPrefix("rock", mat_name, "cpl", "type"), MakePPEntry("VanGenuchten"));
        else if (model == "brooks_corey")
          AddToTable(table, MakePPPrefix("rock", mat_name, "cpl", "type"), MakePPEntry("BrooksCorey"));
        else if (model != "none")
          ThrowErrorIllformed_("materials", "type", "cap_pressure");

        if ((model == "van_genuchten") || (model == "brooks_corey"))
        {
          string alpha = GetChildValueS_(cap_pressure, "alpha", found, true);
          string sr = GetChildValueS_(cap_pressure, "sr", found, true);
          string m = GetChildValueS_(cap_pressure, "m", found, true);
          AddToTable(table, MakePPPrefix("rock", mat_name, "cpl", "alpha"), MakePPEntry(alpha));
          AddToTable(table, MakePPPrefix("rock", mat_name, "cpl", "sr"), MakePPEntry(sr));
          AddToTable(table, MakePPPrefix("rock", mat_name, "cpl", "m"), MakePPEntry(m));
          string optional_krel_smoothing_interval = GetChildValueS_(cap_pressure, "optional_krel_smoothing_interval", found, false);
          if (found)
          {
            AddToTable(table, MakePPPrefix("rock", mat_name, "cpl", "Kr_smoothing_max_pcap"),
                       MakePPEntry(optional_krel_smoothing_interval));
          }
        }

        // FIXME: Something about a WRM plot file??
      }
      // FIXME: Is this correct?
      AddToTable(table, MakePPPrefix("rock", mat_name, "cpl_type"), MakePPEntry(0));

      // Relative permeability.
      DOMElement* rel_perm = GetChildByName_(mat, "rel_perm", found, false);
      if (found)
      {
        bool found;
        string model = GetAttributeValueS_(rel_perm, "model", true);
        if (model == "mualem")
          AddToTable(table, MakePPPrefix("rock", mat_name, "Kr_model"), MakePPEntry("mualem"));
        else if (model == "burdine")
          AddToTable(table, MakePPPrefix("rock", mat_name, "Kr_model"), MakePPEntry("burdine"));
        else if (model != "none")
          ThrowErrorIllformed_("materials", "type", "rel_perm");

        if (model == "mualem")
        {
          // We stick in a default "ell" value, since ell doesn't appear 
          // in the v2.x input spec.
          double Kr_ell = 0.5;
          AddToTable(table, MakePPPrefix("rock", mat_name, "Kr_ell"), MakePPEntry(Kr_ell));
        }
        else if (model == "burdine")
        {
          // We stick in a default "ell" value, since ell doesn't appear 
          // in the v2.x input spec.
          double Kr_ell = 2.0;
          AddToTable(table, MakePPPrefix("rock", mat_name, "Kr_ell"), MakePPEntry(Kr_ell));

          // There's also an "exp" parameter.
          string Kr_exp = GetChildValueS_(cap_pressure, "exp", found, true);
          AddToTable(table, MakePPPrefix("rock", mat_name, "Kr_exp"), MakePPEntry(Kr_exp));
        }

      }
      // FIXME: Is this correct?
      AddToTable(table, MakePPPrefix("rock", mat_name, "kr_type"), MakePPEntry(0));

      // Sorption isotherms.
      DOMElement* sorption_isotherms = GetChildByName_(mat, "sorption_isotherms", found, false);
      if (found)
      {
        // Look for solutes.
        bool found;
        vector<DOMNode*> solutes = GetChildren_(sorption_isotherms, "solute", found, true);
        for (size_t i = 0; i < solutes.size(); ++i)
        {
          DOMElement* solute = static_cast<DOMElement*>(solutes[i]);
          string solute_name = GetAttributeValueS_(solute, "name");

          bool found;
          DOMElement* kd_model = GetChildByName_(solute, "kd_model", found, false);
          if (found)
          {
            // Search for kd, b, or n.
            string kd = GetAttributeValueS_(kd_model, "kd", false);
            if (!kd.empty())
            {
              AddToTable(table, MakePPPrefix("rock", mat_name, "sorption_isotherms", solute_name, "Kd"), 
                         MakePPEntry(kd));
            }
            else
            {
              string b = GetAttributeValueS_(kd_model, "b", false);
              if (!b.empty())
              {
                AddToTable(table, MakePPPrefix("rock", mat_name, "sorption_isotherms", solute_name, "Langmuir b"), 
                           MakePPEntry(b));
              }
              else
              {
                string n = GetAttributeValueS_(kd_model, "n", false);
                if (!n.empty())
                {
                  AddToTable(table, MakePPPrefix("rock", mat_name, "sorption_isotherms", solute_name, "Freundlich n"), 
                             MakePPEntry(n));
                }
                else
                  ThrowErrorIllformed_("materials->sorption_isotherms", "kd_model", solute_name);
              }
            }
          }
        }
      }
    }
    AddToTable(table, MakePPPrefix("rock", "rock"), MakePPEntry(material_names));
  }

  if (!table.empty())
    ParmParse::appendTable(table);
}

void InputConverterS::ParseProcessKernels_()
{
  list<ParmParse::PP_entry> table;
  bool found;

  // Flow, transport, and chemistry must all be present.
  DOMElement* flow = static_cast<DOMElement*>(GetUniqueElementByTagsString_("process_kernels, flow", found));
  if (!found)
    ThrowErrorMisschild_("process_kernels", "flow", "process_kernels");
  DOMElement* transport = static_cast<DOMElement*>(GetUniqueElementByTagsString_("process_kernels, transport", found));
  if (!found)
    ThrowErrorMisschild_("process_kernels", "transport", "process_kernels");
  DOMElement* chemistry = static_cast<DOMElement*>(GetUniqueElementByTagsString_("process_kernels, chemistry", found));
  if (!found)
    ThrowErrorMisschild_("process_kernels", "chemistry", "process_kernels");

  // Flow model.
  string flow_state = GetAttributeValueS_(flow, "state");
  if (flow_state == "on")
  {
    string flow_model = GetAttributeValueS_(flow, "model");
    AddToTable(table, MakePPPrefix("prob", "model_name"), MakePPEntry(flow_model));
  }
  else
    AddToTable(table, MakePPPrefix("prob", "model_name"), MakePPEntry("steady-saturated"));
  AddToTable(table, MakePPPrefix("prob", "have_capillary"), MakePPEntry(0));
  AddToTable(table, MakePPPrefix("prob", "cfl"), MakePPEntry(-1));
    
  // Transport model.
  string transport_state = GetAttributeValueS_(transport, "state");
  if (transport_state == "on")
  {
    AddToTable(table, MakePPPrefix("prob", "do_tracer_advection"), MakePPEntry(1));
  }
  else
  {
    AddToTable(table, MakePPPrefix("prob", "do_tracer_advection"), MakePPEntry(0));
  }
  // FIXME: This is a hack for now. We need to inspect the dispersivity tensor
  // FIXME: to determine whether to do tracer diffusion, but for now we assume
  // FIXME: that diffusion settings use advection settings.
  AddToTable(table, MakePPPrefix("prob", "do_tracer_diffusion"), MakePPEntry((transport_state == "on")));
  // FIXME: What else here?

  // Chemistry model.
  string chemistry_state = GetAttributeValueS_(chemistry, "state");
  if (chemistry_state == "on")
  {
    string chemistry_engine = GetAttributeValueS_(chemistry, "engine");
    if (chemistry_engine == "amanzi")
    {
      AddToTable(table, MakePPPrefix("prob", "chemistry_model"), MakePPEntry("Amanzi"));
      // FIXME: What else here?
    }
    else if (chemistry_engine != "none") // Alquimia!
    {
      AddToTable(table, MakePPPrefix("prob", "chemistry_model"), MakePPEntry("Alquimia"));
      if (chemistry_engine == "pflotran")
        AddToTable(table, MakePPPrefix("Chemistry", "Engine"), MakePPEntry("PFloTran"));
      // FIXME: What else here?
    }
    else
    {
      AddToTable(table, MakePPPrefix("prob", "chemistry_model"), MakePPEntry("Off"));
    }

    // FIXME: This parameter isn't really supported yet, since it only has 
    // FIXME: one meaningful value.
    string chemistry_model = GetAttributeValueS_(chemistry, "process_model");
  }

  if (!table.empty())
    ParmParse::appendTable(table);
}

void InputConverterS::ParsePhases_()
{
  list<ParmParse::PP_entry> table;
  ParmParse::appendTable(table);
}

void InputConverterS::ParseInitialConditions_()
{
  list<ParmParse::PP_entry> table;
  ParmParse::appendTable(table);
}

void InputConverterS::ParseBoundaryConditions_()
{
  list<ParmParse::PP_entry> table;
  bool found;
  DOMNode* boundary_conditions = GetUniqueElementByTagsString_("boundary_conditions", found);
  if (found)
  {
    bool found;
    vector<DOMNode*> bcs = GetChildren_(boundary_conditions, "boundary_condition", found);
    if (found)
    {
      for (size_t i = 0; i < bcs.size(); ++i)
      {
        DOMElement* bc = static_cast<DOMElement*>(bcs[i]);
        string bc_name = GetAttributeValueS_(bc, "name");
        vector<string> times;
      }
    }
  }
  if (!table.empty())
    ParmParse::appendTable(table);
}

void InputConverterS::ParseOutput_()
{
  list<ParmParse::PP_entry> table;
  bool found;

  // Visualization files.
  DOMNode* vis = GetUniqueElementByTagsString_("output, vis", found);
  if (found)
  {
    bool found;
    string base_filename = GetChildValueS_(vis, "base_filename", found, true);
    string num_digits = GetChildValueS_(vis, "num_digits", found, true);
    vector<string> cycle_macros = GetChildVectorS_(vis, "cycle_macros", found, false);
    vector<string> time_macros = GetChildVectorS_(vis, "time_macros", found, false);

    AddToTable(table, MakePPPrefix("amr", "plot_file"), MakePPEntry(base_filename));
    AddToTable(table, MakePPPrefix("amr", "plot_file_digits"), MakePPEntry(num_digits));
    AddToTable(table, MakePPPrefix("amr", "viz_cycle_macros"), MakePPEntry(cycle_macros));
    AddToTable(table, MakePPPrefix("amr", "viz_time_macros"), MakePPEntry(time_macros));
  }

  // Checkpoint files.
  DOMNode* checkpoint = GetUniqueElementByTagsString_("output, checkpoint", found);
  if (found)
  {
    bool found;
    string base_filename = GetChildValueS_(checkpoint, "base_filename", found, true);
    string num_digits = GetChildValueS_(checkpoint, "num_digits", found, true);
    vector<string> cycle_macros = GetChildVectorS_(checkpoint, "cycle_macros", found, true);
    for (size_t i = 0; i < cycle_macros.size(); ++i)
      cycle_macros[i] = MangleString(cycle_macros[i]);

    AddToTable(table, MakePPPrefix("amr", "check_file"), MakePPEntry(base_filename));
    AddToTable(table, MakePPPrefix("amr", "chk_file_digits"), MakePPEntry(num_digits));
    AddToTable(table, MakePPPrefix("amr", "chk_cycle_macros"), MakePPEntry(cycle_macros));
  }

  // Observations.
  DOMNode* observations = GetUniqueElementByTagsString_("output, observations", found);
  if (found)
  {
    bool found;
    string filename = GetChildValueS_(checkpoint, "filename", found, true);
//    string num_digits = GetChildValueS_(checkpoint, "liquid_phase", found, true);

    // FIXME
  }

  // Walkabouts. 
  DOMNode* walkabout = GetUniqueElementByTagsString_("output, walkabout", found);
  if (found)
  {
    bool found;
    string base_filename = GetChildValueS_(walkabout, "base_filename", found, true);
    string num_digits = GetChildValueS_(walkabout, "num_digits", found, true);
    vector<string> cycle_macros = GetChildVectorS_(walkabout, "cycle_macros", found, true);

// FIXME
//    AddToTable(table, MakePPPrefix("amr", "check_file"), MakePPEntry(base_filename));
//    AddToTable(table, MakePPPrefix("amr", "chk_file_digits"), MakePPEntry(num_digits));
//    AddToTable(table, MakePPPrefix("amr", "chk_cycle_macros"), MakePPEntry(cycle_macros));
  }

  if (!table.empty())
    ParmParse::appendTable(table);
}

void InputConverterS::ParseMisc_()
{
  // FIXME: Not yet supported.
}

void InputConverterS::Translate() 
{
  ParseUnits_();
  ParseDefinitions_();
  ParseExecutionControls_();
  ParseNumericalControls_();
  ParseMesh_();
  ParseRegions_();
  ParseGeochemistry_();
  ParseMaterials_();
  ParseProcessKernels_();
  ParsePhases_();
  ParseInitialConditions_();
  ParseBoundaryConditions_();
  ParseOutput_();
  ParseMisc_();
}

}  // namespace AmanziInput
}  // namespace Amanzi

