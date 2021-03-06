// Copyright 2018 Francesco Biscani (bluescarni@gmail.com)
//
// This file is part of the rakau library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <rakau/tree.hpp>

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <tuple>
#include <type_traits>
#include <vector>

#include <rakau/config.hpp>

#include "test_utils.hpp"

using namespace rakau;
using namespace rakau_test;

using fp_types = std::tuple<float, double>;
using macs = std::tuple<std::integral_constant<mac, mac::bh>, std::integral_constant<mac, mac::bh_geom>>;

static std::mt19937 rng(2);

static const std::vector<double> split =
#if defined(RAKAU_WITH_ROCM) || defined(RAKAU_WITH_CUDA)
    {0.5, 0.5}
#else
    {}
#endif
;

TEST_CASE("potential accuracy ordered")
{
    tuple_for_each(macs{}, [](auto mac_type) {
        tuple_for_each(fp_types{}, [mac_type](auto x) {
            using fp_type = decltype(x);
            constexpr auto theta = static_cast<fp_type>(.001), bsize = static_cast<fp_type>(1);
            auto sizes = {10u, 100u, 1000u, 2000u};
            auto max_leaf_ns = {1u, 2u, 8u, 16u};
            auto ncrits = {1u, 16u, 128u, 256u};
            std::vector<fp_type> pots;
            fp_type tot_max_diff(0);
            for (auto s : sizes) {
                auto parts = get_uniform_particles<3>(s, bsize, rng);
                for (auto max_leaf_n : max_leaf_ns) {
                    for (auto ncrit : ncrits) {
                        std::vector<fp_type> diff;
                        octree<fp_type, decltype(mac_type)::value> t{kwargs::x_coords = parts.begin() + s,
                                                                     kwargs::y_coords = parts.begin() + 2u * s,
                                                                     kwargs::z_coords = parts.begin() + 3u * s,
                                                                     kwargs::masses = parts.begin(),
                                                                     kwargs::nparts = s,
                                                                     kwargs::box_size = bsize,
                                                                     kwargs::max_leaf_n = max_leaf_n,
                                                                     kwargs::ncrit = ncrit};
                        t.pots_o(pots, theta);
                        // Check that all potentials are finite.
                        REQUIRE(std::all_of(pots.begin(), pots.end(), [](auto c) { return std::isfinite(c); }));
                        for (auto i = 0u; i < s; ++i) {
                            auto epot = t.exact_pot_o(i);
                            diff.emplace_back(std::abs((epot - pots[i]) / epot));
                        }
                        std::cout << "Results for size=" << s << ", max_leaf_n=" << max_leaf_n << ", ncrit=" << ncrit
                                  << ", mac=" << static_cast<int>(mac_type()) << ".\n=========\n";
                        const auto local_max_diff = *std::max_element(diff.begin(), diff.end());
                        std::cout << "max_diff=" << local_max_diff << '\n';
                        std::cout << "average_diff="
                                  << (std::accumulate(diff.begin(), diff.end(), fp_type(0)) / fp_type(s)) << '\n';
                        std::cout << "median_diff=" << median(diff) << '\n';
                        std::cout << "=========\n\n";
                        tot_max_diff = std::max(local_max_diff, tot_max_diff);
                    }
                }
            }
            std::cout << "\n\n\ntot_max_diff=" << tot_max_diff << "\n\n\n";
            if constexpr (std::is_same_v<fp_type, double> && std::numeric_limits<fp_type>::is_iec559) {
                // These numbers are, of course, totally arbitrary, based
                // on the fact that 'double' is actually double-precision,
                // and derived experimentally.
                REQUIRE(tot_max_diff < fp_type(1E-10));
            }
        });
    });
}

TEST_CASE("potential accuracy unordered")
{
    tuple_for_each(macs{}, [](auto mac_type) {
        tuple_for_each(fp_types{}, [mac_type](auto x) {
            using fp_type = decltype(x);
            constexpr auto theta = static_cast<fp_type>(.001), bsize = static_cast<fp_type>(1);
            auto sizes = {10u, 100u, 1000u, 2000u};
            auto max_leaf_ns = {1u, 2u, 8u, 16u};
            auto ncrits = {1u, 16u, 128u, 256u};
            std::vector<fp_type> pots;
            fp_type tot_max_diff(0);
            for (auto s : sizes) {
                auto parts = get_uniform_particles<3>(s, bsize, rng);
                for (auto max_leaf_n : max_leaf_ns) {
                    for (auto ncrit : ncrits) {
                        std::vector<fp_type> diff;
                        octree<fp_type, decltype(mac_type)::value> t{kwargs::x_coords = parts.begin() + s,
                                                                     kwargs::y_coords = parts.begin() + 2u * s,
                                                                     kwargs::z_coords = parts.begin() + 3u * s,
                                                                     kwargs::masses = parts.begin(),
                                                                     kwargs::nparts = s,
                                                                     kwargs::box_size = bsize,
                                                                     kwargs::max_leaf_n = max_leaf_n,
                                                                     kwargs::ncrit = ncrit};
                        t.pots_u(pots, theta, kwargs::split = split);
                        // Check that all potentials are finite.
                        REQUIRE(std::all_of(pots.begin(), pots.end(), [](auto c) { return std::isfinite(c); }));
                        for (auto i = 0u; i < s; ++i) {
                            auto epot = t.exact_pot_u(i);
                            diff.emplace_back(std::abs((epot - pots[i]) / epot));
                        }
                        std::cout << "Results for size=" << s << ", max_leaf_n=" << max_leaf_n << ", ncrit=" << ncrit
                                  << ", mac=" << static_cast<int>(mac_type()) << ".\n=========\n";
                        const auto local_max_diff = *std::max_element(diff.begin(), diff.end());
                        std::cout << "max_diff=" << local_max_diff << '\n';
                        std::cout << "average_diff="
                                  << (std::accumulate(diff.begin(), diff.end(), fp_type(0)) / fp_type(s)) << '\n';
                        std::cout << "median_diff=" << median(diff) << '\n';
                        std::cout << "=========\n\n";
                        tot_max_diff = std::max(local_max_diff, tot_max_diff);
                    }
                }
            }
            std::cout << "\n\n\ntot_max_diff=" << tot_max_diff << "\n\n\n";
            if constexpr (std::is_same_v<fp_type, double> && std::numeric_limits<fp_type>::is_iec559) {
                // These numbers are, of course, totally arbitrary, based
                // on the fact that 'double' is actually double-precision,
                // and derived experimentally.
                REQUIRE(tot_max_diff < fp_type(1E-10));
            }
        });
    });
}
