#pragma once

#include <torch/csrc/jit/codegen/cuda/fusion.h>
#include <torch/csrc/jit/codegen/cuda/ir_all_nodes.h>
#include <torch/csrc/jit/codegen/cuda/maxinfo_propagator.h>
#include <torch/csrc/jit/codegen/cuda/scheduler/reduction_heuristic.h>

namespace torch {
namespace jit {
namespace fuser {
namespace cuda {

class SchedulerRuntimeInfo;
class ExpressionEvaluator;
class HeuristicSummary;

namespace scheduler_utils {

// Assume any only half of the register file is available to spend on buffers,
// this is because when we allocate a buffer in register is has to be accesed
// with a compile time coonstant index. Unfortunately nvcc seems to be using
// many registers for indexing. This is a bad estimation of extra register use,
// but it's hard to get a better one.
constexpr int64_t register_file_size = 256 * 1024 / 2;
constexpr int64_t x_grid_limit = ((int64_t)1 << (int64_t)31) - (int64_t)1;
constexpr int64_t y_grid_limit = 65535;
constexpr int64_t z_grid_limit = 65535;
constexpr int64_t z_block_limit = 64;

// Largest Power of 2 less-than n
constexpr int64_t lastPow2(int64_t n) {
  TORCH_INTERNAL_ASSERT(n >= 0);
  n |= (n >> 1);
  n |= (n >> 2);
  n |= (n >> 4);
  n |= (n >> 8); // NOLINT(cppcoreguidelines-avoid-magic-numbers)
  n |= (n >> 16); // NOLINT(cppcoreguidelines-avoid-magic-numbers)
  n |= (n >> 32); // NOLINT(cppcoreguidelines-avoid-magic-numbers)
  return std::max((int64_t)1, n - (n >> 1));
}

// Div x by y, but min at 1
inline int64_t safeDiv(const int64_t x, const int64_t y) {
  return std::max(x / y, (int64_t)1);
}

// Merge all reduction to the right side and returns total number of
// reduction axes. Don't merge is typically used for trivial reductions.
size_t mergeReduction(
    TensorView* tv,
    const std::unordered_set<IterDomain*>& dont_merge = {});

// merge all non-reduction axes to the left side and returns total number of
// iteration axes. Don't merge is typically used for trivial reductions.
size_t mergeNonReduction(
    TensorView* tv,
    const std::unordered_set<IterDomain*>& dont_merge = {});

// Propagate the parallelization from the selected dimensions of the reference
// tensor to their corresponding dimensions in all selected tensors in the DAG.
// Position `pos` means selecting all the dimensions [0, 1, ..., pos - 1]. pos =
// -1 means selecting all dimensions. `selected_tvs` are selected tensors in the
// DAG. Empty `selected_tvs` means selecting all tensors in the fusion of
// `reference_tv`. `selected_parallel_types` are the selected parallel types.
// Empty `selected_parallel_types` means selecting all parallel types.
TORCH_CUDA_CU_API void parallelizeAllLike(
    TensorView* reference_tv,
    int64_t pos = -1,
    std::vector<TensorView*> selected_tvs = {},
    const std::unordered_set<ParallelType>& selected_parallel_types = {},
    bool propagate_padding = true);

TORCH_CUDA_CU_API inline void parallelizeAllLike(
    TensorView* reference_tv,
    std::vector<TensorView*> selected_tvs,
    const std::unordered_set<ParallelType>& selected_parallel_types = {},
    bool propagate_padding = true) {
  parallelizeAllLike(
      reference_tv,
      -1,
      std::move(selected_tvs),
      selected_parallel_types,
      propagate_padding);
}

TORCH_CUDA_CU_API void computeAtInputs(
    TensorView* consumer,
    int pos,
    ComputeAtMode mode = ComputeAtMode::Standard);

TORCH_CUDA_CU_API void computeWithOutputs(
    TensorView* producer,
    int pos,
    ComputeAtMode mode = ComputeAtMode::Standard);

struct PersistentBufferInfo {
  std::vector<TensorView*> persistent_buffers;
  std::unordered_set<IterDomain*> unmappable_dims;

  // Persistent buffers are needed until the path through the reduction -
  // broadcast chain is resolved by any other chain using the persistent buffer
  // that is not going through a reduction. This assumes all reduction paths
  // have the same reduction pattern. Order is the same as persistent_buffers
  std::vector<std::vector<TensorView*>> persistent_buffer_resolution_points;

  // Not all persistent buffers can be projected to inputs, if a buffer can be
  // projected to the inputs which may reduce the persistent buffer size (BN
  // Backwards specifically) then keep track of it here. Persistent buffers that
  // have a persistent buffer/reduction before them should not be projected
  // through that.
  std::vector<TensorView*> projectable_persistent_buffers;

