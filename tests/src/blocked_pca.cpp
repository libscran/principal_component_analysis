#include <gtest/gtest.h>

#include "compare_pcs.h"
#include "utils.h"
#include "simulate_vector.h"

#include "tatami/tatami.hpp"

#include "blocked_pca.hpp"
#include "simple_pca.hpp"

TEST(ResidualWrapperTest, EigenDense) {
    size_t NR = 30, NC = 10, NB = 3;
    auto block = generate_blocks(NR, NB);

    Eigen::MatrixXd thing(NR, NC);  
    std::mt19937_64 rng;
    std::normal_distribution<> dist;
    for (size_t i = 0; i < NR; ++i) {
        for (size_t j = 0; j < NC; ++j) {
            thing(i, j) = dist(rng);
        }
    }

    Eigen::MatrixXd centers(NB, NC);
    for (size_t i = 0; i < NB; ++i) {
        for (size_t j = 0; j < NC; ++j) {
            centers(i, j) = dist(rng);
        }
    }

    scran::blocked_pca::internal::ResidualWrapper<decltype(thing), int, Eigen::MatrixXd, Eigen::VectorXd> blocked(thing, block.data(), centers);
    auto realized = blocked.template realize<Eigen::MatrixXd>();

    // Trying in the normal orientation.
    {
        Eigen::VectorXd rhs(NC);
        for (size_t i = 0; i < NC; ++i) {
            rhs[i] = dist(rng);
        }

        Eigen::VectorXd prod1(NR);
        auto wrk = blocked.workspace();
        blocked.multiply(rhs, wrk, prod1);

        Eigen::MatrixXd prod2 = realized * rhs;
        compare_almost_equal(prod1, prod2);
    }

    // Trying in the transposed orientation.
    {
        Eigen::VectorXd rhs(NR);
        for (size_t i = 0; i < NR; ++i) {
            rhs[i] = dist(rng);
        }

        Eigen::VectorXd tprod1(NC);
        auto wrk = blocked.adjoint_workspace();
        blocked.adjoint_multiply(rhs, wrk, tprod1);

        Eigen::MatrixXd tprod2 = realized.adjoint() * rhs;
        compare_almost_equal(tprod1, tprod2);
    }
}

TEST(ResidualWrapperTest, CustomSparse) {
    size_t NR = 30, NC = 10, NB = 3;
    auto block = generate_blocks(NR, NB);

    std::vector<double> values;
    std::vector<int> indices;
    std::vector<size_t> ptrs(NC + 1);

    std::mt19937_64 rng;
    std::normal_distribution<> ndist;
    std::uniform_real_distribution<> udist(0,1);
    Eigen::MatrixXd ref(NR, NC);  
    ref.setZero();

    for (size_t c = 0; c < NC; ++c) {
        for (size_t r = 0; r < NR; ++r) {
            if (udist(rng) < 0.2) {
                auto val = ndist(rng);
                ref(r, c) = val;
                values.push_back(val);
                indices.push_back(r);
                ++ptrs[c+1];
            }
        }
    }

    for (size_t i = 0; i < NC; ++i) {
        ptrs[i+1] += ptrs[i];
    }

    Eigen::MatrixXd centers(NB, NC);
    for (size_t i = 0; i < NB; ++i) {
        for (size_t j = 0; j < NC; ++j) {
            centers(i, j) = ndist(rng);
        }
    }

    irlba::ParallelSparseMatrix thing(NR, NC, std::move(values), std::move(indices), std::move(ptrs), /* column_major = */ true, 1);
    scran::blocked_pca::internal::ResidualWrapper<decltype(thing), int, Eigen::MatrixXd, Eigen::VectorXd> blocked(thing, block.data(), centers);
    auto realized = blocked.template realize<Eigen::MatrixXd>();

    // Checking that the dense reference matches up.
    {
        scran::blocked_pca::internal::ResidualWrapper<decltype(thing), int, Eigen::MatrixXd, Eigen::VectorXd> blockedref(thing, block.data(), centers);
        auto realizedref = blockedref.template realize<Eigen::MatrixXd>();

        for (Eigen::Index i = 0; i < realizedref.cols(); ++i) {
            Eigen::VectorXd refcol = realizedref.col(i);
            Eigen::VectorXd obscol = realized.col(i);
            expect_equal_vectors(refcol, obscol);
        }
    }

    // Trying in the normal orientation.
    {
        Eigen::VectorXd rhs(NC);
        for (size_t i = 0; i < NC; ++i) {
            rhs[i] = ndist(rng);
        }

        Eigen::VectorXd prod1(NR);
        auto wrk = blocked.workspace();
        blocked.multiply(rhs, wrk, prod1);

        Eigen::MatrixXd prod2 = realized * rhs;
        compare_almost_equal(prod1, prod2);
    }

    // Trying in the transposed orientation.
    {
        Eigen::VectorXd rhs(NR);
        for (size_t i = 0; i < NR; ++i) {
            rhs[i] = ndist(rng);
        }

        Eigen::VectorXd tprod1(NC);
        auto wrk = blocked.adjoint_workspace();
        blocked.adjoint_multiply(rhs, wrk, tprod1);

        Eigen::MatrixXd tprod2 = realized.adjoint() * rhs;
        compare_almost_equal(tprod1, tprod2);
    }
}

