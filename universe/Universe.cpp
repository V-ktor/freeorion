#include "Universe.h"

#include "../util/AppInterface.h"
#include "Fleet.h"
#include "Planet.h"
#include "Predicates.h"
#include "../util/Random.h"
#include "../Empire/ServerEmpireManager.h"
#include "Ship.h"
#include "System.h"
#include "UniverseObject.h"
#include "XMLDoc.h"

#ifdef FREEORION_BUILD_HUMAN
#include "../client/human/HumanClientApp.h"
#endif

#ifdef FREEORION_BUILD_SERVER
#include "../server/ServerApp.h"
#endif

#include <stdexcept>
#include <cmath>

namespace {
    // for UniverseObject factory
    UniverseObject* NewFleet(const GG::XMLElement& elem)  {return new Fleet(elem);}
    UniverseObject* NewPlanet(const GG::XMLElement& elem) {return new Planet(elem);}
    UniverseObject* NewShip(const GG::XMLElement& elem)   {return new Ship(elem);}
    UniverseObject* NewSystem(const GG::XMLElement& elem) {return new System(elem);}

    const double  MIN_SYSTEM_SEPARATION = 30.0; // in universe units [0.0, s_universe_width]
    const double  MIN_HOME_SYSTEM_SEPARATION = 200.0; // in universe units [0.0, s_universe_width]
    const int     ADJACENCY_BOXES = 25;
    const double  PI = 3.141592;

    // "only" defined for 1 <= n <= 3999, as we can't
    // display the symbol for 5000
    std::string RomanNumber(unsigned int n)
    {
	static const char N[] = "IVXLCDM??";
	std::string retval;
	int e = 3;
	int mod = 1000;
	for (; 0 <= e; e--, mod /= 10) {
	    unsigned int m = (n / mod) % 10;
	    if (m % 5 == 4) {
		retval += N[e << 1];
		++m;
		if (m == 10) {
		    retval += N[(e << 1) + 2];
		    continue;
		}
	    }
	    if (m >= 5) {
		retval += N[(e << 1) + 1];
		m -= 5;
	    }
	    while (m) {
		retval += N[e << 1];
		--m;
	    }
	}
	return retval;
    }

    void LoadPlanetNames(std::list<std::string>& names)
    {
	std::ifstream ifs("default/starnames.txt");
	while (ifs) {
	    std::string latest_name;
	    std::getline(ifs, latest_name);
	    if (latest_name != "")
		names.push_back(latest_name);
	}
    }

    void SpiralGalaxyCalcPositions(std::vector<std::pair<double,double> > &positions,unsigned int arms,unsigned int stars,double width,double height)
    {
	double arm_offset     = RandDouble(0.0,2.0*PI);
	double arm_angle      = 2.0*PI / arms;
	double arm_spread     = 0.3 * PI / arms;
	double arm_length     = 1.5 * PI;
	double center         = 0.25;
	double x,y;

	unsigned int i,j,attemps;

	GaussianDistType  random_gaussian = GaussianDist(0.0,arm_spread);
	SmallIntDistType  random_arm      = SmallIntDist(0  ,arms);
	DoubleDistType    random_degree   = DoubleDist  (0.0,2.0*PI);
	DoubleDistType    random_radius   = DoubleDist  (0.0,  1.0);
    
	for (i=0,attemps=0; i < stars && attemps<100; i++,attemps++ )
	{
	    double radius = random_radius();

	    if(radius<center)
	    {
		double angle = random_degree();
		x = radius * cos( arm_offset + angle );
		y = radius * sin( arm_offset + angle );
	    }
	    else
	    {
		double arm    = (double)random_arm() * arm_angle;
		double angle  = random_gaussian();

		x = radius * cos( arm_offset + arm + angle + radius * arm_length );
		y = radius * sin( arm_offset + arm + angle + radius * arm_length );
	    }

	    x = (x+1)*width/2.0;
	    y = (y+1)*height/2.0;

	    if(x<0 || width<=x || y<0 || height<=y)
		continue;

	    // ensure all system have a min separation to each other (search isn't opimized, not worth the effort)
	    for(j=0;j<positions.size();j++)
		if((positions[j].first - x)*(positions[j].first - x)+ (positions[j].second - y)*(positions[j].second - y)
		   < MIN_SYSTEM_SEPARATION*MIN_SYSTEM_SEPARATION)
		    break;
	    if(j<positions.size())
	    {
		i--;
		continue;
	    }

	    attemps=0;
	    positions.push_back(std::pair<double,double>(x,y));
	}
    }

