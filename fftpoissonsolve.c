#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <assert.h>
#include <mpi.h>
#include <fftw3-mpi.h>
#include <gsl/gsl_math.h>

#include "raytrace.h"
#include "fftpoissonsolve.h"
#include "lgadgetio.h"
#include "gridcellhash.h"

//global defs for this file
ptrdiff_t NFFT;
ptrdiff_t AllocLocal,N0Local, N0LocalStart;
int *TaskN0Local = NULL;
int *TaskN0LocalStart = NULL;
int MaxN0Local;

#ifdef DOUBLEFFTW
fftw_plan fplan = NULL,bplan = NULL;
double *fftwrin = NULL;
fftw_complex *fftwcout;
#else
fftwf_plan fplan = NULL,bplan = NULL;
float *fftwrin = NULL;
fftwf_complex *fftwcout;
#endif

static void get_partio_decomp(char *fbase, int *startFile, int *NumFiles, int *MyIOGroup, int *NumIOGroups);
static void get_units(char *fbase, double *L, long *Ntot, double *a);

void comp_pot_snap(char *fbase) 
{
  int startFile;
  int NumFiles;
  int file;
  long i,j,k,n,m;
  double L,dL,a,mp;
  long Ntot;
  float *px,*py,*pz;
  int Np;
  char fname[MAX_FILENAME];
  double dx,dy,dz;
  long ii,jj,kk;
  GridCell *gbuff = NULL;
  long Ngbuff = 0;
  long id,ind;
  double time;
  double potfact;
  int MyIOGroup,NumIOGroups,IOGroup;
  
  //init grid cell hash table
  GridCellHash *gch = init_gchash();
  assert(gch != NULL);
  
  //get units
  get_units(fbase,&L,&Ntot,&a);  
  dL = L/NFFT;
  mp = RHO_CRIT*rayTraceData.OmegaM*L*L*L/Ntot;
  potfact = FOUR_PI_G/a*mp/L/L/L;
  
  //check potential factors
  if(potfact == 0.0) {
    fprintf(stderr,"%04d: potfact is zero! potfact = %lg, a = %lg, mp %lg, L = %lg\n",ThisTask,potfact,a,mp,L);
    fflush(stderr);
    assert(potfact != 0.0);
  }
  
  //first get the file decomp
  get_partio_decomp(fbase,&startFile,&NumFiles,&MyIOGroup,&NumIOGroups);
  
  //read all parts and assign to buffer
  logProfileTag(PROFILETAG_PARTIO);
  time = -MPI_Wtime();
  if(ThisTask == 0)
    fprintf(stderr,"reading parts.\n");
  for(IOGroup=0;IOGroup<NumIOGroups;++IOGroup) {
    if(MyIOGroup == IOGroup) {
      
      fprintf(stderr,"%04d: IO group = %d, start file = %d, num files = %d\n",ThisTask,IOGroup,startFile,NumFiles);
      fflush(stderr);
      
      for(file=startFile;file<startFile+NumFiles;++file)
	{
	  //read parts
	  sprintf(fname,"%s.%d",fbase,file);
	  read_LGADGET(fname,&px,&py,&pz,NULL,&Np);
	  for(n=0;n<Np;++n)
	    {
	      px[n] *= rayTraceData.LengthConvFact;
	      py[n] *= rayTraceData.LengthConvFact;
	      pz[n] *= rayTraceData.LengthConvFact;
	    }
	  
	  //assign to grid
	  for(n=0;n<Np;++n)
	    {
	      //now place in grid
	      i = px[n]/dL;
	      dx = px[n]/dL - i;
	      ii = i + 1;
	      i = i%NFFT;
	      ii = ii%NFFT;
	      
	      j = py[n]/dL;
	      dy = py[n]/dL - j;
	      jj = j + 1;
	      j = j%NFFT;
	      jj = jj%NFFT;
	      
	      k = pz[n]/dL;
	      dz = pz[n]/dL - k;
	      kk = k + 1;
	      k = k%NFFT;
	      kk = kk%NFFT;
	      
	      id = (i*NFFT + j)*NFFT + k;
	      ind = getid_gchash(gch,id);
	      gch->GridCells[ind].val += (1.0 - dx)*(1.0 - dy)*(1.0 - dz);
	      
	      id = (i*NFFT + j)*NFFT + kk;
	      ind = getid_gchash(gch,id);
	      gch->GridCells[ind].val += (1.0 - dx)*(1.0 - dy)*dz;
	      
	      id = (i*NFFT + jj)*NFFT + k;
	      ind = getid_gchash(gch,id);
	      gch->GridCells[ind].val += (1.0 - dx)*dy*(1.0 - dz);
	      
	      id = (i*NFFT + jj)*NFFT + kk;
	      ind = getid_gchash(gch,id);
	      gch->GridCells[ind].val  += (1.0 - dx)*dy*dz;
	      
	      id = (ii*NFFT + j)*NFFT + k;
	      ind = getid_gchash(gch,id);
	      gch->GridCells[ind].val += dx*(1.0 - dy)*(1.0 - dz);
	      
	      id = (ii*NFFT + j)*NFFT + kk;
	      ind = getid_gchash(gch,id);
	      gch->GridCells[ind].val  += dx*(1.0 - dy)*dz;
	      
	      id = (ii*NFFT + jj)*NFFT + k;
	      ind = getid_gchash(gch,id);
	      gch->GridCells[ind].val += dx*dy*(1.0 - dz);
	      
	      id = (ii*NFFT + jj)*NFFT + kk;
	      ind = getid_gchash(gch,id);
	      gch->GridCells[ind].val += dx*dy*dz;
	    }
	  
	  free(px);
	  free(py);
	  free(pz);
	  
	  fprintf(stderr,"%04d: IO group = %d, file = %d\n",ThisTask,IOGroup,file);
	  fflush(stderr);
	}
      
      ///////////////////////////////
      MPI_Barrier(MPI_COMM_WORLD);
      ///////////////////////////////	
    }
  }
  
  //manage mem for hash
  minmem_gchash(gch);
  sortcells_gchash(gch);
  destroyhash_gchash(gch);
  logProfileTag(PROFILETAG_PARTIO);
  
  time += MPI_Wtime();
  if(ThisTask == 0)
    fprintf(stderr,"read parts in %lf seconds.\n",time);
  
  //do reduction to get correct global buffer  
  time = -MPI_Wtime();
  if(ThisTask == 0)
    fprintf(stderr,"sharing density.\n");
  for(i=0;i<N0Local;++i)
    for(j=0;j<NFFT;++j)
      for(k=0;k<2*(NFFT/2+1);++k)
	fftwrin[(i*NFFT + j)*(2*(NFFT/2+1)) + k] = 0.0;
  
  /*algorithm to loop through pairs of tasks linearly
    -lifted from Gadget-2 under GPL (http://www.gnu.org/copyleft/gpl.html)
    -see pm_periodic.c from Gadget-2 at http://www.mpa-garching.mpg.de/gadget/
  */  
  int sendTask,recvTask;
  int level,log2NTasks = 0;
  long offset;
  long Nsend,Nrecv;
  MPI_Status Stat;
  while(NTasks > (1 << log2NTasks))
    ++log2NTasks;
  for(level = 0; level < (1 << log2NTasks); level++) /* note: for level=0, target is the same task */
    {
      sendTask = ThisTask;
      recvTask = ThisTask ^ level;
      if(recvTask < NTasks) 
	{
	  //comp offsets
	  offset = -1;
	  Nsend = 0;
	  for(n=0;n<gch->NumGridCells;++n)
	    {
	      if(gch->GridCells[n].id >= TaskN0LocalStart[recvTask]*NFFT*NFFT &&
		 gch->GridCells[n].id < TaskN0LocalStart[recvTask]*NFFT*NFFT + TaskN0Local[recvTask]*NFFT*NFFT
		 && offset < 0)
		offset = n;
	      if(gch->GridCells[n].id >= TaskN0LocalStart[recvTask]*NFFT*NFFT &&
		 gch->GridCells[n].id < TaskN0LocalStart[recvTask]*NFFT*NFFT + TaskN0Local[recvTask]*NFFT*NFFT)
		{
		  Nsend += 1;
		  id2ijk(gch->GridCells[n].id,NFFT,&i,&j,&k);
		  assert(i >= TaskN0LocalStart[recvTask] && i < TaskN0LocalStart[recvTask]+TaskN0Local[recvTask]);
		}
	      if(gch->GridCells[n].id >= TaskN0LocalStart[recvTask]*NFFT*NFFT + TaskN0Local[recvTask]*NFFT*NFFT)
		break;
	    }
	  
	  if(!((offset >= 0 && Nsend > 0) || (offset == -1 && Nsend == 0)))
	    fprintf(stderr,"%d->%d Nsend = %ld, offset = %ld, tot = %ld\n",sendTask,recvTask,Nsend,offset,gch->NumGridCells);
	  assert((offset >= 0 && Nsend > 0) || (offset == -1 && Nsend == 0));
	  
	  if(sendTask != recvTask)
	    {
	      MPI_Sendrecv(&Nsend,1,MPI_LONG,recvTask,TAG_DENS_NUM,
			   &Nrecv,1,MPI_LONG,recvTask,TAG_DENS_NUM,
			   MPI_COMM_WORLD,&Stat);
	      
	      if(Nrecv > 0 || Nsend > 0)
		{
		  if(Nrecv > Ngbuff)
		    {
		      gbuff = (GridCell*)realloc(gbuff,sizeof(GridCell)*Nrecv);
		      assert(gbuff != NULL);
		      Ngbuff = Nrecv;
		    }
		  
		  //get dens from other processor
		  MPI_Sendrecv(gch->GridCells+offset,sizeof(GridCell)*Nsend,MPI_BYTE,recvTask,TAG_DENS_RED,
			       gbuff,sizeof(GridCell)*Nrecv,MPI_BYTE,recvTask,TAG_DENS_RED,
			       MPI_COMM_WORLD,&Stat);
		  
		  //assign dens
		  for(n=0;n<Nrecv;++n)
		    {
		      id2ijk(gbuff[n].id,NFFT,&i,&j,&k);
		      assert(i >= N0LocalStart && i < N0LocalStart+N0Local);
		      fftwrin[((i-N0LocalStart)*NFFT + j) * (2*(NFFT/2+1)) + k] += gbuff[n].val;
		    }
		}
	    }
	  else
	    {
	      //assign dens
	      for(n=0;n<Nsend;++n)
		{
		  id2ijk(gch->GridCells[n+offset].id,NFFT,&i,&j,&k);
		  if(!((i >= N0LocalStart && i < N0LocalStart+N0Local)))
		    {
		      fprintf(stderr,"%d: i = %ld, start = %ld, length = %ld\n",ThisTask,i,N0LocalStart,N0LocalStart+N0Local);
		    }
		  assert(i >= N0LocalStart && i < N0LocalStart+N0Local);
		  fftwrin[((i-N0LocalStart)*NFFT + j) * (2*(NFFT/2+1)) + k] += gch->GridCells[n+offset].val;
		}
	    }
	  
	}/*if(recvTask < NTasks)*/
    }

  //check cells for all zeros
  m = 0;
  for(i=0;i<N0Local;++i)
    for(j=0;j<NFFT;++j)
      for(k=0;k<2*(NFFT/2+1);++k)
	if(fftwrin[(i*NFFT + j)*(2*(NFFT/2+1)) + k] != 0.0) m = 1;
  if(m != 1 && N0Local > 0) {
    fprintf(stderr,"%04d: all density cells are zero in FFTW real array in FFT solver!\n",ThisTask);
    fflush(stderr);
    assert(m == 1);
  }
  
  time += MPI_Wtime();
  if(ThisTask == 0)
    fprintf(stderr,"shared density in %lf seconds.\n",time);

  free_gchash(gch);
  
  if(gbuff != NULL)
    {
      free(gbuff);
      Ngbuff = 0;
    }
  
  //do forward FFT
  time = -MPI_Wtime();
  if(ThisTask == 0)
    fprintf(stderr,"doing forward FFT.\n");
  fftwf_execute(fplan);
  
  //check cells for all zeros
  m = 0;
  for(i=0;i<N0Local;++i)
    for(j=0;j<NFFT;++j)
      for(k=0;k<(NFFT/2+1);++k)
	if( fftwcout[(i*NFFT+j)*(NFFT/2+1)+k][0] != 0.0 || fftwcout[(i*NFFT+j)*(NFFT/2+1)+k][1] != 0.0) m = 1;
  if(m != 1 && N0Local > 0) {
    fprintf(stderr,"%04d: all complex density cells are zero in FFTW real array in FFT solver!\n",ThisTask);
    fflush(stderr);
    assert(m == 1);
  }
  
  time += MPI_Wtime();
  if(ThisTask == 0)
    fprintf(stderr,"finished forward FFT in %lf seconds.\n",time);
  
  //get potential
  double wx,wy,wz,w;
  double *kgrid;
  double k0,k1,k2;
  double kny = 2.0*M_PI/dL/2.0;
  double grfcn;
  
  kgrid = (double*)malloc(NFFT*sizeof(double));
  assert(kgrid != NULL);
  
  for(i=0;i<NFFT;++i)
    {
      if(i <= NFFT/2)
	kgrid[i] = (2.0*M_PI*i)/NFFT/dL;
      else
	kgrid[i] = (2.0*M_PI*(i-NFFT))/NFFT/dL;
    }
  
  for(i=0;i<N0Local;++i)
    for(j=0;j<NFFT;++j)
      for(k=0;k<(NFFT/2+1);++k)
	{
	  if(i != 0 || j != 0 || k != 0) {
	    //k-modes
	    k0 = kgrid[i+N0LocalStart];
	    k1 = kgrid[j];
	    k2 = kgrid[k];
	    
	    //cic decomp
	    if(k0 != 0.0)
	      wx = sin(M_PI*k0/2.0/kny)/(M_PI*k0/2.0/kny);
	    else
	      wx = 1.0;
	    if(k1 != 0.0)
	      wy = sin(M_PI*k1/2.0/kny)/(M_PI*k1/2.0/kny);
	    else
	      wy = 1.0;
	    if(k2 != 0.0)
	      wz = sin(M_PI*k2/2.0/kny)/(M_PI*k2/2.0/kny);
	    else
	      wz = 1.0;
	    w = wx*wy*wz;
	    w = w*w;
	    
	    //greens function
	    grfcn = -1.0*dL*dL/4.0/(sin(k0*dL/2.0)*sin(k0*dL/2.0) + sin(k1*dL/2.0)*sin(k1*dL/2.0) + sin(k2*dL/2.0)*sin(k2*dL/2.0));
	    
	    //check factor for potential
	    if(potfact*grfcn/w/w == 0.0) {
	      fprintf(stderr,"%04d: greens plus CIC factors are zero! total = %lg, greens = %lg, cic win = %lg, potfact = %lg\n",ThisTask,potfact*grfcn/w/w,grfcn,w,potfact);
	      fflush(stderr);
	      assert(potfact*grfcn/w/w != 0.0);
	    }
	    
	    fftwcout[(i*NFFT+j)*(NFFT/2+1)+k][0] *= (potfact*grfcn/w/w); //do CIC decomp twice for interp to rays
	    fftwcout[(i*NFFT+j)*(NFFT/2+1)+k][1] *= (potfact*grfcn/w/w); //do CIC decomp twice for interp to rays
	  } else {
	    //make the zero mode zero!
	    fftwcout[(i*NFFT+j)*(NFFT/2+1)+k][0] = 0.0;
	    fftwcout[(i*NFFT+j)*(NFFT/2+1)+k][1] = 0.0;
	  }
	}
  
  free(kgrid);
  
  //check for all zeros
  m = 0;
  for(i=0;i<N0Local;++i)
    for(j=0;j<NFFT;++j)
      for(k=0;k<(NFFT/2+1);++k)
	if(fftwcout[(i*NFFT+j)*(NFFT/2+1)+k][0] != 0.0 || fftwcout[(i*NFFT+j)*(NFFT/2+1)+k][1] != 0.0) m = 1;
  if(m != 1 && N0Local > 0) {
    fprintf(stderr,"%04d: all complex potential cells are zero in FFTW real array in FFT solver!\n",ThisTask);
    fflush(stderr);
    assert(m == 1);
  }
  
  //do reverse FFT
  time = -MPI_Wtime();
  if(ThisTask == 0)
    fprintf(stderr,"doing backward FFT.\n");
  fftwf_execute(bplan);

  //check for all zeros
  m = 0;
  for(i=0;i<N0Local;++i)
    for(j=0;j<NFFT;++j)
      for(k=0;k<2*(NFFT/2+1);++k)
	if(fftwrin[(i*NFFT + j)*(2*(NFFT/2+1)) + k] != 0.0) m = 1;
  if(m != 1 && N0Local > 0) {
    fprintf(stderr,"%04d: all potential cells are zero in FFTW real array in FFT solver!\n",ThisTask);
    fflush(stderr);
    assert(m == 1);
  }
  
  time += MPI_Wtime();
  if(ThisTask == 0)
    fprintf(stderr,"finished backward FFT in %lf seconds.\n",time);
}