/******************************************/

class BlockedPcaTestCore {
protected:
    inline static std::shared_ptr<tatami::NumericMatrix> dense_row;

    static void assemble() {
        size_t nr = 121, nc = 155;
        auto vec = simulate_sparse_vector<double>(nr * nc, 0.1, /* lower = */ -10, /* upper = */ 10, /* seed = */ 123456);
        dense_row.reset(new tatami::DenseRowMatrix<double, int>(nr, nc, std::move(vec)));
        return;
    }
};

/******************************************/

class BlockedPcaBasicTest : public ::testing::TestWithParam<std::tuple<bool, bool, int, int, int> >, public BlockedPcaTestCore {
protected:
    inline static std::shared_ptr<tatami::NumericMatrix> dense_column, sparse_row, sparse_column;

    static void SetUpTestSuite() {
        assemble();
        dense_column = tatami::convert_to_dense(dense_row.get(), false);
        sparse_row = tatami::convert_to_compressed_sparse(dense_row.get(), true);
        sparse_column = tatami::convert_to_compressed_sparse(dense_row.get(), false);
    }
};

TEST_P(BlockedPcaBasicTest, BasicConsistency) {
    auto param = GetParam();
    bool scale = std::get<0>(param);
    bool use_resids = std::get<1>(param);
    int rank = std::get<2>(param);
    int nblocks = std::get<3>(param);
    int nthreads = std::get<4>(param);

    auto block = generate_blocks(dense_row->ncol(), nblocks);

    scran::blocked_pca::Options opts;
    opts.scale = scale;
    opts.components_from_residuals = use_resids;
    opts.block_weight_policy = scran::block_weights::Policy::NONE;
    auto ref = scran::blocked_pca::compute(dense_row.get(), block.data(), rank, opts);

    if (nthreads == 1) {
        EXPECT_EQ(ref.components.rows(), rank);
        EXPECT_EQ(ref.components.cols(), dense_row->ncol());
        EXPECT_EQ(ref.variance_explained.size(), rank);

        are_pcs_centered(ref.components);
        EXPECT_TRUE(ref.total_variance >= std::accumulate(ref.variance_explained.begin(), ref.variance_explained.end(), 0.0));

        // Total variance makes sense. Remember, this doesn't consider the
        // loss of d.f. from calculation of the block means.
        if (scale) {
            EXPECT_FLOAT_EQ(dense_row->nrow(), ref.total_variance);
        } else {
            auto collected = fragment_matrices_by_block(dense_row, block, nblocks);

            double total_var = 0;
            for (int b = 0, end = collected.size(); b < end; ++b) {
                const auto& sub = collected[b];
                auto vars = tatami_stats::variances::by_row(sub.get());
                total_var += std::accumulate(vars.begin(), vars.end(), 0.0) * (sub->ncol() - 1);
            }

            EXPECT_FLOAT_EQ(total_var / (dense_row->ncol() - 1), ref.total_variance);
        }

    } else {
        opts.num_threads = nthreads;
        auto res1 = scran::blocked_pca::compute(dense_row.get(), block.data(), rank, opts);

        // Results should be EXACTLY the same with parallelization.
        EXPECT_EQ(ref.components, res1.components);
        EXPECT_EQ(ref.variance_explained, res1.variance_explained);
        EXPECT_EQ(ref.total_variance, res1.total_variance);
    }

    // Checking that we get more-or-less the same results. 
    auto res2 = scran::blocked_pca::compute(dense_column.get(), block.data(), rank, opts);
    expect_equal_pcs(ref.components, res2.components);
    expect_equal_vectors(ref.variance_explained, res2.variance_explained);
    EXPECT_FLOAT_EQ(ref.total_variance, res2.total_variance);

    auto res3 = scran::blocked_pca::compute(sparse_row.get(), block.data(), rank, opts);
    expect_equal_pcs(ref.components, res3.components);
    expect_equal_vectors(ref.variance_explained, res3.variance_explained);
    EXPECT_FLOAT_EQ(ref.total_variance, res3.total_variance);

    auto res4 = scran::blocked_pca::compute(sparse_column.get(), block.data(), rank, opts);
    expect_equal_pcs(ref.components, res4.components);
    expect_equal_vectors(ref.variance_explained, res4.variance_explained);
    EXPECT_FLOAT_EQ(ref.total_variance, res4.total_variance);

    // Checking that we get more-or-less the same results. 
    opts.realize_matrix = false;
    auto tres1 = scran::blocked_pca::compute(dense_row.get(), block.data(), rank, opts);
    expect_equal_pcs(ref.components, tres1.components);
    expect_equal_vectors(ref.variance_explained, tres1.variance_explained);
    EXPECT_FLOAT_EQ(ref.total_variance, tres1.total_variance);

    auto tres2 = scran::blocked_pca::compute(dense_column.get(), block.data(), rank, opts);
    expect_equal_pcs(ref.components, tres2.components);
    expect_equal_vectors(ref.variance_explained, tres2.variance_explained);
    EXPECT_FLOAT_EQ(ref.total_variance, tres2.total_variance);

    auto tres3 = scran::blocked_pca::compute(sparse_row.get(), block.data(), rank, opts);
    expect_equal_pcs(ref.components, tres3.components);
    expect_equal_vectors(ref.variance_explained, tres3.variance_explained);
    EXPECT_FLOAT_EQ(ref.total_variance, tres3.total_variance);

    auto tres4 = scran::blocked_pca::compute(sparse_column.get(), block.data(), rank, opts);
    expect_equal_pcs(ref.components, tres4.components);
    expect_equal_vectors(ref.variance_explained, tres4.variance_explained);
    EXPECT_FLOAT_EQ(ref.total_variance, tres4.total_variance);
}

