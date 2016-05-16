#include "nngridder.h"

// GPU routines for nngridder

#define BLOCKDIM (8)
#define THREADSPERBLOCK (8*8*8)

#define LOG2_WARP_SIZE 5U
#define WARP_SIZE (1U << LOG2_WARP_SIZE)

__shared__ volatile uint scanScratch[THREADSPERBLOCK * 2];
__shared__ uint scanOutput[2][THREADSPERBLOCK];
__shared__ uint atomIndices[THREADSPERBLOCK];
__shared__ uint atomMask[THREADSPERBLOCK];


//do a scan and return ptr to result (could be either place in double-buffer)
__device__ uint* scan(int thid)
{
	int pout = 0, pin = 1;
// load input into shared memory.
// This is exclusive scan, so shift right by one and set first elt to 0
	scanOutput[0][thid] = (thid > 0) ? atomMask[thid - 1] : 0;
	scanOutput[1][thid] = 0;
	__syncthreads();
	
	for(int offset = 1; offset < THREADSPERBLOCK; offset *= 2){
		pout = 1 - pout; // swap double buffer indices
		pin = 1 - pin;
		if(thid >= offset)
			scanOutput[pout][thid] = scanOutput[pin][thid] + scanOutput[pin][thid - offset];
		else
			scanOutput[pout][thid] = scanOutput[pin][thid];
		__syncthreads();
	}
	return scanOutput[pout];
}

//Almost the same as naive scan1Inclusive, but doesn't need __syncthreads()
//assuming size <= WARP_SIZE
inline __device__ uint
warpScanInclusive(int threadIndex, uint idata, volatile uint *s_Data, uint size){
	uint pos = 2 * threadIndex - (threadIndex & (size - 1));
	s_Data[pos] = 0;
	pos += size;
	s_Data[pos] = idata;

	for(uint offset = 1; offset < size; offset <<= 1)
	s_Data[pos] += s_Data[pos - offset];

	return s_Data[pos];
}

inline __device__ uint
warpScanExclusive(int threadIndex, uint idata, volatile uint *sScratch, uint size){
	return warpScanInclusive(threadIndex, idata, sScratch, size) - idata;
}

__inline__ __device__ void
sharedMemExclusiveScan(int threadIndex, uint* sInput, uint* sOutput, volatile uint* sScratch, uint size)
{
	uint idata = sInput[threadIndex];

	//Bottom-level inclusive warp scan
	uint warpResult = warpScanInclusive(threadIndex, idata, sScratch, WARP_SIZE);

	// Save top elements of each warp for exclusive warp scan sync
	// to wait for warp scans to complete (because s_Data is being
	// overwritten)
	__syncthreads();

	if ( (threadIndex & (WARP_SIZE - 1)) == (WARP_SIZE - 1) )
	sScratch[threadIndex >> LOG2_WARP_SIZE] = warpResult;

	// wait for warp scans to complete
	__syncthreads();

	if ( threadIndex < (THREADSPERBLOCK / WARP_SIZE)){
		// grab top warp elements
		uint val = sScratch[threadIndex];
		// calculate exclusive scan and write back to shared memory
		sScratch[threadIndex] = warpScanExclusive(threadIndex, val, sScratch, size >> LOG2_WARP_SIZE);
	}

	//return updated warp scans with exclusive scan results
	__syncthreads();

	sOutput[threadIndex] = warpResult + sScratch[threadIndex >> LOG2_WARP_SIZE] - idata;
}

//return squared distance between pt and (x,y,z)
__device__
float sqDistance(float3 pt,float x,float y,float z){
	float ret;
	float tmp = pt.x - x;
	ret = tmp * tmp;
	tmp = pt.y - y;
	ret += tmp * tmp;
	tmp = pt.z - z;
	ret += tmp * tmp;
	return ret;
}