  // Track inputs of input projectable buffers
  std::vector<TensorView*> projectable_buffer_inputs;

  // Map unmappable dims to projectable_buffer_inputs
  std::unordered_set<IterDomain*> unamppable_dims_projected_to_inputs;
};

// Buffers whos roots can't map to all producer roots based on compute at. These
// are the buffers we would make persistent in a persistent kerenl or would have
// to recompute if we can't make a persistent kernel. This function will also
// return inputs as being marked persistent if they follow this pattern. It is
// important to note however inputs don't strictly have to be persistent as they
// can simply be read multiple times from GMEM in the same kernel.
TORCH_CUDA_CU_API PersistentBufferInfo persistentBuffers(Fusion* fusion);

struct TvProperties {
  // How many elements in tensor view are there to reduce.
  int64_t total_reduction_numel = 1;

  // How many reductions do we need to perform, i.e. how many iter dimension.
  // elements are there
  int64_t total_iteration_numel = 1;

  // Is the inner most dimension a reduction, if no reductions mark true.
  bool fastest_dim_reduction = true;

  // How many elements in the inner most dimension merging surrounding domains
  // that match in type. This is used for 3D schedulers in
  // reduction/normalization.
  int64_t inner_most_dimension_numel = 1;

  // Same thing as above, but the number of dimensions instead of the numel.
  int64_t inner_most_dimension_ndims = 1;

  // Merging neighboring iteration domains, and reduction domains, what's the
  // resulting dimensionality of the problem.
  int64_t dimensionality = 1;
};

// Fill TvProperties structure about tv
TvProperties getProperties(
    Fusion* fusion,
    SchedulerRuntimeInfo& runtime_info,
    TensorView* tv);

// Will call computeAt once on each producer, with the first consumer found that
// is a consumer of the individual producer
void computeAtBetween(
    const std::vector<TensorView*>& producers,
    const std::vector<TensorView*>& consumers,
    int pos,
    ComputeAtMode mode,
    std::unordered_set<IterDomain*> mapped_to_trivial_reduction = {});

// Struct to store persistent buffer sizes. also holds the persistent buffer
// size of the buffers are projected to the inputs.
struct PersistentBufferSizeReturn {
  int64_t persistent_buffer_size = 0;
  int64_t projected_persistent_buffer_size = 0;
};

// Compute the amount of register space would be needed to perform this kernel
// persistently, only based on buffers that must be persistent, and based on the
// maximum of all minimum size requirement. i.e. if must be persistent, only
// hold persistent dimension.
TORCH_CUDA_CU_API PersistentBufferSizeReturn persistentBufferSize(
    Fusion* fusion,
    SchedulerRuntimeInfo& runtime_info,
    PersistentBufferInfo& persistent_buffers,
    HeuristicSummary* data_cache = nullptr);

// Returns a set of all iteration domains (in roots of tensors) that map to a
// trivial reduction
std::unordered_set<IterDomain*> getTrivialReductionMap(Fusion* fusion);

// Merges tensor view to the form:
// [IterationDomain, ReductionDomain, TrivialReductionDim0,
// TrivialReductionDim1, ...] Returns if <iteration dimensions, reduction
// dimensions>
std::pair<bool, bool> canonicalDimReduction(
    Fusion* fusion,
    TensorView* tv,
    bool schedule_3D = false);

// Return a list of tensor views that are outputs of reduction operations. If
// multiple outputs of an expression are found, only include one in the list
TORCH_CUDA_CU_API std::vector<TensorView*> getReductionTvs(
    Fusion* fusion,
    bool ignore_trivial = true);

// Returns a list of TensorViews that are the consumer tv for a view operation.
std::vector<TensorView*> getViewTVs(Fusion* fusion);

// Reset inputs and outputs to global memory, everything else to local.
void clearMemorySpace(Fusion* fusion);

// Returns cached after tensors of the fusion inputs if unrolled. Otherwise
// return empty vector.
TORCH_CUDA_CU_API std::vector<TensorView*> cacheInputs(
    Fusion* fusion,
    bool unroll);

// Returns the pairs of <cache of each fusion output, corresponding output> for
// all outputs.
TORCH_CUDA_CU_API std::vector<std::pair<TensorView*, TensorView*>>
cacheAndForkOutputs(Fusion* fusion, bool unroll);

// Ignores broadcast and reduction, returns iter domain in root domain that's
// "inner most". If this is an rfactored reduction domain, actually check the
// root domain, this is because the rfactored reduction tensorview has the
// vectorized dimension, but that means the rfactor domain could have reordered
// what we consider the "inner most" allocated position on it if we consider the
// rfactor dimension.
//
// If reduction tv and has rfactor return root domain, otherwise return rfactor
// domain.
IterDomain* innerMostRootDim(TensorView* tv);

// Uses a lot of logic from TransformPropagator in the implementation
class FindAllMappedDims {
 private:
  FindAllMappedDims(
      TensorView* from,
      IterDomain* starting_id,
      bool vectorize_pass);

