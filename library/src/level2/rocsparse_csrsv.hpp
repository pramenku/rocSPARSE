/* ************************************************************************
 * Copyright (c) 2018 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * ************************************************************************ */

#pragma once
#ifndef ROCSPARSE_CSRSV_HPP
#define ROCSPARSE_CSRSV_HPP

#include "rocsparse.h"
#include "definitions.h"
#include "handle.h"
#include "utility.h"
#include "csrsv_device.h"

#include <limits>
#include <hip/hip_runtime.h>
#include <hipcub/hipcub.hpp>

template <typename T>
rocsparse_status rocsparse_csrsv_buffer_size_template(rocsparse_handle handle,
                                                      rocsparse_operation trans,
                                                      rocsparse_int m,
                                                      rocsparse_int nnz,
                                                      const rocsparse_mat_descr descr,
                                                      const T* csr_val,
                                                      const rocsparse_int* csr_row_ptr,
                                                      const rocsparse_int* csr_col_ind,
                                                      rocsparse_mat_info info,
                                                      size_t* buffer_size)
{
    // Check for valid handle and matrix descriptor
    if(handle == nullptr)
    {
        return rocsparse_status_invalid_handle;
    }
    else if(descr == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }
    else if(info == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }

    // Logging
    log_trace(handle,
              replaceX<T>("rocsparse_Xcsrsv_buffer_size"),
              trans,
              m,
              nnz,
              (const void*&)descr,
              (const void*&)csr_val,
              (const void*&)csr_row_ptr,
              (const void*&)csr_col_ind,
              (const void*&)info,
              (const void*&)buffer_size);

    // Check index base
    if(descr->base != rocsparse_index_base_zero && descr->base != rocsparse_index_base_one)
    {
        return rocsparse_status_invalid_value;
    }
    if(descr->type != rocsparse_matrix_type_general)
    {
        // TODO
        return rocsparse_status_not_implemented;
    }

    // Check sizes
    if(m < 0)
    {
        return rocsparse_status_invalid_size;
    }
    else if(nnz < 0)
    {
        return rocsparse_status_invalid_size;
    }

    // Check pointer arguments
    if(csr_row_ptr == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }
    else if(csr_col_ind == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }
    else if(csr_val == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }
    else if(buffer_size == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }

    // Quick return if possible
    if(m == 0 || nnz == 0)
    {
        // Do not return 0 as buffer size
        *buffer_size = 4;
        return rocsparse_status_success;
    }

    // rocsparse_int max depth
    *buffer_size = 256;

    // unsigned long long total_spin
    *buffer_size += 256;

    // rocsparse_int max_nnz
    *buffer_size += 256;

    // rocsparse_int done_array[m]
    *buffer_size += sizeof(rocsparse_int) * ((m - 1) / 256 + 1) * 256;

    // rocsparse_int rows_per_level[m]
    *buffer_size += sizeof(rocsparse_int) * ((m - 1) / 256 + 1) * 256;

    size_t hipcub_size = 0;
    rocsparse_int* ptr = nullptr;
    RETURN_IF_HIP_ERROR(hipcub::DeviceScan::InclusiveSum(nullptr, hipcub_size, ptr, ptr, m));

    // hipcub buffer
    *buffer_size += hipcub_size;

    return rocsparse_status_success;
}

