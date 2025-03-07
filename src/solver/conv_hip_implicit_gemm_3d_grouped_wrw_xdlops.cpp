/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (c) 2023 Advanced Micro Devices, Inc.
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

#include <vector>
#include <cstdint>

#include <miopen/solver.hpp>
#include <miopen/generic_search.hpp>
#include <miopen/conv/wrw_invoke_params.hpp>
#include <miopen/solver/problem_description_interpreter.hpp>
#if MIOPEN_BACKEND_HIP && MIOPEN_USE_COMPOSABLEKERNEL
#include <miopen/solver/ck_utility_common.hpp>
#include <ck/library/tensor_operation_instance/gpu/grouped_convolution_backward_weight.hpp>
#endif
#include <miopen/solver/implicitgemm_ck_util.hpp>
MIOPEN_DECLARE_ENV_VAR_BOOL(MIOPEN_DEBUG_3D_CONV_IMPLICIT_GEMM_HIP_WRW_XDLOPS)

namespace miopen {
namespace solver {
namespace conv {

using ProblemDescription = miopen::conv::ProblemDescription;

#if MIOPEN_BACKEND_HIP && MIOPEN_USE_COMPOSABLEKERNEL
template <typename DataType>
using DeviceOpGWrw = ck::tensor_operation::device::DeviceGroupedConvBwdWeight<
    3,
    ck::tensor_layout::convolution::NDHWGC,
    ck::tensor_layout::convolution::GKZYXC,
    ck::tensor_layout::convolution::NDHWGK,
    DataType,
    DataType,
    DataType,
    ck::tensor_operation::element_wise::PassThrough,
    ck::tensor_operation::element_wise::PassThrough,
    ck::tensor_operation::element_wise::PassThrough>;

template <typename DataType>
using DeviceOpGWrwPtrs =
    ck::tensor_operation::device::instance::DeviceOperationInstanceFactory<DeviceOpGWrw<DataType>>;

namespace {

struct CKArgs
{
    CKArgs(const ProblemDescription& problem)
    {
        G  = ProblemInterpreter::GetGroupCountG(problem);
        N  = ProblemInterpreter::GetBatchN(problem);
        K1 = ProblemInterpreter::GetOutputChannelK(problem);
        C1 = ProblemInterpreter::GetInputChannelC(problem);
        C  = C1 / G; // Number of input Channel per group
        K  = K1 / G; // Number of output Channel per group
        Hi = ProblemInterpreter::GetInputHeightHi(problem);
        Wi = ProblemInterpreter::GetInputWidthWi(problem);
        Ho = ProblemInterpreter::GetOutputHeightHo(problem);
        Wo = ProblemInterpreter::GetOutputWidthWo(problem);
        Y  = ProblemInterpreter::GetFilterHeightY(problem);
        X  = ProblemInterpreter::GetFilterWidthX(problem);
        Di = ProblemInterpreter::GetInputDepthDi(problem);
        Do = ProblemInterpreter::GetOutputDepthDo(problem);
        Z  = ProblemInterpreter::GetFilterDepthZ(problem);

        input  = {G, N, C, Di, Hi, Wi};
        output = {G, N, K, Do, Ho, Wo};
        weight = {G, K, C, Z, Y, X};

        // miopen strides to CK strides
        // On a backward pass, problem.GetIn() means y(or out),
        // and problem.GetOut means x(or in)
        auto miopen_in_strides  = problem.GetOut().GetStrides();
        auto miopen_out_strides = problem.GetIn().GetStrides();
        auto miopen_wei_strides = problem.GetWeights().GetStrides();
        miopen_in_strides.insert(miopen_in_strides.begin(), C);
        miopen_out_strides.insert(miopen_out_strides.begin(), K);
        miopen_wei_strides.insert(miopen_wei_strides.begin(), K * miopen_wei_strides[0]);
        std::copy(miopen_in_strides.begin(), miopen_in_strides.end(), in_strides.begin());
        std::copy(miopen_out_strides.begin(), miopen_out_strides.end(), out_strides.begin());
        std::copy(miopen_wei_strides.begin(), miopen_wei_strides.end(), wei_strides.begin());

        strides  = {ProblemInterpreter::GetAdjustedConvolutionStrideD(problem),
                   ProblemInterpreter::GetAdjustedConvolutionStrideH(problem),
                   ProblemInterpreter::GetAdjustedConvolutionStrideW(problem)};
        dilation = {ProblemInterpreter::GetAdjustedConvolutionDilationD(problem),
                    ProblemInterpreter::GetAdjustedConvolutionDilationH(problem),
                    ProblemInterpreter::GetAdjustedConvolutionDilationW(problem)};
        lPadding = {ProblemInterpreter::GetInputLeftPadD(problem),
                    ProblemInterpreter::GetInputLeftPadH(problem),
                    ProblemInterpreter::GetInputLeftPadW(problem)};
        rPadding = {ProblemInterpreter::GetAdjustedInputRightPadD(problem),
                    ProblemInterpreter::GetAdjustedInputRightPadH(problem),
                    ProblemInterpreter::GetAdjustedInputRightPadW(problem)};
    }
    CKArgs(const CKArgs&) = default;
    CKArgs(CKArgs&&)      = default;
    CKArgs& operator=(const CKArgs&) = default;

