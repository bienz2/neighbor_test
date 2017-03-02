/*BHEADER**********************************************************************
 * Copyright (c) 2008,  Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * This file is part of HYPRE.  See file COPYRIGHT for details.
 *
 * HYPRE is free software; you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License (as published by the Free
 * Software Foundation) version 2.1 dated February 1999.
 *
 * $Revision$
 ***********************************************************************EHEADER*/

/******************************************************************************
 *
 * Header info for the BoxLoop
 *
 *****************************************************************************/

/*--------------------------------------------------------------------------
 * BoxLoop macros:
 *--------------------------------------------------------------------------*/

#ifndef HYPRE_NEWBOXLOOP_HEADER
#define HYPRE_NEWBOXLOOP_HEADER

#include <cuda.h>
#include <cuda_runtime.h>

extern "C++" {
#include <curand.h>
#include <curand_kernel.h>
}

static HYPRE_Complex* global_recv_buffer;
static HYPRE_Complex* global_send_buffer;
static HYPRE_Int      global_recv_size = 0;
static HYPRE_Int      global_send_size = 0;

struct cuda_traversal {};
struct omp_traversal  {};
struct cuda_reduce {};

typedef struct hypre_Boxloop_struct
{
	HYPRE_Int lsize0,lsize1,lsize2;
	HYPRE_Int strides0,strides1,strides2;
	HYPRE_Int bstart0,bstart1,bstart2;
	HYPRE_Int bsize0,bsize1,bsize2;
} hypre_Boxloop;

#define AxCheckError(err) CheckError(err, __FUNCTION__, __LINE__)
inline void CheckError(cudaError_t const err, char const* const fun, const int line)
{
    if (err)
    {
        printf("CUDA Error Code[%d]: %s\n%s() Line:%d\n", err, cudaGetErrorString(err), fun, line);
		int *p = NULL; *p = 1;
    }
}
#define BLOCKSIZE 128

#define hypre_rand(val) \
{\
    curandState_t state;\
    curand_init(0,0,0,&state);\
    val = curand(&state);\
}
extern "C++" {
template <typename LOOP_BODY>
__global__ void forall_kernel(LOOP_BODY loop_body, int length)
{
	int idx = blockDim.x * blockIdx.x + threadIdx.x;
	if (idx < length)
		loop_body(idx);
}

template<typename LOOP_BODY>
void BoxLoopforall (cuda_traversal, HYPRE_Int length, LOOP_BODY loop_body)
{	
	size_t const blockSize = 128;
	size_t gridSize  = (length + blockSize - 1) / blockSize;
	if (gridSize == 0) gridSize = 1;
	
	//hypre_printf("length= %d, blocksize = %d, gridsize = %d\n",length,blockSize,gridSize);
	forall_kernel<<<gridSize, blockSize>>>(loop_body,length);
}

template<typename LOOP_BODY>
void BoxLoopforall (omp_traversal, HYPRE_Int length, LOOP_BODY loop_body)
{

#pragma omp parallel for schedule(static)
	for (int idx = 0;idx < length;idx++)
		loop_body(idx);
}

#define zypre_BoxLoopIncK(k,box)					\
{									\
   local_idx  = idx_local % box.lsize0;					\
   idx_local  = idx_local / box.lsize0;					\
   i##k += (local_idx*box.strides0 + box.bstart0) * hypre_boxD##k;		\
   hypre_boxD##k *= hypre_max(0, box.bsize0 + 1);			\
}

  
template<class T>
__global__ void dot (T * a, T * b, T *c, HYPRE_Int hypre__tot,
                     hypre_Boxloop box1,hypre_Boxloop box2)
{
    int id = (blockIdx.x * blockDim.x) + threadIdx.x;
	HYPRE_Int local_idx;
    HYPRE_Int d,idx_local = id;
    HYPRE_Int hypre_boxD1 = 1.0,hypre_boxD2 = 1.0;
    HYPRE_Int i1 = 0, i2 = 0;
    //// reducted output
    __shared__ T shared_cache [BLOCKSIZE];
	T sum = 0;
    local_idx  = idx_local % box1.lsize0;
    idx_local  = idx_local / box1.lsize0;
    i1 += (local_idx*box1.strides0 + box1.bstart0) * hypre_boxD1;
    hypre_boxD1 *= hypre_max(0, box1.bsize0 + 1);
    i2 += (local_idx*box2.strides0 + box2.bstart0) * hypre_boxD2;
    hypre_boxD2 *= hypre_max(0, box2.bsize0 + 1);
    local_idx  = idx_local % box1.lsize1;
    idx_local  = idx_local / box1.lsize1;
    i1 += (local_idx*box1.strides1 + box1.bstart1) * hypre_boxD1;
    hypre_boxD1 *= hypre_max(0, box1.bsize1 + 1);
    i2 += (local_idx*box2.strides1 + box2.bstart1) * hypre_boxD2;   
    hypre_boxD2 *= hypre_max(0, box2.bsize1 + 1);	
    local_idx  = idx_local % box1.lsize2;	      
    idx_local  = idx_local / box1.lsize2;		      
    i1 += (local_idx*box1.strides2 + box1.bstart2) * hypre_boxD1;
    hypre_boxD1 *= hypre_max(0, box1.bsize2 + 1);	
    i2 += (local_idx*box2.strides2 + box2.bstart2) * hypre_boxD2;
    hypre_boxD2 *= hypre_max(0, box2.bsize2 + 1);
    
	if (id < hypre__tot)
		sum = a[i1] * hypre_conj(b[i2]);
    *(shared_cache + threadIdx.x) = sum;
	
    __syncthreads();
	
    ///////// sum of internal cache
	
    int i;    
    
    for (i=(BLOCKSIZE /2); i>0 ; i= i/2){
		if (threadIdx.x < i){
			*(shared_cache + threadIdx.x) += *(shared_cache + threadIdx.x + i);
		}
		__syncthreads();
    }
	
    if ( threadIdx.x == 0){
        *(c+ blockIdx.x) = shared_cache[0];
    }
}

template<class T>
__global__ void reduction_mult (T * a, T * b, HYPRE_Int hypre__tot,
				hypre_Boxloop box1)
{
    int id = (blockIdx.x * blockDim.x) + threadIdx.x;
	HYPRE_Int local_idx;
    HYPRE_Int d,idx_local = id;
    HYPRE_Int hypre_boxD1 = 1.0;
    HYPRE_Int i1 = 0;
    //// reducted output
    __shared__ T shared_cache [BLOCKSIZE];
    T sum = 0;
    local_idx  = idx_local % box1.lsize0;
    idx_local  = idx_local / box1.lsize0;
    i1 += (local_idx*box1.strides0 + box1.bstart0) * hypre_boxD1;
    hypre_boxD1 *= hypre_max(0, box1.bsize0 + 1);
    local_idx  = idx_local % box1.lsize1;
    idx_local  = idx_local / box1.lsize1;
    i1 += (local_idx*box1.strides1 + box1.bstart1) * hypre_boxD1;
    hypre_boxD1 *= hypre_max(0, box1.bsize1 + 1);	
    local_idx  = idx_local % box1.lsize2;	      
    idx_local  = idx_local / box1.lsize2;		      
    i1 += (local_idx*box1.strides2 + box1.bstart2) * hypre_boxD1;
    hypre_boxD1 *= hypre_max(0, box1.bsize2 + 1);	
    if (id < hypre__tot)
      sum = a[i1];
    *(shared_cache + threadIdx.x) = sum;
    
    __syncthreads();
    
    ///////// sum of internal cache
    
    int i;    
    
    for (i=(BLOCKSIZE /2); i>0 ; i= i/2){
      if (threadIdx.x < i){
	*(shared_cache + threadIdx.x) *= *(shared_cache + threadIdx.x + i);
      }
      __syncthreads();
    }
    
    if ( threadIdx.x == 0){
      *(b+ blockIdx.x) = shared_cache[0];
    }
}
  
}