static void get_units(char *fbase, double *L, long *Ntot, double *a) 
{
  char fname[MAX_FILENAME];
  
  sprintf(fname,"%s.0",fbase);
  
  if(ThisTask == 0)
    *Ntot = get_numparts_LGADGET(fname);
  MPI_Bcast(Ntot,1,MPI_LONG,0,MPI_COMM_WORLD);
  
  if(ThisTask == 0)
    *L = get_period_length_LGADGET(fname);
  MPI_Bcast(L,1,MPI_DOUBLE,0,MPI_COMM_WORLD);
  *L = (*L)*rayTraceData.LengthConvFact;
  
  if(ThisTask == 0)
    *a = get_scale_factor_LGADGET(fname);
  MPI_Bcast(a,1,MPI_DOUBLE,0,MPI_COMM_WORLD);
}

static void get_partio_decomp(char *fbase, int *startFile, int *NumFiles, int *MyIOGroup, int *NumIOGroups)
{
  char fname[MAX_FILENAME];
  int Nf;
  int Nfpertask;
  
  sprintf(fname,"%s.0",fbase);
  if(ThisTask == 0)
    Nf = get_numfiles_LGADGET(fname);
  MPI_Bcast(&Nf,1,MPI_INT,0,MPI_COMM_WORLD);
  
  if(Nf >= NTasks)
    {
      Nfpertask = Nf/NTasks;
      if(NTasks*Nfpertask < Nf)
	++Nfpertask;
      
      *startFile = ThisTask*Nfpertask;
      if((*startFile) + Nfpertask-1 >= Nf)
	*NumFiles = (Nf-1) - (*startFile) + 1;
      else
	*NumFiles = Nfpertask;
    }
  else
    {
      if(ThisTask < Nf)
	{
	  *startFile = ThisTask;
	  *NumFiles = 1;
	}
      else
	{
	  *startFile = -1;
	  *NumFiles = 0;
	}
    }
  
  //do IO groups
  *NumIOGroups = NTasks/rayTraceData.NumFilesIOInParallel;
  if(rayTraceData.NumFilesIOInParallel*(*NumIOGroups) < NTasks)
    *NumIOGroups += 1;
  *MyIOGroup = ThisTask/rayTraceData.NumFilesIOInParallel;
  
  if(NumFiles == 0)
    *MyIOGroup = -1;
}