//go through the n atoms referenced in atomIndices and set a grid point
template<bool Binary> __device__ void set_atoms(float3 origin, int dim, float resolution, float rmult, unsigned n, float3 *coords, short *gridindex, float *radii, float *grids)
{
	//figure out what grid point we are 
	unsigned xi = threadIdx.x + blockIdx.x*blockDim.x;
	unsigned yi = threadIdx.y + blockIdx.y*blockDim.y;
	unsigned zi = threadIdx.z + blockIdx.z*blockDim.z;

	if(xi >= dim || yi >= dim || zi >= dim)
	return;//bail if we're off-grid, this should not be common

	unsigned gsize = dim*dim*dim;
	//compute x,y,z coordinate of grid point
	float x = xi*resolution+origin.x;
	float y = yi*resolution+origin.y;
	float z = zi*resolution+origin.z;

	//iterate over all atoms
	for(unsigned ai = 0; ai < n; ai++)
	{
		unsigned i = atomIndices[ai];
		float3 coord = coords[i];
		short which = gridindex[i];

		if(which >= 0){ //because of hydrogens on ligands
			float r = radii[i];
			float rsq = r*r;
			float d = sqDistance(coord, x,y,z);

			if(Binary)
			{
				if(d < rsq)
				{
					//set gridpoint to 1
					unsigned goffset = which*gsize;
					unsigned off = (xi*dim+yi)*dim+zi;
					//printf("%f,%f,%f %d,%d,%d  %d  %d %d\n",x,y,z, xi,yi,zi, which, goffset,off);
					grids[goffset+off] = 1.0;
				}
			}
			else
			{
				//for non binary we want a gaussian were 2 std occurs at the radius
				//after which which switch to a quadratic
				//the quadratic is to fit to have both the same value and first order
				//derivative at the cross over point and a value and derivative of zero
				//at 1.5*radius
				//TODO: figure if we can do the math without sqrt
				double dist = sqrt(d);
				if (dist < r * rmult)
				{
					unsigned goffset = which*gsize;
					unsigned off = (xi*dim+yi)*dim+zi;
					unsigned gpos = goffset+off;

					if (dist <= r)
					{
						//return gaussian
						double h = 0.5 * r;
						double ex = -dist * dist / (2 * h * h);
						grids[gpos] += exp(ex);
					}
					else //return quadratic
					{
						double h = 0.5 * r;
						double eval = 1.0 / (M_E * M_E); //e^(-2)
						double q = dist * dist * eval / (h * h) - 6.0 * eval * dist / h
						+ 9.0 * eval;
						grids[gpos] += q;
					}
				}
			}
		}
	}
}

//return 1 if atom potentially overlaps block, 0 otherwise
__device__
unsigned atomOverlapsBlock(unsigned aindex,float3 origin,float resolution,
		float3 *coords,float *radii,short *gridindex,float rmult){

	if(gridindex[aindex] < 0)
		return 0; //hydrogen

	unsigned xi = blockIdx.x * BLOCKDIM;
	unsigned yi = blockIdx.y * BLOCKDIM;
	unsigned zi = blockIdx.z * BLOCKDIM;

	//compute corners of block
	float startx = xi * resolution + origin.x;
	float starty = yi * resolution + origin.y;
	float startz = zi * resolution + origin.z;

	float endx = startx + resolution * BLOCKDIM;
	float endy = starty + resolution * BLOCKDIM;
	float endz = startz + resolution * BLOCKDIM;

	float r = radii[aindex] * rmult;
	float3 center = coords[aindex];

	//does atom overlap box?
	if(center.x - r > endx)
		return 0;
	if(center.x + r < startx)
		return 0;

	if(center.y - r > endy)
		return 0;
	if(center.y + r < starty)
		return 0;

	if(center.z - r > endz)
		return 0;
	if(center.z + r < startz)
		return 0;

	return 1;
}

__device__
bool scanValid(unsigned idx,uint *scanresult){
	if(idx == 0){
		for(uint i = 1; i < THREADSPERBLOCK; i++){
			//printf("%d %d %d\n",scanOutput[i-1],scanOutput[i],atomMask[i]); 
			assert(scanresult[i] >= scanresult[i - 1]);
			if(scanresult[i] > scanresult[i - 1]){
				if(!atomMask[i-1]) {
					printf("ERROR %d,%d,%d,%d %d %d %d\n",atomMask[i-2],atomMask[i-1],atomMask[i],atomMask[i+1],scanresult[i-1],scanresult[i],i);
					
				}
				assert(atomMask[i - 1]);
			}
		}
	}
	return true;
}

