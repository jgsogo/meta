/**
 * @file parallel_test.h
 * @author Sean Massung
 */

#ifndef _META_PARALLEL_TEST_H_
#define _META_PARALLEL_TEST_H_

#include <cmath>
#include <algorithm>
#include <numeric>

#include "util/time.h"
#include "parallel/parallel_for.h"
#include "parallel/thread_pool.h"

namespace meta
{
namespace testing
{

template <class Type>
void hard_func(Type& x)
{
    x = std::sin(x) + std::exp(std::cos(x)) / std::exp(std::sin(x));
}

template <class Type>
void easy_func(Type& x)
{
    if (x != 1.0)
        FAIL("vector contents was modified");
    --x;
}

/**
 * Assumes multi-core machine...
 */
int test_speed(std::vector<double>& v)
{
    return testing::run_test("parallel-speed", 10, [&]()
    {

        std::iota(v.begin(), v.end(), 0);
        auto serial_time = common::time([&]()
            { std::for_each(v.begin(), v.end(), hard_func<double>); });

        std::iota(v.begin(), v.end(), 0);
        auto parallel_time = common::time([&]()
            { parallel::parallel_for(v.begin(), v.end(), hard_func<double>); });

        ASSERT(parallel_time.count() < serial_time.count());
    });
}

int test_correctness(std::vector<double>& v)
{
    // this makes sure every single element is touched exactly once
    return testing::run_test("parallel-correctness", 10, [&]()
    {
        std::fill(v.begin(), v.end(), 1.0);
        std::mutex mtx;
        parallel::parallel_for(v.begin(), v.end(), easy_func<double>);
        ASSERT(std::accumulate(v.begin(), v.end(), 0.0) == 0.0);
    });
}

int test_threadpool()
{
    return testing::run_test("parallel-thread-pool", 5, []()
    {
        parallel::thread_pool pool{};
        std::vector<std::future<size_t>> futures;

        for (size_t i = 0; i < 16; ++i)
            futures.emplace_back(pool.submit_task([]() { return size_t{1}; }));

        size_t sum = 0;
        for (auto& fut : futures)
        {
            auto val = fut.get();
            ASSERT(val == 1);
            sum += val;
        }

        ASSERT(sum == 16);
    });
}

int parallel_tests()
{
    size_t n = 10000000;
    std::vector<double> v(n);
    int num_failed = 0;
    num_failed += test_speed(v);
    num_failed += test_correctness(v);
    num_failed += test_threadpool();
    testing::report(num_failed);
    return num_failed;
}
}
}

#endif