void init_ffts(void) 
{
  //set var for FFT size
  NFFT = rayTraceData.NFFT;
  
  //get local data size
#ifdef DOUBLEFFTW
  AllocLocal = fftw_mpi_local_size_3d(NFFT, NFFT, NFFT/2+1, MPI_COMM_WORLD, &N0Local, &N0LocalStart);
#else
  AllocLocal = fftwf_mpi_local_size_3d(NFFT, NFFT, NFFT/2+1, MPI_COMM_WORLD, &N0Local, &N0LocalStart);
#endif
  
  //collect sizes for MPI work
  int i;
  
  TaskN0Local = (int*)malloc(sizeof(int)*NTasks);
  assert(TaskN0Local != NULL);
  i = N0Local;
  MPI_Allgather(&i,1,MPI_INT,TaskN0Local,1,MPI_INT,MPI_COMM_WORLD);
  
  TaskN0LocalStart = (int*)malloc(sizeof(int)*NTasks);
  assert(TaskN0LocalStart != NULL);
  i = N0LocalStart;
  MPI_Allgather(&i,1,MPI_INT,TaskN0LocalStart,1,MPI_INT,MPI_COMM_WORLD);
  
  i = N0Local;
  MPI_Allreduce(&i,&MaxN0Local,1,MPI_INT,MPI_MAX,MPI_COMM_WORLD);
  
  if(ThisTask == 0)
    {
      fprintf(stderr,"using %0.2lf MB of memory for FFT.\n",(2.0*AllocLocal)*sizeof(FFT_TYPE)/1024.0/1024.0);
      fflush(stderr);
    }
}