#define hypre_CallocIndex(indexVar, dim)									\
	cudaMallocManaged((void**)&indexVar,sizeof(HYPRE_Int)*dim, cudaMemAttachGlobal);

#define hypre_CallocReal(indexVar, dim)								\
	cudaMallocManaged((void**)&indexVar,sizeof(HYPRE_Real)*dim, cudaMemAttachGlobal);


#define zypre_BoxLoopCUDAInit(ndim)											\
	HYPRE_Int hypre__tot = 1.0;											\
	for (HYPRE_Int i = 0;i < ndim;i ++)									\
		hypre__tot *= loop_size[i];


#define zypre_BoxLoopCUDADeclare()										\
	HYPRE_Int local_idx;												\
	HYPRE_Int d,idx_local = idx;

#define zypre_newBoxLoop0Begin(ndim, loop_size,idx)			\
{    														\
    HYPRE_Int hypre__tot = 1.0;												\
    for (HYPRE_Int d = 0;d < ndim;d ++)										\
	{																	\
		hypre__tot *= loop_size[d];										\
	}																	\
	BoxLoopforall(cuda_traversal(),hypre__tot,[=] __device__ (HYPRE_Int idx) \
	{


#define zypre_newBoxLoop0End()					\
	});											\
	cudaDeviceSynchronize();					\
}

extern "C++" {
template <typename LOOP_BODY>
__global__ void forall_kernel1(LOOP_BODY loop_body, hypre_Boxloop box1,int length)
{
	int idx = blockDim.x * blockIdx.x + threadIdx.x;
	if (idx < length)
		loop_body(idx,box1);
}

template<typename LOOP_BODY>
void BoxLoop1forall (cuda_traversal, HYPRE_Int length, hypre_Boxloop box1, LOOP_BODY loop_body)
{	
	size_t const blockSize = 128;
	size_t gridSize  = (length + blockSize - 1) / blockSize;
	if (gridSize == 0) gridSize = 1;
	
	//hypre_printf("length= %d, blocksize = %d, gridsize = %d\n",length,blockSize,gridSize);
	forall_kernel1<<<gridSize, blockSize>>>(loop_body,box1,length);
}
}