    template <typename ConvPtr>
    auto MakeArgPtr(const ConvPtr& conv_ptr, ConstData_t x, Data_t dw, ConstData_t dy) const
    {
#if 0 // Leaving for debugging needs
        std::cout << "y ptr = " << dy << std::endl;
        std::cout << "w ptr = " << dw << std::endl;
        std::cout << "x ptr = " << x << std::endl;

        auto print_vec = [](const char* name, const auto& vec) {
            std::cout << name << " = [ ";
            for(const auto& v : vec)
            {
                std::cout << v << ", ";
            }
            std::cout << "]\n";
        };
#define PRINT_VEC(x) print_vec(#x, x);

        PRINT_VEC(output);
        PRINT_VEC(out_strides);
        PRINT_VEC(input);
        PRINT_VEC(in_strides);
        PRINT_VEC(weight);
        PRINT_VEC(wei_strides);
#endif
        return conv_ptr->MakeArgumentPointer(x,
                                             dw,
                                             dy,
                                             input,
                                             in_strides,
                                             weight,
                                             wei_strides,
                                             output,
                                             out_strides,
                                             strides,
                                             dilation,
                                             lPadding,
                                             rPadding,
                                             {},
                                             {},
                                             {},
                                             split_k);
    }

    template <typename ConvPtr>
    auto MakeArgPtr(const ConvPtr& conv_ptr, const ConvWrwTensors& tensors) const
    {
        return MakeArgPtr(conv_ptr, tensors.x, tensors.dw, tensors.dy);
    }

    template <typename ConvPtr>
    bool IsSupportedBy(const ConvPtr& conv_ptr) const
    {
        auto arg_ptr = MakeArgPtr(conv_ptr, nullptr, nullptr, nullptr);
        return conv_ptr->IsSupportedArgument(arg_ptr.get());
    }