 private:
  std::unordered_map<TensorView*, IterDomain*> mapped_ids;
  TensorView* starting_tv = nullptr;
  IterDomain* starting_id = nullptr;

 public:
  // Looks through fusion and finds all dims that match to the one provided in
  // the tensorview provided. Iter domain must be a root domain. If vectorize
  // pass, will only map dimensions if they're the inner most position. This is
  // important when projecting a dimension from an rfactor position to its root
  // position when mapping from consumer to producer. If vectorize_pass=true,
  // takes the rfactor dimensions that maps, projects it to the root domain, but
  // only following the inner most pass when encounting split/merge. For split
  // it will only propagate backwards if the mapped dimension is the inner
  // portion of the split. For merge, vectorize_pass doesn't make a dimension
  // and will propagate through the inner portion of the merge.
  static std::unordered_set<IterDomain*> from(
      TensorView* tv,
      IterDomain* id,
      bool vectorize_pass);
};

// Checks if tensor view has an iteration domain in vector dims in its inner
// most root position (excluding broadcast and reduction), and checks if it is a
// contiguous dimension
bool hasInnerDim(
    TensorView* tv,
    std::unordered_set<IterDomain*> vector_dims,
    bool should_vectorize);

// Returns all inputs and outputs that share the inner most dimension of the
// provided reference. If reference is an input it ignores reduction axes, will
// ignore all broadcast axes. If can_vectorize, will check contiguity for
// vectorization, otherwise it just checks it has that inner dim.
std::vector<TensorView*> getInputsOutputsWithInnerDim(
    TensorView* reference_tv,
    bool vectorize_pass);

// Structure to hold byte multiples for break points. I.e. if we have the
// tensors:
// T0[I0, I1] float
// T1[I0, I1] bool
// T2[I0]     half
// T3    [I1] double
// and a break point of 1 the multiples would be:
// lhs_multiple = 4 + 1 + 2 = 7
// rhs_multiple = 4 + 1 + 8 = 13
struct BroadcastMultiple {
  int64_t rhs_multiple = 0;
  int64_t lhs_multiple = 0;
};

// Returns a vector of counts, size = reference_tv->getRootDomain().size(), each
// entry [i] is the number of inputs/outputs that have a non-broadcast dimension
// mapped to the corresponding dimension in reference_tv. Count includes
// reference_tv if reference_tv is an input or output. Count is multiplied by
// data type size.
std::vector<BroadcastMultiple> getBroadcastMultiples(
    TensorView* reference_tv,
    DataType index_type);

//! Collect maximum vectorization word size of a tensor whose
//! innermost domain is leaf_merged_domain. Contig merging is taken
//! into account to expand vectorization if possible.
size_t collectMaxVectorizeSizeWithContigMerge(
    TensorView* tv,
    IterDomain* leaf_merged_domain,
    size_t max_word_size_in_byte,
    ExpressionEvaluator& expression_evaluator,
    DataType index_type);

namespace matmul_utils {
//! Utilities in this namespace facilitates scheduling matmul kernels with
//!  hierarchichal tiling specified in MatMulTileOptions.

//! Schedule utility for matmul prolog:
//!   Use all the threads on a CTA tile to load matmul operands
//!  into shared memory with the given vectorization word.
//! TODO:
//!  will need to add bank conflict removal swizzle in a follow up.
TORCH_CUDA_CU_API void scheduleContiguousVectorLoad(
    TensorView* tv,
    MatMulTileOptions tile,
    int vector_word,
    bool vectorize = true);

//! Schedule utility for mma output in matmul main loop:
//!  Realize the hierarchical tiling based on the given tiling options.
//! TODO: rewrite this one with makeTile
TORCH_CUDA_CU_API void scheduleWarpTileWithReduction(
    TensorView* tv,
    MatMulTileOptions tile);

//! Schedule utility for mma output in matmul main loop:
//!  Realize the hierarchical tiling based on the given tiling options
//! on consumers of mma ops in epilog.
//! TODO: remove this one eventually.
TORCH_CUDA_CU_API void scheduleWarpTileWithNoReduction(
    TensorView* tv,
    MatMulTileOptions tile);

//! Lower level primitive spliting inner iterdomains into tiles:
//! Eg.
//!  A[B,I0,I1,I2] -> makeTile({1,2,3})
//! Gives A[B, I0o, I1o, I2o, I0i(1), I1i(2), I2i(3)]
TORCH_CUDA_CU_API void makeTile(TensorView* tv, std::vector<int> tile_sizes);

//! Order the inner tile dimensions as the original order in
//!  root domain. Also putting broadcast domains on the left.
//! Eg. A[I0o,I1o,B2o,I0i,I1i,B2i] (root domain: I1,B,I0)
//! -> A[I0o, I1o, B2o, B2i, I1i, I0i]
//! This is used to facilitate data layout swizzling and
//!  defining vectorized loads.
TORCH_CUDA_CU_API void orderTiledConcreteIdAsRoot(TensorView* tv);

//! Orders the root id ordering of the given tv as
//! [Batch, Previous Reduction, M, N, K]
//!  for easier processing of later scheduling steps.
//!
//! This matching works on root domain only, and
//!  will throw if the tv has a leaf iterdomain that is
//!  not a root id.
TORCH_CUDA_CU_API void canonicalizeMmaTvOrdering(TensorView* tv);

} // namespace matmul_utils

//! Propagate current transformations on from_tv up to the given
//!  position, to all tensorviews on the owning fusion that has
//!  a connection with `from_tv` on the fusion graph.
TORCH_CUDA_CU_API void transformPropagateToAllFrom(
    TensorView* from_tv,
    int pos);

//! A type of custom transform propagator that propagates iterdomain
//!  transforms from a source tv to all tvs that are selected
//!  using a "direction" and a "boundary".
//!
//! The propagation model always assumes a `from_tv`, a `direction` and a
//! `boundary`.
//!
//! This propagator will only transform producers and consumers
//! of `from_tv`, and all propagation modes **require** a boundary to be
//! specified to signify where the propagation should stop.
//!
//! There are currently three modes of propagation: forward, backward and
//! both-way, see comment on the interface functions for details.
struct TORCH_CUDA_CU_API BoundedDirectionalTransformPropagator {
  //! Custom option container for configuring
  //!  the transform propagation actions.
  //! All option values default to false unless
  //!  the corresponding setter is called.
  struct Options {
    //! If true, the transform propagator will
    //!   also propagate parallel types from
    //!   `from_tv` to all selected tvs.
    bool propagate_parallel_type = false;