#define zypre_BoxLoopDataDeclareK(k,ndim,loop_size,dbox,start,stride)	\
	hypre_Boxloop databox##k;											\
	databox##k.lsize0 = loop_size[0];									\
	databox##k.strides0 = stride[0];									\
	databox##k.bstart0  = start[0] - dbox->imin[0];					\
	databox##k.bsize0   = dbox->imax[0]-dbox->imin[0];				\
	databox##k.lsize1 = loop_size[1];									\
	databox##k.strides1 = stride[1];									\
	databox##k.bstart1  = start[1] - dbox->imin[1];					\
	databox##k.bsize1   = dbox->imax[1]-dbox->imin[1];				\
	if (ndim == 3)														\
	{																	\
		databox##k.lsize2 = loop_size[2];								\
		databox##k.strides2 = stride[2];								\
		databox##k.bstart2  = start[2] - dbox->imin[2];				\
		databox##k.bsize2   = dbox->imax[2]-dbox->imin[2];			\
	}																	\
	else																\
	{																	\
		databox##k.lsize2 = 0;											\
		databox##k.strides2 = 0;									\
		databox##k.bstart2  = 0;									\
		databox##k.bsize2   = 0;							\
	}

#define zypre_newBoxLoop1Begin(ndim, loop_size,				\
							   dbox1, start1, stride1, i1) 	\
{    														\
    zypre_BoxLoopCUDAInit(ndim);					\
    zypre_BoxLoopDataDeclareK(1,ndim,loop_size,dbox1,start1,stride1);	\
    BoxLoopforall(cuda_traversal(),hypre__tot,[=] __device__ (HYPRE_Int idx) \
    {									\
      zypre_BoxLoopCUDADeclare();					\
      HYPRE_Int hypre_boxD1 = 1.0;					\
      HYPRE_Int i1 = 0;							\
      local_idx  = idx_local % databox1.lsize0;				\
      idx_local  = idx_local / databox1.lsize0;				\
      i1 += (local_idx*databox1.strides0 + databox1.bstart0) * hypre_boxD1; \
      hypre_boxD1 *= hypre_max(0, databox1.bsize0 + 1);			\
      local_idx  = idx_local % databox1.lsize1;				\
      idx_local  = idx_local / databox1.lsize1;				\
      i1 += (local_idx*databox1.strides1 + databox1.bstart1) * hypre_boxD1; \
      hypre_boxD1 *= hypre_max(0, databox1.bsize1 + 1);			\
      local_idx  = idx_local % databox1.lsize2;				\
      idx_local  = idx_local / databox1.lsize2;				\
      i1 += (local_idx*databox1.strides2 + databox1.bstart2) * hypre_boxD1; \
      hypre_boxD1 *= hypre_max(0, databox1.bsize2 + 1);
      
#define zypre_newBoxLoop1End(i1)				\
	});											\
cudaError err = cudaGetLastError();\
if ( cudaSuccess != err ) {\
printf("\n ERROR zypre_newBoxLoop1End: %s in %s(%d) function %s\n",cudaGetErrorString(err),__FILE__,__LINE__,__FUNCTION__); \
int *p = NULL; *p = 1;\
}\
}

extern "C++" {
template <typename LOOP_BODY>
__global__ void forall_kernel2(LOOP_BODY loop_body, hypre_Boxloop box1,hypre_Boxloop box2,int length)
{
	int idx = blockDim.x * blockIdx.x + threadIdx.x;
	if (idx < length)
		loop_body(idx,box1,box2);
}

template<typename LOOP_BODY>
void BoxLoop2forall (cuda_traversal, HYPRE_Int length, hypre_Boxloop box1,hypre_Boxloop box2, LOOP_BODY loop_body)
{	
	size_t const blockSize = 128;
	size_t gridSize  = (length + blockSize - 1) / blockSize;
	if (gridSize == 0) gridSize = 1;
	
	//hypre_printf("length= %d, blocksize = %d, gridsize = %d\n",length,blockSize,gridSize);
	forall_kernel2<<<gridSize, blockSize>>>(loop_body,box1,box2,length);
}
}
	
#define zypre_newBoxLoop2Begin(ndim, loop_size,				\
                                dbox1, start1, stride1, i1,	\
                                dbox2, start2, stride2, i2)	\
{    														\
        zypre_BoxLoopCUDAInit(ndim);						\
	zypre_BoxLoopDataDeclareK(1,ndim,loop_size,dbox1,start1,stride1); \
	zypre_BoxLoopDataDeclareK(2,ndim,loop_size,dbox2,start2,stride2); \
	BoxLoop2forall(cuda_traversal(),hypre__tot,databox1,databox2,[=] __device__ (HYPRE_Int idx,hypre_Boxloop box1,hypre_Boxloop box2) \
	{																	\
	    zypre_BoxLoopCUDADeclare()											\
	    HYPRE_Int hypre_boxD1 = 1.0,hypre_boxD2 = 1.0;						\
	    HYPRE_Int i1 = 0, i2 = 0;											\
		local_idx  = idx_local % box1.lsize0;							\
		idx_local  = idx_local / box1.lsize0;							\
		i1 += (local_idx*box1.strides0 + box1.bstart0) * hypre_boxD1;			\
		hypre_boxD1 *= hypre_max(0, box1.bsize0 + 1);					\
		i2 += (local_idx*box2.strides0 + box2.bstart0) * hypre_boxD2;			\
		hypre_boxD2 *= hypre_max(0, box2.bsize0 + 1);					\
		local_idx  = idx_local % box1.lsize1;							\
		idx_local  = idx_local / box1.lsize1;							\
		i1 += (local_idx*box1.strides1 + box1.bstart1) * hypre_boxD1;			\
		hypre_boxD1 *= hypre_max(0, box1.bsize1 + 1);					\
		i2 += (local_idx*box2.strides1 + box2.bstart1) * hypre_boxD2;			\
		hypre_boxD2 *= hypre_max(0, box2.bsize1 + 1);					\
		local_idx  = idx_local % box1.lsize2;							\
		idx_local  = idx_local / box1.lsize2;							\
		i1 += (local_idx*box1.strides2 + box1.bstart2) * hypre_boxD1;			\
		hypre_boxD1 *= hypre_max(0, box1.bsize2 + 1);					\
		i2 += (local_idx*box2.strides2 + box2.bstart2) * hypre_boxD2;			\
		hypre_boxD2 *= hypre_max(0, box2.bsize2 + 1);					\



#define zypre_newBoxLoop2End(i1, i2)			\
	});											\
cudaError err = cudaGetLastError();\
if ( cudaSuccess != err ) {\
printf("\n ERROR zypre_newBoxLoop2End: %s in %s(%d) function %s\n",cudaGetErrorString(err),__FILE__,__LINE__,__FUNCTION__); \
int *p = NULL; *p = 1;\
}\
AxCheckError(cudaDeviceSynchronize());\
}