static rocsparse_status rocsparse_csrtr_analysis(rocsparse_handle handle,
                                                 rocsparse_operation trans,
                                                 rocsparse_int m,
                                                 rocsparse_int nnz,
                                                 const rocsparse_mat_descr descr,
                                                 const rocsparse_int* csr_row_ptr,
                                                 const rocsparse_int* csr_col_ind,
                                                 rocsparse_csrtr_info info,
                                                 void* temp_buffer)
{
    // Stream
    hipStream_t stream = handle->stream;

    // Buffer
    char* ptr = reinterpret_cast<char*>(temp_buffer);

    // Initialize temporary buffer
    size_t buffer_size = 256 + 256 + 256 + sizeof(rocsparse_int) * ((m - 1) / 256 + 1) * 256 +
                         sizeof(rocsparse_int) * ((m - 1) / 256 + 1) * 256;

    // Set temporary buffer to 0
    RETURN_IF_HIP_ERROR(hipMemsetAsync(ptr, 0, sizeof(char) * buffer_size, stream));

    // max_depth
    rocsparse_int* d_max_depth = reinterpret_cast<rocsparse_int*>(ptr);
    ptr += 256;

    // total_spin
    unsigned long long* d_total_spin = reinterpret_cast<unsigned long long*>(ptr);
    ptr += 256;

    // max_nnz
    rocsparse_int* d_max_nnz = reinterpret_cast<rocsparse_int*>(ptr);
    ptr += 256;

    // done array
    rocsparse_int* d_done_array = reinterpret_cast<rocsparse_int*>(ptr);
    ptr += sizeof(rocsparse_int) * ((m - 1) / 256 + 1) * 256;

    // rows_per_level
    rocsparse_int* d_rows_per_level = reinterpret_cast<rocsparse_int*>(ptr);
    ptr += sizeof(rocsparse_int) * ((m - 1) / 256 + 1) * 256;

    // hipcub buffer
    void* hipcub_buffer = reinterpret_cast<void*>(ptr);

    // Allocate buffer to hold diagonal entry point
    RETURN_IF_HIP_ERROR(hipMalloc((void**)&info->csr_diag_ind, sizeof(rocsparse_int) * m));

    // Allocate buffer to hold zero pivot
    RETURN_IF_HIP_ERROR(hipMalloc((void**)&info->zero_pivot, sizeof(rocsparse_int)));

    // Allocate buffer to hold row map
    RETURN_IF_HIP_ERROR(hipMalloc((void**)&info->d_row_map, sizeof(rocsparse_int) * (m + 1)));

    // Allocate host buffer to hold row map
    RETURN_IF_HIP_ERROR(hipHostMalloc((void**)&info->h_row_map, sizeof(rocsparse_int) * (m + 1)));

    // Initialize row map
    memset(info->h_row_map, 0, sizeof(rocsparse_int) * (m + 1));

    // Initialize zero pivot
    rocsparse_int max = std::numeric_limits<rocsparse_int>::max();
    RETURN_IF_HIP_ERROR(
        hipMemcpy(info->zero_pivot, &max, sizeof(rocsparse_int), hipMemcpyHostToDevice));

// Run analysis
#define CSRILU0_DIM 1024
    dim3 csrsv_blocks((handle->wavefront_size * m - 1) / CSRILU0_DIM + 1);
    dim3 csrsv_threads(CSRILU0_DIM);
#undef CSRILU0_DIM

    if(handle->wavefront_size == 32)
    {
        if(descr->fill_mode == rocsparse_fill_mode_upper)
        {
            hipLaunchKernelGGL((csrsv_analysis_kernel<32, rocsparse_fill_mode_upper>),
                               csrsv_blocks,
                               csrsv_threads,
                               0,
                               stream,
                               m,
                               csr_row_ptr,
                               csr_col_ind,
                               info->csr_diag_ind,
                               d_done_array,
                               d_rows_per_level,
                               d_max_depth,
                               d_total_spin,
                               d_max_nnz,
                               info->zero_pivot,
                               descr->base);
        }
        else if(descr->fill_mode == rocsparse_fill_mode_lower)
        {
            hipLaunchKernelGGL((csrsv_analysis_kernel<32, rocsparse_fill_mode_lower>),
                               csrsv_blocks,
                               csrsv_threads,
                               0,
                               stream,
                               m,
                               csr_row_ptr,
                               csr_col_ind,
                               info->csr_diag_ind,
                               d_done_array,
                               d_rows_per_level,
                               d_max_depth,
                               d_total_spin,
                               d_max_nnz,
                               info->zero_pivot,
                               descr->base);
        }
    }
    else if(handle->wavefront_size == 64)
    {
        if(descr->fill_mode == rocsparse_fill_mode_upper)
        {
            hipLaunchKernelGGL((csrsv_analysis_kernel<64, rocsparse_fill_mode_upper>),
                               csrsv_blocks,
                               csrsv_threads,
                               0,
                               stream,
                               m,
                               csr_row_ptr,
                               csr_col_ind,
                               info->csr_diag_ind,
                               d_done_array,
                               d_rows_per_level,
                               d_max_depth,
                               d_total_spin,
                               d_max_nnz,
                               info->zero_pivot,
                               descr->base);
        }
        else if(descr->fill_mode == rocsparse_fill_mode_lower)
        {
            hipLaunchKernelGGL((csrsv_analysis_kernel<64, rocsparse_fill_mode_lower>),
                               csrsv_blocks,
                               csrsv_threads,
                               0,
                               stream,
                               m,
                               csr_row_ptr,
                               csr_col_ind,
                               info->csr_diag_ind,
                               d_done_array,
                               d_rows_per_level,
                               d_max_depth,
                               d_total_spin,
                               d_max_nnz,
                               info->zero_pivot,
                               descr->base);
        }
    }
    else
    {
        return rocsparse_status_arch_mismatch;
    }

    // Post processing
    RETURN_IF_HIP_ERROR(
        hipMemcpy(&info->max_depth, d_max_depth, sizeof(rocsparse_int), hipMemcpyDeviceToHost));
    RETURN_IF_HIP_ERROR(hipMemcpy(
        &info->total_spin, d_total_spin, sizeof(unsigned long long), hipMemcpyDeviceToHost));
    RETURN_IF_HIP_ERROR(
        hipMemcpy(&info->max_nnz, d_max_nnz, sizeof(rocsparse_int), hipMemcpyDeviceToHost));

    // Inclusive sum to obtain rows per level
    size_t hipcub_size = 0;
    RETURN_IF_HIP_ERROR(hipcub::DeviceScan::InclusiveSum(
        nullptr, hipcub_size, d_rows_per_level, d_rows_per_level, info->max_depth));
    RETURN_IF_HIP_ERROR(hipcub::DeviceScan::InclusiveSum(
        hipcub_buffer, hipcub_size, d_rows_per_level, d_rows_per_level, info->max_depth));

    // Allocate host memory for meta data
    info->rows_per_level.resize(info->max_depth);
    std::vector<rocsparse_int> done_array(m);

    // Move meta data to host (required for kernel launching)
    RETURN_IF_HIP_ERROR(hipMemcpy(info->rows_per_level.data(),
                                  d_rows_per_level,
                                  sizeof(rocsparse_int) * info->max_depth,
                                  hipMemcpyDeviceToHost));
    RETURN_IF_HIP_ERROR(hipMemcpy(
        done_array.data(), d_done_array, sizeof(rocsparse_int) * m, hipMemcpyDeviceToHost));

    std::vector<rocsparse_int> counter(info->max_depth, 0);

    // Create row map
    for(rocsparse_int i = 0; i < m; ++i)
    {
        rocsparse_int level        = done_array[i] - 1;
        rocsparse_int prev_level   = level - 1;
        rocsparse_int depth_offset = (level == 0) ? 0 : info->rows_per_level[prev_level];

        info->h_row_map[depth_offset + counter[level]] = i;
        ++counter[level];
    }

    // Copy row map to device
    RETURN_IF_HIP_ERROR(hipMemcpyAsync(info->d_row_map,
                                       info->h_row_map,
                                       sizeof(rocsparse_int) * (m + 1),
                                       hipMemcpyHostToDevice,
                                       stream));

    // Store some pointers to verify correct execution
    info->m           = m;
    info->nnz         = nnz;
    info->descr       = descr;
    info->csr_row_ptr = csr_row_ptr;
    info->csr_col_ind = csr_col_ind;

    return rocsparse_status_success;
}