    void EllipticalGalaxyCalcPositions(std::vector<std::pair<double,double> > &positions,unsigned int stars,double width,double height)
    {
	const double ellipse_width_vs_height = RandDouble(0.4, 0.6);
	const double rotation = RandDouble(0.0, PI),
	    rotation_sin = std::sin(rotation),
	    rotation_cos = std::cos(rotation);
	const unsigned int fail_attempts = 100;
	const double gap_constant = .95;
	const double gap_size = 1.0 - gap_constant * gap_constant * gap_constant;

	// Random number generators.
	DoubleDistType radius_dist = DoubleDist(0.0, gap_constant);
	DoubleDistType random_angle  = DoubleDist(0.0, 2.0 * PI);

	// Used to give up when failing to place a star too often.
	unsigned int attempts = 0;

	// For each attempt to place a star...
	for (unsigned int i = 0; i < stars && attempts < fail_attempts; ++i, ++attempts){
	    double radius = radius_dist();
	    // Adjust for bigger density near center and create gap.
	    radius = radius * radius * radius + gap_size;
	    double angle  = random_angle();

	    // Rotate for individual angle and apply elliptical shape.
	    double x1 = radius * std::cos(angle);
	    double y1 = radius * std::sin(angle) * ellipse_width_vs_height;

	    // Rotate for ellipse angle.
	    double x = x1 * rotation_cos - y1 * rotation_sin;
	    double y = x1 * rotation_sin + y1 * rotation_cos;

	    // Move from [-1.0, 1.0] universe coordinates.
	    x = (x + 1.0) * width / 2.0;
	    y = (y + 1.0) * height / 2.0;

	    // Discard stars that are outside boundaries (due to possible rounding errors).
	    if (x < 0 || x >= width || y < 0 || y >= height)
	      continue;

	    // See if new star is too close to any existing star.
	    unsigned int j = 0;
	    for(; j < positions.size(); ++j){
        if((positions[j].first - x) * (positions[j].first - x) + 
           (positions[j].second - y) * (positions[j].second - y)
          < MIN_SYSTEM_SEPARATION * MIN_SYSTEM_SEPARATION)
          break;
      }
	    // If so, we try again.
	    if(j < positions.size()){
		--i;
		continue;
	    }

	    // Note that attempts is reset for every star.
	    attempts = 0;

	    // Add the new star location.
	    positions.push_back(std::pair<double,double>(x, y));
	}
    }

    void ClusterGalaxyCalcPositions(std::vector<std::pair<double,double> > &positions,unsigned int clusters,unsigned int stars,double width,double height)
    {
	//propability of systems which don't belong to a cluster
	const double system_noise = 0.15;
	double ellipse_width_vs_height = RandDouble(0.2,0.5);
	// first innermost pair hold cluster position, second innermost pair stores help values for cluster rotation (sin,cos)
	std::vector<std::pair<std::pair<double,double>,std::pair<double,double> > > clusters_position;
	unsigned int i,j,attemps;

	DoubleDistType    random_zero_to_one = DoubleDist  (0.0,  1.0);
	DoubleDistType    random_angle  = DoubleDist  (0.0,2.0*PI);

	for(i=0,attemps=0;i<clusters && attemps<100;i++,attemps++)
	{
	    // prevent cluster position near borders (and on border)
	    double x=((random_zero_to_one()*2.0-1.0) /(clusters+1.0))*clusters,
		y=((random_zero_to_one()*2.0-1.0) /(clusters+1.0))*clusters;

      
	    // ensure all clusters have a min separation to each other (search isn't opimized, not worth the effort)
	    for(j=0;j<clusters_position.size();j++)
		if((clusters_position[j].first.first - x)*(clusters_position[j].first.first - x)+ (clusters_position[j].first.second - y)*(clusters_position[j].first.second - y)
		   < (2.0/clusters))
		    break;
	    if(j<clusters_position.size())
	    {
		i--;
		continue;
	    }

	    attemps=0;
	    double rotation = RandDouble(0.0,PI);
	    clusters_position.push_back(std::pair<std::pair<double,double>,std::pair<double,double> >(std::pair<double,double>(x,y),std::pair<double,double>(sin(rotation),cos(rotation))));
	}

	for (i=0,attemps=0; i < stars && attemps<100; i++,attemps++ )
	{
	    double x,y;
	    if(random_zero_to_one()<system_noise)
	    {
		x = random_zero_to_one() * 2.0 - 1.0;
		y = random_zero_to_one() * 2.0 - 1.0;
	    }
	    else
	    {
		short  cluster = i%clusters_position.size();
		double radius  = random_zero_to_one();
		double angle   = random_angle();
		double x1,y1;

		x1 = radius * cos(angle);
		y1 = radius * sin(angle)*ellipse_width_vs_height;

		x = x1*clusters_position[cluster].second.second + y1*clusters_position[cluster].second.first;
		y =-x1*clusters_position[cluster].second.first  + y1*clusters_position[cluster].second.second;
        
		x = x/sqrt((double)clusters) + clusters_position[cluster].first.first;
		y = y/sqrt((double)clusters) + clusters_position[cluster].first.second;
	    }
	    x = (x+1)*width /2.0;
	    y = (y+1)*height/2.0;

	    if(x<0 || width<=x || y<0 || height<=y)
		continue;

	    // ensure all system have a min separation to each other (search isn't opimized, not worth the effort)
	    for(j=0;j<positions.size();j++)
		if((positions[j].first - x)*(positions[j].first - x)+ (positions[j].second - y)*(positions[j].second - y)
		   < MIN_SYSTEM_SEPARATION*MIN_SYSTEM_SEPARATION)
		    break;
	    if(j<positions.size())
	    {
		i--;
		continue;
	    }

	    attemps=0;
	    positions.push_back(std::pair<double,double>(x,y));
	}
    }