    //! If true, the specified boundary tvs
    //!  will also be replayed as `from_tv`.
    //!  If false, they will not be affected
    //!  by the propagation pass.
    bool transform_boundary = false;

    //! Sets the position boundary in parallel
    //!  type propagation, see comment on
    //!  scheduler_utils::parallelizeAllLike.
    //! Only used if propagate_parallel_type==true.
    int parallel_propagation_pos = -1;

    //! Setter for enabling parallel type
    //!  propagation. see comment on the variable.
    //!
    //! \param up_to_pos, sets the parallel type
    //!  propagation boundary. see comment on
    //!  scheduler_utils::parallelizeAllLike.
    Options propagateParallelType(int up_to_pos = -1) {
      propagate_parallel_type = true;
      parallel_propagation_pos = up_to_pos;
      return *this;
    }

    //! Setter for enabling propagation to
    //!  boundary tvs. see comment on the variable
    Options propagateToBoundary() {
      transform_boundary = true;
      return *this;
    }
  };

  //! Replay transforms from tensorview `from`
  //!  to the tensorviews that are consumers
  //!  of boundary tensorviews in `to` and producers of `from`.
  static void backward(
      TensorView* from,
      int pos,
      std::vector<TensorView*> to,
      c10::optional<Options> options = c10::nullopt);

  //! Replay transforms from tensorview `from`
  //! to the tensorviews that are producers
  //!  of boundary tensorviews in `to` and consumers of `from`.
  static void forward(
      TensorView* from,
      int pos,
      std::vector<TensorView*> to,
      c10::optional<Options> options = c10::nullopt);

  //! Replay transforms from tensorview `from`
  //!  to all the tensorviews that are consumers
  //!  of tensorviews in `backward_to` and producers
  //!  of tensorviews in `forward_to` while being
  //!  either a producer or a consumer of tensorview `from`.
  static void bothWays(
      TensorView* from,
      int pos,
      std::vector<TensorView*> backward_to,
      std::vector<TensorView*> forward_to,
      c10::optional<Options> options = c10::nullopt);

 private:
  //! Utility function:
  //!  Will realize the transform propagation to the
  //! tensorview's in `included_tvs`.
  //!  Assumes that all tvs in included_tvs are either
  //! a producer or a consumer of from_tv.
  static void propagate(
      TensorView* from_tv,
      int pos,
      std::unordered_set<TensorView*> included_tvs,
      Options options);
};

} // namespace scheduler_utils
} // namespace cuda
} // namespace fuser
} // namespace jit
} // namespace torch