void alloc_and_plan_ffts(void) 
{
  //allocate mem
  if(AllocLocal != 0) {
#ifdef DOUBLEFFTW
    fftwrin = fftw_alloc_real(2*AllocLocal);
    fftwcout = (fftw_complex*)fftwrin;
#else
    fftwrin = fftwf_alloc_real(2*AllocLocal);
    fftwcout = (fftwf_complex*)fftwrin;
#endif
      
    if(fftwrin == NULL) {
      fprintf(stderr,"%04d: fftwrin is NULL! AllocLocal = %ld, NFFT = %ld\n",ThisTask,AllocLocal,NFFT);
      fflush(stderr);
      assert(fftwrin != NULL);
    }
  }
  
  if(ThisTask == 0) { fprintf(stderr,"did alloc of FFT memory!\n"); fflush(stderr);}
  
  //create plan
#ifdef DOUBLEFFTW
  fplan = fftw_mpi_plan_dft_r2c_3d(NFFT, NFFT, NFFT, fftwrin, fftwcout, MPI_COMM_WORLD, FFTW_ESTIMATE);
  if(ThisTask == 0) { fprintf(stderr,"did plan for forward FFT!\n"); fflush(stderr);}
  bplan = fftw_mpi_plan_dft_c2r_3d(NFFT, NFFT, NFFT, fftwcout, fftwrin, MPI_COMM_WORLD, FFTW_ESTIMATE);
  if(ThisTask == 0) { fprintf(stderr,"did plan of backward FFT!\n"); fflush(stderr);}
#else
  fplan = fftwf_mpi_plan_dft_r2c_3d(NFFT, NFFT, NFFT, fftwrin, fftwcout, MPI_COMM_WORLD, FFTW_ESTIMATE);
  if(ThisTask == 0) { fprintf(stderr,"did plan for forward FFT!\n"); fflush(stderr);}
  bplan = fftwf_mpi_plan_dft_c2r_3d(NFFT, NFFT, NFFT, fftwcout, fftwrin, MPI_COMM_WORLD, FFTW_ESTIMATE);
  if(ThisTask == 0) { fprintf(stderr,"did plan of backward FFT!\n"); fflush(stderr);}
#endif
  
  if(fplan == NULL) {
    fprintf(stderr,"%04d: forward plan failed!\n",ThisTask);
    fflush(stderr);
    assert(fplan != NULL);
  }
  
  if(bplan == NULL) {
    fprintf(stderr,"%04d: backward plan failed!\n",ThisTask);
    fflush(stderr);
    assert(bplan != NULL);
  }
}

void cleanup_ffts(void)
{
#ifdef DOUBLEFFTW
  if(fftwrin != NULL) {
    fftw_free(fftwrin);
    fftwrin = NULL;
  }
  if(fplan != NULL) {
    fftw_destroy_plan(fplan);
    fplan = NULL;
  }
  if(bplan != NULL) {
    fftw_destroy_plan(bplan);
    bplan = NULL;
  }
#else
  if(fftwrin !=NULL) {
    fftwf_free(fftwrin);
    fftwrin = NULL;
  }
  if(fplan != NULL) {
    fftwf_destroy_plan(fplan);
    fplan = NULL;
  }
  if(bplan != NULL) {
    fftwf_destroy_plan(bplan);
    bplan = NULL;
  }
#endif
  
  if(TaskN0Local != NULL) {
    free(TaskN0Local);
    TaskN0Local = NULL;
  }
  if(TaskN0LocalStart != NULL) {
    free(TaskN0LocalStart);
    TaskN0LocalStart = NULL;
  }
}