    void GenerateStarField(Universe &universe,const std::vector<std::pair<double,double> > &positions, Universe::AdjacencyGrid& adjacency_grid, double adjacency_box_size)
    {
	std::list<std::string> star_names;
	LoadPlanetNames(star_names);

	SmallIntDistType star_type_gen = SmallIntDist(0, System::NUM_STARTYPES - 1);

	// generate star field
	for (unsigned int star_cnt = 0; star_cnt < positions.size(); ++star_cnt) {
	    // generate new star
	    int star_name_idx = RandSmallInt(0, static_cast<int>(star_names.size()) - 1);
	    std::list<std::string>::iterator it = star_names.begin();
	    std::advance(it, star_name_idx);
	    std::string star_name(*it);
	    star_names.erase(it);
	    System::StarType star_type = System::StarType(star_type_gen());
	    int num_orbits = 5;
	    double orbits_rand = RandZeroToOne();
	    if      (orbits_rand < 0.05) num_orbits = 0;
	    else if (orbits_rand < 0.25) num_orbits = 1;
	    else if (orbits_rand < 0.55) num_orbits = 2;
	    else if (orbits_rand < 0.85) num_orbits = 3;
	    else if (orbits_rand < 0.95) num_orbits = 4;
	    // above 0.95 stays at the default 5

	    System* system = new System(star_type, num_orbits, star_name, positions[star_cnt].first,positions[star_cnt].second);

	    int new_sys_id = universe.Insert(system);
	    if (new_sys_id == UniverseObject::INVALID_OBJECT_ID) {
		throw std::runtime_error("Universe::GenerateIrregularGalaxy : Attemp to insert system " + 
					 star_name + " into the object map failed.");
	    }

	    adjacency_grid[static_cast<int>(system->X() / adjacency_box_size)]
		[static_cast<int>(system->Y() / adjacency_box_size)].insert(std::make_pair(system->X(), system->Y()));
	}
    }
}

// static(s)
double Universe::s_universe_width = 1000.0;

Universe::Universe()
{
    m_factory.AddGenerator("Fleet", &NewFleet);
    m_factory.AddGenerator("Planet", &NewPlanet);
    m_factory.AddGenerator("Ship", &NewShip);
    m_factory.AddGenerator("System", &NewSystem);
    m_last_allocated_id = -1;
}

Universe::Universe(const GG::XMLElement& elem)
{
    m_factory.AddGenerator("Fleet", &NewFleet);
    m_factory.AddGenerator("Planet", &NewPlanet);
    m_factory.AddGenerator("Ship", &NewShip);
    m_factory.AddGenerator("System", &NewSystem);

    SetUniverse(elem);
}

Universe::~Universe()
{
    for (ObjectMap::iterator it = m_objects.begin(); it != m_objects.end(); ++it)
        delete it->second;
}