extern "C++" {
template <typename LOOP_BODY>
__global__ void forall_kernel3(LOOP_BODY loop_body, hypre_Boxloop box1,hypre_Boxloop box2,hypre_Boxloop box3,int length)
{
	int idx = blockDim.x * blockIdx.x + threadIdx.x;
	if (idx < length)
		loop_body(idx,box1,box2,box3);
}

template<typename LOOP_BODY>
void BoxLoop3forall (cuda_traversal, HYPRE_Int length, hypre_Boxloop box1,hypre_Boxloop box2,hypre_Boxloop box3, LOOP_BODY loop_body)
{	
	size_t const blockSize = 128;
	size_t gridSize  = (length + blockSize - 1) / blockSize;
	if (gridSize == 0) gridSize = 1;
	
	//hypre_printf("length= %d, blocksize = %d, gridsize = %d\n",length,blockSize,gridSize);
	forall_kernel3<<<gridSize, blockSize>>>(loop_body,box1,box2,box3,length);
}
}

#define zypre_newBoxLoop3Begin(ndim, loop_size,				\
			       dbox1, start1, stride1, i1,		\
			       dbox2, start2, stride2, i2,		\
			       dbox3, start3, stride3, i3)		\
  {									\
        zypre_BoxLoopCUDAInit(ndim);						\
	zypre_BoxLoopDataDeclareK(1,ndim,loop_size,dbox1,start1,stride1); \
	zypre_BoxLoopDataDeclareK(2,ndim,loop_size,dbox2,start2,stride2); \
        zypre_BoxLoopDataDeclareK(3,ndim,loop_size,dbox3,start3,stride3); \
   BoxLoop3forall(cuda_traversal(),hypre__tot,databox1,databox2,databox3,[=] __device__ (HYPRE_Int idx,hypre_Boxloop box1,hypre_Boxloop box2,hypre_Boxloop box3) \
      {							\
	zypre_BoxLoopCUDADeclare();					\
	HYPRE_Int hypre_boxD1 = 1.0,hypre_boxD2 = 1.0,hypre_boxD3 = 1.0; \
	HYPRE_Int i1 = 0, i2 = 0, i3 = 0;				\
	local_idx  = idx_local % box1.lsize0;				\
	idx_local  = idx_local / box1.lsize0;				\
	i1 += (local_idx*box1.strides0 + box1.bstart0) * hypre_boxD1;	\
	hypre_boxD1 *= hypre_max(0, box1.bsize0 + 1);			\
	i2 += (local_idx*box2.strides0 + box2.bstart0) * hypre_boxD2;	\
	hypre_boxD2 *= hypre_max(0, box2.bsize0 + 1);			\
	i3 += (local_idx*box3.strides0 + box3.bstart0) * hypre_boxD3;	\
	hypre_boxD3 *= hypre_max(0, box3.bsize0 + 1);			\
	local_idx  = idx_local % box1.lsize1;				\
	idx_local  = idx_local / box1.lsize1;				\
	i1 += (local_idx*box1.strides1 + box1.bstart1) * hypre_boxD1;	\
	hypre_boxD1 *= hypre_max(0, box1.bsize1 + 1);			\
	i2 += (local_idx*box2.strides1 + box2.bstart1) * hypre_boxD2;	\
	hypre_boxD2 *= hypre_max(0, box2.bsize1 + 1);			\
	i3 += (local_idx*box3.strides1 + box3.bstart1) * hypre_boxD3;	\
	hypre_boxD3 *= hypre_max(0, box3.bsize1 + 1);			\
	local_idx  = idx_local % box1.lsize2;				\
	idx_local  = idx_local / box1.lsize2;				\
	i1 += (local_idx*box1.strides2 + box1.bstart2) * hypre_boxD1;	\
	hypre_boxD1 *= hypre_max(0, box1.bsize2 + 1);			\
	i2 += (local_idx*box2.strides2 + box2.bstart2) * hypre_boxD2;	\
	hypre_boxD2 *= hypre_max(0, box2.bsize2 + 1);			\
	i3 += (local_idx*box3.strides2 + box3.bstart2) * hypre_boxD3;	\
	hypre_boxD3 *= hypre_max(0, box3.bsize2 + 1);			\
	

#define zypre_newBoxLoop3End(i1, i2,i3)			\
	});											\
cudaError err = cudaGetLastError();\
if ( cudaSuccess != err ) {\
printf("\n ERROR zypre_newBoxLoop3End: %s in %s(%d) function %s\n",cudaGetErrorString(err),__FILE__,__LINE__,__FUNCTION__); \
int *p = NULL; *p = 1;\
}\
}

