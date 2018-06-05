#include "CL/cl.hpp"
#include <string>
#include <functional>
#include <opencl_manager.h>
#include <random>
#include <iostream>
#include <cassert>

template<typename T>
void print_vector(std::vector<T> vector)
{
    std::cout << "[ ";
    for (auto& it : vector)
    {
        std::cout << it << " ";
    }
    std::cout << "]" << std::endl;
}

void random_fill_vector(const int size, std::vector<int>& v)
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, size);
    generate_n(back_inserter(v), size, bind(dist, gen));
}

void sequential_fill_vector(const int size, std::vector<int>& v)
{
    v.reserve(size);
    for (auto i = 0; i < size; ++i)
    {
        v.push_back(i);
    }
}

//scanl (+) 0 [1..5]
// [0,1,3,6,10,15]
std::vector<int> sequential_scan(std::vector<int> input)
{

    auto result = std::vector<int>{ 0 };
    for (auto& it : input)
    {
        result.emplace_back(result.back() + it);
    }
    return result;
}

void gpu_prefixsum(cl::Context& context, cl::CommandQueue& queue, cl::Kernel& kernel, std::vector<int>& input, std::vector<int>& output)
{
    const auto input_buffer_size = input.size() * sizeof(int);
    const auto output_buffer_size = output.size() * sizeof(int);

    const auto input_buffer = cl::Buffer(context, CL_MEM_READ_ONLY, input_buffer_size);
    const auto output_buffer = cl::Buffer(context, CL_MEM_READ_WRITE, output_buffer_size);

    queue.enqueueWriteBuffer(input_buffer, CL_TRUE, 0, input_buffer_size, input.data());

    kernel.setArg(0, input_buffer);
    kernel.setArg(1, output_buffer);

    const int local_size = sizeof(int) * input.size();
    kernel.setArg(2, cl::LocalSpaceArg(cl::Local(local_size)));
    kernel.setArg(3, cl::LocalSpaceArg(cl::Local(local_size)));

    const auto offset = cl::NDRange(0);
    const auto local = cl::NDRange(32);
    const auto global = cl::NDRange(input.size());

    const auto rv = queue.enqueueNDRangeKernel(kernel, offset, global, local);
    if (rv != CL_SUCCESS)
    {
        throw std::runtime_error("Could not enqueue kernel. Return value was:  " + std::to_string(rv));
    }

    auto event = cl::Event{};
    queue.enqueueReadBuffer(output_buffer, CL_TRUE, 0, output_buffer_size, &output[0], nullptr, &event);

    queue.finish();
    event.wait();
}

const auto group_size = 256;

size_t round_for_block(size_t val) {
    auto res = val;

    if (val % group_size != 0) {
        res += group_size - val % group_size;
    }

    return res;
}