void Universe::SetUniverse(const GG::XMLElement& elem)
{
    using GG::XMLElement;

    if (elem.Tag() != "Universe")
        throw std::invalid_argument("Attempted to construct a Universe from an XMLElement that had a tag other than \"Universe\"");

    // wipe out anything present in the object map
    for (ObjectMap::iterator itr= m_objects.begin(); itr != m_objects.end(); ++itr)
        delete itr->second;
    m_objects.clear(); 

    s_universe_width = boost::lexical_cast<double>(elem.Child("s_universe_width").Text());

    for (int i = 0; i < elem.Child("m_objects").NumChildren(); ++i) {
	if (UniverseObject* obj = m_factory.GenerateObject(elem.Child("m_objects").Child(i))) {
            m_objects[obj->ID()] = obj;
	}
    }

    m_last_allocated_id = boost::lexical_cast<int>(elem.Child("m_last_allocated_id").Text());
}

const UniverseObject* Universe::Object(int id) const
{
    const_iterator it = m_objects.find(id);
    return (it != m_objects.end() ? it->second : 0);
}

UniverseObject* Universe::Object(int id)
{
    iterator it = m_objects.find(id);
    return (it != m_objects.end() ? it->second : 0);
}

GG::XMLElement Universe::XMLEncode() const
{
    GG::XMLElement retval("Universe");
    GG::XMLElement temp("m_objects");

    retval.AppendChild(GG::XMLElement("s_universe_width", boost::lexical_cast<std::string>(s_universe_width)));

    for (const_iterator it = begin(); it != end(); ++it)
        temp.AppendChild(it->second->XMLEncode());
    retval.AppendChild(temp);

    retval.AppendChild(GG::XMLElement("m_last_allocated_id", boost::lexical_cast<std::string>(m_last_allocated_id)));

    return retval;
}

GG::XMLElement Universe::XMLEncode(int empire_id) const
{
   using GG::XMLElement;

   XMLElement element("Universe");
   XMLElement object_map("m_objects");

   element.AppendChild(XMLElement("s_universe_width", boost::lexical_cast<std::string>(s_universe_width)));

   for(const_iterator itr = begin(); itr != end(); ++itr)
   {
      // determine visibility
      UniverseObject::Visibility vis = (*itr).second->Visible(empire_id);
      XMLElement univ_object("UniverseObject");
      XMLElement univ_element;

      if (vis == UniverseObject::FULL_VISIBILITY)
      {
	  univ_element = (*itr).second->XMLEncode( );
      }
      else if (vis == UniverseObject::PARTIAL_VISIBILITY)
      {
          univ_element = (*itr).second->XMLEncode( empire_id );
      }

      //univ_element.AppendChild( univ_object );
      object_map.AppendChild( univ_element );

      // for NO_VISIBILITY no element is added
   }
   element.AppendChild(object_map);

   element.AppendChild(GG::XMLElement("m_last_allocated_id", boost::lexical_cast<std::string>(m_last_allocated_id)));

   return element;
}

/********************************************************************
 Methods for universe creation and object creation -- merged in from
ServerUniverse
**********************************************************************/

void Universe::CreateUniverse(Shape shape, int size, int players, int ai_players)
{
    // wipe out anything present in the object map
    for (ObjectMap::iterator itr = m_objects.begin(); itr != m_objects.end(); ++itr)
        delete itr->second;
    m_objects.clear();

    m_last_allocated_id = -1;

    Logger().debugStream() << "Creating universe with " << size << " stars and " << players << " players.";

    std::vector<int> homeworlds;

    // a grid of ADJACENCY_BOXES x ADJACENCY_BOXES boxes to hold the positions of the systems as they are generated, 
    // in order to ensure that they get spaced out properly
    AdjacencyGrid adjacency_grid(ADJACENCY_BOXES, std::vector<std::set<std::pair<double, double> > >(ADJACENCY_BOXES));

    s_universe_width = size * 1000.0 / 150.0; // chosen so that the width of a medium galaxy is 1000.0

    // generate the stars
    switch (shape) {
    case SPIRAL_2:
        GenerateSpiralGalaxy(2, size, adjacency_grid);
        break;
    case SPIRAL_3:
        GenerateSpiralGalaxy(3, size, adjacency_grid);
        break;
    case SPIRAL_4:
        GenerateSpiralGalaxy(4, size, adjacency_grid);
        break;
    case CLUSTER:
        GenerateClusterGalaxy(size, adjacency_grid);
        break;
    case ELLIPTICAL:
        GenerateEllipticalGalaxy(size, adjacency_grid);
        break;
    case IRREGULAR:
        GenerateIrregularGalaxy(size, adjacency_grid);
        break;
    default:
        Logger().errorStream() << "Universe::Universe : Unknown galaxy shape: "<< shape << ".  Using IRREGULAR as default.";
        GenerateIrregularGalaxy(size, adjacency_grid);
    }

    PopulateSystems();
    GenerateHomeworlds(players + ai_players, homeworlds, adjacency_grid);
    GenerateEmpires(players + ai_players, homeworlds);
}