#define zypre_newBoxLoop4Begin(ndim, loop_size,				\
			       dbox1, start1, stride1, i1,		\
			       dbox2, start2, stride2, i2,		\
			       dbox3, start3, stride3, i3,		\
			       dbox4, start4, stride4, i4)		\
{								       \
     zypre_BoxLoopCUDAInit(ndim);						\
     zypre_BoxLoopDataDeclareK(1,ndim,loop_size,dbox1,start1,stride1); \
     zypre_BoxLoopDataDeclareK(2,ndim,loop_size,dbox2,start2,stride2); \
     zypre_BoxLoopDataDeclareK(3,ndim,loop_size,dbox3,start3,stride3); \
     zypre_BoxLoopDataDeclareK(4,ndim,loop_size,dbox4,start4,stride4); \
  BoxLoopforall(cuda_traversal(),hypre__tot,[=] __device__ (HYPRE_Int idx) \
		{							\
		  zypre_BoxLoopCUDADeclare();				\
		  HYPRE_Int hypre_boxD1 = 1.0,hypre_boxD2 = 1.0,hypre_boxD3 = 1.0,hypre_boxD4 = 1.0; \
		  HYPRE_Int i1 = 0, i2 = 0, i3 = 0,i4 = 0;		\
		  local_idx  = idx_local % databox1.lsize0;			\
		  idx_local  = idx_local / databox1.lsize0;			\
		  i1 += (local_idx*databox1.strides0 + databox1.bstart0) * hypre_boxD1;	\
		  hypre_boxD1 *= hypre_max(0, databox1.bsize0 + 1);		\
		  i2 += (local_idx*databox2.strides0 + databox2.bstart0) * hypre_boxD2;	\
		  hypre_boxD2 *= hypre_max(0, databox2.bsize0 + 1);		\
		  i3 += (local_idx*databox3.strides0 + databox3.bstart0) * hypre_boxD3;	\
		  hypre_boxD3 *= hypre_max(0, databox3.bsize0 + 1);		\
		  i4 += (local_idx*databox4.strides0 + databox4.bstart0) * hypre_boxD4;	\
		  hypre_boxD4 *= hypre_max(0, databox4.bsize0 + 1);		\
		  local_idx  = idx_local % databox1.lsize1;			\
		  idx_local  = idx_local / databox1.lsize1;			\
		  i1 += (local_idx*databox1.strides1 + databox1.bstart1) * hypre_boxD1;	\
		  hypre_boxD1 *= hypre_max(0, databox1.bsize1 + 1);		\
		  i2 += (local_idx*databox2.strides1 + databox2.bstart1) * hypre_boxD2;	\
		  hypre_boxD2 *= hypre_max(0, databox2.bsize1 + 1);		\
		  i3 += (local_idx*databox3.strides1 + databox3.bstart1) * hypre_boxD3;	\
		  hypre_boxD3 *= hypre_max(0, databox3.bsize1 + 1);		\
		  i4 += (local_idx*databox4.strides1 + databox4.bstart1) * hypre_boxD4;	\
		  hypre_boxD4 *= hypre_max(0, databox4.bsize1 + 1);		\
		  local_idx  = idx_local % databox1.lsize2;			\
		  idx_local  = idx_local / databox1.lsize2;			\
		  i1 += (local_idx*databox1.strides2 + databox1.bstart2) * hypre_boxD1;	\
		  hypre_boxD1 *= hypre_max(0, databox1.bsize2 + 1);		\
		  i2 += (local_idx*databox2.strides2 + databox2.bstart2) * hypre_boxD2;	\
		  hypre_boxD2 *= hypre_max(0, databox2.bsize2 + 1);		\
		  i3 += (local_idx*databox3.strides2 + databox3.bstart2) * hypre_boxD3;	\
		  hypre_boxD3 *= hypre_max(0, databox3.bsize2 + 1);		\
		  i4 += (local_idx*databox4.strides2 + databox4.bstart2) * hypre_boxD4;	\
		  hypre_boxD4 *= hypre_max(0, databox4.bsize2 + 1);		\
		
#define zypre_newBoxLoop4End(i1, i2, i3, i4)	\
  });						\
  cudaError err = cudaGetLastError();		\
  if ( cudaSuccess != err ) {						\
    printf("\n ERROR zypre_newBoxLoop4End: %s in %s(%d) function %s\n",cudaGetErrorString(err),__FILE__,__LINE__,__FUNCTION__); \
    int *p = NULL; *p = 1;						\
  }									\
  AxCheckError(cudaDeviceSynchronize());				\
}

