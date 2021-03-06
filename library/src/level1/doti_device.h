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
#ifndef DOTI_DEVICE_H
#define DOTI_DEVICE_H

#include <hip/hip_runtime.h>

template <rocsparse_int n, typename T>
__device__ void rocsparse_sum_reduce(rocsparse_int tid, T* x)
{
    // clang-format off
    __syncthreads();
    if(n > 512) { if(tid < 512 && tid + 512 < n) { x[tid] += x[tid + 512]; } __syncthreads(); }
    if(n > 256) { if(tid < 256 && tid + 256 < n) { x[tid] += x[tid + 256]; } __syncthreads(); }
    if(n > 128) { if(tid < 128 && tid + 128 < n) { x[tid] += x[tid + 128]; } __syncthreads(); } 
    if(n >  64) { if(tid <  64 && tid +  64 < n) { x[tid] += x[tid +  64]; } __syncthreads(); }
    if(n >  32) { if(tid <  32 && tid +  32 < n) { x[tid] += x[tid +  32]; } __syncthreads(); }
    if(n >  16) { if(tid <  16 && tid +  16 < n) { x[tid] += x[tid +  16]; } __syncthreads(); }
    if(n >   8) { if(tid <   8 && tid +   8 < n) { x[tid] += x[tid +   8]; } __syncthreads(); }
    if(n >   4) { if(tid <   4 && tid +   4 < n) { x[tid] += x[tid +   4]; } __syncthreads(); }
    if(n >   2) { if(tid <   2 && tid +   2 < n) { x[tid] += x[tid +   2]; } __syncthreads(); }
    if(n >   1) { if(tid <   1 && tid +   1 < n) { x[tid] += x[tid +   1]; } __syncthreads(); }
    // clang-format on
}

template <typename T, rocsparse_int NB>
__global__ void doti_kernel_part1(rocsparse_int nnz,
                                  const T* x_val,
                                  const rocsparse_int* x_ind,
                                  const T* y,
                                  T* workspace,
                                  rocsparse_index_base idx_base)
{
    rocsparse_int tid = hipThreadIdx_x;
    rocsparse_int gid = hipBlockDim_x * hipBlockIdx_x + tid;

    __shared__ T sdata[NB];
    sdata[tid] = static_cast<T>(0);

    for(rocsparse_int idx = gid; idx < nnz; idx += hipGridDim_x * hipBlockDim_x)
    {
        sdata[tid] += y[x_ind[idx] - idx_base] * x_val[idx];
    }

    rocsparse_sum_reduce<NB, T>(tid, sdata);

    if(tid == 0)
    {
        workspace[hipBlockIdx_x] = sdata[0];
    }
}

template <typename T, rocsparse_int NB, rocsparse_int flag>
__global__ void doti_kernel_part2(rocsparse_int n, T* workspace, T* result)
{
    rocsparse_int tid = hipThreadIdx_x;

    __shared__ T sdata[NB];

    sdata[tid] = static_cast<T>(0);

    for(rocsparse_int i = tid; i < n; i += NB)
    {
        sdata[tid] += workspace[i];
    }
    __syncthreads();

    rocsparse_sum_reduce<NB, T>(tid, sdata);

    if(tid == 0)
    {
        if(flag)
        {
            *result = sdata[0];
        }
        else
        {
            workspace[0] = sdata[0];
        }
    }
}

#endif // DOTI_DEVICE_H
