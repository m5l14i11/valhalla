#ifndef VALHALLA_GEO_TILEHIERARCHY_H
#define VALHALLA_GEO_TILEHIERARCHY_H

#include <set>
#include <string>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include "geo/tiles.h"

namespace valhalla {
namespace geo {


/**
 * struct used to get information about a given hierarchy of tiles
 */
struct TileHierarchy {
 public:
  /**
   * Constructor
   */
  TileHierarchy(const boost::property_tree::ptree& pt);

  /**
   * Encapsulates a few types together to define a level in the hierarchy
   */
  struct TileLevel{
    TileLevel(const unsigned char level, const std::string& name, const Tiles& tiles);
    bool operator<(const TileLevel& other) const;
    unsigned char level_;
    std::string name_;
    Tiles tiles_;
  };

  // a place to keep each level of the hierarchy
  std::set<TileLevel> levels_;
  // the tiles are stored
  std::string tile_dir_;

 private:
  explicit TileHierarchy();
};

}
}

#endif  // VALHALLA_GEO_TILEHIERARCHY_H