TEST_P(BlockedPcaBasicTest, WeightedConsistency) {
    auto param = GetParam();
    bool scale = std::get<0>(param);
    bool use_resids = std::get<1>(param);
    int rank = std::get<2>(param);
    int nblocks = std::get<3>(param);
    int nthreads = std::get<4>(param);

    auto block = generate_blocks(dense_row->ncol(), nblocks);

    scran::blocked_pca::Options opts;
    opts.scale = scale;
    opts.components_from_residuals = use_resids;
    opts.block_weight_policy = scran::block_weights::Policy::EQUAL;
    auto ref = scran::blocked_pca::compute(dense_row.get(), block.data(), rank, opts);

    if (nthreads == 1) {
        are_pcs_centered(ref.components);
        EXPECT_TRUE(ref.total_variance >= std::accumulate(ref.variance_explained.begin(), ref.variance_explained.end(), 0.0));

        if (scale) {
            EXPECT_FLOAT_EQ(dense_row->nrow(), ref.total_variance);
        } else {
            auto collected = fragment_matrices_by_block(dense_row, block, nblocks);

            // Here, the 'variance' is really just the grand sum (across blocks) of
            // the sum (across cells) of the squared difference from the mean.
            double total_var = 0;
            for (int b = 0, end = collected.size(); b < end; ++b) {
                const auto& sub = collected[b];
                auto vars = tatami_stats::variances::by_row(sub.get());
                total_var += std::accumulate(vars.begin(), vars.end(), 0.0) * (sub->ncol() - 1) / sub->ncol();
            }

            EXPECT_FLOAT_EQ(total_var / (dense_row->ncol() - 1), ref.total_variance);
        }

    } else {
        opts.num_threads = nthreads;
        auto res1 = scran::blocked_pca::compute(dense_row.get(), block.data(), rank, opts);

        // Results should be EXACTLY the same with parallelization.
        EXPECT_EQ(ref.components, res1.components);
        EXPECT_EQ(ref.variance_explained, res1.variance_explained);
        EXPECT_EQ(ref.total_variance, res1.total_variance);
    }

    // Checking that we get more-or-less the same results. 
    auto res2 = scran::blocked_pca::compute(dense_column.get(), block.data(), rank, opts);
    expect_equal_pcs(ref.components, res2.components);
    expect_equal_vectors(ref.variance_explained, res2.variance_explained);
    EXPECT_FLOAT_EQ(ref.total_variance, res2.total_variance);

    auto res3 = scran::blocked_pca::compute(sparse_row.get(), block.data(), rank, opts);
    expect_equal_pcs(ref.components, res3.components);
    expect_equal_vectors(ref.variance_explained, res3.variance_explained);
    EXPECT_FLOAT_EQ(ref.total_variance, res3.total_variance);

    auto res4 = scran::blocked_pca::compute(sparse_column.get(), block.data(), rank, opts);
    expect_equal_pcs(ref.components, res4.components);
    expect_equal_vectors(ref.variance_explained, res4.variance_explained);
    EXPECT_FLOAT_EQ(ref.total_variance, res4.total_variance);

    // Checking that we get more-or-less the same results. 
    opts.realize_matrix = false;
    auto tres1 = scran::blocked_pca::compute(dense_row.get(), block.data(), rank, opts);
    expect_equal_pcs(ref.components, tres1.components);
    expect_equal_vectors(ref.variance_explained, tres1.variance_explained);
    EXPECT_FLOAT_EQ(ref.total_variance, tres1.total_variance);

    auto tres2 = scran::blocked_pca::compute(dense_column.get(), block.data(), rank, opts);
    expect_equal_pcs(ref.components, tres2.components);
    expect_equal_vectors(ref.variance_explained, tres2.variance_explained);
    EXPECT_FLOAT_EQ(ref.total_variance, tres2.total_variance);

    auto tres3 = scran::blocked_pca::compute(sparse_row.get(), block.data(), rank, opts);
    expect_equal_pcs(ref.components, tres3.components);
    expect_equal_vectors(ref.variance_explained, tres3.variance_explained);
    EXPECT_FLOAT_EQ(ref.total_variance, tres3.total_variance);

    auto tres4 = scran::blocked_pca::compute(sparse_column.get(), block.data(), rank, opts);
    expect_equal_pcs(ref.components, tres4.components);
    expect_equal_vectors(ref.variance_explained, tres4.variance_explained);
    EXPECT_FLOAT_EQ(ref.total_variance, tres4.total_variance);
}

