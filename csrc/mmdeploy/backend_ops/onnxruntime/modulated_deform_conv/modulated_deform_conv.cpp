// Copyright (c) OpenMMLab. All rights reserved
#include "modulated_deform_conv.h"

#include <cmath>
#include <thread>
#include <vector>

#include "modulated_deform_conv/modulated_deform_conv_cpu.h"
#include "ort_utils.h"

namespace mmdeploy {

void parallel_unroll_gemm(const float *A, const float *B, const float *V, const float *H,
                          const int32_t M, const int32_t N, const int32_t K, const float alpha,
                          const float beta, float *Y, const int32_t start_row,
                          const int32_t end_row) {
  float tmp[N];  // tmp
  for (int32_t m = start_row; m < end_row; ++m) {
    for (int32_t n = 0; n < N; n++) {
      tmp[n] = 0;
    }
    {
      int32_t remainder = K % 8;  // unroll
      for (int32_t k = 0; k < K; k += 8) {
        for (int32_t n = 0; n < N; n++) {
          tmp[n] += A[m * K + k] * B[k * N + n];
          tmp[n] += A[m * K + k + 1] * B[k * N + N + n];
          tmp[n] += A[m * K + k + 2] * B[k * N + 2 * N + n];
          tmp[n] += A[m * K + k + 3] * B[k * N + 3 * N + n];
          tmp[n] += A[m * K + k + 4] * B[k * N + 4 * N + n];
          tmp[n] += A[m * K + k + 5] * B[k * N + 5 * N + n];
          tmp[n] += A[m * K + k + 6] * B[k * N + 6 * N + n];
          tmp[n] += A[m * K + k + 7] * B[k * N + 7 * N + n];
        }
      }
      for (int32_t k = K - remainder; k < K; k++) {
        for (int32_t n = 0; n < N; n++) {
          tmp[n] += A[m * K + k] * B[k * N + n];
        }
      }
    }
    for (int32_t n = 0; n < N; n++) {
      tmp[n] *= alpha;
      if (V) tmp[n] += beta * V[n];
      if (H) tmp[n] += beta * H[m * N + n];
      Y[m * N + n] = tmp[n];
    }
  }
}

void deformable_conv2d_ref_fp32(const float *src, const float *offset, const float *mask,
                                const float *filter, const float *bias, const int64_t batch,
                                const int64_t src_c, const int64_t src_h, const int64_t src_w,
                                const int64_t dst_c, const int64_t dst_h, const int64_t dst_w,
                                const int64_t group, const int64_t offset_group,
                                const int64_t channels, const int64_t num_output,
                                const int64_t kernel_h, const int64_t kernel_w,
                                const int64_t stride_h, const int64_t stride_w, const int64_t pad_h,
                                const int64_t pad_w, const int64_t dilation_h,
                                const int64_t dilation_w, float *columns, float *dst) {
  const int64_t ic_per_gp = channels / group;
  const int64_t oc_per_gp = num_output / group;
  // Set up for launching threads
  std::size_t num_threads = std::thread::hardware_concurrency();
  std::vector<std::thread> threads;
  threads.reserve(num_threads);

  for (int64_t b = 0; b < batch; ++b) {
    for (int64_t g = 0; g < group; ++g) {
      deformable_im2col_2d<float>(
          src + b * src_c * src_h * src_w + g * ic_per_gp * src_h * src_w,
          offset + b * offset_group * 2 * kernel_h * kernel_w * dst_h * dst_w,
          mask + b * offset_group * kernel_h * kernel_w * dst_h * dst_w, src_h, src_w, kernel_h,
          kernel_w, pad_h, pad_w, stride_h, stride_w, dilation_h, dilation_w, ic_per_gp,
          offset_group, dst_h, dst_w, mask != nullptr, columns);
      float *dst_ptr = dst + b * dst_c * dst_h * dst_w + g * oc_per_gp * dst_h * dst_w;
      if (bias != nullptr) {
        const float *bias_ptr = bias + g * oc_per_gp;
        for (int64_t oc = 0; oc < oc_per_gp; ++oc) {
          for (int64_t hw = 0; hw < dst_h * dst_w; ++hw) {
            dst_ptr[oc * dst_h * dst_w + hw] = bias_ptr[oc];
          }
        }
      } else {
        memset(dst_ptr, 0.0f, sizeof(float) * oc_per_gp * dst_h * dst_w);
      }
      if (num_threads > 1) {
        // Calculate values to pass to threads
        int32_t n_rows = (oc_per_gp + num_threads - 1) / num_threads;
        int32_t end_row = 0;
        for (int32_t i = 0; i < num_threads; i++) {
          auto start_row = i * n_rows;
          end_row = start_row + n_rows;
          if (end_row > oc_per_gp) end_row = oc_per_gp;
          std::thread t(parallel_unroll_gemm,
                        filter + g * oc_per_gp * ic_per_gp * kernel_h * kernel_w, columns, nullptr,
                        dst_ptr, oc_per_gp, dst_h * dst_w, ic_per_gp * kernel_h * kernel_w, 1.0f,
                        1.0f, dst_ptr, start_row, end_row);
          threads.emplace_back(std::move(t));
        }
        // Wait for all threads to complete
        for (auto &t : threads) t.join();
        threads.clear();
      } else {  // parallel gemm degrade to serial gemm with start_row=0 and end_row= oc_per_gp
        parallel_unroll_gemm(filter + g * oc_per_gp * ic_per_gp * kernel_h * kernel_w, columns,
                             nullptr, dst_ptr, oc_per_gp, dst_h * dst_w,
                             ic_per_gp * kernel_h * kernel_w, 1.0f, 1.0f, dst_ptr, 0, oc_per_gp);
      }
    }
  }
}

MMCVModulatedDeformConvKernel::MMCVModulatedDeformConvKernel(const OrtApi &api,
                                                             const OrtKernelInfo *info)
    : ort_(api), info_(info) {
  std::vector<int64_t> stride = ort_.KernelInfoGetAttribute<std::vector<int64_t>>(info, "stride");
  stride_height_ = stride[0];
  stride_width_ = stride[1];
  std::vector<int64_t> padding = ort_.KernelInfoGetAttribute<std::vector<int64_t>>(info, "padding");
  padding_height_ = padding[0];
  padding_width_ = padding[1];
  std::vector<int64_t> dilation =
      ort_.KernelInfoGetAttribute<std::vector<int64_t>>(info, "dilation");
  dilation_height_ = dilation[0];
  dilation_width_ = dilation[1];
  deformable_group_ = ort_.KernelInfoGetAttribute<int64_t>(info, "deform_groups");
  group_ = ort_.KernelInfoGetAttribute<int64_t>(info, "groups");

  // create allocator
  allocator_ = Ort::AllocatorWithDefaultOptions();
}

void MMCVModulatedDeformConvKernel::Compute(OrtKernelContext *context) {
  const int64_t stride_height = stride_height_;
  const int64_t stride_width = stride_width_;
  const int64_t padding_height = padding_height_;
  const int64_t padding_width = padding_width_;
  const int64_t dilation_height = dilation_height_;
  const int64_t dilation_width = dilation_width_;
  const int64_t deformable_group = deformable_group_;
  const int64_t group = group_;

  const OrtValue *input = ort_.KernelContext_GetInput(context, 0);
  const float *input_data = reinterpret_cast<const float *>(ort_.GetTensorData<float>(input));

  const OrtValue *offset = ort_.KernelContext_GetInput(context, 1);
  const float *offset_data = reinterpret_cast<const float *>(ort_.GetTensorData<float>(offset));

  const OrtValue *mask = ort_.KernelContext_GetInput(context, 2);
  const float *mask_data = reinterpret_cast<const float *>(ort_.GetTensorData<float>(mask));

  const OrtValue *filter = ort_.KernelContext_GetInput(context, 3);
  const float *filter_data = reinterpret_cast<const float *>(ort_.GetTensorData<float>(filter));

  const OrtValue *bias = ort_.KernelContext_GetInput(context, 4);
  const float *bias_data = (bias != nullptr)
                               ? reinterpret_cast<const float *>(ort_.GetTensorData<float>(bias))
                               : nullptr;
  // const float *bias_data = nullptr;

  OrtTensorDimensions input_dims(ort_, input);
  OrtTensorDimensions filter_dims(ort_, filter);

  int64_t batch = input_dims[0];
  int64_t channels = input_dims[1];
  int64_t in_height = input_dims[2];
  int64_t in_width = input_dims[3];
  int64_t num_output = filter_dims[0];
  int64_t kernel_height = filter_dims[2];
  int64_t kernel_width = filter_dims[3];

  // get output memory
  int64_t out_height = floor(
      (in_height + 2 * padding_height - dilation_height * (kernel_height - 1) - 1) / stride_height +
      1);
  int64_t out_width = floor(
      (in_width + 2 * padding_width - dilation_width * (kernel_width - 1) - 1) / stride_width + 1);

  std::vector<int64_t> output_dims = {batch, num_output, out_height, out_width};
  OrtValue *output =
      ort_.KernelContext_GetOutput(context, 0, output_dims.data(), output_dims.size());
  float *out_ptr = ort_.GetTensorMutableData<float>(output);

  // allocate tmp memory
  int64_t column_len = (channels / group) * kernel_height * kernel_width * out_height * out_width;
  float *columns = (float *)allocator_.Alloc(sizeof(float) * column_len);

  deformable_conv2d_ref_fp32(input_data, offset_data, mask_data, filter_data, bias_data, batch,
                             channels, in_height, in_width, num_output, out_height, out_width,
                             group, deformable_group, channels, num_output, kernel_height,
                             kernel_width, stride_height, stride_width, padding_height,
                             padding_width, dilation_height, dilation_width, columns, out_ptr);

  allocator_.Free(columns);
}
REGISTER_ONNXRUNTIME_OPS(mmdeploy, MMCVModulatedDeformConvOp);
REGISTER_ONNXRUNTIME_OPS(mmcv, MMCVModulatedDeformConvOp);
}  // namespace mmdeploy