#define MAX_BLOCK 512

extern "C++" {
template<class T>
__inline__ __device__
int fake_shfl_down(T val, HYPRE_Int offset, HYPRE_Int width=32) {
  static __shared__ T shared[MAX_BLOCK];
  HYPRE_Int lane=threadIdx.x%32;

  shared[threadIdx.x]=val;
  __syncthreads();

  val = (lane+offset<width) ? shared[threadIdx.x+offset] : 0;
  __syncthreads();

  return val;
}

template<class T>  
__inline__ __device__
HYPRE_Real warpReduceSum (T val) {
  for (int offset = warpSize/2; offset > 0; offset /= 2)
    val += __shfl_down(val,offset);
  return val;
}


template<class T> 
__inline__ __device__
HYPRE_Real blockReduceSum(T val) {
  static __shared__ T shared[32];
  HYPRE_Int lane=threadIdx.x%warpSize;
  HYPRE_Int wid=threadIdx.x/warpSize;
  val=warpReduceSum<T>(val);

  //write reduced value to shared memory
  if(lane==0) shared[wid]=val;
  __syncthreads();

  //ensure we only grab a value from shared memory if that warp existed
  val = (threadIdx.x<blockDim.x/warpSize) ? shared[lane] : int(0);
  if(wid==0) val=warpReduceSum<T>(val);

  return val;
}

template<class T>
__global__ void hypre_device_reduce_stable_kernel(T*a, T*b, T* out, HYPRE_Int N,
						  hypre_Boxloop box1,hypre_Boxloop box2) {
  HYPRE_Int local_idx;
  HYPRE_Int d,idx_local;
  HYPRE_Int hypre_boxD1 = 1.0,hypre_boxD2 = 1.0;
  HYPRE_Int i1 = 0, i2 = 0;
  T sum=T(0);
  HYPRE_Int i;
  
  for(i=blockIdx.x*blockDim.x+threadIdx.x;i<N;i+=blockDim.x*gridDim.x)
  {
    idx_local = i;
    local_idx  = idx_local % box1.lsize0;
    idx_local  = idx_local / box1.lsize0;
    i1 += (local_idx*box1.strides0 + box1.bstart0) * hypre_boxD1;
    hypre_boxD1 *= hypre_max(0, box1.bsize0 + 1);
    i2 += (local_idx*box2.strides0 + box2.bstart0) * hypre_boxD2;
    hypre_boxD2 *= hypre_max(0, box2.bsize0 + 1);
    local_idx  = idx_local % box1.lsize1;
    idx_local  = idx_local / box1.lsize1;
    i1 += (local_idx*box1.strides1 + box1.bstart1) * hypre_boxD1;
    hypre_boxD1 *= hypre_max(0, box1.bsize1 + 1);
    i2 += (local_idx*box2.strides1 + box2.bstart1) * hypre_boxD2;   
    hypre_boxD2 *= hypre_max(0, box2.bsize1 + 1);	
    local_idx  = idx_local % box1.lsize2;	      
    idx_local  = idx_local / box1.lsize2;		      
    i1 += (local_idx*box1.strides2 + box1.bstart2) * hypre_boxD1;
    hypre_boxD1 *= hypre_max(0, box1.bsize2 + 1);	
    i2 += (local_idx*box2.strides2 + box2.bstart2) * hypre_boxD2;
    hypre_boxD2 *= hypre_max(0, box2.bsize2 + 1);
    sum += a[i1] * hypre_conj(b[i2]);
  }
  sum=blockReduceSum<T>(sum);
  if(threadIdx.x==0)
    out[blockIdx.x]=sum;
}

template<class T>       
__global__ void hypre_device_reduce_stable_kernel2(T *in, T* out, int N) {
  T sum=T(0);
  for(int i=blockIdx.x*blockDim.x+threadIdx.x;i<N;i+=blockDim.x*gridDim.x) {
    sum+=in[i];
  }
  sum=blockReduceSum<T>(sum);
  if(threadIdx.x==0)
    out[blockIdx.x]=sum;
}

template<class T>   
void hypre_device_reduce_stable(T*a,T*b, T* out, HYPRE_Int N,
				hypre_Boxloop box1,hypre_Boxloop box2) {
  HYPRE_Int threads=512;
  HYPRE_Int blocks=min((N+threads-1)/threads,1024);

  hypre_device_reduce_stable_kernel<T><<<blocks,threads>>>(a,b,out,N,box1,box2);
  hypre_device_reduce_stable_kernel2<T><<<1,1024>>>(out,out,blocks); 
}

}


#define zypre_newBoxLoop2ReductionCUDA(ndim, loop_size,				\
				       dbox1, start1, stride1, i1,xp,	\
				       dbox2, start2, stride2, i2,yp,sum) \
{    									   \
        zypre_BoxLoopCUDAInit(ndim);						\
	zypre_BoxLoopDataDeclareK(1,ndim,loop_size,dbox1,start1,stride1); \
	zypre_BoxLoopDataDeclareK(2,ndim,loop_size,dbox2,start2,stride2); \
        HYPRE_Real *d_c;				      		\
        cudaMalloc((void**) &d_c, 1024 * sizeof(HYPRE_Real));			\
        hypre_device_reduce_stable<HYPRE_Real>(xp,yp,d_c,hypre__tot,databox1,databox2);\
        cudaMemcpy(&sum,d_c,sizeof(HYPRE_Real),cudaMemcpyDeviceToHost);\
        cudaFree(d_c);\
}