void Universe::CreateUniverse(const std::string& map_file, int size, int players, int ai_players)
{
    // intialize the ID counter
    m_last_allocated_id = -1;   

    // TODO
}

int Universe::Insert(UniverseObject* obj)
{
   int retval = UniverseObject::INVALID_OBJECT_ID;
   if (obj) {
       if (m_last_allocated_id + 1 < UniverseObject::MAX_ID) {
           m_objects[++m_last_allocated_id] = obj;
           obj->SetID(m_last_allocated_id);
           retval = m_last_allocated_id;
       } else { // we'll probably never execute this branch, considering how many IDs are available
           // find a hole in the assigned IDs in which to place the object
           int last_id_seen = UniverseObject::INVALID_OBJECT_ID;
           for (ObjectMap::iterator it = m_objects.begin(); it != m_objects.end(); ++it) {
               if (1 < it->first - last_id_seen) {
                   m_objects[last_id_seen + 1] = obj;
                   obj->SetID(last_id_seen + 1);
                   retval = last_id_seen + 1;
                   break;
               }
           }
       }
   }

   return retval;
}

bool Universe::InsertID(UniverseObject* obj, int id )
{
   bool retval = false;

   if (obj) {
       if ( id < UniverseObject::MAX_ID) {
           m_objects[id] = obj;
           obj->SetID(id);
           retval = true;
       }       
   }
   return retval;
}

UniverseObject* Universe::Remove(int id)
{
   UniverseObject* retval = 0;
   iterator it = m_objects.find(id);
   if (it != m_objects.end()) {
      retval = it->second;
      if (System* sys = retval->GetSystem())
          sys->Remove(id);
      m_objects.erase(id);
   }
   return retval;
}

bool Universe::Delete(int id)
{
   UniverseObject* obj = Remove(id);
   delete obj;
   return obj;
}

void Universe::MovementPhase(std::vector<SitRepEntry>& sit_reps)
{
   // TODO
}

void Universe::PopGrowthProductionResearch(std::vector<SitRepEntry>& sit_reps)
{
   // TODO
}

void Universe::GenerateSpiralGalaxy(int arms, int stars, AdjacencyGrid& adjacency_grid)
{
    std::vector<std::pair<double,double> > positions;

    SpiralGalaxyCalcPositions(positions,arms,stars,s_universe_width,s_universe_width);
    GenerateStarField(*this,positions,adjacency_grid,s_universe_width/ADJACENCY_BOXES);
}

void Universe::GenerateEllipticalGalaxy(int stars, AdjacencyGrid& adjacency_grid)
{
    std::vector<std::pair<double,double> > positions;
    
    EllipticalGalaxyCalcPositions(positions,stars,s_universe_width,s_universe_width);
    GenerateStarField(*this,positions,adjacency_grid,s_universe_width/ADJACENCY_BOXES);
}

void Universe::GenerateClusterGalaxy(int stars, AdjacencyGrid& adjacency_grid)
{
    std::vector<std::pair<double,double> > positions;
    
    ClusterGalaxyCalcPositions(positions,5,stars,s_universe_width,s_universe_width);
    GenerateStarField(*this,positions,adjacency_grid,s_universe_width/ADJACENCY_BOXES);
}

