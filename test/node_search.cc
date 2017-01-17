#include "test.h"
#include "loki/node_search.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <unordered_set>

#include <valhalla/baldr/graphid.h>
#include <valhalla/baldr/graphreader.h>
#include <valhalla/baldr/location.h>
#include <valhalla/midgard/pointll.h>
#include <valhalla/midgard/vector2.h>
#include <valhalla/baldr/tilehierarchy.h>

namespace vm = valhalla::midgard;
namespace vb = valhalla::baldr;

/*
 * to regenerate the test tile you'll want to:
 *
 * 1. add " libvalhalla_mjolnir = unstable" to the end of the pkg_check for
 *    VALHALLA_DEPS in configure.ac and re-run autogen/configure.
 * 2. uncomment the #define below.
 * 3. run "make check".
 */

// #define MAKE_TEST_TILES

#ifdef MAKE_TEST_TILES
#include <valhalla/mjolnir/graphtilebuilder.h>
#include <valhalla/mjolnir/directededgebuilder.h>

namespace vj = valhalla::mjolnir;
#endif /* MAKE_TEST_TILES */

namespace {

#ifdef MAKE_TEST_TILES
struct graph_writer {
  graph_writer(const vb::TileHierarchy &hierarchy, uint8_t level);

  vj::GraphTileBuilder &builder(vb::GraphId tile_id);

  inline vm::PointLL node_latlng(vb::GraphId node_id) {
    return builder(node_id.Tile_Base()).nodes()[node_id.id()].latlng();
  }