template <typename T>
rocsparse_status rocsparse_csrsv_analysis_template(rocsparse_handle handle,
                                                   rocsparse_operation trans,
                                                   rocsparse_int m,
                                                   rocsparse_int nnz,
                                                   const rocsparse_mat_descr descr,
                                                   const T* csr_val,
                                                   const rocsparse_int* csr_row_ptr,
                                                   const rocsparse_int* csr_col_ind,
                                                   rocsparse_mat_info info,
                                                   rocsparse_analysis_policy analysis,
                                                   rocsparse_solve_policy solve,
                                                   void* temp_buffer)
{
    // Check for valid handle and matrix descriptor
    if(handle == nullptr)
    {
        return rocsparse_status_invalid_handle;
    }
    else if(descr == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }
    else if(info == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }

    // Logging
    log_trace(handle,
              replaceX<T>("rocsparse_Xcsrsv_analysis"),
              trans,
              m,
              nnz,
              (const void*&)descr,
              (const void*&)csr_val,
              (const void*&)csr_row_ptr,
              (const void*&)csr_col_ind,
              (const void*&)info,
              solve,
              analysis,
              (const void*&)temp_buffer);

    // Check index base
    if(descr->base != rocsparse_index_base_zero && descr->base != rocsparse_index_base_one)
    {
        return rocsparse_status_invalid_value;
    }

    // Check matrix type
    if(descr->type != rocsparse_matrix_type_general)
    {
        // TODO
        return rocsparse_status_not_implemented;
    }

    // Check analysis policy
    if(analysis != rocsparse_analysis_policy_reuse && analysis != rocsparse_analysis_policy_force)
    {
        return rocsparse_status_invalid_value;
    }

    // Check solve policy
    if(solve != rocsparse_solve_policy_auto)
    {
        return rocsparse_status_invalid_value;
    }

    // Check sizes
    if(m < 0)
    {
        return rocsparse_status_invalid_size;
    }
    else if(nnz < 0)
    {
        return rocsparse_status_invalid_size;
    }

    // Check pointer arguments
    if(csr_row_ptr == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }
    else if(csr_col_ind == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }
    else if(csr_val == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }
    else if(temp_buffer == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }

    // Quick return if possible
    if(m == 0 || nnz == 0)
    {
        return rocsparse_status_success;
    }

    // Switch between lower and upper triangular analysis
    if(descr->fill_mode == rocsparse_fill_mode_upper)
    {
        // This is currently the only case where we need upper triangular analysis,
        // therefore we ignore the analysis policy

        // Clear csrsv info
        RETURN_IF_ROCSPARSE_ERROR(rocsparse_destroy_csrtr_info(info->csrsv_upper_info));

        // Create csrsv info
        RETURN_IF_ROCSPARSE_ERROR(rocsparse_create_csrtr_info(&info->csrsv_upper_info));

        // Perform analysis
        RETURN_IF_ROCSPARSE_ERROR(rocsparse_csrtr_analysis(handle,
                                                           trans,
                                                           m,
                                                           nnz,
                                                           descr,
                                                           csr_row_ptr,
                                                           csr_col_ind,
                                                           info->csrsv_upper_info,
                                                           temp_buffer));
    }
    else
    {
        // Differentiate the analysis policies
        if(analysis == rocsparse_analysis_policy_reuse)
        {
            // We try to re-use already analyzed lower part, if available.
            // It is the user's responsibility that this data is still valid,
            // since he passed the 'reuse' flag.

            // If csrsv meta data is already available, do nothing
            if(info->csrsv_lower_info != nullptr)
            {
                return rocsparse_status_success;
            }

            // Check for other lower analysis meta data
            rocsparse_csrtr_info reuse = nullptr;

            // csrilu0 meta data
            if(info->csrilu0_info != nullptr)
            {
                reuse = info->csrilu0_info;
            }

            // TODO add more crossover data here

            // If data has been found, use it
            if(reuse != nullptr)
            {
                info->csrsv_lower_info = reuse;

                return rocsparse_status_success;
            }
        }

        // User is explicitly asking to force a re-analysis, or no valid data has been
        // found to be re-used.

        // Clear csrsv info
        RETURN_IF_ROCSPARSE_ERROR(rocsparse_destroy_csrtr_info(info->csrsv_lower_info));

        // Create csrsv info
        RETURN_IF_ROCSPARSE_ERROR(rocsparse_create_csrtr_info(&info->csrsv_lower_info));

        // Perform analysis
        RETURN_IF_ROCSPARSE_ERROR(rocsparse_csrtr_analysis(handle,
                                                           trans,
                                                           m,
                                                           nnz,
                                                           descr,
                                                           csr_row_ptr,
                                                           csr_col_ind,
                                                           info->csrsv_lower_info,
                                                           temp_buffer));
    }

    return rocsparse_status_success;
}