void Universe::GenerateIrregularGalaxy(int stars, AdjacencyGrid& adjacency_grid)
{
    std::list<std::string> star_names;
    LoadPlanetNames(star_names);

    SmallIntDistType star_type_gen = SmallIntDist(0, System::NUM_STARTYPES - 1);

    // generate star field
    for (int star_cnt = 0; star_cnt < stars; ++star_cnt) {
        // generate new star
        int star_name_idx = RandSmallInt(0, static_cast<int>(star_names.size()) - 1);
        std::list<std::string>::iterator it = star_names.begin();
        std::advance(it, star_name_idx);
        std::string star_name(*it);
        star_names.erase(it);
        System::StarType star_type = System::StarType(star_type_gen());
        int num_orbits = 5;
        double orbits_rand = RandZeroToOne();
        if (orbits_rand < 0.05) {
            num_orbits = 0;
        } else if (orbits_rand < 0.25) {
            num_orbits = 1;
        } else if (orbits_rand < 0.55) {
            num_orbits = 2;
        } else if (orbits_rand < 0.85) {
            num_orbits = 3;
        } else if (orbits_rand < 0.95) {
            num_orbits = 4;
        }
        // above 0.95 stays at the default 5

        bool placed = false;
        int attempts = 0;

        System* system = new System(star_type, num_orbits, star_name, (s_universe_width-.1) * RandZeroToOne(), (s_universe_width-.1) * RandZeroToOne());

        int new_sys_id = Insert(system);
        if (new_sys_id == UniverseObject::INVALID_OBJECT_ID) {
            throw std::runtime_error("Universe::GenerateIrregularGalaxy : Attemp to insert system " + 
                                     star_name + " into the object map failed.");
        }

	const double ADJACENCY_BOX_SIZE = UniverseWidth() / ADJACENCY_BOXES;

        while (!placed && attempts++ < 10) {
            // look in the box into which this system falls, and those boxes immediately around that box; 
            // make sure this system doesn't get slapped down too close to or on top of any systems
            std::pair<unsigned int, unsigned int> grid_box(static_cast<unsigned int>(system->X() / ADJACENCY_BOX_SIZE), 
                                                           static_cast<unsigned int>(system->Y() / ADJACENCY_BOX_SIZE));
            std::set<std::pair<double, double> > neighbors(adjacency_grid[grid_box.first][grid_box.second]);
            if (0 < grid_box.first) {
                if (0 < grid_box.second) {
                    std::set<std::pair<double, double> >& grid_square = adjacency_grid[grid_box.first - 1][grid_box.second - 1];
                    neighbors.insert(grid_square.begin(), grid_square.end());
                }
                std::set<std::pair<double, double> >& grid_square = adjacency_grid[grid_box.first - 1][grid_box.second];
                neighbors.insert(grid_square.begin(), grid_square.end());
                if (grid_box.second < adjacency_grid[grid_box.first].size() - 1) {
                    std::set<std::pair<double, double> >& grid_square = adjacency_grid[grid_box.first - 1][grid_box.second + 1];
                    neighbors.insert(grid_square.begin(), grid_square.end());
                }
            }
            if (0 < grid_box.second) {
                std::set<std::pair<double, double> >& grid_square = adjacency_grid[grid_box.first][grid_box.second - 1];
                neighbors.insert(grid_square.begin(), grid_square.end());
            }
            if (grid_box.second < adjacency_grid[grid_box.first].size() - 1) {
                std::set<std::pair<double, double> >& grid_square = adjacency_grid[grid_box.first][grid_box.second + 1];
                neighbors.insert(grid_square.begin(), grid_square.end());
            }

            if (grid_box.first < adjacency_grid.size() - 1) {
                if (0 < grid_box.second) {
                    std::set<std::pair<double, double> >& grid_square = adjacency_grid[grid_box.first + 1][grid_box.second - 1];
                    neighbors.insert(grid_square.begin(), grid_square.end());
                }
                std::set<std::pair<double, double> >& grid_square = adjacency_grid[grid_box.first + 1][grid_box.second];
                neighbors.insert(grid_square.begin(), grid_square.end());
                if (grid_box.second < adjacency_grid[grid_box.first].size() - 1) {
                    std::set<std::pair<double, double> >& grid_square = adjacency_grid[grid_box.first + 1][grid_box.second + 1];
                    neighbors.insert(grid_square.begin(), grid_square.end());
                }
            }

            std::pair<double, double> neighbor_centroid;
            bool too_close = false;
            double system_x = system->X();
            double system_y = system->Y();
            for (std::set<std::pair<double, double> >::iterator it = neighbors.begin(); it != neighbors.end(); ++it) {
                double x_dist = system_x - it->first;
                double y_dist = system_y - it->second;
                if (x_dist * x_dist + y_dist * y_dist < MIN_SYSTEM_SEPARATION * MIN_SYSTEM_SEPARATION)
                    too_close = true;
                neighbor_centroid.first += it->first;
                neighbor_centroid.second += it->second;
            }

            if (too_close) {
                neighbor_centroid.first /= neighbors.size();
                neighbor_centroid.second /= neighbors.size();
                double move_x = system_x - neighbor_centroid.first;
                double move_y = system_y - neighbor_centroid.second;
                move_x = std::max(0.0, std::min(move_x + system_x, s_universe_width-.1));
                move_y = std::max(0.0, std::min(move_y + system_y, s_universe_width-.1));
                system->MoveTo(move_x, move_y);
            } else {
                placed = true;
            }

        }

        // if placed is false system couldn't be placed anywhere without getting to close to another system
        if (placed)
            adjacency_grid[static_cast<int>(system->X() / ADJACENCY_BOX_SIZE)]
                          [static_cast<int>(system->Y() / ADJACENCY_BOX_SIZE)].insert(std::make_pair(system->X(), system->Y()));
        else
            Delete(system->ID());
    }
}