  void write_tiles();

private:
  const vb::TileHierarchy &m_hierarchy;
  const uint8_t m_level;
  std::unordered_map<vb::GraphId, std::shared_ptr<vj::GraphTileBuilder> > m_builders;
};

graph_writer::graph_writer(const vb::TileHierarchy &hierarchy, uint8_t level)
  : m_hierarchy(hierarchy), m_level(level) {}

vj::GraphTileBuilder &graph_writer::builder(vb::GraphId tile_id) {
  auto itr = m_builders.find(tile_id);

  if (itr == m_builders.end()) {
    bool inserted = false;
    auto builder = std::make_shared<vj::GraphTileBuilder>(m_hierarchy, tile_id, false);
    std::tie(itr, inserted) = m_builders.emplace(tile_id, builder);
    // should be new, since itr == end.
    assert(inserted);
  }

  assert(itr != m_builders.end());
  return *(itr->second);
}

void graph_writer::write_tiles() {
  using vj::GraphTileBuilder;

  GraphTileBuilder::tweeners_t all_tweeners;

  for (auto &entry : m_builders) {
    auto tile_id = entry.first;
    auto &tile = entry.second;

    // write the tile
    tile->StoreTileData();

    // drop the pointer, so that the only copy of the data is on disk.
    tile.reset();

    // write the bin data
    GraphTileBuilder::tweeners_t tweeners;
    GraphTile reloaded(m_hierarchy, tile_id);
    auto bins = GraphTileBuilder::BinEdges(m_hierarchy, &reloaded, tweeners);
    GraphTileBuilder::AddBins(m_hierarchy, &reloaded, bins);

    // merge tweeners into global
    for (const auto &entry : tweeners) {
      auto status = all_tweeners.insert(entry);
      if (!status.second) {
        auto tile_id = entry.first;
        const auto &bins = entry.second;
        auto itr = status.first;

        for (size_t i = 0; i < vb::kBinCount; ++i) {
          auto &target_bin = itr->second[i];
          target_bin.insert(target_bin.end(), bins[i].cbegin(), bins[i].cend());
        }
      }
    }
  }

  for (const auto &entry : all_tweeners) {
    // re-open tiles to add tweeners back in.
    vb::GraphTile tile(m_hierarchy, entry.first);
    vj::GraphTileBuilder::AddBins(m_hierarchy, &tile, entry.second);
  }
}

struct edge_count_tracker {
  uint32_t update(vb::GraphId tile_id, uint32_t count) {
    auto itr = m_counts.find(tile_id);
    if (itr == m_counts.end()) {
      bool inserted;
      std::tie(itr, inserted) = m_counts.emplace(tile_id, 0);
    }
    uint32_t index = itr->second;
    itr->second += count;
    return index;
  }
  void clear() { m_counts.clear(); }
private:
  std::unordered_map<vb::GraphId, uint32_t> m_counts;
};

// temporary structure for holding a bunch of nodes and edges until they can be
// renumbered to the format needed for storing in tiles.
struct graph_builder {
  std::vector<vm::PointLL> nodes;
  std::vector<std::pair<size_t, size_t> > edges;
  void write_tiles(const vb::TileHierarchy &hierarchy, uint8_t level) const;
};

void graph_builder::write_tiles(const TileHierarchy &hierarchy, uint8_t level) const {
  using namespace valhalla::mjolnir;

  const size_t num_nodes = nodes.size();
  const size_t num_edges = edges.size();

  graph_writer writer(hierarchy, level);
  edge_count_tracker edge_counts;

  // count the number of edges originating at a node
  std::vector<uint32_t> edges_from_node(num_nodes, 0);
  for (const auto &e : edges) {
    edges_from_node[e.first] += 1;
  }

  // renumber nodes into tiles
  std::vector<vb::GraphId> node_ids;
  node_ids.reserve(num_nodes);
  for (size_t i = 0; i < num_nodes; ++i) {
    auto coord = nodes[i];
    auto tile_id = hierarchy.GetGraphId(coord, level);
    uint32_t n = edges_from_node[i];

    NodeInfo node_builder;
    node_builder.set_latlng(coord);
    node_builder.set_edge_index(edge_counts.update(tile_id, n));
    node_builder.set_edge_count(n);

    auto &tile = writer.builder(tile_id);
    node_ids.emplace_back(tile_id.tileid(), level, tile.nodes().size());
    tile.nodes().emplace_back(std::move(node_builder));
  }

  // don't need these any more
  edges_from_node.clear();
  edge_counts.clear();

  // renumber the nodes of all the edges
  typedef std::vector<std::pair<vb::GraphId, vb::GraphId> > edge_vector_t;
  edge_vector_t renumbered_edges;
  renumbered_edges.reserve(num_edges);
  for (auto e : edges) {
    renumbered_edges.emplace_back(node_ids[e.first], node_ids[e.second]);
  }

  // sort edges so that they come in tile, node order. this allows us to figure
  // out which edges start at which nodes in the tile, to assign them. it also
  // allows us to easily look up the opposing edges by binary search.
  std::sort(renumbered_edges.begin(), renumbered_edges.end());

  // find the first renumbered edge for each tile. this allows us to easily
  // calculate the index of the edge in the tile from the offset of the two
  // iterators.
  std::unordered_map<vb::GraphId, edge_vector_t::iterator> tile_bases;
  vb::GraphId last_tile_id;
  for (edge_vector_t::iterator itr = renumbered_edges.begin();
       itr != renumbered_edges.end(); ++itr) {
    auto tile_id = itr->first.Tile_Base();
    if (last_tile_id != tile_id) {
      last_tile_id = tile_id;
      tile_bases[tile_id] = itr;
    }
  }

  for (auto e : renumbered_edges) {
    auto tile_id = e.first.Tile_Base();
    auto &tile = writer.builder(tile_id);

    bool forward = e.first < e.second;
    vm::PointLL start_point = writer.node_latlng(e.first);
    vm::PointLL end_point = writer.node_latlng(e.second);

    DirectedEdgeBuilder edge_builder(
      {}, e.second, forward, start_point.Distance(end_point), 1, 1, 1, {}, {},
      0, false, 0, 0);

    auto opp = std::make_pair(e.second, e.first);
    auto itr = std::lower_bound(renumbered_edges.begin(),
                                renumbered_edges.end(),
                                opp);

    // check that we found the opposite edge, which should always exist.
    assert(itr != renumbered_edges.end() && *itr == opp);

    uint32_t opp_index = itr - tile_bases[e.second.Tile_Base()];
    edge_builder.set_opp_index(opp_index);

    std::vector<PointLL> shape = {start_point, end_point};
    if(!forward)
      std::reverse(shape.begin(), shape.end());

    bool add;
    // make more complex edge geom so that there are 3 segments, affine
    // combination doesnt properly handle arcs but who cares
    uint32_t edge_index = tile.directededges().size();
    uint32_t edge_info_offset = tile.AddEdgeInfo(
      edge_index, e.first, e.second, 123, shape, {std::to_string(edge_index)},
      add);
    edge_builder.set_edgeinfo_offset(edge_info_offset);

    tile.directededges().emplace_back(std::move(edge_builder));
  }

  writer.write_tiles();
}

void make_tile() {
  graph_builder builder;

  vm::AABB2<vm::PointLL> box{{0.0, 0.0}, {0.5, 0.5}};
  const uint32_t rows = 100, cols = 100;

  float row_stride = box.Height() / (rows - 1);
  float col_stride = box.Width() / (cols - 1);

  for (uint32_t row = 0; row < rows; ++row) {
    for (uint32_t col = 0; col < cols; ++col) {
      builder.nodes.emplace_back(box.minx() + col_stride * col,
                                 box.miny() + row_stride * row);
    }
  }

  // add horizontal edges
  for (uint32_t row = 0; row < rows; ++row) {
    for (uint32_t col = 1; col < cols; ++col) {
      uint32_t index = row * cols + col - 1;
      // add edge and its opposite explicitly
      builder.edges.emplace_back(index, index + 1);
      builder.edges.emplace_back(index + 1, index);
    }
  }

  // add vertical edges
  for (uint32_t row = 1; row < rows; ++row) {
    for (uint32_t col = 0; col < cols; ++col) {
      uint32_t index = (row - 1) * cols + col;
      // add edge and its opposite explicitly
      builder.edges.emplace_back(index, index + cols);
      builder.edges.emplace_back(index + cols, index);
    }
  }

  vb::TileHierarchy h("test/node_search_tiles");
  uint8_t level = 2;
  builder.write_tiles(h, level);
}
#endif /* MAKE_TEST_TILES */

void test_single_node() {
  //make the config file
  std::stringstream json; json << "{ \"tile_dir\": \"test/node_search_tiles\" }";
  boost::property_tree::ptree conf;
  boost::property_tree::json_parser::read_json(json, conf);

  vb::GraphReader reader(conf);
  vm::AABB2<vm::PointLL> box{{-0.0025, 0.0025}, {0.0025, 0.0025}};

  auto nodes = valhalla::loki::nodes_in_bbox(box, reader);

  if (nodes.size() != 1) {
    throw std::runtime_error("Expecting to find one node, but got " +
                             std::to_string(nodes.size()));
  }
}

} // anonymous namespace

int main() {
  test::suite suite("node_search");

#ifdef MAKE_TEST_TILES
  suite.test(TEST_CASE(make_tile));
#endif /* MAKE_TEST_TILES */

  // TODO: uncomment when implemented
  //suite.test(TEST_CASE(test_single_node));

  return suite.tear_down();
}
