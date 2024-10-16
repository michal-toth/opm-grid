//===========================================================================
//
// File: addLgrsOnDistributedGrid_test.cpp
//
// Created: Monday 5 August 2024 14:44
//
// Author(s): Antonella Ritorto   <antonella.ritorto@opm-op.com>
//
// $Date$
//
// $Revision$
//
//===========================================================================
/*
  Copyright 2024 Equinor ASA.

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "config.h"

#define BOOST_TEST_MODULE LGRsOnDistributedGridTest
#include <boost/test/unit_test.hpp>
#include <boost/version.hpp>
#if BOOST_VERSION / 100000 == 1 && BOOST_VERSION / 100 % 1000 < 71
#include <boost/test/floating_point_comparison.hpp>
#else
#include <boost/test/tools/floating_point_comparison.hpp>
#endif
#include <opm/grid/CpGrid.hpp>
#include <opm/grid/cpgrid/CpGridData.hpp>
#include <opm/grid/cpgrid/DefaultGeometryPolicy.hpp>
#include <opm/grid/cpgrid/Entity.hpp>
#include <opm/grid/cpgrid/EntityRep.hpp>
#include <opm/grid/cpgrid/Geometry.hpp>
#include <opm/grid/LookUpData.hh>
#include <opm/grid/common/CommunicationUtils.hpp>

#include <dune/grid/common/mcmgmapper.hh>

#include <sstream>
#include <iostream>
#include <cstdlib>
#include <cmath>

struct Fixture
{
    Fixture()
    {
        int m_argc = boost::unit_test::framework::master_test_suite().argc;
        char** m_argv = boost::unit_test::framework::master_test_suite().argv;
        Dune::MPIHelper::instance(m_argc, m_argv);
        Opm::OpmLog::setupSimpleDefaultLogging();
    }

    static int rank()
    {
        int m_argc = boost::unit_test::framework::master_test_suite().argc;
        char** m_argv = boost::unit_test::framework::master_test_suite().argv;
        return Dune::MPIHelper::instance(m_argc, m_argv).rank();
    }
};

BOOST_GLOBAL_FIXTURE(Fixture);



#define CHECK_COORDINATES(c1, c2)                                       \
    for (int c = 0; c < 3; c++) {                                       \
        BOOST_TEST(c1[c] == c2[c], boost::test_tools::tolerance(1e-12)); \
    }

void refinePatch_and_check(Dune::CpGrid& coarse_grid,
                           const std::vector<std::array<int,3>>& cells_per_dim_vec,
                           const std::vector<std::array<int,3>>& startIJK_vec,
                           const std::vector<std::array<int,3>>& endIJK_vec,
                           const std::vector<std::string>& lgr_name_vec)
{
    auto& data = coarse_grid.currentData(); // what data current_view_data_ is pointing at (data_ or distributed_data_)

    BOOST_CHECK(data.size() == startIJK_vec.size() + 2);
    BOOST_CHECK( data[0]->child_to_parent_cells_.empty());
    BOOST_CHECK(coarse_grid.getLgrNameToLevel().at("GLOBAL") == 0);

    for (long unsigned int level = 1; level < startIJK_vec.size() +1; ++level) // only 1 when there is only 1 patch
    {
        BOOST_CHECK( (*data[level]).parent_to_children_cells_.empty());
        BOOST_CHECK(coarse_grid.getLgrNameToLevel().at(lgr_name_vec[level-1]) == static_cast<int>(level));

        // GLOBAL grid
        for (int cell = 0; cell <  data[0]-> size(0); ++cell)
        {
            Dune::cpgrid::Entity<0> entity = Dune::cpgrid::Entity<0>(*data[0], cell, true);
            BOOST_CHECK( entity.hasFather() == false);
            BOOST_CHECK_THROW(entity.father(), std::logic_error);
            BOOST_CHECK_THROW(entity.geometryInFather(), std::logic_error);
            BOOST_CHECK( entity.getOrigin() ==  entity);
            BOOST_CHECK( entity.getOrigin().level() == 0);
            auto it = entity.hbegin(coarse_grid.maxLevel());
            auto endIt = entity.hend(coarse_grid.maxLevel());
            const auto& [lgr, childrenList] = (*data[0]).parent_to_children_cells_[cell];
            if (entity.isLeaf()){ // In particular, cell has no children/is not a father.
                BOOST_CHECK_EQUAL(lgr, -1);
                BOOST_CHECK(childrenList.empty());
                BOOST_CHECK( entity.isLeaf() == true);
                // If it == endIt, then entity.isLeaf() true (when dristibuted_data_ is empty)
                BOOST_CHECK( it == endIt);
            }
            else{
                BOOST_CHECK(lgr != -1);
                BOOST_CHECK(childrenList.size() > 1);
                // Auxiliary int to check amount of children
                double referenceElemOneParent_volume = 0.;
                std::array<double,3> referenceElem_entity_center = {0.,0.,0.}; // Expected {.5,.5,.5}
                for (const auto& child : childrenList) {
                    BOOST_CHECK( child != -1);
                    BOOST_CHECK( data[lgr]-> child_to_parent_cells_[child][0] == 0);
                    BOOST_CHECK( data[lgr]-> child_to_parent_cells_[child][1] == cell);

                    const auto& childElem =  Dune::cpgrid::Entity<0>(*data[lgr], child, true);
                    BOOST_CHECK(childElem.hasFather() == true);
                    BOOST_CHECK(childElem.level() == lgr);
                    referenceElemOneParent_volume += childElem.geometryInFather().volume();
                    for (int c = 0; c < 3; ++c)  {
                        referenceElem_entity_center[c] += (childElem.geometryInFather().center())[c];
                    }
                }
                BOOST_CHECK_EQUAL( entity.isLeaf(), false); // parent cells do not appear in the LeafView
                // Auxiliary int to check hierarchic iterator functionality
                double referenceElemOneParent_volume_it = 0.;
                std::array<double,3> referenceElem_entity_center_it = {0.,0.,0.}; // Expected {.5,.5,.5}
                // If it != endIt, then entity.isLeaf() false (when dristibuted_data_ is empty)
                BOOST_CHECK( it != endIt );
                for (; it != endIt; ++it)
                {
                    // Do something with the son available through it->
                    BOOST_CHECK(it ->hasFather() == true);
                    BOOST_CHECK(it ->level() == lgr);
                    referenceElemOneParent_volume_it += it-> geometryInFather().volume();
                    for (int c = 0; c < 3; ++c)
                    {
                        referenceElem_entity_center_it[c] += (it-> geometryInFather().center())[c];
                    }
                }
                for (int c = 0; c < 3; ++c)
                {
                    referenceElem_entity_center[c]
                        /= cells_per_dim_vec[lgr-1][0]*cells_per_dim_vec[lgr-1][1]*cells_per_dim_vec[lgr-1][2];
                    referenceElem_entity_center_it[c]
                        /= cells_per_dim_vec[lgr-1][0]*cells_per_dim_vec[lgr-1][1]*cells_per_dim_vec[lgr-1][2];
                }
                BOOST_CHECK_CLOSE(referenceElemOneParent_volume, 1, 1e-13);
                BOOST_CHECK_CLOSE(referenceElem_entity_center[0], .5, 1e-13);
                BOOST_CHECK_CLOSE(referenceElem_entity_center[1], .5, 1e-13);
                BOOST_CHECK_CLOSE(referenceElem_entity_center[2], .5, 1e-13);
                BOOST_CHECK_CLOSE(referenceElemOneParent_volume_it, 1, 1e-13);
                BOOST_CHECK_CLOSE(referenceElem_entity_center_it[0], .5, 1e-13);
                BOOST_CHECK_CLOSE(referenceElem_entity_center_it[1], .5, 1e-13);
                BOOST_CHECK_CLOSE(referenceElem_entity_center_it[2], .5, 1e-13);
            }
            BOOST_CHECK( entity.level() == 0);
        }

        if (!(data[level] -> global_cell_.empty()))
        {
            auto itMin = std::min_element((data[level] -> global_cell_).begin(),  (data[level] -> global_cell_).end());
            auto itMax = std::max_element((data[level] -> global_cell_).begin(),  (data[level] -> global_cell_).end());
            BOOST_CHECK_EQUAL( *itMin, 0);
            const auto& maxCartesianIdxLevel = data[level]->logical_cartesian_size_[0]*data[level]->logical_cartesian_size_[1]* data[level]->logical_cartesian_size_[2];
            BOOST_CHECK( *itMax < maxCartesianIdxLevel);
        }


        // LGRs
        for (int cell = 0; cell <  data[level]-> size(0); ++cell)
        {
            Dune::cpgrid::Entity<0> entity = Dune::cpgrid::Entity<0>(*data[level], cell, true);
            BOOST_CHECK( entity.hasFather() == true);
            BOOST_CHECK( entity.getOrigin() ==  entity.father());
            BOOST_CHECK( entity.getOrigin().level() == 0);
            BOOST_CHECK_CLOSE(entity.geometryInFather().volume(),
                              1./(cells_per_dim_vec[level-1][0]*cells_per_dim_vec[level-1][1]*cells_per_dim_vec[level-1][2]), 1e-6);
            BOOST_CHECK(entity.father().level() == 0);
            const auto& child_to_parent = (*data[level]).child_to_parent_cells_[cell];
            BOOST_CHECK_EQUAL( child_to_parent[0] == -1, false);
            BOOST_CHECK_EQUAL( child_to_parent[0] == 0, true);
            BOOST_CHECK_EQUAL( child_to_parent[1], entity.father().index());
            BOOST_CHECK( std::get<0>((*data[0]).parent_to_children_cells_[child_to_parent[1]]) == entity.level());
            BOOST_CHECK_EQUAL((std::find(std::get<1>((*data[0]).parent_to_children_cells_[child_to_parent[1]]).begin(),
                                         std::get<1>((*data[0]).parent_to_children_cells_[child_to_parent[1]]).end(),
                                         entity.index()) ==
                               std::get<1>((*data[0]).parent_to_children_cells_[child_to_parent[1]]).end()) , false);
            // Check amount of children cells of the parent cell
            BOOST_CHECK_EQUAL(std::get<1>((*data[0]).parent_to_children_cells_[child_to_parent[1]]).size(),
                              cells_per_dim_vec[level-1][0]*cells_per_dim_vec[level-1][1]*cells_per_dim_vec[level-1][2]);
            BOOST_CHECK( entity.level() == static_cast<int>(level));
            BOOST_CHECK( entity.isLeaf() == true);
            auto it = entity.hbegin(coarse_grid.maxLevel());
            auto endIt = entity.hend(coarse_grid.maxLevel());
            // If entity.isLeaf(), then it == endIt (when dristibuted_data_ is empty)
            BOOST_CHECK( it == endIt);
        }

        // LeafView faces
        for (int face = 0; face <  data[startIJK_vec.size()+1]-> face_to_cell_.size(); ++face)
        {
            const auto& faceToPoint =  (*data[startIJK_vec.size() +1]).face_to_point_[face];
            BOOST_CHECK(faceToPoint.size() == 4);
            for (int i = 0; i < 4; ++i) {
                BOOST_CHECK((*data[startIJK_vec.size() +1]).face_to_point_[face][i] != -1);
            }

            Dune::cpgrid::EntityRep<1> faceEntity(face, true);
            BOOST_CHECK((*data[startIJK_vec.size() +1]).face_to_cell_[faceEntity].size() < 3);
        }

        auto itMin = std::min_element((data.back() -> global_cell_).begin(),  (data.back()-> global_cell_).end());
        auto itMax = std::max_element((data.back() -> global_cell_).begin(),  (data.back() -> global_cell_).end());
        BOOST_CHECK( *itMin >= 0);
        const auto& maxCartesianIdx = coarse_grid.logicalCartesianSize()[0]*coarse_grid.logicalCartesianSize()[1]*coarse_grid.logicalCartesianSize()[2];
        BOOST_CHECK( *itMax < maxCartesianIdx);

        // LeafView
        for (int cell = 0; cell <  data[startIJK_vec.size()+1]-> size(0); ++cell)
        {
            BOOST_CHECK( data[startIJK_vec.size()+1] -> cell_to_point_[cell].size() == 8);
            for (int i = 0; i < 8; ++i)
            {
                BOOST_CHECK( data[startIJK_vec.size()+1] -> cell_to_point_[cell][i] != -1);
            }
            Dune::cpgrid::Entity<0> entity = Dune::cpgrid::Entity<0>(*data[startIJK_vec.size()+1], cell, true);
            for (int i = 0; i < data[startIJK_vec.size()+1] -> cell_to_face_[entity].size(); ++i)
            {
                BOOST_CHECK( data[startIJK_vec.size()+1] -> cell_to_face_[entity][i].index() != -1);
            }
            const auto& child_to_parent = (*data[startIJK_vec.size()+1]).child_to_parent_cells_[cell];
            const auto& level_cellIdx = (*data[startIJK_vec.size()+1]).leaf_to_level_cells_[entity.index()];
            auto it = entity.hbegin(coarse_grid.maxLevel());
            auto endIt = entity.hend(coarse_grid.maxLevel());
            BOOST_CHECK(entity.isLeaf());
            BOOST_CHECK(it == endIt);
            if (entity.hasFather()){
                BOOST_CHECK_CLOSE(entity.geometryInFather().volume(),
                                  1./(cells_per_dim_vec[entity.level()-1][0]
                                      *cells_per_dim_vec[entity.level()-1][1]
                                      *cells_per_dim_vec[entity.level()-1][2]), 1e-6);
                BOOST_CHECK(entity.father().level() == 0);
                BOOST_CHECK(child_to_parent[0] != -1);
                BOOST_CHECK_EQUAL( child_to_parent[0] == 0, true);
                BOOST_CHECK_EQUAL( child_to_parent[1], entity.father().index());
                BOOST_CHECK( entity.father() == entity.getOrigin());
                BOOST_CHECK( entity.getOrigin().level() == 0);
                BOOST_CHECK( std::get<0>((*data[0]).parent_to_children_cells_[child_to_parent[1]]) == entity.level());
                BOOST_CHECK_EQUAL((std::find(std::get<1>((*data[0]).parent_to_children_cells_[child_to_parent[1]]).begin(),
                                             std::get<1>((*data[0]).parent_to_children_cells_[child_to_parent[1]]).end(),
                                             level_cellIdx[1]) ==
                                   std::get<1>((*data[0]).parent_to_children_cells_[child_to_parent[1]]).end()) , false);
                // Check amount of children cells of the parent cell
                BOOST_CHECK_EQUAL(std::get<1>((*data[0]).parent_to_children_cells_[child_to_parent[1]]).size(),
                                  cells_per_dim_vec[entity.level()-1][0]*
                                  cells_per_dim_vec[entity.level()-1][1]*cells_per_dim_vec[entity.level()-1][2]);
                BOOST_CHECK( entity.father().isLeaf() == false);
                BOOST_CHECK( (entity.level() > 0) || (entity.level() < static_cast<int>(startIJK_vec.size()) +1));
                BOOST_CHECK( level_cellIdx[0] == entity.level());
            }
            else{
                BOOST_CHECK_THROW(entity.father(), std::logic_error);
                BOOST_CHECK_THROW(entity.geometryInFather(), std::logic_error);
                BOOST_CHECK_EQUAL(child_to_parent[0], -1);
                BOOST_CHECK_EQUAL(child_to_parent[1], -1);
                BOOST_CHECK( level_cellIdx[0] == 0);
                BOOST_CHECK( std::get<0>((*data[0]).parent_to_children_cells_[level_cellIdx[1]]) == -1);
                BOOST_CHECK( std::get<1>((*data[0]).parent_to_children_cells_[level_cellIdx[1]]).empty());
                BOOST_CHECK( entity.level() == 0);
                // Get index of the cell in level 0
                const auto& entityOldIdx =  (*data[startIJK_vec.size()+1]).leaf_to_level_cells_[entity.index()][1];
                BOOST_CHECK( entity.getOrigin().index() == entityOldIdx);
                BOOST_CHECK( entity.getOrigin().level() == 0);
            } // end else
        }
    } // end-level-for-loop

    BOOST_CHECK( static_cast<int>(startIJK_vec.size()) == coarse_grid.maxLevel());
    BOOST_CHECK( (*data[data.size()-1]).parent_to_children_cells_.empty());

    auto it_min = std::min_element((data.back() -> global_cell_).begin(),  (data.back()-> global_cell_).end());
    auto it_max = std::max_element((data.back() -> global_cell_).begin(),  (data.back() -> global_cell_).end());
    auto it_min_level_zero = std::min_element((data.front() -> global_cell_).begin(),  (data.front() -> global_cell_).end());
    auto it_max_level_zero = std::max_element((data.front() -> global_cell_).begin(),  (data.front() -> global_cell_).end());
    BOOST_CHECK_EQUAL( *it_min, *it_min_level_zero);
    BOOST_CHECK_EQUAL( *it_max, *it_max_level_zero);

    for (long unsigned int l = 0; l < startIJK_vec.size() +1; ++l) // level 0,1,2,... , last patch
    {
        const auto& view = coarse_grid.levelGridView(l);
        for (const auto& element: elements(view)){
            BOOST_CHECK_EQUAL(element.level(), l);
        }
    }

    std::vector<int> leaf_to_parent_cell; // To store parent cell index, when leaf cell has a parent. Empty entry otherwise.
    leaf_to_parent_cell.resize(data[startIJK_vec.size()+1]-> size(0)); // Correct size.

    Dune::MultipleCodimMultipleGeomTypeMapper<Dune::CpGrid::LeafGridView> leafMapper(coarse_grid.leafGridView(), Dune::mcmgElementLayout());
    Dune::MultipleCodimMultipleGeomTypeMapper<Dune::CpGrid::LevelGridView> level0Mapper(coarse_grid.levelGridView(0), Dune::mcmgElementLayout());

    for (const auto& element: elements(coarse_grid.leafGridView())){
        BOOST_CHECK( ((element.level() >= 0) || (element.level() < static_cast<int>(startIJK_vec.size()) +1)));
        if (element.hasFather()) { // leaf_cell has a father!
            leaf_to_parent_cell[leafMapper.index(element)] = level0Mapper.index(element.father());
            const auto& parent_id = data[0]->localIdSet().id(element.father());
            BOOST_CHECK(element.index() == leafMapper.index(element));
            BOOST_CHECK(element.father().index() == leaf_to_parent_cell[element.index()]);
            BOOST_CHECK(element.father().index() == parent_id);
            BOOST_CHECK(element.father().index() == level0Mapper.index(element.father()));
        }
    }

    // Ids on the leaf grid view (local id and global id coincide ON THE LEAF GRID VIEW, might differ in level grids)
    std::set<int> allIds_set;
    std::vector<int> allIds_vec;
    allIds_vec.reserve(data.back()->size(0) + data.back()->size(3));
    for (const auto& element: elements(coarse_grid.leafGridView())){
        const auto& localId = data.back()->localIdSet().id(element);
        const auto& globalId = data.back()->globalIdSet().id(element);
        if (coarse_grid.comm().size()==0) {
            // In serial run, local and global id coincide:
            BOOST_CHECK_EQUAL(localId, globalId);
        }
        allIds_set.insert(localId);
        allIds_vec.push_back(localId);
        // Check that the global_id_set_ptr_ has the correct id (id from the level where the entity was born).
        BOOST_CHECK_EQUAL( coarse_grid.globalIdSet().id(element), data[element.level()]->globalIdSet().id(element.getEquivLevelElem()));
    }
    // Check injectivity of the map local_id_set_ (and, indirectly, global_id_set_) after adding cell ids.
    BOOST_CHECK( allIds_set.size() == allIds_vec.size());

    for (const auto& point : vertices(coarse_grid.leafGridView())){
        const auto& localId = data.back()->localIdSet().id(point);
        const auto& globalId = data.back()->globalIdSet().id(point);
        if (coarse_grid.comm().size()==0) {
            // In serial run, local and global id coincide:
            BOOST_CHECK_EQUAL(localId, globalId);
        }
        allIds_set.insert(localId);
        allIds_vec.push_back(localId);
    }
    // Check injectivity of the map local_id_set_ (and, indirectly, global_id_set_) after adding point ids.
    BOOST_CHECK( allIds_set.size() == allIds_vec.size());

    // ------------------------------------------------------------- 
    // Check global id is not duplicated for interior cells
    std::vector<int> localInteriorCellIds_vec;
    localInteriorCellIds_vec.reserve(data.back()->size(0)); // more than actually needed since only care about interior cells
    int local_interior_cells_count = 0;
    for (const auto& element: elements(coarse_grid.leafGridView())) {
         const auto& elemPartitionType = element.getEquivLevelElem().partitionTypeWhenLgrs(true);
         if ( elemPartitionType == Dune::InteriorEntity) {
            localInteriorCellIds_vec.push_back(data.back()->globalIdSet().id(element));
            ++local_interior_cells_count;
        }
    }
    auto global_cells_count  = coarse_grid.comm().sum(local_interior_cells_count);
    auto [allGlobalIds_cells, displ] = Opm::allGatherv(localInteriorCellIds_vec, coarse_grid.comm());

    const std::set<int> allGlobalIds_cells_set(allGlobalIds_cells.begin(), allGlobalIds_cells.end());
    BOOST_CHECK( static_cast<int>(allGlobalIds_cells.size()) == global_cells_count);
    BOOST_CHECK( allGlobalIds_cells.size() == allGlobalIds_cells_set.size() );

    // Check global id is not duplicated for points
    std::vector<int> localPointIds_vec;
    localPointIds_vec.reserve(data.back()->size(3));
    for (const auto& point : vertices(coarse_grid.leafGridView())) {
        // Notice that all partition type points are pushed back. Selecting only interior points does not bring us to the expected value.
        localPointIds_vec.push_back(data.back()->globalIdSet().id(point));
    }
    auto [allGlobalIds_points, displPoint ] = Opm::allGatherv(localPointIds_vec, coarse_grid.comm());

    // Create a set out of the collection of repeteated global ids for points.
    const std::set<int> allGlobalIds_points_set(allGlobalIds_points.begin(), allGlobalIds_points.end());
    // Compute expected amount of global ids for points via cells_per_dim_vec, startIJK_vec, endIJK_vec
    // Notice that checking point.partitionType() == InteriorEntity is not enough, it considers fewer points.
    auto point_count = (coarse_grid.logicalCartesianSize()[0]+1)*(coarse_grid.logicalCartesianSize()[1]+1)*(coarse_grid.logicalCartesianSize()[2]+1);
    for (std::size_t level = 0; level < cells_per_dim_vec.size(); ++level) {
        const std::array<int,3>& patch_dim = {endIJK_vec[level][0]-startIJK_vec[level][0], endIJK_vec[level][1]-startIJK_vec[level][1], endIJK_vec[level][2]-startIJK_vec[level][2]};
        // Subtract corners of parent cells
        point_count -= (patch_dim[0] +1)*(patch_dim[1] +1)*(patch_dim[2] +1);
        // Add level refined corners
        point_count += ((patch_dim[0]*cells_per_dim_vec[level][0]) +1)*((patch_dim[1]*cells_per_dim_vec[level][1]) +1)*((patch_dim[2]*cells_per_dim_vec[level][2]) +1);
    }
    BOOST_CHECK( point_count == static_cast<int>(allGlobalIds_points_set.size()) );
    // ---------------------------------------------------------------

    // Local/Global id sets for level grids (level 0, 1, ..., maxLevel). For level grids, local might differ from global id.
    for (int level = 0; level < coarse_grid.maxLevel() +1; ++level)
    {
        std::set<int> levelIds_set;
        std::vector<int> levelIds_vec;
        levelIds_vec.reserve(data[level]->size(0) + data[level]->size(3));
        for (const auto& element: elements(coarse_grid.levelGridView(level))){
            const auto& localId = data[level]->localIdSet().id(element);
            // In parallel run, local and global id might not coincide.
            levelIds_set.insert(localId);
            levelIds_vec.push_back(localId);
            // Search in the leaf grid view elements for the element with the same id, if it exists.
            if (auto itIsLeaf = std::find_if( elements(coarse_grid.leafGridView()).begin(),
                                              elements(coarse_grid.leafGridView()).end(),
                                              [localId, data](const Dune::cpgrid::Entity<0>& leafElem)
                                              { return (localId == data.back()->localIdSet().id(leafElem)); });
                itIsLeaf != elements(coarse_grid.leafGridView()).end()) {
                BOOST_CHECK( itIsLeaf->getEquivLevelElem() == element);
            }
            if (element.isLeaf()) { // Check that the id of a cell not involved in any further refinement appears on the IdSet of the leaf grid view.
                BOOST_CHECK( std::find(allIds_set.begin(), allIds_set.end(), localId) != allIds_set.end());
            }
            else { // Check that the id of a cell that vanished during refinement does not appear on the IdSet of the leaf grid view.
                BOOST_CHECK( std::find(allIds_set.begin(), allIds_set.end(), localId) == allIds_set.end());
            }
            const auto& idx = data[level]->indexSet().index(element);
            BOOST_CHECK_EQUAL(idx, element.index());
        }

        for (const auto& point : vertices(coarse_grid.levelGridView(level))) {
            const auto& localId = data[level]->localIdSet().id(point);
            levelIds_set.insert(localId);
            levelIds_vec.push_back(localId);
            // Search in the leaf grid view elements for the element with the same id, if it exists.
            if (auto itIsLeaf = std::find_if( vertices(coarse_grid.leafGridView()).begin(),
                                              vertices(coarse_grid.leafGridView()).end(),
                                              [localId, data](const Dune::cpgrid::Entity<3>& leafPoint)
                                              { return (localId == data.back()->localIdSet().id(leafPoint)); });
                itIsLeaf != vertices(coarse_grid.leafGridView()).end()) {
                BOOST_CHECK( (*itIsLeaf).geometry().center() == point.geometry().center() );
            }
        }
        // Check injectivity of the map local_id_set_ (and, indirectly, global_id_set_)
        BOOST_CHECK( levelIds_set.size() == levelIds_vec.size());
    }
}

BOOST_AUTO_TEST_CASE(threeLgrs)
{
    // Create a grid
    Dune::CpGrid grid;
    const std::array<double, 3> cell_sizes = {1.0, 1.0, 1.0};
    const std::array<int, 3> grid_dim = {10,8,8};
    grid.createCartesian(grid_dim, cell_sizes);

    std::vector<int> parts(640);
    for (int k = 0; k < 8; ++k) {
        for (int j = 0; j < 8; ++j) {
            for (int i = 0; i < 10; ++i)
            {
                const auto& elemIdx = (k*80) + (j*10) + i;
                if (i<5) {
                    if(j<4) {
                        parts[elemIdx] = 0;
                    }
                    else {
                        parts[elemIdx] = 1;
                    }
                }
                else {
                    if (j<4) {
                        parts[elemIdx] = 2;
                    }
                    else {
                        parts[elemIdx] = 3;
                    }
                }
            }
        }
    }
    if(grid.comm().size()>1)
    {
        grid.loadBalance(parts);

        const std::vector<std::array<int,3>> cells_per_dim_vec = {{2,2,2}, {3,3,3}, {4,4,4}};
        const std::vector<std::array<int,3>> startIJK_vec = {{0,0,0}, {0,0,3}, {3,2,2}};
        const std::vector<std::array<int,3>> endIJK_vec = {{2,1,2}, {1,1,4}, {4,3,3}};
        const std::vector<std::string> lgr_name_vec = {"LGR1", "LGR2", "LGR3"};
        // LGR1 element indices = 0,1,80,81 -> 4x(2x2x2) = 32 refined cells
        // LGR2 element indices = 240  -> refined into 3x3x3 = 27 cells
        // LGR3 element indices = 183 -> refined into 4x4x4 = 64 cells
        grid.addLgrsUpdateLeafView(cells_per_dim_vec, startIJK_vec, endIJK_vec, lgr_name_vec);
        
        refinePatch_and_check(grid, cells_per_dim_vec, startIJK_vec, endIJK_vec, lgr_name_vec);
    }
}

BOOST_AUTO_TEST_CASE(atLeastOneLgr_per_process_attempt)
{
    // Create a grid
    Dune::CpGrid grid;
    const std::array<double, 3> cell_sizes = {1.0, 1.0, 1.0};
    const std::array<int, 3> grid_dim = {4,3,3};
    grid.createCartesian(grid_dim, cell_sizes);

    std::vector<int> parts(36);
    std::vector<std::vector<int>> cells_per_rank = { {0,1,4,5,8,9,16,20,21},
                                                     {12,13,17,24,25,28,29,32,33},
                                                     {2,3,6,7,10,11,18,22,23},
                                                     {14,15,19,26,27,30,31,34,35} };
    for (int rank = 0; rank < 4; ++rank) {
        for (const auto& elemIdx : cells_per_rank[rank]) {
            parts[elemIdx] = rank;
        }
    }
    if(grid.comm().size()>1)
    {
        grid.loadBalance(parts);

        const std::vector<std::array<int,3>> cells_per_dim_vec = {{2,2,2}, {3,3,3}, {4,4,4}, {2,2,2}};
        const std::vector<std::array<int,3>> startIJK_vec = {{0,1,0}, {0,0,2}, {3,2,0}, {3,0,2}};
        const std::vector<std::array<int,3>> endIJK_vec = {{1,3,1}, {1,1,3}, {4,3,1}, {4,2,3}};
        const std::vector<std::string> lgr_name_vec = {"LGR1", "LGR2", "LGR3", "LGR4"};
        // LGR1 element indices = 4,8 in rank 0. Total 16 refined cells, 45 points (45-12 = 33 with new global id).
        // LGR2 element indices = 24 in rank 1. Total 27 refined cells, 64 points (64-8 = 56 with new global id).
        // LGR3 element indices = 11 in rank 2. Total 64 refined cells, 125 points (125-8 = 117 with new global id).
        // LGR4 element indices = 27, 31 in rank 3.Total 16 refined cells, 45 points (45-12 = 33 with new global id).
        grid.addLgrsUpdateLeafView(cells_per_dim_vec, startIJK_vec, endIJK_vec, lgr_name_vec);

        refinePatch_and_check(grid, cells_per_dim_vec, startIJK_vec, endIJK_vec, lgr_name_vec);
    }
}

BOOST_AUTO_TEST_CASE(throw_not_fully_interior_lgr)
{
    // Create a grid
    Dune::CpGrid grid;
    const std::array<double, 3> cell_sizes = {1.0, 1.0, 1.0};
    const std::array<int, 3> grid_dim = {4,3,3};
    grid.createCartesian(grid_dim, cell_sizes);

    std::vector<int> parts(36);
    std::vector<std::vector<int>> cells_per_rank = { {0,1,4,5,8,9,16,20,21},
                                                     {12,13,17,24,25,28,29,32,33},
                                                     {2,3,6,7,10,11,18,22,23},
                                                     {14,15,19,26,27,30,31,34,35} };
    for (int rank = 0; rank < 4; ++rank) {
        for (const auto& elemIdx : cells_per_rank[rank]) {
            parts[elemIdx] = rank;
        }
    }
    if(grid.comm().size()>1)
    {
        grid.loadBalance(parts);

        const std::vector<std::array<int,3>> cells_per_dim_vec = {{2,2,2}, {3,3,3}, {4,4,4}, {2,2,2}};
        const std::vector<std::array<int,3>> startIJK_vec = {{0,1,0}, {0,0,2}, {3,1,0}, {3,0,2}};
        const std::vector<std::array<int,3>> endIJK_vec = {{1,3,1}, {1,1,3}, {4,2,1}, {4,2,3}};
        const std::vector<std::string> lgr_name_vec = {"LGR1", "LGR2", "LGR3", "LGR4"};
        // LGR1 element indices = 4,8 in rank 0. Total 16 refined cells, 45 points (45-12 = 33 with new global id).
        // LGR2 element indices = 24 in rank 1. Total 27 refined cells, 64 points (64-8 = 56 with new global id).
        // LGR3 element indices = 7 in rank 2. This cell is interior but it has a neighboring cell sharing its top face, cell 19 belonging to rank 3.
        // LGR4 element indices = 27, 31 in rank 3.Total 16 refined cells, 45 points (45-12 = 33 with new global id).

        // Throw due to LGR3 not verifying being fully interior on a process.
        BOOST_CHECK_THROW( grid.addLgrsUpdateLeafView(cells_per_dim_vec, startIJK_vec, endIJK_vec, lgr_name_vec) , std::logic_error);
    }
}

//Calling globalRefine on a distributed grid is not supported yet.
BOOST_AUTO_TEST_CASE(globalRefine2)
{
    // Create a grid
    Dune::CpGrid grid;
    const std::array<double, 3> cell_sizes = {1.0, 1.0, 1.0};
    const std::array<int, 3> grid_dim = {4,3,3};
    grid.createCartesian(grid_dim, cell_sizes);
    // Distribute the grid
    if(grid.comm().size()>1)
    {
        grid.loadBalance();

        BOOST_CHECK_THROW(grid.globalRefine(1), std::logic_error);
    }
}