void gpu_workefficient_prefixsum(cl::Context& context, cl::CommandQueue& queue, cl::Kernel& kernel, std::vector<int>& input, std::vector<int>& output, const opencl_manager& manager)
{
    const auto input_buffer_size = input.size() * sizeof(int);
    const auto output_buffer_size = output.size() * sizeof(int);

    const auto input_buffer = cl::Buffer(context, CL_MEM_READ_ONLY, input_buffer_size);
    const auto output_buffer = cl::Buffer(context, CL_MEM_READ_WRITE, output_buffer_size);

    auto result = queue.enqueueWriteBuffer(input_buffer, CL_TRUE, 0, input_buffer_size, input.data());
    result = queue.finish();

    std::function<void(const cl::Buffer&, const cl::Buffer&, std::size_t)> scan;

    scan = [&context, &queue, &kernel, &manager, &scan](const cl::Buffer& input_buffer, const cl::Buffer& output_buffer, const std::size_t array_size)
    {
        auto range = round_for_block(array_size);
        const auto group_count = range / group_size;
        constexpr int local_size = sizeof(int) * group_size;

        auto group_sums_buffer = cl::Buffer(context, CL_MEM_READ_WRITE, sizeof(int) * group_count);

        if (array_size <= group_size)
        {
            kernel.setArg(0, input_buffer);
            kernel.setArg(1, output_buffer);
            kernel.setArg(2, group_sums_buffer);
            kernel.setArg(3, cl::LocalSpaceArg(cl::Local(local_size)));
            kernel.setArg(4, (int)array_size);

            auto event = cl::Event{};
            const auto rv = queue.enqueueNDRangeKernel(kernel, cl::NullRange, cl::NDRange(range), cl::NDRange(group_size), nullptr, &event);
            if (rv != CL_SUCCESS)
            {
                throw std::runtime_error("Could not enqueue kernel. Return value was:  " + std::to_string(rv));
            }
            auto result = event.wait();
        }
        else
        {
            cl::Buffer scan_output_buffer(context, CL_MEM_READ_WRITE, sizeof(int) * array_size);
            cl::Buffer add_output_buffer(context, CL_MEM_READ_WRITE, sizeof(int) * group_count);

            kernel.setArg(0, input_buffer);
            kernel.setArg(1, scan_output_buffer);
            kernel.setArg(2, group_sums_buffer);
            kernel.setArg(3, cl::LocalSpaceArg(cl::Local(local_size)));
            kernel.setArg(4, (int)array_size);

            auto event = cl::Event{};

            auto rv = queue.enqueueNDRangeKernel(kernel, cl::NullRange, cl::NDRange(range), cl::NDRange(group_size), nullptr, &event);
            if (rv != CL_SUCCESS)
            {
                throw std::runtime_error("Could not enqueue kernel. Return value was:  " + std::to_string(rv));
            }
            auto result = event.wait();

            scan(group_sums_buffer, add_output_buffer, group_count);

            //ADD
            auto add_kernel = manager.get_kernel("add_groups");

            //DEBUG
            std::vector<int> output(array_size);
            result = queue.enqueueReadBuffer(scan_output_buffer, CL_TRUE, 0, sizeof(int)* array_size, &output[0], nullptr, &event);
            event.wait();

            add_kernel.setArg(0, scan_output_buffer);
            add_kernel.setArg(1, output_buffer);
            add_kernel.setArg(2, add_output_buffer);

            event = cl::Event{};
            rv = queue.enqueueNDRangeKernel(add_kernel, cl::NullRange, cl::NDRange(range), cl::NDRange(group_size), nullptr, &event);
            if (rv != CL_SUCCESS)
            {
                throw std::runtime_error("Could not enqueue kernel. Return value was:  " + std::to_string(rv));
            }
            result = event.wait();
        }
    };

    scan(input_buffer, output_buffer, input.size());

    auto event = cl::Event{};
    result = queue.enqueueReadBuffer(output_buffer, CL_TRUE, 0, output_buffer_size, &output[0], nullptr, &event);
}

int main(int argc, char* argv[])
{
    try
    {

        auto open_cl = opencl_manager{};
        open_cl.compile_program("scan.cl");
        open_cl.load_kernel("single_workgroup_prefixsum");
        open_cl.load_kernel("blelloch_scan");
        open_cl.load_kernel("add_groups");
        open_cl.load_kernel("naive_parallel_prefixsum");

        //Fill test vector

        auto threads = open_cl.get_max_workgroup_size();
        auto items = threads;

        auto test = std::vector<int>{};
        sequential_fill_vector(items, test);
        auto result = sequential_scan(test);

        std::function<void(cl::Context& context, cl::CommandQueue& queue, cl::Kernel& kernel, std::vector<int>&, std::vector<int>&)> fun = gpu_prefixsum;
        std::function<void(cl::Context& context, cl::CommandQueue& queue, cl::Kernel& kernel, std::vector<int>&, std::vector<int>&, opencl_manager&)> workefficient_scan = gpu_workefficient_prefixsum;

        auto output = std::vector<int>(test.size());
        //open_cl.execute_kernel("single_workgroup_prefixsum", fun, test, output);
        open_cl.execute_kernel("blelloch_scan", workefficient_scan, test, output, open_cl);

        //open_cl.execute_kernel("naive_parallel_prefixsum", fun, test, output);
        //output.insert(output.begin(), 0);//only for naive because it is a non inclusive scan

        for (auto i = 0; i < test.size(); ++i)
        {
            if (result[i] != output[i])
            {
                std::cout << "At pos " << i << " Result was: " << output[i] << " Should be: " << result[i] << std::endl;

                std::getchar();
                return -1;
            }
        }
        std::cout << "GPU Result OK. " << std::endl;
    }
    catch (std::runtime_error ex)
    {
        std::cout << ex.what() << std::endl;
    }

    std::getchar();
    return 0;
}