/*
	int n_blocks = (hypre__tot+BLOCKSIZE-1)/BLOCKSIZE;					\
	HYPRE_Real *d_c;													\
	HYPRE_Real * c = new HYPRE_Real[n_blocks];							\
	cudaMalloc((void**) &d_c, n_blocks * sizeof(HYPRE_Real));			\
	dot<HYPRE_Real><<< n_blocks ,BLOCKSIZE>>>(xp,yp,d_c,hypre__tot,databox1,databox2);		\
        cudaError err = cudaGetLastError();\
if ( cudaSuccess != err ) {\
  printf("\n ERROR zypre_newBoxLoop2Reduction: %s in %s(%d) function %s\n",cudaGetErrorString(err),__FILE__,__LINE__,__FUNCTION__); \
  int *p = NULL; *p = 1;						\
 }									\
	AxCheckError(cudaMemcpy(c,d_c,n_blocks*sizeof(HYPRE_Real),cudaMemcpyDeviceToHost)); \
	for (int j = 0 ; j< n_blocks ; ++j){								\
		sum += c[j];													\
	}																	\
	delete c;															\
}
*/

#define zypre_newBoxLoop1ReductionBegin(ndim, loop_size,				\
										dbox1, start1, stride1, i1,xp,sum) \
{    																	\
        zypre_BoxLoopCUDAInit(ndim);						\
	zypre_BoxLoopDataDeclareK(1,ndim,loop_size,dbox1,start1,stride1); \
	int n_blocks = (hypre__tot+BLOCKSIZE-1)/BLOCKSIZE;					\
	HYPRE_Real *d_b;													\
	HYPRE_Real * b = new HYPRE_Real[n_blocks];							\
	cudaMalloc((void**) &d_b, n_blocks * sizeof(HYPRE_Real));			\
	reduction_mult<HYPRE_Real><<< n_blocks ,BLOCKSIZE>>>(xp,d_b,hypre__tot,databox1);		\
cudaError err = cudaGetLastError();\
if ( cudaSuccess != err ) {\
printf("\n ERROR zypre_newBoxLoop1Reduction: %s in %s(%d) function %s\n",cudaGetErrorString(err),__FILE__,__LINE__,__FUNCTION__); \
int *p = NULL; *p = 1;\
}\
AxCheckError(cudaMemcpy(b,d_b,n_blocks*sizeof(HYPRE_Real),cudaMemcpyDeviceToHost)); \
	for (int j = 0 ; j< n_blocks ; ++j){								\
		sum += b[j];											\
	}																	\
	delete b;															\
}

#define zypre_BoxBoundaryCopyBegin(ndim, loop_size, stride1, i1, idx) 	\
{    														\
    HYPRE_Int hypre__tot = 1.0;											\
	const size_t block_size = 256;										\
	hypre_Boxloop databox1;												\
	HYPRE_Int nd = ndim;												\
	databox1.lsize0 = loop_size[0];									\
    databox1.lsize1 = loop_size[1];									\
	databox1.lsize2 = loop_size[2];									\
	databox1.strides0 = stride1[0];										\
	databox1.strides1 = stride1[1];										\
	databox1.strides2 = stride1[2];										\
	for (HYPRE_Int d = 0;d < ndim;d ++)									\
	{																	\
		hypre__tot *= loop_size[d];										\
	}																	\
	BoxLoopforall(cuda_traversal(),hypre__tot,[=] __device__ (HYPRE_Int idx) \
	{																	\
	    zypre_BoxLoopCUDADeclare()											\
	    HYPRE_Int i1 = 0;											\
		local_idx  = idx_local % databox1.lsize0;							\
		idx_local  = idx_local / databox1.lsize0;							\
		i1 += local_idx*databox1.strides0;			\
		local_idx  = idx_local % databox1.lsize1;							\
		idx_local  = idx_local / databox1.lsize1;							\
		i1 += local_idx*databox1.strides1;								\
		local_idx  = idx_local % databox1.lsize2;							\
		idx_local  = idx_local / databox1.lsize2;							\
		i1 += local_idx*databox1.strides2;			\
		
#define zypre_BoxBoundaryCopyEnd()				\
	});											\
cudaError err = cudaGetLastError();\
if ( cudaSuccess != err ) {\
printf("\n ERROR zypre_newBoxLoop1End: %s in %s(%d) function %s\n",cudaGetErrorString(err),__FILE__,__LINE__,__FUNCTION__); \
int *p = NULL; *p = 1;\
}\
}