    int G;
    int N;
    int K;
    int C;
    int C1;
    int K1;
    int Hi;
    int Wi;
    int Di;
    int Ho;
    int Wo;
    int Do;
    int Y;
    int X;
    int Z;
    ck::index_t split_k = 1;
    std::array<ck::index_t, 6> input;
    std::array<ck::index_t, 6> in_strides;
    std::array<ck::index_t, 6> output;
    std::array<ck::index_t, 6> out_strides;
    std::array<ck::index_t, 6> weight;
    std::array<ck::index_t, 6> wei_strides;
    std::array<ck::index_t, 3> strides;
    std::array<ck::index_t, 3> dilation;
    std::array<ck::index_t, 3> lPadding;
    std::array<ck::index_t, 3> rPadding;
};
} // namespace

template <typename DataType>
void PerformanceConfigHipImplicitGemm3DGroupWrwXdlops::Init(const ProblemDescription& problem)
{
    valid_kernels = FillValidKernelsIDs<DeviceOpGWrwPtrs<DataType>, CKArgs>(problem);
    index         = 0;
    kernel_id     = valid_kernels[index];
}

template <typename DataType>
bool PerformanceConfigHipImplicitGemm3DGroupWrwXdlops::CheckIsSupportCKArgs(
    const ProblemDescription& problem) const
{
    return IsCKArgsSupported<DeviceOpGWrwPtrs<DataType>, CKArgs>(problem, kernel_id);
}

template <typename DataType>
bool ConvHipImplicitGemm3DGroupWrwXdlops::CheckCKApplicability(
    const ProblemDescription& problem) const
{
    return IsCKApplicable<DeviceOpGWrwPtrs<DataType>, CKArgs>(problem);
}
#endif

void PerformanceConfigHipImplicitGemm3DGroupWrwXdlops::HeuristicInit(
    [[maybe_unused]] const ProblemDescription& problem)
{
    index     = 0;
    kernel_id = "";

#if MIOPEN_BACKEND_HIP && MIOPEN_USE_COMPOSABLEKERNEL
    switch(problem.GetInDataType())
    {
    case miopenHalf: Init<ck::half_t>(problem); break;
    case miopenFloat: Init<float>(problem); break;
    case miopenInt8: Init<int8_t>(problem); break;
    case miopenInt32:
    case miopenBFloat16:
    case miopenFloat8:
    case miopenBFloat8:
    case miopenDouble: break;
    }
#endif
}

bool PerformanceConfigHipImplicitGemm3DGroupWrwXdlops::SetNextValue(
    const ProblemDescription& problem)
{
    if(valid_kernels.empty())
    {
        HeuristicInit(problem);
        assert(!valid_kernels.empty());
        return true;
    }
    if((index + 1) < valid_kernels.size())
    {
        ++index;
        kernel_id = valid_kernels[index];
        return true;
    }
    else
        return false;
}

bool PerformanceConfigHipImplicitGemm3DGroupWrwXdlops::IsValidValue() const
{
    return index < valid_kernels.size();
}

bool PerformanceConfigHipImplicitGemm3DGroupWrwXdlops::IsValid(
    [[maybe_unused]] const ProblemDescription& problem) const
{
#if MIOPEN_BACKEND_HIP && MIOPEN_USE_COMPOSABLEKERNEL
    switch(problem.GetInDataType())
    {
    case miopenHalf: return CheckIsSupportCKArgs<ck::half_t>(problem);
    case miopenFloat: return CheckIsSupportCKArgs<float>(problem);
    case miopenInt8: return CheckIsSupportCKArgs<int8_t>(problem);
    case miopenInt32:
    case miopenBFloat16:
    case miopenFloat8:
    case miopenBFloat8:
    case miopenDouble: break;
    }
#endif
    return false;
}

bool PerformanceConfigHipImplicitGemm3DGroupWrwXdlops::operator==(
    const PerformanceConfigHipImplicitGemm3DGroupWrwXdlops& other) const
{
    return kernel_id == other.kernel_id;
}

PerformanceConfigHipImplicitGemm3DGroupWrwXdlops
ConvHipImplicitGemm3DGroupWrwXdlops::GetDefaultPerformanceConfig(
    const ExecutionContext&, const ProblemDescription& problem) const
{
    PerformanceConfigHipImplicitGemm3DGroupWrwXdlops pp;
    pp.HeuristicInit(problem);
    return pp;
}

bool ConvHipImplicitGemm3DGroupWrwXdlops::IsValidPerformanceConfig(
    const ExecutionContext&,
    const ProblemDescription& problem,
    const PerformanceConfigHipImplicitGemm3DGroupWrwXdlops& config) const
{
    return config.IsValid(problem);
}

PerformanceConfigHipImplicitGemm3DGroupWrwXdlops
ConvHipImplicitGemm3DGroupWrwXdlops::Search(const ExecutionContext& ctx,
                                            const ProblemDescription& problem,
                                            const AnyInvokeParams& invoke_ctx) const
{
    return GenericSearch(*this, ctx, problem, invoke_ctx);
}

bool ConvHipImplicitGemm3DGroupWrwXdlops::IsApplicable(
    [[maybe_unused]] const ExecutionContext& ctx,
    [[maybe_unused]] const ProblemDescription& problem) const
{
#if MIOPEN_BACKEND_HIP && MIOPEN_USE_COMPOSABLEKERNEL
    if(miopen::IsDisabled(ENV(MIOPEN_DEBUG_3D_CONV_IMPLICIT_GEMM_HIP_WRW_XDLOPS)))
        return false;
    if(miopen::IsEnabled(ENV(MIOPEN_DEBUG_CONVOLUTION_DETERMINISTIC)))
        return false;
    if(problem.HasMixedDataTypes())
        return false;
    if(!problem.IsDirectionBackwardWrW())
        return false;
    if(!problem.Is3d())
        return false;
    if(!problem.IsLayoutNHWC())
        return false;
    if(!ck_utility::is_ck_whitelist(ctx.GetStream().GetDeviceName()))
        return false;
    switch(problem.GetInDataType())
    {
    case miopenHalf: return CheckCKApplicability<ck::half_t>(problem);
    case miopenFloat: return CheckCKApplicability<float>(problem);
    case miopenInt8: return CheckCKApplicability<int8_t>(problem);
    case miopenInt32:
    case miopenBFloat16:
    case miopenFloat8:
    case miopenBFloat8:
    case miopenDouble: break;
    }
#endif
    return false;
}

ConvSolution ConvHipImplicitGemm3DGroupWrwXdlops::GetSolution(
    [[maybe_unused]] const ExecutionContext& ctx,
    [[maybe_unused]] const ProblemDescription& problem,
    [[maybe_unused]] const PerformanceConfigHipImplicitGemm3DGroupWrwXdlops& config) const
{
#if MIOPEN_BACKEND_HIP && MIOPEN_USE_COMPOSABLEKERNEL
    switch(problem.GetInDataType())
    {
    case miopenInt8:
        return MakeInvokerFactory<DeviceOpGWrwPtrs<int8_t>, CKArgs, miopen::conv::WrWInvokeParams>(
            problem, config.kernel_id);
    case miopenHalf:
        return MakeInvokerFactory<DeviceOpGWrwPtrs<ck::half_t>,
                                  CKArgs,
                                  miopen::conv::WrWInvokeParams>(problem, config.kernel_id);
    case miopenFloat:
        return MakeInvokerFactory<DeviceOpGWrwPtrs<float>, CKArgs, miopen::conv::WrWInvokeParams>(
            problem, config.kernel_id);
    case miopenInt32:
    case miopenBFloat16:
    case miopenFloat8:
    case miopenBFloat8:
    case miopenDouble:
    default:
        MIOPEN_THROW(miopenStatusInternalError,
                     "ConvHipImplicitGemmWrwXdlops operation not implemented for this data type");
    }
#endif
    return {};
}

} // namespace conv
} // namespace solver
} // namespace miopen