void Universe::GenerateHomeworlds(int players, std::vector<int>& homeworlds, AdjacencyGrid& adjacency_grid)
{
    homeworlds.clear();

    ObjectVec sys_vec = FindObjects(IsSystem);

    for (int i = 0; i < players; ++i) {
        if (sys_vec.empty())
            throw std::runtime_error("Attempted to generate homeworlds in an empty galaxy.");
        
        int system_index;
        System* system;

        // make sure it has planets and it's not too close to the other homeworlds
        bool too_close = true;
        int attempts = 0;
        do {
            too_close = false;
            system_index = RandSmallInt(0, static_cast<int>(sys_vec.size()) - 1);
            system = dynamic_cast<System*>(sys_vec[system_index]);
            for (unsigned int j = 0; j < homeworlds.size(); ++j) {
                System* existing_system = Object(homeworlds[j])->GetSystem();
                double x_dist = existing_system->X() - system->X();
                double y_dist = existing_system->Y() - system->Y();
                if (x_dist * x_dist + y_dist * y_dist < MIN_HOME_SYSTEM_SEPARATION * MIN_HOME_SYSTEM_SEPARATION) {
                    too_close = true;
                    break;
                }
            }
        } while ((!system->Orbits() || too_close) && ++attempts < 50);

        sys_vec.erase(sys_vec.begin() + system_index);

        // find a place to put the homeworld, and replace whatever planet is there already
        int planet_id,home_orbit;
        std::string planet_name;

        if(system->Orbits()>0) {
            home_orbit = RandSmallInt(0, system->Orbits() - 1);
            System::ObjectIDVec planet_IDs = system->FindObjectIDsInOrbit(home_orbit, IsPlanet);
            planet_name = Object(planet_IDs.back())->Name();
            Delete(planet_IDs.back());
        }
        else {
            home_orbit = 0;
            planet_name = system->Name() + " " + RomanNumber(home_orbit+1);
        }

        Planet* planet = new Planet(Planet::TERRAN, Planet::SZ_MEDIUM);
        planet_id = Insert(planet);
        planet->Rename(planet_name);
        system->Insert(planet, home_orbit);
        
        homeworlds.push_back(planet_id);
    }
}

void Universe::PopulateSystems()
{
    ObjectVec sys_vec = FindObjects(IsSystem);

    if (sys_vec.empty())
        throw std::runtime_error("Attempted to populate an empty galaxy.");

    SmallIntDistType planet_type_gen = SmallIntDist(0, Planet::MAX_PLANET_TYPE - 1);

    for (ObjectVec::iterator it = sys_vec.begin(); it != sys_vec.end(); ++it) {
        System* system = dynamic_cast<System*>(*it);

        for (int orbit = 0; orbit < system->Orbits(); orbit++) {
            Planet::PlanetSize plt_size;       

            // Type is selected at random. Type only affects the planet's image in v0.1
            Planet::PlanetType plt_type = Planet::PlanetType(planet_type_gen());
            double size_rnd = RandZeroToOne();
            if (size_rnd < 0.10)
                plt_size = Planet::SZ_TINY;
            else if (size_rnd < 0.35)
                plt_size = Planet::SZ_SMALL;
            else if (size_rnd < 0.65)
                plt_size = Planet::SZ_MEDIUM;
            else if (size_rnd < 0.90)
                plt_size = Planet::SZ_LARGE;
            else
                plt_size = Planet::SZ_HUGE;

            Planet* planet = new Planet(plt_type, plt_size);

            Insert(planet); // add planet to universe map
            system->Insert(planet, orbit);  // add planet to system map
            planet->Rename(system->Name() + " " + RomanNumber(orbit + 1));
        }
    }
}