#define zypre_BoxDataExchangeBegin(ndim, loop_size,				\
                                   stride1, i1,	\
                                   stride2, i2)	\
{    														\
    HYPRE_Int hypre__tot = 1.0;											\
	const size_t block_size = 256;										\
	hypre_Boxloop databox1,databox2;									\
	HYPRE_Int nd = ndim;												\
	databox1.lsize0 = loop_size[0];									\
    databox1.lsize1 = loop_size[1];									\
	databox1.lsize2 = loop_size[2];									\
	databox1.strides0 = stride1[0];										\
	databox1.strides1 = stride1[1];										\
	databox1.strides2 = stride1[2];										\
	databox2.lsize0 = loop_size[0];									\
    databox2.lsize1 = loop_size[1];									\
	databox2.lsize2 = loop_size[2];									\
	databox2.strides0 = stride2[0];										\
	databox2.strides1 = stride2[1];										\
	databox2.strides2 = stride2[2];										\
	for (HYPRE_Int d = 0;d < ndim;d ++)									\
	{																	\
		hypre__tot *= loop_size[d];										\
	}																	\
	BoxLoop2forall(cuda_traversal(),hypre__tot,databox1,databox2,[=] __device__ (HYPRE_Int idx,hypre_Boxloop box1,hypre_Boxloop box2) \
	{																	\
	    zypre_BoxLoopCUDADeclare()											\
	    HYPRE_Int hypre_boxD1 = 1.0,hypre_boxD2 = 1.0;						\
	    HYPRE_Int i1 = 0, i2 = 0;											\
		local_idx  = idx_local % box1.lsize0;							\
		idx_local  = idx_local / box1.lsize0;							\
		i1 += local_idx*box1.strides0;			\
		i2 += local_idx*box2.strides0;			\
		local_idx  = idx_local % box1.lsize1;							\
		idx_local  = idx_local / box1.lsize1;							\
		i1 += local_idx*box1.strides1;			\
		i2 += local_idx*box2.strides1;			\
		local_idx  = idx_local % box1.lsize2;							\
		idx_local  = idx_local / box1.lsize2;							\
		i1 += local_idx*box1.strides2;			\
		i2 += local_idx*box2.strides2;



#define zypre_BoxDataExchangeEnd()				\
	});											\
cudaError err = cudaGetLastError();\
if ( cudaSuccess != err ) {\
printf("\n ERROR zypre_newBoxLoop2End: %s in %s(%d) function %s\n",cudaGetErrorString(err),__FILE__,__LINE__,__FUNCTION__); \
int *p = NULL; *p = 1;\
}\
AxCheckError(cudaDeviceSynchronize());\
}

#define hypre_SerialBoxLoop1Begin(ndim, loop_size,			\
				  dbox1, start1, stride1, i1)		\
{									\
        HYPRE_Int hypre__tot = 1.0;						\
	const size_t block_size = 256;					\
	HYPRE_Int nd = ndim;						\
	for (HYPRE_Int d = 0;d < ndim;d ++)				\
	{								\
	    hypre__tot *= loop_size[d];					\
	}								\
	HYPRE_Int idx;							\
	for (idx = 0;idx < hypre__tot;idx ++)				\
	{								\
	    zypre_BoxLoopCUDADeclare()											\
	    HYPRE_Int hypre_boxD1 = 1.0;				\
	    HYPRE_Int i1 = 0;						\
	    for (d = 0;d < ndim;d ++)					\
	    {								\
	      local_idx  = idx_local % loop_size[d];			\
	      idx_local  = idx_local / loop_size[d];			\
	      i1 += (local_idx*stride1[d] + start1[d] - hypre_BoxIMinD(dbox1, d)) * hypre_boxD1; \
	      hypre_boxD1 *= hypre_BoxSizeD(dbox1, d);			\
	    }

#define hypre_SerialBoxLoop1End(i1)				\
        }								\
}

#define zypre_newBoxLoop0For() {}

#define zypre_newBoxLoop1For(i1) {}

#define zypre_newBoxLoop2For(i1, i2) {}

#define zypre_newBoxLoop3For(i1, i2, i3) {}

#define zypre_newBoxLoop4For(i1, i2, i3, i4) {}

#define zypre_newBoxLoopSetOneBlock() {}


//#define hypre_BoxLoopGetIndex    zypre_newBoxLoopGetIndex
#define hypre_BoxLoopSetOneBlock zypre_newBoxLoopSetOneBlock
//#define hypre_BoxLoopBlock       zypre_newBoxLoopBlock

#define hypre_BoxLoop0Begin      zypre_BoxLoop0Begin
#define hypre_BoxLoop0For        zypre_BoxLoop0For
#define hypre_BoxLoop0End        zypre_BoxLoop0End
#define hypre_BoxLoop1Begin      zypre_newBoxLoop1Begin
#define hypre_BoxLoop1For        zypre_newBoxLoop1For
#define hypre_BoxLoop1End        zypre_newBoxLoop1End
#define hypre_BoxLoop2Begin      zypre_newBoxLoop2Begin
#define hypre_BoxLoop2For        zypre_newBoxLoop2For
#define hypre_BoxLoop2End        zypre_newBoxLoop2End
#define hypre_BoxLoop3Begin      zypre_newBoxLoop3Begin
#define hypre_BoxLoop3For        zypre_newBoxLoop3For
#define hypre_BoxLoop3End        zypre_newBoxLoop3End
#define hypre_BoxLoop4Begin      zypre_newBoxLoop4Begin
#define hypre_BoxLoop4For        zypre_newBoxLoop4For
#define hypre_BoxLoop4End        zypre_newBoxLoop4End

#define hypre_BoxLoop1Begin1      zypre_newBoxLoop1Begin1
#define hypre_BoxLoop1End1        zypre_newBoxLoop1End1