template <typename T, rocsparse_int BLOCKSIZE, rocsparse_int WF_SIZE>
__launch_bounds__(BLOCKSIZE) __global__
    void csrsv_host_pointer(rocsparse_int m,
                            T alpha,
                            const rocsparse_int* __restrict__ csr_row_ptr,
                            const rocsparse_int* __restrict__ csr_col_ind,
                            const T* __restrict__ csr_val,
                            const T* __restrict__ x,
                            T* __restrict__ y,
                            rocsparse_int* __restrict__ done_array,
                            rocsparse_int* __restrict__ map,
                            rocsparse_int offset,
                            rocsparse_int* __restrict__ zero_pivot,
                            rocsparse_index_base idx_base,
                            rocsparse_fill_mode fill_mode,
                            rocsparse_diag_type diag_type)
{
    csrsv_device<T, BLOCKSIZE, WF_SIZE>(m,
                                        alpha,
                                        csr_row_ptr,
                                        csr_col_ind,
                                        csr_val,
                                        x,
                                        y,
                                        done_array,
                                        map,
                                        offset,
                                        zero_pivot,
                                        idx_base,
                                        fill_mode,
                                        diag_type);
}

template <typename T, rocsparse_int BLOCKSIZE, rocsparse_int WF_SIZE>
__launch_bounds__(BLOCKSIZE) __global__
    void csrsv_device_pointer(rocsparse_int m,
                              const T* alpha,
                              const rocsparse_int* __restrict__ csr_row_ptr,
                              const rocsparse_int* __restrict__ csr_col_ind,
                              const T* __restrict__ csr_val,
                              const T* __restrict__ x,
                              T* __restrict__ y,
                              rocsparse_int* __restrict__ done_array,
                              rocsparse_int* __restrict__ map,
                              rocsparse_int offset,
                              rocsparse_int* __restrict__ zero_pivot,
                              rocsparse_index_base idx_base,
                              rocsparse_fill_mode fill_mode,
                              rocsparse_diag_type diag_type)
{
    csrsv_device<T, BLOCKSIZE, WF_SIZE>(m,
                                        *alpha,
                                        csr_row_ptr,
                                        csr_col_ind,
                                        csr_val,
                                        x,
                                        y,
                                        done_array,
                                        map,
                                        offset,
                                        zero_pivot,
                                        idx_base,
                                        fill_mode,
                                        diag_type);
}