//origin is grid origin
//dim is dimension of cubic grid
//resolution is grid resolution
//n is number of atoms
//coords are xyz coors
//gridindex is which grid they belong in
//radii are atom radii
//grids are the output and are assumed to be zeroed
template<bool Binary> __global__ void gpu_grid_set(float3 origin, int dim, float resolution, float rmult, int n, float3 *coords, short *gridindex, float *radii, float *grids)
{
	//PLAN: the below needs to be put into a function that takes an array of atom indices (with its length) that's in shared memory
	//then we can write a loop that, in shared memsized chunks (size of arrays should be identical to threads per block)
	//"assignCircles" the atoms, that is, in parallel evaluate if atom possibly overlaps grid, set a shared mem bitmask,
	//scan the bitmask and then do a scatter operation to create the array that will be passed to the grid point setting function

	unsigned tIndex = ((threadIdx.x*BLOCKDIM) + threadIdx.y)*BLOCKDIM+threadIdx.z;

	//there may be more than THREADPERBLOCK atoms, in which case we have to chunk them
	for(unsigned atomoffset = 0; atomoffset < n; atomoffset += THREADSPERBLOCK)
	{
		unsigned aindex = atomoffset+tIndex;
		if(aindex < n)
			atomMask[tIndex] = atomOverlapsBlock(aindex, origin, resolution, coords, radii, gridindex, rmult);
		else
			atomMask[tIndex] = 0;

		__syncthreads();

		uint *scanresult = scan(tIndex); //implicitly uses atomMask and scanOutput
		//sharedMemExclusiveScan(tIndex, atomMask, scanOutput, scanScratch, THREADSPERBLOCK);
		__syncthreads();

		unsigned nAtoms = scanresult[THREADSPERBLOCK-1] + atomMask[THREADSPERBLOCK-1];

		assert(scanValid(tIndex,scanresult));
		
			
		//printf("%d %d %d\n",nAtoms, min(THREADSPERBLOCK,n-atomoffset),n);
		//do scatter
		if(atomMask[tIndex])
		{
			atomIndices[scanresult[tIndex]] = tIndex+atomoffset;
		}
		__syncthreads();

		//atomIndex is now a list of nAtoms atom indices
		set_atoms<Binary>(origin, dim, resolution, rmult, nAtoms, coords, gridindex, radii, grids);
		__syncthreads();//everyone needs to finish before we muck with atomIndices again
	}
}

void NNGridder::setAtomsGPU(unsigned natoms,float3 *coords,short *gridindex,
		float *radii,unsigned ngrids,float *grids){
	//each thread is responsible for a grid point location and will handle all atom types
	//each block is 8x8x8=512 threads
	float3 origin = make_float3(dims[0].begin, dims[1].begin, dims[2].begin);
	dim3 threads(BLOCKDIM, BLOCKDIM, BLOCKDIM);
	unsigned dim = dims[0].n + 1;	//number of grid points
	unsigned blocksperside = ceil(dim / float(BLOCKDIM));
	dim3 blocks(blocksperside, blocksperside, blocksperside);

	CUDA_CHECK(cudaMemset(grids, 0, ngrids * dim * dim * dim * sizeof(float)));	//TODO: see if faster to do in kernel
	if(binary){
		gpu_grid_set<true><<<blocks,threads>>>(origin, dim, resolution, 1.0, natoms, coords, gridindex, radii, grids);
		CUDA_CHECK (cudaPeekAtLastError() );}
		else
		{
			gpu_grid_set<false><<<blocks,threads>>>(origin, dim, resolution, radiusmultiple, natoms, coords, gridindex, radii, grids);
			CUDA_CHECK(cudaPeekAtLastError() );
		}
	}