INSTANTIATE_TEST_SUITE_P(
    BlockedPca,
    BlockedPcaBasicTest,
    ::testing::Combine(
        ::testing::Values(false, true), // to scale or not to scale?
        ::testing::Values(false, true), // to compute PCs from the residuals?
        ::testing::Values(2, 5, 10), // number of PCs to obtain
        ::testing::Values(1, 2, 3), // number of blocks
        ::testing::Values(1, 3) // number of threads
    )
);

/******************************************/

class BlockedPcaMoreTest : public ::testing::TestWithParam<std::tuple<bool, bool, int, int> >, public BlockedPcaTestCore {
protected:
    static void SetUpTestSuite() {
        assemble();
    }
};

TEST_P(BlockedPcaMoreTest, VersusSimple) {
    auto param = GetParam();
    bool scale = std::get<0>(param);
    bool use_resids = std::get<1>(param);
    int rank = std::get<2>(param);
    int nblocks = std::get<3>(param);
    auto block = generate_blocks(dense_row->ncol(), nblocks);

    scran::blocked_pca::Options opt;
    opt.scale = scale;
    opt.components_from_residuals = use_resids;
    opt.block_weight_policy = scran::block_weights::Policy::NONE;
    auto res1 = scran::blocked_pca::compute(dense_row.get(), block.data(), rank, opt);

    scran::simple_pca::Options refopt;
    refopt.scale = scale;

    if (nblocks == 1) {
        // Checking that we get more-or-less the same results
        // from the vanilla PCA algorithm in the absence of blocks.
        auto res2 = scran::simple_pca::compute(dense_row.get(), rank, refopt);

        expect_equal_pcs(res1.components, res2.components);
        expect_equal_vectors(res1.variance_explained, res2.variance_explained);
        EXPECT_FLOAT_EQ(res1.total_variance, res2.total_variance);
    } else {
        // Manually regressing things out.
        size_t nr = dense_row->nrow(), nc = dense_row->ncol();
        std::vector<double> regressed(nr * nc);
        for (int b = 0; b < nblocks; ++b) {
            std::vector<int> keep;
            for (size_t i = 0; i < block.size(); ++i) {
                if (block[i] == b) {
                    keep.push_back(i);
                }
            }

            if (keep.empty()) {
                continue;
            }

            auto sub = tatami::make_DelayedSubset<1>(dense_row, keep);
            auto center = tatami_stats::sums::by_row(sub.get());
            for (auto& x : center) {
                x /= keep.size();
            }

            auto ext = dense_row->dense_column();
            for (auto i : keep) {
                auto store = regressed.data() + i * static_cast<size_t>(nr);
                auto ptr = ext->fetch(i, store);
                tatami::copy_n(ptr, nr, store);
                for (auto x : center) {
                    *store -= x;
                    ++store;
                }
            }
        }

        tatami::DenseColumnMatrix<double, int> refmat(nr, nc, std::move(regressed));
        auto res2 = scran::simple_pca::compute(&refmat, rank, refopt);

        expect_equal_rotation(res1.rotation, res2.rotation);
        expect_equal_vectors(res1.variance_explained, res2.variance_explained);
        EXPECT_FLOAT_EQ(res1.total_variance, res2.total_variance);

        if (use_resids) {
            expect_equal_pcs(res1.components, res2.components);
        } else {
            are_pcs_centered(res1.components);

            Eigen::MatrixXd payload(nc, nr);
            tatami::convert_to_dense(dense_row.get(), true, payload.data());

            Eigen::MatrixXd rotation = res2.rotation;
            if (scale) {
                rotation.array().colwise() /= res2.scale.array();
            }

            Eigen::MatrixXd expected = (payload * rotation).adjoint();
            Eigen::VectorXd means = expected.array().rowwise().sum() / nc;
            expected.colwise() -= means;
            expect_equal_pcs(res1.components, expected);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    BlockedPca,
    BlockedPcaMoreTest,
    ::testing::Combine(
        ::testing::Values(false, true), // to scale or not to scale?
        ::testing::Values(false, true), // to compute PCs from the residuals?
        ::testing::Values(2, 5, 10), // number of PCs to obtain
        ::testing::Values(1, 2, 3) // number of blocks
    )
);

/******************************************/

class BlockedPcaWeightedTest : public ::testing::TestWithParam<std::tuple<bool, bool, int, int, int> > {};

TEST_P(BlockedPcaWeightedTest, VersusReference) {
    auto param = GetParam();
    bool scale = std::get<0>(param);
    bool use_resids = std::get<1>(param);
    int rank = std::get<2>(param);
    int nblocks = std::get<3>(param);
    int nthreads = std::get<4>(param);

    std::vector<std::shared_ptr<tatami::NumericMatrix> > components;
    std::vector<int> blocking;
    size_t nr = 80, nc = 50;
    for (int b = 0; b < nblocks; ++b) {
        auto vec = simulate_vector(nr * nc, /* lower = */ -10.0, /* upper = */ 10.0, /* seed = */ b + 100);
        components.emplace_back(new tatami::DenseRowMatrix<double, int>(nr, nc, std::move(vec)));
        blocking.insert(blocking.end(), nc, b);
    }

    scran::blocked_pca::Options base_opt;
    base_opt.scale = scale;
    base_opt.components_from_residuals = use_resids;
    base_opt.num_threads = nthreads;

    auto combined = tatami::make_DelayedBind(components, false);
    auto ref = scran::blocked_pca::compute(combined.get(), blocking.data(), rank, [&]{
        auto opt = base_opt;
        opt.num_threads = 1; // using a single thread for a consistent reference.
        opt.block_weight_policy = scran::block_weights::Policy::NONE;
        return opt;
    }());

    // Some adjustment is required to adjust for the global scaling.
    ref.components.array() /= ref.components.norm();
    ref.variance_explained.array() /= ref.total_variance;

    {
        // Checking that we get more-or-less the same results with equiweighting
        // when all blocks are of the same size.
        auto opt = base_opt;
        opt.block_weight_policy = scran::block_weights::Policy::EQUAL;

        auto res1 = scran::blocked_pca::compute(combined.get(), blocking.data(), rank, opt);
        res1.components.array() /= res1.components.norm();
        expect_equal_pcs(ref.components, res1.components);

        res1.variance_explained.array() /= res1.total_variance;
        expect_equal_vectors(ref.variance_explained, res1.variance_explained);
    }

    // Manually adding more instances of a block.
    auto expanded_block = blocking;
    for (int b = 0; b < nblocks; ++b) {
        for (int b0 = 0; b0 < b; ++b0) {
            components.push_back(components[b]);
        }
        expanded_block.insert(expanded_block.end(), b * components[b]->ncol(), b);
    }
    auto expanded = tatami::make_DelayedBind(components, false);

    // With a large size cap, each block is weighted by its size,
    // which is equivalent to the total absence of re-weighting.
    {
        auto opt = base_opt;
        opt.block_weight_policy = scran::block_weights::Policy::VARIABLE;
        opt.variable_block_weight_parameters.upper_bound = 1000000;
        auto res2 = scran::blocked_pca::compute(expanded.get(), expanded_block.data(), rank, opt);

        opt.block_weight_policy = scran::block_weights::Policy::NONE;
        auto ref2 = scran::blocked_pca::compute(expanded.get(), expanded_block.data(), rank, opt);

        ref2.components.array() /= ref2.components.norm();
        res2.components.array() /= res2.components.norm();
        expect_equal_pcs(ref2.components, res2.components);

        ref2.variance_explained.array() /= ref2.total_variance;
        res2.variance_explained.array() /= res2.total_variance;
        expect_equal_vectors(ref2.variance_explained, res2.variance_explained);
    }

    // We turn down the size cap so that every batch is equally weighted,
    // despite the imbalance. This should give us the same coordinates as
    // when the PCA was performed with batches of equal size. 
    {
        auto opt = base_opt;
        opt.block_weight_policy = scran::block_weights::Policy::VARIABLE;
        opt.variable_block_weight_parameters.upper_bound = 0;
        auto res2 = scran::blocked_pca::compute(expanded.get(), expanded_block.data(), rank, opt);

        // Mocking up the expected results.
        Eigen::MatrixXd expanded_pcs(rank, expanded->ncol());
        expanded_pcs.leftCols(combined->ncol()) = ref.components;
        size_t host_counter = 0, dest_counter = combined->ncol();
        for (int b = 0; b < nblocks; ++b) {
            size_t nc = components[b]->ncol();
            for (int b0 = 0; b0 < b; ++b0) {
                expanded_pcs.middleCols(dest_counter, nc) = ref.components.middleCols(host_counter, nc);
                dest_counter += nc; 
            }
            host_counter += nc;
        }

        Eigen::VectorXd recenters = expanded_pcs.rowwise().sum();
        recenters /= expanded_pcs.cols();
        for (size_t i = 0, end = expanded_pcs.cols(); i < end; ++i) {
            expanded_pcs.col(i) -= recenters;
        }

        // Comparing the results.
        expanded_pcs.array() /= expanded_pcs.norm();
        res2.components.array() /= res2.components.norm();
        expect_equal_pcs(expanded_pcs, res2.components);

        res2.variance_explained.array() /= res2.total_variance;
        expect_equal_vectors(ref.variance_explained, res2.variance_explained);
    }
}

INSTANTIATE_TEST_SUITE_P(
    BlockedPca,
    BlockedPcaWeightedTest,
    ::testing::Combine(
        ::testing::Values(false, true), // to scale or not to scale?
        ::testing::Values(false, true), // to compute PCs from the residuals?
        ::testing::Values(2, 5, 10), // number of PCs to obtain
        ::testing::Values(2, 3), // number of blocks
        ::testing::Values(1, 3) // number of threads
    )
);