template <typename T>
rocsparse_status rocsparse_csrsv_solve_template(rocsparse_handle handle,
                                                rocsparse_operation trans,
                                                rocsparse_int m,
                                                rocsparse_int nnz,
                                                const T* alpha,
                                                const rocsparse_mat_descr descr,
                                                const T* csr_val,
                                                const rocsparse_int* csr_row_ptr,
                                                const rocsparse_int* csr_col_ind,
                                                rocsparse_mat_info info,
                                                const T* x,
                                                T* y,
                                                rocsparse_solve_policy policy,
                                                void* temp_buffer)
{
    // Check for valid handle and matrix descriptor
    if(handle == nullptr)
    {
        return rocsparse_status_invalid_handle;
    }
    else if(descr == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }
    else if(info == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }

    // Logging
    if(handle->pointer_mode == rocsparse_pointer_mode_host)
    {
        log_trace(handle,
                  replaceX<T>("rocsparse_Xcsrsv"),
                  trans,
                  m,
                  nnz,
                  *alpha,
                  (const void*&)descr,
                  (const void*&)csr_val,
                  (const void*&)csr_row_ptr,
                  (const void*&)csr_col_ind,
                  (const void*&)info,
                  (const void*&)x,
                  (const void*&)y,
                  policy,
                  (const void*&)temp_buffer);

        log_bench(handle,
                  "./rocsparse-bench -f csrsv -r",
                  replaceX<T>("X"),
                  "--mtx <matrix.mtx> ",
                  "--alpha",
                  *alpha);
    }
    else
    {
        log_trace(handle,
                  replaceX<T>("rocsparse_Xcsrsv"),
                  trans,
                  m,
                  nnz,
                  (const void*&)alpha,
                  (const void*&)descr,
                  (const void*&)csr_val,
                  (const void*&)csr_row_ptr,
                  (const void*&)csr_col_ind,
                  (const void*&)info,
                  (const void*&)x,
                  (const void*&)y,
                  policy,
                  (const void*&)temp_buffer);
    }

    // Check index base
    if(descr->base != rocsparse_index_base_zero && descr->base != rocsparse_index_base_one)
    {
        return rocsparse_status_invalid_value;
    }
    if(descr->type != rocsparse_matrix_type_general)
    {
        // TODO
        return rocsparse_status_not_implemented;
    }

    // Check sizes
    if(m < 0)
    {
        return rocsparse_status_invalid_size;
    }
    else if(nnz < 0)
    {
        return rocsparse_status_invalid_size;
    }

    // Check pointer arguments
    if(csr_val == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }
    else if(csr_row_ptr == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }
    else if(csr_col_ind == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }
    else if(alpha == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }
    else if(x == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }
    else if(y == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }
    else if(temp_buffer == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }

    // Quick return if possible
    if(m == 0 || nnz == 0)
    {
        return rocsparse_status_success;
    }

    // Stream
    hipStream_t stream = handle->stream;

    // Buffer
    char* ptr = reinterpret_cast<char*>(temp_buffer);

    ptr += 256;
    ptr += 256;
    ptr += 256;

    // done array
    rocsparse_int* d_done_array = reinterpret_cast<rocsparse_int*>(ptr);

    // Initialize buffers
    RETURN_IF_HIP_ERROR(hipMemsetAsync(d_done_array, 0, sizeof(rocsparse_int) * m, stream));

    rocsparse_csrtr_info csrsv = (descr->fill_mode == rocsparse_fill_mode_upper)
                                     ? info->csrsv_upper_info
                                     : info->csrsv_lower_info;

    // If diag type is unit, re-initialize zero pivot to remove structural zeros
    if(descr->diag_type == rocsparse_diag_type_unit)
    {
        rocsparse_int max = std::numeric_limits<rocsparse_int>::max();
        RETURN_IF_HIP_ERROR(
            hipMemcpy(csrsv->zero_pivot, &max, sizeof(rocsparse_int), hipMemcpyHostToDevice));
    }

/*
#define CSRSV_DIM 1024
    rocsparse_int depth_offset = 0;
    rocsparse_int running_total = 0;
    rocsparse_int wf_size = handle->wavefront_size;
    rocsparse_int wf_per_wg = CSRSV_DIM / wf_size;
    rocsparse_int cutoff = (m / csrsv->max_depth < 32) ? 2560 : 81920;

    for(rocsparse_int level = 0; level < csrsv->max_depth; ++level)
    {
        if(level != 0 && running_total == 0)
        {
            depth_offset = csrsv->rows_per_level[level - 1];
        }

        running_total = csrsv->rows_per_level[level] - depth_offset;

        if(running_total >= cutoff)
        {
            dim3 csrsv_blocks(((running_total + (running_total % wf_per_wg)) * wf_size - 1) /
CSRSV_DIM + 1);
            dim3 csrsv_threads(CSRSV_DIM);

            if(handle->pointer_mode == rocsparse_pointer_mode_device)
            {
                // rocsparse_pointer_mode_device
                if(wf_size == 32)
                {
                    hipLaunchKernelGGL((csrsv_device_pointer<T, CSRSV_DIM, 32>),
                                       csrsv_blocks,
                                       csrsv_threads,
                                       0,
                                       stream,
                                       m,
                                       alpha,
                                       csr_row_ptr,
                                       csr_col_ind,
                                       csr_val,
                                       x,
                                       y,
                                       d_done_array,
                                       csrsv->d_row_map,
                                       depth_offset,
                                       csrsv->zero_pivot,
                                       descr->base,
                                       descr->fill_mode,
                                       descr->diag_type);
                }
                else if(wf_size == 64)
                {
                    hipLaunchKernelGGL((csrsv_device_pointer<T, CSRSV_DIM, 64>),
                                       csrsv_blocks,
                                       csrsv_threads,
                                       0,
                                       stream,
                                       m,
                                       alpha,
                                       csr_row_ptr,
                                       csr_col_ind,
                                       csr_val,
                                       x,
                                       y,
                                       d_done_array,
                                       csrsv->d_row_map,
                                       depth_offset,
                                       csrsv->zero_pivot,
                                       descr->base,
                                       descr->fill_mode,
                                       descr->diag_type);
                }
                else
                {
                    return rocsparse_status_arch_mismatch;
                }
            }
            else
            {
                // rocsparse_pointer_mode_host
                if(wf_size == 32)
                {
                    hipLaunchKernelGGL((csrsv_host_pointer<T, CSRSV_DIM, 32>),
                                       csrsv_blocks,
                                       csrsv_threads,
                                       0,
                                       stream,
                                       m,
                                       *alpha,
                                       csr_row_ptr,
                                       csr_col_ind,
                                       csr_val,
                                       x,
                                       y,
                                       d_done_array,
                                       csrsv->d_row_map,
                                       depth_offset,
                                       csrsv->zero_pivot,
                                       descr->base,
                                       descr->fill_mode,
                                       descr->diag_type);
                }
                else if(wf_size == 64)
                {
                    hipLaunchKernelGGL((csrsv_host_pointer<T, CSRSV_DIM, 64>),
                                       csrsv_blocks,
                                       csrsv_threads,
                                       0,
                                       stream,
                                       m,
                                       *alpha,
                                       csr_row_ptr,
                                       csr_col_ind,
                                       csr_val,
                                       x,
                                       y,
                                       d_done_array,
                                       csrsv->d_row_map,
                                       depth_offset,
                                       csrsv->zero_pivot,
                                       descr->base,
                                       descr->fill_mode,
                                       descr->diag_type);
                }
                else
                {
                    return rocsparse_status_arch_mismatch;
                }
            }

            running_total = 0;
        }
    }

    if(running_total)
    {
        dim3 csrsv_blocks(((running_total + (running_total % wf_per_wg)) * wf_size - 1) / CSRSV_DIM
+ 1);
        dim3 csrsv_threads(CSRSV_DIM);

        if(handle->pointer_mode == rocsparse_pointer_mode_device)
        {
            // rocsparse_pointer_mode_device
            if(wf_size == 32)
            {
                hipLaunchKernelGGL((csrsv_device_pointer<T, CSRSV_DIM, 32>),
                                   csrsv_blocks,
                                   csrsv_threads,
                                   0,
                                   stream,
                                   m,
                                   alpha,
                                   csr_row_ptr,
                                   csr_col_ind,
                                   csr_val,
                                   x,
                                   y,
                                   d_done_array,
                                   csrsv->d_row_map,
                                   depth_offset,
                                   csrsv->zero_pivot,
                                   descr->base,
                                   descr->fill_mode,
                                   descr->diag_type);
            }
            else if(wf_size == 64)
            {
                hipLaunchKernelGGL((csrsv_device_pointer<T, CSRSV_DIM, 64>),
                                   csrsv_blocks,
                                   csrsv_threads,
                                   0,
                                   stream,
                                   m,
                                   alpha,
                                   csr_row_ptr,
                                   csr_col_ind,
                                   csr_val,
                                   x,
                                   y,
                                   d_done_array,
                                   csrsv->d_row_map,
                                   depth_offset,
                                   csrsv->zero_pivot,
                                   descr->base,
                                   descr->fill_mode,
                                   descr->diag_type);
            }
            else
            {
                return rocsparse_status_arch_mismatch;
            }
        }
        else
        {
            // rocsparse_pointer_mode_host
            if(wf_size == 32)
            {
                hipLaunchKernelGGL((csrsv_host_pointer<T, CSRSV_DIM, 32>),
                                   csrsv_blocks,
                                   csrsv_threads,
                                   0,
                                   stream,
                                   m,
                                   *alpha,
                                   csr_row_ptr,
                                   csr_col_ind,
                                   csr_val,
                                   x,
                                   y,
                                   d_done_array,
                                   csrsv->d_row_map,
                                   depth_offset,
                                   csrsv->zero_pivot,
                                   descr->base,
                                   descr->fill_mode,
                                   descr->diag_type);
            }
            else if(wf_size == 64)
            {
                hipLaunchKernelGGL((csrsv_host_pointer<T, CSRSV_DIM, 64>),
                                   csrsv_blocks,
                                   csrsv_threads,
                                   0,
                                   stream,
                                   m,
                                   *alpha,
                                   csr_row_ptr,
                                   csr_col_ind,
                                   csr_val,
                                   x,
                                   y,
                                   d_done_array,
                                   csrsv->d_row_map,
                                   depth_offset,
                                   csrsv->zero_pivot,
                                   descr->base,
                                   descr->fill_mode,
                                   descr->diag_type);
            }
            else
            {
                return rocsparse_status_arch_mismatch;
            }
        }
    }
#undef CSRSV_DIM
*/
#define CSRSV_DIM 1024
    dim3 csrsv_blocks((handle->wavefront_size * m - 1) / CSRSV_DIM + 1);
    dim3 csrsv_threads(CSRSV_DIM);

    if(handle->pointer_mode == rocsparse_pointer_mode_device)
    {
        // rocsparse_pointer_mode_device
        if(handle->wavefront_size == 32)
        {
            hipLaunchKernelGGL((csrsv_device_pointer<T, CSRSV_DIM, 32>),
                               csrsv_blocks,
                               csrsv_threads,
                               0,
                               stream,
                               m,
                               alpha,
                               csr_row_ptr,
                               csr_col_ind,
                               csr_val,
                               x,
                               y,
                               d_done_array,
                               csrsv->d_row_map,
                               0,
                               csrsv->zero_pivot,
                               descr->base,
                               descr->fill_mode,
                               descr->diag_type);
        }
        else if(handle->wavefront_size == 64)
        {
            hipLaunchKernelGGL((csrsv_device_pointer<T, CSRSV_DIM, 64>),
                               csrsv_blocks,
                               csrsv_threads,
                               0,
                               stream,
                               m,
                               alpha,
                               csr_row_ptr,
                               csr_col_ind,
                               csr_val,
                               x,
                               y,
                               d_done_array,
                               csrsv->d_row_map,
                               0,
                               csrsv->zero_pivot,
                               descr->base,
                               descr->fill_mode,
                               descr->diag_type);
        }
        else
        {
            return rocsparse_status_arch_mismatch;
        }
    }
    else
    {
        // rocsparse_pointer_mode_host
        if(handle->wavefront_size == 32)
        {
            hipLaunchKernelGGL((csrsv_host_pointer<T, CSRSV_DIM, 32>),
                               csrsv_blocks,
                               csrsv_threads,
                               0,
                               stream,
                               m,
                               *alpha,
                               csr_row_ptr,
                               csr_col_ind,
                               csr_val,
                               x,
                               y,
                               d_done_array,
                               csrsv->d_row_map,
                               0,
                               csrsv->zero_pivot,
                               descr->base,
                               descr->fill_mode,
                               descr->diag_type);
        }
        else if(handle->wavefront_size == 64)
        {
            hipLaunchKernelGGL((csrsv_host_pointer<T, CSRSV_DIM, 64>),
                               csrsv_blocks,
                               csrsv_threads,
                               0,
                               stream,
                               m,
                               *alpha,
                               csr_row_ptr,
                               csr_col_ind,
                               csr_val,
                               x,
                               y,
                               d_done_array,
                               csrsv->d_row_map,
                               0,
                               csrsv->zero_pivot,
                               descr->base,
                               descr->fill_mode,
                               descr->diag_type);
        }
        else
        {
            return rocsparse_status_arch_mismatch;
        }
    }
#undef CSRSV_DIM

    return rocsparse_status_success;
}

#endif // ROCSPARSE_CSRSV_HPP
