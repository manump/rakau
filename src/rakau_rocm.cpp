#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wdeprecated-dynamic-exception-spec"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"

#include <hc.hpp>
#include <hc_math.hpp>

#pragma GCC diagnostic pop

#include <boost/preprocessor/seq/elem.hpp>
#include <boost/preprocessor/seq/for_each_product.hpp>

#include <rakau/detail/rocm_fwd.hpp>
#include <rakau/detail/tree_fwd.hpp>

namespace rakau
{

inline namespace detail
{

// Minimum number of particles needed for running on the accelerator.
unsigned rocm_min_size()
{
    return static_cast<unsigned>(__HSA_WAVEFRONT_SIZE__);
}

// Check if a ROCm accelerator is available.
bool rocm_has_accelerator()
{
    // NOTE: it seems like at the moment the CPU is also seen as
    // an accelerator, so we always have at least 1 device.
    // We are interested in using ROCm only in presence
    // of actual GPU accelerators, so we check if there are at
    // least 2 accelerators on the system.
    return hc::accelerator::get_all().size() > 1u;
}

// Implementation of the rocm_state machinery. We will store in here views to
// the internal vectors of a tree.
template <std::size_t NDim, typename F, typename UInt, mac MAC>
struct rocm_state_impl {
    explicit rocm_state_impl(const std::array<const F *, NDim + 1u> &parts, const UInt *codes, int nparts,
                             const tree_node_t<NDim, F, UInt, MAC> *tree, int tree_size)
        : m_parts_views(index_apply<NDim + 1u>(
              [nparts, &parts](auto... I) { return std::array{hc::array_view<const F, 1>(nparts, parts[I()])...}; })),
          m_codes_view(nparts, codes), m_nparts(nparts), m_tree_view(tree_size, tree), m_tree_size(tree_size)
    {
    }

    // NOTE: make sure we don't end up accidentally copying/moving
    // objects of this class.
    rocm_state_impl(const rocm_state_impl &) = delete;
    rocm_state_impl(rocm_state_impl &&) = delete;
    rocm_state_impl &operator=(const rocm_state_impl &) = delete;
    rocm_state_impl &operator=(rocm_state_impl &&) = delete;

    // Views to the particle data.
    std::array<hc::array_view<const F, 1>, NDim + 1u> m_parts_views;
    // View to the codes.
    hc::array_view<const UInt, 1> m_codes_view;
    // Number of particles/codes.
    int m_nparts;
    // View into the tree structure.
    hc::array_view<const tree_node_t<NDim, F, UInt, MAC>, 1> m_tree_view;
    // Number of nodes in the tree.
    int m_tree_size;
};

// Constructor: forward all the arguments to the constructor of the internal structure (stored as a raw pointer).
template <std::size_t NDim, typename F, typename UInt, mac MAC>
rocm_state<NDim, F, UInt, MAC>::rocm_state(const std::array<const F *, NDim + 1u> &parts, const UInt *codes, int nparts,
                                           const tree_node_t<NDim, F, UInt, MAC> *tree, int tree_size)
    : m_state(::new rocm_state_impl(parts, codes, nparts, tree, tree_size))
{
}

// Destructor: delete the internal structure.
template <std::size_t NDim, typename F, typename UInt, mac MAC>
rocm_state<NDim, F, UInt, MAC>::~rocm_state()
{
    ::delete static_cast<rocm_state_impl<NDim, F, UInt, MAC> *>(m_state);
}

// Main function for the computation of the accelerations/potentials. It will fetch
// the views to the tree from the state structure, create views into the output data
// and then traverse the tree computing the acceleration for each particle.
template <std::size_t NDim, typename F, typename UInt, mac MAC>
template <unsigned Q>
void rocm_state<NDim, F, UInt, MAC>::acc_pot(int p_begin, int p_end,
                                             const std::array<F *, tree_nvecs_res<Q, NDim>> &out, F mac_value, F G,
                                             F eps2, bool offset_output) const
{
    assert(p_begin <= p_end);

    // Fetch the state structure.
    auto &state = *static_cast<const rocm_state_impl<NDim, F, UInt, MAC> *>(m_state);

    assert(p_end <= state.m_nparts);

    // Create views into the output buffers.
    auto out_views = index_apply<tree_nvecs_res<Q, NDim>>([p_end, p_begin, &out, offset_output](auto... I) {
        return std::tuple{hc::array_view<F, 1>(p_end - p_begin, out[I()] + (offset_output ? p_begin : 0))...};
    });

    auto fut = hc::parallel_for_each(hc::extent<1>(p_end - p_begin).tile(__HSA_WAVEFRONT_SIZE__), [
        p_begin,
        // Create on the fly the tuple of views into the particles data.
        pt = index_apply<NDim + 1u>([&state](auto... I) { return std::tuple{state.m_parts_views[I()]...}; }),
        codes_view = state.m_codes_view, nparts = state.m_nparts, tree_view = state.m_tree_view,
        tree_size = state.m_tree_size, rt = out_views, mac_value, G,
        eps2
    ](hc::tiled_index<1> thread_id) [[hc]] {
        // Get the global particle index into the tree data.
        const auto pidx = thread_id.global[0] + p_begin;
        if (pidx >= nparts) {
            // Don't do anything if we are in the remainder
            // of the last tile.
            return;
        }

        // Turn the tuples of views back into arrays of views.
        auto p_view = index_apply<NDim + 1u>([&pt](auto... I) { return std::array{std::get<I()>(pt)...}; });
        auto res_view
            = index_apply<tree_nvecs_res<Q, NDim>>([&rt](auto... I) { return std::array{std::get<I()>(rt)...}; });

        // Array of results, inited to zeroes.
        F res_array[tree_nvecs_res<Q, NDim>]{};

        // Load the particle code, position and mass.
        const auto p_code = codes_view[pidx];
        F p_pos[NDim];
        for (std::size_t j = 0; j < NDim; ++j) {
            p_pos[j] = p_view[j][pidx];
        }
        const auto p_mass = p_view[NDim][pidx];

        // Temporary arrays that will be used in the loop.
        F dist_vec[NDim], props[NDim + 1u];

        // Add a 1 bit just above the highest possible bit position for the particle code.
        // This value is used in the loop, we precompute it here.
        const auto s_p_code_init = static_cast<UInt>(p_code | (UInt(1) << (cbits_v<UInt, NDim> * NDim)));

        // Loop over the tree.
        for (auto src_idx = 0; src_idx < tree_size;) {
            // Get a reference to the current source node, and cache locally a few quantities.
            const auto &src_node = tree_view[src_idx];
            // Code of the source node.
            const auto src_code = src_node.code;
            // Range of the source node.
            const auto src_begin = static_cast<int>(src_node.begin), src_end = static_cast<int>(src_node.end);
            // Number of children of the source node.
            const auto n_children_src = static_cast<int>(src_node.n_children);
            // Node properties.
            for (std::size_t j = 0; j < NDim + 1u; ++j) {
                props[j] = src_node.props[j];
            }
            // Level of the source node.
            const auto src_level = src_node.level;
            // Left-hand side of the MAC check.
            const auto mac_lh = [mac_value, &src_node]() {
                if constexpr (MAC == mac::bh) {
                    // NOTE: for the BH MAC, mac_value is theta**-2.
                    return src_node.dim2 * mac_value;
                } else {
                    // NOTE: for the geometric BH MAC, mac_value is theta**-1.
                    static_assert(MAC == mac::bh_geom);
                    const auto tmp = src_node.dim * mac_value + src_node.delta;
                    return tmp * tmp;
                }
            }();

            // Compute the shifted particle code. This is the particle code with one extra
            // top bit and then shifted down according to the level of the source node, so that
            // the top 1 bits of s_p_code and src_code are at the same position.
            // If s_p_code == src_code, then it means that the source node contains the target particle
            // (or, in other words, the source node is an ancestor of the leaf node containing
            // the target particle).
            const auto s_p_code = s_p_code_init >> ((cbits_v<UInt, NDim> - src_level) * NDim);
            // If the source node contains the target particle, we will need to account for self interactions
            // in the tree traversal. There are two different approaches that can be taken.
            //
            // The first is to modify on-the-fly the properties of the source node with the removal of the target
            // particle. In the classic BH scheme, this will alter the COM position of the source node
            // and its mass. The alteration needs to take place if the source node is an ancestor of
            // NT, the leaf node of the target particle. However, if the source node coincides with NT
            // and NT contains *only* the target particle, then the alteration must not take place because
            // otherwise we generate infinities (the COM of a system of only 1 particle is the particle
            // itself). The alteration can be done by defining a mass factor mf as
            //
            // mf = orig_node_mass / (orig_node_mass - p_mass * needs_alteration),
            //
            // where needs_alteration is a boolean that expresses whether the source node needs to be
            // adjusted or not (so that mf == 1 if no adjustment needs to happen). It can then be shown
            // that the target particle's distance from the adjusted COM is
            //
            // new_dist = mf * orig_dist,
            //
            // where orig_dist is the (vector) distance from the original COM. The new node mass will be:
            //
            // new_node_mass = orig_node_mass - p_mass * needs_alteration.
            //
            // The other approach is not to modify the properties of the COM, and instead just continue
            // in the tree traversal as if the current source node didn't satisfy the BH check. By doing this
            // we will eventually land into the leaf node of the target particle, where we will compute
            // local particle-particle interactions in the usual N**2 way (avoiding self interactions for the
            // target particle).
            //
            // The first method is more arithmetically-intensive and requires less flow control. The other
            // method will result in longer tree traversals and higher flow control, but requires less arithmetics.
            // At the moment it seems like the first method might be a bit faster on the GPU, but it's also not
            // entirely clear how more complicated/intensive the source node alteration would become once we
            // implement quadrupole moments and other MACs. Thus, for now, let's go with the second approach.

            // Compute the distance between target particle and source COM.
            // NOTE: if we are in a source node which contains only the target particle,
            // then dist2 and dist_vec will be zero.
            F dist2(0);
            for (std::size_t j = 0; j < NDim; ++j) {
                const auto diff = props[j] - p_pos[j];
                dist2 += diff * diff;
                dist_vec[j] = diff;
            }
            // Now let's run the MAC/ancestor check on all the target particles in the same wavefront.
            if (hc::__all(s_p_code != src_code && mac_lh < dist2)) {
                // The source node does not contain the target particle and it satisfies the MAC.
                // We will then add the (approximated) contribution of the source node
                // to the final result.
                //
                // Start by adding the softening.
                dist2 += eps2;
                // Compute the (softened) distance.
                const auto dist = sqrt(dist2);
                if constexpr (Q == 0u || Q == 2u) {
                    // Q == 0 or 2: accelerations are requested.
                    const auto node_mass_dist3 = props[NDim] / (dist * dist2);
                    for (std::size_t j = 0; j < NDim; ++j) {
                        res_array[j] += dist_vec[j] * node_mass_dist3;
                    }
                }
                if constexpr (Q == 1u || Q == 2u) {
                    // Q == 1 or 2: potentials are requested.
                    // Establish the index of the potential in the result array:
                    // 0 if only the potentials are requested, NDim otherwise.
                    constexpr auto pot_idx = static_cast<std::size_t>(Q == 1u ? 0u : NDim);
                    // Add the potential due to the node.
                    res_array[pot_idx] -= p_mass * props[NDim] / dist;
                }
                // We can now skip all the children of the source node.
                src_idx += n_children_src + 1;
            } else {
                // Either the source node contains the target particle, or it fails the MAC check.
                if (!n_children_src) {
                    // We are in a leaf node (possibly containing the target particle).
                    // Compute all the interactions with the target particle.
                    for (auto i = src_begin; i < src_end; ++i) {
                        // Test if the current particle of the source leaf node coincides
                        // with the target particle.
                        const bool is_tgt_particle = pidx == i;
                        // Init the distance with the softening, plus add some extra
                        // softening if i is the target particle. This will avoid
                        // infinites when dividing by the distance below.
                        dist2 = eps2 + is_tgt_particle;
                        for (std::size_t j = 0; j < NDim; ++j) {
                            const auto diff = p_view[j][i] - p_pos[j];
                            dist2 += diff * diff;
                            dist_vec[j] = diff;
                        }
                        // Compute the distance, load the current source mass.
                        const auto dist = sqrt(dist2), m_i = p_view[NDim][i];
                        if constexpr (Q == 0u || Q == 2u) {
                            // Q == 0 or 2: accelerations are requested.
                            const auto m_i_dist3 = m_i / (dist * dist2);
                            for (std::size_t j = 0; j < NDim; ++j) {
                                // NOTE: if i == pidx, then dist_vec will be a vector
                                // of zeroes and res_array will not be modified.
                                res_array[j] += dist_vec[j] * m_i_dist3;
                            }
                        }
                        if constexpr (Q == 1u || Q == 2u) {
                            // Q == 1 or 2: potentials are requested.
                            // Establish the index of the potential in the result array:
                            // 0 if only the potentials are requested, NDim otherwise.
                            constexpr auto pot_idx = static_cast<std::size_t>(Q == 1u ? 0u : NDim);
                            // NOTE: for the potential, we need an extra multiplication by
                            // !is_tgt_particle to set the rhs to zero in case i == pidx (for
                            // the accelerations, the same effect was achieved via dist_vec).
                            res_array[pot_idx] -= !is_tgt_particle * p_mass * m_i / dist;
                        }
                    }
                }
                // Keep traversing the tree moving to the next node in depth-first order.
                ++src_idx;
            }
        }

        // Handle the G constant and write out the result.
        for (std::size_t j = 0; j < tree_nvecs_res<Q, NDim>; ++j) {
            // NOTE: for writing the results, we use the loop index
            // without offset.
            res_view[j][thread_id.global[0]] = G * res_array[j];
        }
    });
    // Sync the output views, once done.
    fut.then([&out_views]() {
        index_apply<tree_nvecs_res<Q, NDim>>(
            [&out_views](auto... I) { (..., std::get<I()>(out_views).synchronize()); });
    });
    // Wait for the parallel for to finish, and re-throw any exception that
    // might have been raised.
    fut.get();
}

// Explicit instantiations of the templates implemented above. We are going to use Boost.Preprocessor.
// It's gonna look ugly, but it will allow us to avoid a lot of typing.

// Define the values/types that we will use for the concrete instantiations.

// Only quadtrees and octrees for the moment.
#define RAKAU_ROCM_INST_DIM_SEQUENCE (2)(3)

// float and double only on the gpu.
#define RAKAU_ROCM_INST_FP_SEQUENCE (float)(double)

// 32/64bit types for the particle codes.
#define RAKAU_ROCM_INST_UINT_SEQUENCE (std::uint32_t)(std::uint64_t)

// Computation of accelerations, potentials or both.
#define RAKAU_ROCM_INST_Q_SEQUENCE (0)(1)(2)

// Enable all MACs.
#define RAKAU_ROCM_INST_MACS_SEQUENCE (mac::bh)(mac::bh_geom)

// Macro for the instantiation of the member function. NDim, F, UInt, Q and MAC will be passed in
// as a sequence named Args (in that order).
#define RAKAU_ROCM_EXPLICIT_INST_MEMFUN(r, Args)                                                                       \
    template void rocm_state<BOOST_PP_SEQ_ELEM(0, Args), BOOST_PP_SEQ_ELEM(1, Args), BOOST_PP_SEQ_ELEM(2, Args),       \
                             BOOST_PP_SEQ_ELEM(4, Args)>::acc_pot<BOOST_PP_SEQ_ELEM(3,                                 \
                                                                                    Args)>(                            \
        int, int,                                                                                                      \
        const std::array<BOOST_PP_SEQ_ELEM(1, Args) *,                                                                 \
                         tree_nvecs_res<BOOST_PP_SEQ_ELEM(3, Args), BOOST_PP_SEQ_ELEM(0, Args)>> &,                    \
        BOOST_PP_SEQ_ELEM(1, Args), BOOST_PP_SEQ_ELEM(1, Args), BOOST_PP_SEQ_ELEM(1, Args), bool) const;

// Do the actual instantiation via a cartesian product over the sequences.
// clang-format off
BOOST_PP_SEQ_FOR_EACH_PRODUCT(RAKAU_ROCM_EXPLICIT_INST_MEMFUN, (RAKAU_ROCM_INST_DIM_SEQUENCE)(RAKAU_ROCM_INST_FP_SEQUENCE)(RAKAU_ROCM_INST_UINT_SEQUENCE)(RAKAU_ROCM_INST_Q_SEQUENCE)(RAKAU_ROCM_INST_MACS_SEQUENCE));
// clang-format on

// Macro for the instantiation of the state class. Same idea as above.
#define RAKAU_ROCM_EXPLICIT_INST_STATE(r, Args)                                                                        \
    template class rocm_state<BOOST_PP_SEQ_ELEM(0, Args), BOOST_PP_SEQ_ELEM(1, Args), BOOST_PP_SEQ_ELEM(2, Args),      \
                              BOOST_PP_SEQ_ELEM(3, Args)>;

// Instantiation.
// clang-format off
BOOST_PP_SEQ_FOR_EACH_PRODUCT(RAKAU_ROCM_EXPLICIT_INST_STATE, (RAKAU_ROCM_INST_DIM_SEQUENCE)(RAKAU_ROCM_INST_FP_SEQUENCE)(RAKAU_ROCM_INST_UINT_SEQUENCE)(RAKAU_ROCM_INST_MACS_SEQUENCE));
// clang-format on

} // namespace detail
} // namespace rakau
