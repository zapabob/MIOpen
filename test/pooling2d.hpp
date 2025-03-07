/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (c) 2019 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *******************************************************************************/

#include <gtest/gtest.h>
#include "pooling_common.hpp"

#define WORKAROUND_ISSUE_1670 1
#define TEST_GET_INPUT_TENSOR 0

template <class T>
struct pooling2d_driver : pooling_driver<T>
{
private:
    using U = typename std::vector<int>;
    std::vector<U> get_2d_pooling_input_shapes()
    {
        return {{1, 19, 1024, 2048},
                {10, 3, 32, 32},
                {5, 32, 8, 8},
                {2, 1024, 12, 12},
                {4, 3, 231, 231},
                {8, 3, 227, 227},
                {1, 384, 13, 13},
                {1, 96, 27, 27},
                {2, 160, 7, 7},
                {1, 192, 256, 512},
                {2, 192, 28, 28},
                {1, 832, 64, 128},
                {1, 256, 56, 56},
                {4, 3, 224, 224},
                {2, 64, 112, 112},
                {2, 608, 4, 4},
                {1, 2048, 11, 11},
                {1, 16, 4096, 4096}};
    }

    // Dataset 1 is intended for testing of asymmetric configs.
    std::vector<U> get_2d_pooling_input_shapes_minimal() { return {{1, 4, 4, 4}}; }

    // Dataset 2 is intended for testing of configs with wide window.
    std::vector<U> get_2d_pooling_input_shapes_wide()
    {
        return {{1, 3, 255, 255}, {2, 3, 227, 227}, {1, 7, 127, 127}, {1, 1, 410, 400}};
    }

public:
    pooling2d_driver() : pooling_driver<T>()
    {
#if TEST_GET_INPUT_TENSOR
        std::set<U> in_dim_set = get_inputs(this->batch_factor);
        std::vector<U> in_dim_vec(in_dim_set.begin(), in_dim_set.end());
        this->add(this->in_shape, "input", this->generate_data(in_dim_vec, {16, 32, 8, 8}));
#else
        this->add(
            this->in_shape,
            "input",
            this->template generate_multi_data_limited<U>({get_2d_pooling_input_shapes(),
                                                           get_2d_pooling_input_shapes_minimal(),
                                                           get_2d_pooling_input_shapes_wide()},
                                                          9));
#endif
        this->add(this->lens,
                  "lens",
                  this->template generate_multi_data<U>(
                      {{{2, 2}, {3, 3}},         //
                       {{2, 2}, {1, 2}, {2, 1}}, //
                       {{35, 35}, {100, 100}, {255, 255}, {410, 400}}}));
        this->add(this->strides,
                  "strides",
                  this->template generate_multi_data<U>({{{2, 2}, {1, 1}},                 //
                                                         {{1, 1}, {2, 1}, {1, 2}, {2, 2}}, //
                                                         {{1, 1}}}));
        // clang-format off
        this->add(this->pads, "pads", this->template generate_multi_data<U>({
            {{0, 0}, {1, 1}}, //
#if WORKAROUND_ISSUE_1670
            {{0, 0}}, //
#else
            {{0, 0}, {0, 1}, {1, 0}, {1, 1}}, //
#endif
            {{0, 0}}}));
        // clang-format on
        this->add(this->wsidx, "wsidx", this->generate_data({0, 1}));
    }
};

class Pooling2dFloat : public testing::TestWithParam<std::vector<std::string>>
{
};

class Pooling2dHalf : public testing::TestWithParam<std::vector<std::string>>
{
};