void Universe::GenerateEmpires(int players, std::vector<int>& homeworlds)
{
#ifdef FREEORION_BUILD_SERVER
   // create empires and assign homeworlds, names, colors, and fleet ranges to
   // for each one

   const std::map<int, PlayerInfo>& player_info = ServerApp::GetApp()->NetworkCore().Players();
   int i = 0;
   for (std::map<int, PlayerInfo>::const_iterator it = player_info.begin(); it != player_info.end(); ++it, ++i) {
      // TODO: select name at random from default list
      std::string name = "Empire" + boost::lexical_cast<std::string>(i);

      // TODO: select color at random from default list
      GG::Clr color(RandZeroToOne(), RandZeroToOne(), RandZeroToOne(), 1.0);
  
      int home_planet_id = homeworlds[i];

      // create new Empire object through empire manager
      // TODO : ******* find a way to distinguish between humans and AIs *******
      Empire* empire = 
          dynamic_cast<ServerEmpireManager*>(&Empires())->CreateEmpire(it->first, name, color, home_planet_id, Empire::CONTROL_HUMAN);

      // set ownership of home planet
      int empire_id = empire->EmpireID();
      Planet* home_planet = dynamic_cast<Planet*>(Object(homeworlds[i]));
      Logger().debugStream() << "Setting " << home_planet->GetSystem()->Name() << " (Planet #" <<  home_planet->ID() << 
          ") to be home system for Empire " << empire_id;
      home_planet->AddOwner(empire_id);

      // TODO: adding an owner to a planet should probably add that owner to the 
      //       system automatically...
      System* home_system = home_planet->GetSystem();
      home_system->AddOwner(empire_id);

      // create population and industry on home planet
      home_planet->AdjustPop(20);
      home_planet->SetWorkforce(20);
      home_planet->SetMaxWorkforce( home_planet->MaxPop() );
      home_planet->AdjustIndustry(0.10);
      home_planet->AdjustDefBases(3);

      // create the empire's initial ship designs
      // for now, the order that these are created need to match
      // the enums for ship designs in ships.h
      ShipDesign scout_design;
      scout_design.name = "Scout";
      scout_design.attack = 0;
      scout_design.defense = 1;
      scout_design.cost = 50;
      scout_design.colonize = false;
      scout_design.empire = empire_id;
      int scout_id = empire->AddShipDesign(scout_design);

      ShipDesign colony_ship_design;
      colony_ship_design.name = "Colony Ship";
      colony_ship_design.attack = 0;
      colony_ship_design.defense = 1;
      colony_ship_design.cost = 250;
      colony_ship_design.colonize = true;
      colony_ship_design.empire = empire_id;
      int colony_id = empire->AddShipDesign(colony_ship_design);

      ShipDesign design;
      design.name = "Mark I";
      design.attack = 2;
      design.defense = 1;
      design.cost = 100;
      design.colonize = false;
      design.empire = empire_id;
      empire->AddShipDesign(design);

      design.name = "Mark II";
      design.attack = 5;
      design.defense = 2;
      design.cost = 200;
      design.colonize = false;
      design.empire = empire_id;
      empire->AddShipDesign(design);

      design.name = "Mark III";
      design.attack = 10;
      design.defense = 3;
      design.cost = 375;
      design.colonize = false;
      design.empire = empire_id;
      empire->AddShipDesign(design);

      design.name = "Mark IV";
      design.attack = 15;
      design.defense = 5;
      design.cost = 700;
      design.colonize = false;
      design.empire = empire_id;
      empire->AddShipDesign(design);

      // create the empire's starting fleet
      Fleet* home_fleet = new Fleet("Home Fleet", home_system->X(), home_system->Y(), empire_id);
      int fleet_id = Insert(home_fleet);
      home_system->Insert(home_fleet);
      empire->AddFleet(fleet_id);

      int ship_id = Insert(new Ship(empire_id, scout_id));
      home_fleet->AddShip(ship_id);

      ship_id = Insert(new Ship(empire_id, scout_id));
      home_fleet->AddShip(ship_id);

      ship_id = Insert(new Ship(empire_id, colony_id));
      home_fleet->AddShip(ship_id);
   }
#endif
}

int Universe::GenerateObjectID( )
{
  return( ++m_last_allocated_id );
}
