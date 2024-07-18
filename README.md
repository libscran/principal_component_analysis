# Principal components analysis, duh

![Unit tests](https://github.com/libscran/scran_pca/actions/workflows/run-tests.yaml/badge.svg)
![Documentation](https://github.com/libscran/scran_pca/actions/workflows/doxygenate.yaml/badge.svg)
[![Codecov](https://codecov.io/gh/libscran/scran_pca/graph/badge.svg?token=qklLZtJSE9)](https://codecov.io/gh/libscran/scran_pca)

## Overview

As the name suggests, this repository implements functions to perform a PCA on the gene-by-cell expression matrix,
returning low-dimensional coordinates for each cell that can be used for efficient downstream analyses, e.g., clustering, visualization.
The code itself was originally derived from the [**scran**](https://bioconductor.org/packages/scran) and [**batchelor**](https://bioconductor.org/packages/batchelor) R packages
factored out into a separate C++ library for easier re-use.

## Quick start

Given a [`tatami::Matrix`](https://github.com/tatami-inc/tatami), the `scran_pca::simple_pca()` function will compute the PCA to obtain a low-dimensional representation of the cells:

```cpp
#include "scran_pca/scran_pca.hpp"

const tatami::Matrix<double, int>& mat = some_data_source();

// Take the top 20 PCs:
scran_pca::SimplePcaOptions opt;
auto res = scran_pca::simple_pca(mat, 20, opt);

res.components; // rows are PCs, columns are cells.
res.rotation; // rows are genes, columns correspond to PCs.
res.variance_explained; // one per PC, in decreasing order.
res.total_variance; // total variance in the dataset.
```

Advanced users can fiddle with the options:

```cpp
opt.scale = true;
opt.num_threads = 4;
opt.realize_matrix = false;
auto res2 = scran_pca::simple_pca(mat, 20, opt);
```

In the presence of multiple blocks, we can perform the PCA on the residuals after regressing out the blocking factor.
This ensures that the inter-block differences do not contribute to the first few PCs, instead favoring the representation of intra-block variation.

```cpp
std::vector<int> blocks = some_blocks();

scran_pca::BlockedPcaOptions bopt;
auto bres = scran_pca::blocked_pca(mat, blocks.data(), 20, bopt);

bres.components; // rows are PCs, columns are cells.
bres.center; // rows are blocks, columns are genes.
```

The components derived from the residuals will only be free of inter-block differences under certain conditions (equal population composition with a consistent shift between blocks).
If this is not the case, more sophisticated batch correction methods are required.
If those methods accept a low-dimensional representation for the cells as input, 
we can use `scran_pca::blocked_pca()` to obtain an appropriate matrix that focuses on intra-block variation without making assumptions about the inter-block differences:

```cpp
bopt.components_from_residuals = false;
auto bres2 = scran_pca::blocked_pca(mat, blocks.data(), 20, bopt);
```

Check out the [reference documentation](https://libscran.github.io/scran_pca) for more details.

## Building projects

### CMake with `FetchContent`

If you're using CMake, you just need to add something like this to your `CMakeLists.txt`:

```cmake
include(FetchContent)

FetchContent_Declare(
  scran_pca
  GIT_REPOSITORY https://github.com/libscran/scran_pca
  GIT_TAG master # or any version of interest
)

FetchContent_MakeAvailable(scran_pca)
```

Then you can link to **scran_pca** to make the headers available during compilation:

```cmake
# For executables:
target_link_libraries(myexe libscran::scran_pca)

# For libaries
target_link_libraries(mylib INTERFACE libscran::scran_pca)
```

### CMake with `find_package()`

```cmake
find_package(libscran_scran_pca CONFIG REQUIRED)
target_link_libraries(mylib INTERFACE libscran::scran_pca)
```

To install the library, use:

```sh
mkdir build && cd build
cmake .. -DSCRAN_PCA_TESTS=OFF
cmake --build . --target install
```

By default, this will use `FetchContent` to fetch all external dependencies.
If you want to install them manually, use `-DSCRAN_PCA_FETCH_EXTERN=OFF`.
See the tags in [`extern/CMakeLists.txt`](extern/CMakeLists.txt) to find compatible versions of each dependency.

### Manual

If you're not using CMake, the simple approach is to just copy the files in `include/` - either directly or with Git submodules - and include their path during compilation with, e.g., GCC's `-I`.
This requires the external dependencies listed in [`extern/CMakeLists.txt`](extern/CMakeLists.txt), which also need to be made available during compilation.
