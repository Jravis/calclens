#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include <fftw3.h>
#include <mpi.h>
#include <hdf5.h>
#include <gsl/gsl_math.h>
#include <string.h>

#include "raytrace.h"
#include "fftpoissonsolve.h"
#include "gridcellhash.h"
#include "lgadgetio.h"

#define WRAPIF(id,N) {if(id >= N) id -= N; if(id < 0) id += N; assert(id >= 0 && id < N);}
#define THREEDIND(i,j,k,N) ((i*N+j)*N+k)

#define VERTEX_MIXED_PARTIAL
#define FACE_GRAD

typedef struct {
  char fname[MAX_FILENAME];
  double a;
  double chi;
} NbodySnap;

static void read_snaps(NbodySnap **snaps, long *Nsnaps);
static void get_units(char *fbase, double *L, double *a);

/* Notes for how to do this

1) compute for each bundle cell the range of grid cells needed
   for now do this step using an array of cells and inthash.c

2) sort cells by index

3) send/recv cells needed from other processors

4) do integral over the cells

*/

void threedpot_poissondriver(void)
{
  //make sure compute FFT of correct snap
  static long currFTTsnap = -1;
  static long initFTTsnaps = 1;
  static long Nsnaps;
  static NbodySnap *snaps;
  static long NFFTcurr = -1;
  char fbase[MAX_FILENAME];
  
  if(initFTTsnaps == 1) {
    initFTTsnaps = 0;
    read_snaps(&snaps,&Nsnaps);
  }
  
  //get closest snap
  long i;
  long mysnap = 0;
  double dsnap = fabs(snaps[mysnap].chi-rayTraceData.planeRad);
  for(i=0;i<Nsnaps;++i) {
    if(fabs(snaps[i].chi-rayTraceData.planeRad) < dsnap) {
      mysnap = i;
      dsnap = fabs(snaps[i].chi-rayTraceData.planeRad);
    }
  }
  sprintf(fbase,"%s",snaps[mysnap].fname); 

  //init FFTs
  double L,a;
  get_units(fbase,&L,&a);
  
  //solve for potential
  double t0;
  /*FIXME comment out until bug in FFTW is fixed
  double pfacs[7] = {1.0,3.0,5.0,7.0,9.0,11.0,13.0};
  long Npfacs = 7;
  */
  double pfacs[1] = {1.0};
  long Npfacs = 1;
  long lgb2;
  long bsize,bdiff;
  long dlgb2,pfacind;
  if(mysnap != currFTTsnap) {
    currFTTsnap = mysnap;
    
    t0 = -MPI_Wtime();
    if(ThisTask == 0) {
      fprintf(stderr,"getting potential for snapshot %ld.\n",currFTTsnap);
      fflush(stderr);
    }
    
    /* Code to test FFTW3's FFTS. I found that some sizes fail.
      if(ThisTask == 0) {fprintf(stderr,"------TEST------\n"); fflush(stderr);}
      rayTraceData.NFFT = 128;
      fftw_cleanup();
      if(ThisTask == 0) {fprintf(stderr,"cleaned FFTs!\n"); fflush(stderr);}
      init_ffts();
      if(ThisTask == 0) {fprintf(stderr,"init FFTs!\n"); fflush(stderr);}
      alloc_and_plan_ffts();
      if(ThisTask == 0) {fprintf(stderr,"planned FFTs!\n"); fflush(stderr);}
      comp_pot_snap(snaps[mysnap].fname);
      if(ThisTask == 0) {fprintf(stderr,"------END OF TEST------\n"); fflush(stderr);}
    */
    
    rayTraceData.NFFT = L/(rayTraceData.planeRad*rayTraceData.minSL/2.0);
    lgb2 = (int) (log(rayTraceData.NFFT)/log(2.0));
    bsize = pow(2.0,lgb2);
    bdiff = labs(bsize -  rayTraceData.NFFT);
    for(dlgb2=-4;dlgb2<=1;++dlgb2) {
      for(pfacind=0;pfacind<Npfacs;++pfacind) {
	if(fabs(pow(2.0,lgb2+dlgb2)*pfacs[pfacind] -  rayTraceData.NFFT) < bdiff) {
	  bsize = pow(2.0,lgb2+dlgb2)*pfacs[pfacind];
	  bdiff = fabs(pow(2.0,lgb2+dlgb2)*pfacs[pfacind] -  rayTraceData.NFFT);
	}
      }
    }
    rayTraceData.NFFT = bsize;
    if(ThisTask == 0) {
      fprintf(stderr,"raw NFFT (of not crazy prime factor size) = %ld (wanted %d), cell size = %.2lf Mpc/h, L = %.2lf Mpc/h.\n",rayTraceData.NFFT,
	      (int) (L/(rayTraceData.planeRad*rayTraceData.minSL/2.0)),L/rayTraceData.NFFT,L);
      fflush(stderr);
    }
    if(rayTraceData.NFFT > rayTraceData.MaxNFFT)
      rayTraceData.NFFT = rayTraceData.MaxNFFT;
    NFFTcurr = rayTraceData.NFFT;
    
    cleanup_ffts();
    if(ThisTask == 0) {fprintf(stderr,"cleaned up old FFTs!\n"); fflush(stderr);}
    init_ffts();
    if(ThisTask == 0) {
      fprintf(stderr,"min smooth length = %.2lg rad.\n",rayTraceData.minSL);
      fprintf(stderr,"NFFT = %ld (wanted %d), cell size = %.2lf Mpc/h, L = %.2lf Mpc/h.\n",NFFT,
	      (int) (L/(rayTraceData.planeRad*rayTraceData.minSL/2.0)),L/NFFT,L);
      fflush(stderr);
    }
    if(ThisTask == 0) {fprintf(stderr,"init FFTs!\n"); fflush(stderr);}
    alloc_and_plan_ffts();
    if(ThisTask == 0) {fprintf(stderr,"planned FFTs!\n"); fflush(stderr);}
    
    comp_pot_snap(snaps[mysnap].fname);
    
    t0 += MPI_Wtime();
    if(ThisTask == 0) {
      fprintf(stderr,"got potential for snapshot %ld in %lf seconds.\n",currFTTsnap,t0);
      fflush(stderr);
    }
  }  
  
  t0 = -MPI_Wtime();
  if(ThisTask == 0) {
    fprintf(stderr,"doing interp and integral to rays.\n");
    fflush(stderr);
  }
  
  //get lengths  
  double dL = L/NFFT;
  double binL = (rayTraceData.maxComvDistance)/((double) (rayTraceData.NumLensPlanes));
  int Nint = binL/dL*2;
  double chimin = rayTraceData.planeRad - binL/2.0;
  double chimax = rayTraceData.planeRad + binL/2.0;
  double dchi = (chimax-chimin)/Nint;
  
  //init grid cell hash table
  GridCellHash *gch;
  
  double vec[3];
  long j,k;
  long ip1,jp1,kp1;
  long id,n,m;
  long di,dj,dk;
  long ii,jj,kk;
  double rad;
  long bind;
  long rind;
  long ind;
  long pp[3],pm[3],mp[3],mm[3];
  long indvec[3][3][3];
  double cost,cosp,sint,sinp;
  double theta,phi,r;
  double dx,dy,dz;
  double val,fac1,fac2;
  
  long NumActiveBundleCells;
  long *activeBundleCellInds;
  long MaxNumActiveBundleCells;
  NumActiveBundleCells = 0;
  for(bind=0;bind<NbundleCells;++bind) {
    if(ISSETBITFLAG(bundleCells[bind].active,PRIMARY_BUNDLECELL)) {
      ++NumActiveBundleCells;
    }
  }
  activeBundleCellInds = (long*)malloc(sizeof(long)*NumActiveBundleCells);
  assert(activeBundleCellInds != NULL);
  n = 0;
  for(bind=0;bind<NbundleCells;++bind) {
    if(ISSETBITFLAG(bundleCells[bind].active,PRIMARY_BUNDLECELL)) {
      activeBundleCellInds[n] = bind;
      ++n;
    }
  }
  assert(n == NumActiveBundleCells);
  MPI_Allreduce(&NumActiveBundleCells,&MaxNumActiveBundleCells,1,MPI_LONG,MPI_MAX,MPI_COMM_WORLD);
  long abind;
  
  long Ngbuff = 0;
  GridCell *gbuff = NULL;
  
  int sendTask,recvTask;
  int level,log2NTasks = 0;
  long offset;
  long Nsend,Nrecv;
  MPI_Status Stat;
  while(NTasks > (1 << log2NTasks))
    ++log2NTasks;
  
  for(abind=0;abind<MaxNumActiveBundleCells;++abind) {
    //setup gridcell hash
    gch = init_gchash();
    
    //get index of bundle cell working with
    if(abind < NumActiveBundleCells) {
      bind = activeBundleCellInds[abind];
    }
    
    //get grid cells needed
    if(abind < NumActiveBundleCells) {
      if(ISSETBITFLAG(bundleCells[bind].active,PRIMARY_BUNDLECELL)) {
	for(rind=0;rind<bundleCells[bind].Nrays;++rind) {
	  r = sqrt(bundleCells[bind].rays[rind].n[0]*bundleCells[bind].rays[rind].n[0] + 
		   bundleCells[bind].rays[rind].n[1]*bundleCells[bind].rays[rind].n[1] + 
		   bundleCells[bind].rays[rind].n[2]*bundleCells[bind].rays[rind].n[2]);
	  
	  for(n=0;n<Nint;++n) {
	    //comp 3D loc
	    rad = chimin + n*dchi + 0.5*dchi;
	    
	    vec[0] = bundleCells[bind].rays[rind].n[0]*rad/r;
	    vec[1] = bundleCells[bind].rays[rind].n[1]*rad/r;
	    vec[2] = bundleCells[bind].rays[rind].n[2]*rad/r;
	    
	    for(m=0;m<3;++m) {
	      while(vec[m] < 0)
		vec[m] += L;
	      while(vec[m] >= L)
		vec[m] -= L;
	    }
	    
	    i = (long) (vec[0]/dL);
	    WRAPIF(i,NFFT);
	    
	    j = (long) (vec[1]/dL);
	    WRAPIF(j,NFFT);
	    
	    k = (long) (vec[2]/dL);
	    WRAPIF(k,NFFT);
	    
	    //get all eight cells for interp plus those needed for all of the derivs
	    for(di=-1;di<=2;++di) {
	      ii = i + di;
	      WRAPIF(ii,NFFT);
	      
	      for(dj=-1;dj<=2;++dj) {
		jj = j + dj;
		WRAPIF(jj,NFFT);
		
		for(dk=-1;dk<=2;++dk) {
		  kk = k + dk;
		  WRAPIF(kk,NFFT);
		  
		  id = THREEDIND(ii,jj,kk,NFFT);
		  ind = getid_gchash(gch,id);
		}//for(dk=-1;dk<=2;++dk)
	      }//for(dj=-1;dj<=2;++dj)
	    }//for(di=-1;di<=2;++di)
	  }//for(n=0;n<Nint;++n)
	}//for(rind=0;rind<bundleCells[bind].Nrays;++rind)
      
	assert(gch->NumGridCells > 0);
	
      }//if(ISSETBITFLAG(bundleCells[bind].active,PRIMARY_BUNDLECELL))
    }//if(abind < NumActiveBundleCells)
    
    //sort to get into slab order
    sortcells_gchash(gch);
        
    //do send/recvs to get cells from other processors
    /*algorithm to loop through pairs of tasks linearly
      -lifted from Gadget-2 under GPL (http://www.gnu.org/copyleft/gpl.html)
      -see pm_periodic.c from Gadget-2 at http://www.mpa-garching.mpg.de/gadget/
    */  
    for(level = 0; level < (1 << log2NTasks); level++) {
      // note: for level=0, target is the same task
      sendTask = ThisTask;
      recvTask = ThisTask ^ level;
      if(recvTask < NTasks) { 
	//comp # of cells needed from other processor and offset
	offset = -1;
	Nrecv = 0;
	for(n=0;n<gch->NumGridCells;++n)
	  {
	    if(gch->GridCells[n].id >= TaskN0LocalStart[recvTask]*NFFT*NFFT &&
                 gch->GridCells[n].id < TaskN0LocalStart[recvTask]*NFFT*NFFT + TaskN0Local[recvTask]*NFFT*NFFT
	       && offset < 0)
	      offset = n;
	    if(gch->GridCells[n].id >= TaskN0LocalStart[recvTask]*NFFT*NFFT &&
	       gch->GridCells[n].id < TaskN0LocalStart[recvTask]*NFFT*NFFT + TaskN0Local[recvTask]*NFFT*NFFT)
	      {
		Nrecv += 1;
		id2ijk(gch->GridCells[n].id,NFFT,&i,&j,&k);
		assert(i >= TaskN0LocalStart[recvTask] && i < TaskN0LocalStart[recvTask]+TaskN0Local[recvTask]);
	      }
	    if(gch->GridCells[n].id >= TaskN0LocalStart[recvTask]*NFFT*NFFT + TaskN0Local[recvTask]*NFFT*NFFT)
	      break;
	  }
	
	if(!((offset >= 0 && Nrecv > 0) || (offset == -1 && Nrecv == 0))) {
	  fprintf(stderr,"%04d: %d->%d Nrecv = %ld, offset = %ld, tot = %ld\n",ThisTask,sendTask,recvTask,Nrecv,offset,gch->NumGridCells);
	  fflush(stderr);
	}
	assert((offset >= 0 && Nrecv > 0) || (offset == -1 && Nrecv == 0));
	
	if(sendTask != recvTask) {
	  MPI_Sendrecv(&Nrecv,1,MPI_LONG,recvTask,TAG_POTCELL_NUM,
		       &Nsend,1,MPI_LONG,recvTask,TAG_POTCELL_NUM,
		       MPI_COMM_WORLD,&Stat);
	  
	  if(Nrecv > 0 || Nsend > 0) {
	    //get cells to send
	    if(Nsend > Ngbuff) {
	      gbuff = (GridCell*)realloc(gbuff,sizeof(GridCell)*Nsend);
	      assert(gbuff != NULL);
	      Ngbuff = Nsend;
	    }
	    MPI_Sendrecv(gch->GridCells+offset,sizeof(GridCell)*Nrecv,MPI_BYTE,recvTask,TAG_POTCELL_IDS,
			 gbuff,sizeof(GridCell)*Nsend,MPI_BYTE,recvTask,TAG_POTCELL_IDS,
			 MPI_COMM_WORLD,&Stat);
	    
	    //fill cells for other processor
	    for(m=0;m<Nsend;++m) {
	      id2ijk(gbuff[m].id,NFFT,&i,&j,&k);
	      
	      if(!(i >= N0LocalStart && i < N0LocalStart+N0Local)) {
		fprintf(stderr,"%04d: send != recv slab assertion going to fail! %s:%d\n",ThisTask,__FILE__,__LINE__);
		fflush(stderr);
	      }
	      assert(i >= N0LocalStart && i < N0LocalStart+N0Local);
	      
	      gbuff[m].val = fftwrin[((i-N0LocalStart)*NFFT + j) * (2*(NFFT/2+1)) + k];
	    }
	    
	    //send cells to other processor
	    MPI_Sendrecv(gbuff,sizeof(GridCell)*Nsend,MPI_BYTE,recvTask,TAG_POTCELL_VALS,
			 gch->GridCells+offset,sizeof(GridCell)*Nrecv,MPI_BYTE,recvTask,TAG_POTCELL_VALS,
			 MPI_COMM_WORLD,&Stat);
	    
	  }// if(Nrecv > 0 || Nsend > 0)
	}// if(sendTask != recvTask)
	else {
	  //store pot
	  for(m=0;m<Nrecv;++m) {
	    id2ijk(gch->GridCells[m+offset].id,NFFT,&i,&j,&k);
	    
	    if(!(i >= N0LocalStart && i < N0LocalStart+N0Local)) {
	      fprintf(stderr,"%04d: send == recv slab assertion going to fail! %s:%d\n",ThisTask,__FILE__,__LINE__);
	      fflush(stderr);
	    }
	    
	    assert(i >= N0LocalStart && i < N0LocalStart+N0Local);
	    gch->GridCells[m+offset].val = fftwrin[((i-N0LocalStart)*NFFT + j) * (2*(NFFT/2+1)) + k];
	  }//for(m=0;m<Nrecv;++m
	}//else for if(sendTask != recvTask) 
      }// if(recvTask < NTasks)
    }// for(level = 0; level < (1 << log2NTasks); level++)
    
    
    //double check FFTs for all zeros - catches errors
    m = 0;
    for(i=0;i<N0Local;++i)
      for(j=0;j<NFFT;++j)
	for(k=0;k<2*(NFFT/2+1);++k)
	  if(fftwrin[(i*NFFT + j)*(2*(NFFT/2+1)) + k] != 0.0) m = 1;
    if(m != 1 && N0Local > 0) {
      fprintf(stderr,"%04d: all potential cells are zero in FFTW real array!\n",ThisTask);
      fflush(stderr);
      assert(m == 1);
    }
    m = 0;
    for(i=0;i<gch->NumGridCells;++i)
      if(gch->GridCells[i].val != 0.0) m = 1;
    if(m != 1 && gch->NumGridCells > 0) {
      fprintf(stderr,"%04d: all potential cells are zero in gch!\n",ThisTask);
      fflush(stderr);
      assert(m == 1);
    }
    
    //interp to rays and comp derivs
    int dind1,dind2;
    double jac[3][3];
    if(abind < NumActiveBundleCells) {
      if(ISSETBITFLAG(bundleCells[bind].active,PRIMARY_BUNDLECELL)) {
	
	//make sure buff cells are the same length as gch cells
	Ngbuff = gch->NumGridCells;
	gbuff = (GridCell*)realloc(gbuff,sizeof(GridCell)*Ngbuff);
	assert(gbuff != NULL);
	
	//do pot
	for(m=0;m<gch->NumGridCells;++m) {
	  gbuff[m].id = gch->GridCells[m].id;
	  gbuff[m].val = gch->GridCells[m].val;
	}
	
	for(rind=0;rind<bundleCells[bind].Nrays;++rind) {
	  r = sqrt(bundleCells[bind].rays[rind].n[0]*bundleCells[bind].rays[rind].n[0] +
		   bundleCells[bind].rays[rind].n[1]*bundleCells[bind].rays[rind].n[1] +
		   bundleCells[bind].rays[rind].n[2]*bundleCells[bind].rays[rind].n[2]);
	  
	  for(n=0;n<Nint;++n) {
	    //comp 3D loc
	    rad = chimin + n*dchi + 0.5*dchi;
	    
	    vec[0] = bundleCells[bind].rays[rind].n[0]*rad/r;
	    vec[1] = bundleCells[bind].rays[rind].n[1]*rad/r;
	    vec[2] = bundleCells[bind].rays[rind].n[2]*rad/r;
	    
	    for(m=0;m<3;++m) {
	      while(vec[m] < 0)
		vec[m] += L;
	      while(vec[m] >= L)
		vec[m] -= L;
	    }
	    
	    i = (long) (vec[0]/dL);
	    dx = (vec[0] - i*dL)/dL;
	    
	    j = (long) (vec[1]/dL);
	    dy = (vec[1] - j*dL)/dL;
	    
	    k = (long) (vec[2]/dL);
	    dz = (vec[2] - k*dL)/dL;
	    
	    WRAPIF(i,NFFT);
	    ip1 = i + 1;
	    WRAPIF(ip1,NFFT);
	    
	    WRAPIF(j,NFFT);
	    jp1 = j + 1;
	    WRAPIF(jp1,NFFT);
	    
	    WRAPIF(k,NFFT);
	    kp1 = k + 1;
	    WRAPIF(kp1,NFFT);
	      
	    //interp deriv val
	    val = 0.0;
	      
	    id = THREEDIND(i,j,k,NFFT);
	    ind = getonlyid_gchash(gch,id);
	    assert(ind != GCH_INVALID);
	    assert(gbuff[ind].id != -1);
	    assert(gbuff[ind].id == gch->GridCells[ind].id);
	    val += gbuff[ind].val*(1.0 - dx)*(1.0 - dy)*(1.0 - dz);
	    
	    id = THREEDIND(i,j,kp1,NFFT);
	    ind = getonlyid_gchash(gch,id);
	    assert(ind != GCH_INVALID);
	    assert(gbuff[ind].id != -1);
	    assert(gbuff[ind].id == gch->GridCells[ind].id);
	    val += gbuff[ind].val*(1.0 - dx)*(1.0 - dy)*dz;
	    
	    id = THREEDIND(i,jp1,k,NFFT);
	    ind = getonlyid_gchash(gch,id);
	    assert(ind != GCH_INVALID);
	    assert(gbuff[ind].id != -1);
	    assert(gbuff[ind].id == gch->GridCells[ind].id);
	    val += gbuff[ind].val*(1.0 - dx)*dy*(1.0 - dz);
	    
	    id = THREEDIND(i,jp1,kp1,NFFT);
	    ind = getonlyid_gchash(gch,id);
	    assert(ind != GCH_INVALID);
	    assert(gbuff[ind].id != -1);
	    assert(gbuff[ind].id == gch->GridCells[ind].id);
	    val += gbuff[ind].val*(1.0 - dx)*dy*dz;
	    
	    id = THREEDIND(ip1,j,k,NFFT);
	    ind = getonlyid_gchash(gch,id);
	    assert(ind != GCH_INVALID);
	    assert(gbuff[ind].id != -1);
	    assert(gbuff[ind].id == gch->GridCells[ind].id);
	    val += gbuff[ind].val*dx*(1.0 - dy)*(1.0 - dz);
	    
	    id = THREEDIND(ip1,j,kp1,NFFT);
	    ind = getonlyid_gchash(gch,id);
	    assert(ind != GCH_INVALID);
	    assert(gbuff[ind].id != -1);
	    assert(gbuff[ind].id == gch->GridCells[ind].id);
	    val += gbuff[ind].val*dx*(1.0 - dy)*dz;
	    
	    id = THREEDIND(ip1,jp1,k,NFFT);
	    ind = getonlyid_gchash(gch,id);
	    assert(ind != GCH_INVALID);
	    assert(gbuff[ind].id != -1);
	    assert(gbuff[ind].id == gch->GridCells[ind].id);
	    val += gbuff[ind].val*dx*dy*(1.0 - dz);
	    
	    id = THREEDIND(ip1,jp1,kp1,NFFT);
	    ind = getonlyid_gchash(gch,id);
	    assert(ind != GCH_INVALID);
	    assert(gbuff[ind].id != -1);
	    assert(gbuff[ind].id == gch->GridCells[ind].id);
	    val += gbuff[ind].val*dx*dy*dz;
	    
	    bundleCells[bind].rays[rind].phi += val;
	    
	    //check to make sure not inf or nan
	    assert(gsl_finite(bundleCells[bind].rays[rind].phi));
	    
	  }//for(n=0;n<Nint;++n)
	}//for(rind=0;rind<bundleCells[bind].Nrays;++rind)
	
	//do first derivs
	for(dind1=0;dind1<3;++dind1) {
	  //comp deriv for this direction
	  //mark cells with no deriv with -1 for id
	  for(m=0;m<gch->NumGridCells;++m)
	    {
	      //get ids of nbr cells
	      gbuff[m].id = -1;
	      id2ijk(gch->GridCells[m].id,NFFT,&i,&j,&k);
	      
	      for(di=-1;di<=1;++di) {
		ii = i + di;
		WRAPIF(ii,NFFT);
		for(dj=-1;dj<=1;++dj) {
		  jj = j + dj;
		  WRAPIF(jj,NFFT);
		  for(dk=-1;dk<=1;++dk) {
		    kk = k + dk;
		    WRAPIF(kk,NFFT);
		    
		    indvec[di+1][dj+1][dk+1] = THREEDIND(ii,jj,kk,NFFT);
		  }
		}
	      }
	      
	      //get derivs
	      //build the stencil
	      for(n=0;n<3;++n) {
		if(n == dind1) {
#ifdef FACE_GRAD
		  pp[n] = 2;
		  pm[n] = 1;
#else
		  pp[n] = 2;
		  pm[n] = 0;
#endif
		} 
		else {
		  pp[n] = 1;
		  pm[n] = 1;
		}
	      }
	      
	      //eval stencil parts
	      gbuff[m].val = 0.0;
		  
	      id = indvec[pp[0]][pp[1]][pp[2]];
	      ind = getonlyid_gchash(gch,id);
	      if(ind == GCH_INVALID)
		continue;
	      gbuff[m].val += gch->GridCells[ind].val;
		  
	      id = indvec[pm[0]][pm[1]][pm[2]];
	      ind = getonlyid_gchash(gch,id);
	      if(ind == GCH_INVALID)
		continue;
	      gbuff[m].val -= gch->GridCells[ind].val;
	      
	      gbuff[m].val /= dL;
#ifndef FACE_GRAD
	      gbuff[m].val /= 2.0;
#endif
	      gbuff[m].id = gch->GridCells[m].id;
	    }//for(m=0;m<gch-NumGridCells;++m)
	
	  //now add part needed to the rays
	  for(rind=0;rind<bundleCells[bind].Nrays;++rind) {
	    //comp jacobian matrix
	    vec2ang(bundleCells[bind].rays[rind].n,&theta,&phi);
	    cost = cos(theta);
	    sint = sin(theta);
	    cosp = cos(phi);
	    sinp = sin(phi);
	    
	    //xhat = jac[0][0] that + jac[0][1] phat + jac[0][2] rhat
	    jac[0][0] = cosp*cost;
	    jac[0][1] = -sinp;
	    jac[0][2] = cosp*sint;
	    
	    //yhat = jac[1][0] that + jac[1][1] phat + jac[1][2] rhat
	    jac[1][0] = sinp*cost;
	    jac[1][1] = cosp;
	    jac[1][2] = sinp*sint;
	    
	    //zhat = jac[2][0] that + jac[2][1] phat + jac[2][2] rhat
	    jac[2][0] = -sint;
	    jac[2][1] = 0.0;
	    jac[2][2] = cost;
	    
	    r = sqrt(bundleCells[bind].rays[rind].n[0]*bundleCells[bind].rays[rind].n[0] +
		     bundleCells[bind].rays[rind].n[1]*bundleCells[bind].rays[rind].n[1] +
		     bundleCells[bind].rays[rind].n[2]*bundleCells[bind].rays[rind].n[2]);
	    
	    for(n=0;n<Nint;++n) {
	      //comp 3D loc
	      rad = chimin + n*dchi + 0.5*dchi;
	      
	      vec[0] = bundleCells[bind].rays[rind].n[0]*rad/r;
	      vec[1] = bundleCells[bind].rays[rind].n[1]*rad/r;
	      vec[2] = bundleCells[bind].rays[rind].n[2]*rad/r;
	      
	      for(m=0;m<3;++m) {
		while(vec[m] < 0)
		  vec[m] += L;
		while(vec[m] >= L)
		  vec[m] -= L;
	      }
	      
	      i = (long) (vec[0]/dL);
	      dx = (vec[0] - i*dL)/dL;

	      j = (long) (vec[1]/dL);
	      dy = (vec[1] - j*dL)/dL;

	      k = (long) (vec[2]/dL);
	      dz = (vec[2] - k*dL)/dL;
	      
#ifdef FACE_GRAD
	      if(dind1 == 0) {
		if(dx < 0.5) {
		  --i;
		  dx += 0.5;
		} else {
		  dx -= 0.5;
		}
	      } else if(dind1 == 1) {
		if(dy < 0.5) {
		  --j;
		  dy += 0.5;
		} else {
		  dy -= 0.5;
		}		
	      } else {
		if(dz < 0.5) {
		  --k;
		  dz += 0.5;
		} else {
		  dz -= 0.5;
		}				
	      }
#endif
	      
	      WRAPIF(i,NFFT);
	      ip1 = i + 1;
	      WRAPIF(ip1,NFFT);
	      
	      WRAPIF(j,NFFT);
	      jp1 = j + 1;
	      WRAPIF(jp1,NFFT);
	      
	      WRAPIF(k,NFFT);
	      kp1 = k + 1;
	      WRAPIF(kp1,NFFT);
	      
	      //interp deriv val
	      val = 0.0;
	      
	      id = THREEDIND(i,j,k,NFFT);
	      ind = getonlyid_gchash(gch,id);
	      assert(ind != GCH_INVALID);
	      assert(gbuff[ind].id != -1);
	      assert(gbuff[ind].id == gch->GridCells[ind].id);
	      val += gbuff[ind].val*(1.0 - dx)*(1.0 - dy)*(1.0 - dz);
	      
	      id = THREEDIND(i,j,kp1,NFFT);
	      ind = getonlyid_gchash(gch,id);
	      assert(ind != GCH_INVALID);
	      assert(gbuff[ind].id != -1);
	      assert(gbuff[ind].id == gch->GridCells[ind].id);
	      val += gbuff[ind].val*(1.0 - dx)*(1.0 - dy)*dz;
	      
	      id = THREEDIND(i,jp1,k,NFFT);
	      ind = getonlyid_gchash(gch,id);
	      assert(ind != GCH_INVALID);
	      assert(gbuff[ind].id != -1);
	      assert(gbuff[ind].id == gch->GridCells[ind].id);
	      val += gbuff[ind].val*(1.0 - dx)*dy*(1.0 - dz);
	      
	      id = THREEDIND(i,jp1,kp1,NFFT);
	      ind = getonlyid_gchash(gch,id);
	      assert(ind != GCH_INVALID);
	      assert(gbuff[ind].id != -1);
	      assert(gbuff[ind].id == gch->GridCells[ind].id);
	      val += gbuff[ind].val*(1.0 - dx)*dy*dz;
	      
	      id = THREEDIND(ip1,j,k,NFFT);
	      ind = getonlyid_gchash(gch,id);
	      assert(ind != GCH_INVALID);
	      assert(gbuff[ind].id != -1);
	      assert(gbuff[ind].id == gch->GridCells[ind].id);
	      val += gbuff[ind].val*dx*(1.0 - dy)*(1.0 - dz);
	      
	      id = THREEDIND(ip1,j,kp1,NFFT);
	      ind = getonlyid_gchash(gch,id);
	      assert(ind != GCH_INVALID);
	      assert(gbuff[ind].id != -1);
	      assert(gbuff[ind].id == gch->GridCells[ind].id);
	      val += gbuff[ind].val*dx*(1.0 - dy)*dz;
	      
	      id = THREEDIND(ip1,jp1,k,NFFT);
	      ind = getonlyid_gchash(gch,id);
	      assert(ind != GCH_INVALID);
	      assert(gbuff[ind].id != -1);
	      assert(gbuff[ind].id == gch->GridCells[ind].id);
	      val += gbuff[ind].val*dx*dy*(1.0 - dz);
	      
	      id = THREEDIND(ip1,jp1,kp1,NFFT);
	      ind = getonlyid_gchash(gch,id);
	      assert(ind != GCH_INVALID);
	      assert(gbuff[ind].id != -1);
	      assert(gbuff[ind].id == gch->GridCells[ind].id);
	      val += gbuff[ind].val*dx*dy*dz;
	      
	      //do the projections and add to ray
	      for(ii=0;ii<2;++ii)
		bundleCells[bind].rays[rind].alpha[ii] += val*jac[dind1][ii];
	      
	      //check to make sure not inf or nan
	      assert(gsl_finite(bundleCells[bind].rays[rind].alpha[0]));
	      assert(gsl_finite(bundleCells[bind].rays[rind].alpha[1]));
	      
	    }//for(n=0;n<Nint;++n)
	  }//for(rind=0;rind<bundleCells[bind].Nrays;++rind)
	}//for(dind1=0;dind1<3;++dind1)
	
	//do second derivs
	for(dind1=0;dind1<3;++dind1)
	  for(dind2=dind1;dind2<3;++dind2) {
	    //comp deriv for this direction
	    //mark cells with no deriv with -1 for id
	    for(m=0;m<gch->NumGridCells;++m)
	      {
		//get ids of nbr cells
		gbuff[m].id = -1;
		id2ijk(gch->GridCells[m].id,NFFT,&i,&j,&k);
		
		for(di=-1;di<=1;++di) {
		  ii = i + di;
		  WRAPIF(ii,NFFT);
		  for(dj=-1;dj<=1;++dj) {
		    jj = j + dj;
		    WRAPIF(jj,NFFT);
		    for(dk=-1;dk<=1;++dk) {
		      kk = k + dk;
		      WRAPIF(kk,NFFT);
		      
		      indvec[di+1][dj+1][dk+1] = THREEDIND(ii,jj,kk,NFFT);
		    }
		  }
		}
		
		//get derivs
		if(dind1 == dind2) {
		  //build the stencil
		  for(n=0;n<3;++n) {
		    if(n == dind1) {
		      pp[n] = 2;
		      pm[n] = 0;
		    } else {
		      pp[n] = 1;
		      pm[n] = 1;
		    }
		  }
		  
		  //eval stencil parts
		  gbuff[m].val = -2.0*(gch->GridCells[m].val);
		  
		  id = indvec[pp[0]][pp[1]][pp[2]];
		  ind = getonlyid_gchash(gch,id);
		  if(ind == GCH_INVALID)
		    continue;
		  gbuff[m].val += gch->GridCells[ind].val;
		  
		  id = indvec[pm[0]][pm[1]][pm[2]];
		  ind = getonlyid_gchash(gch,id);
		  if(ind == GCH_INVALID)
		    continue;
		  gbuff[m].val += gch->GridCells[ind].val;
		  
		  gbuff[m].val /= dL;
		  gbuff[m].val /= dL;
		  gbuff[m].id = gch->GridCells[m].id;

		} else {
		  //build the stencil
		  for(n=0;n<3;++n) {
		    if(n == dind1) {
		      pp[n] = 2;
		      pm[n] = 2;
#ifdef VERTEX_MIXED_PARTIAL
		      mp[n] = 1;
		      mm[n] = 1;
#else
		      mp[n] = 0;
		      mm[n] = 0;
#endif
		    } else if(n == dind2) {
		      pp[n] = 2;
		      mp[n] = 2;
#ifdef VERTEX_MIXED_PARTIAL
		      pm[n] = 1;
		      mm[n] = 1;
#else
		      pm[n] = 0;
		      mm[n] = 0;
#endif
		    } else {
		      pp[n] = 1;
		      pm[n] = 1;
		      mp[n] = 1;
		      mm[n] = 1;
		    }
		  }
		  
		  //eval stencil parts
		  gbuff[m].val = 0.0;
		  
		  id = indvec[pp[0]][pp[1]][pp[2]];
                  ind = getonlyid_gchash(gch,id);
                  if(ind == GCH_INVALID)
                    continue;
                  gbuff[m].val += gch->GridCells[ind].val;
		  
                  id = indvec[pm[0]][pm[1]][pm[2]];
                  ind = getonlyid_gchash(gch,id);
                  if(ind == GCH_INVALID)
                    continue;
                  gbuff[m].val -= gch->GridCells[ind].val;
		  
		  id = indvec[mp[0]][mp[1]][mp[2]];
                  ind = getonlyid_gchash(gch,id);
                  if(ind == GCH_INVALID)
                    continue;
                  gbuff[m].val -= gch->GridCells[ind].val;
		  
		  id = indvec[mm[0]][mm[1]][mm[2]];
                  ind = getonlyid_gchash(gch,id);
                  if(ind == GCH_INVALID)
                    continue;
                  gbuff[m].val += gch->GridCells[ind].val;
		  
                  gbuff[m].val /= dL;
		  gbuff[m].val /= dL;
#ifndef VERTEX_MIXED_PARTIAL
		  gbuff[m].val /= 2.0;
		  gbuff[m].val /= 2.0;
#endif
                  gbuff[m].id = gch->GridCells[m].id;
		}//end of else
		
	      }//for(m=0;m<gch-NumGridCells;++m)
	    
	    //now add part needed to the rays
	    for(rind=0;rind<bundleCells[bind].Nrays;++rind) {
	      //comp jacobian matrix
	      vec2ang(bundleCells[bind].rays[rind].n,&theta,&phi);
	      cost = cos(theta);
	      sint = sin(theta);
	      cosp = cos(phi);
	      sinp = sin(phi);
	      
	      //xhat = jac[0][0] that + jac[0][1] phat + jac[0][2] rhat
	      jac[0][0] = cosp*cost;
	      jac[0][1] = -sinp;
	      jac[0][2] = cosp*sint;
	      
	      //yhat = jac[1][0] that + jac[1][1] phat + jac[1][2] rhat
	      jac[1][0] = sinp*cost;
	      jac[1][1] = cosp;
	      jac[1][2] = sinp*sint;
	      
	      //zhat = jac[2][0] that + jac[2][1] phat + jac[2][2] rhat
	      jac[2][0] = -sint;
	      jac[2][1] = 0.0;
	      jac[2][2] = cost;
	      
	      r = sqrt(bundleCells[bind].rays[rind].n[0]*bundleCells[bind].rays[rind].n[0] +
		       bundleCells[bind].rays[rind].n[1]*bundleCells[bind].rays[rind].n[1] +
		       bundleCells[bind].rays[rind].n[2]*bundleCells[bind].rays[rind].n[2]);
	      
	      for(n=0;n<Nint;++n) {
		//comp 3D loc
		rad = chimin + n*dchi + 0.5*dchi;
		
		vec[0] = bundleCells[bind].rays[rind].n[0]*rad/r;
		vec[1] = bundleCells[bind].rays[rind].n[1]*rad/r;
		vec[2] = bundleCells[bind].rays[rind].n[2]*rad/r;
		
		for(m=0;m<3;++m) {
		  while(vec[m] < 0)
		    vec[m] += L;
		  while(vec[m] >= L)
		    vec[m] -= L;
		}
		
		if(dind1 != dind2) {
		  //vertex centered for dind1 != dind2
		  i = (long) (vec[0]/dL);
		  dx = (vec[0] - i*dL)/dL;
#ifdef VERTEX_MIXED_PARTIAL
		  if(dx < 0.5) {
		    --i;
		    dx += 0.5;
		  } else {
		    dx -= 0.5;
		  }
#endif
		  WRAPIF(i,NFFT);
		  ip1 = i + 1;
		  WRAPIF(ip1,NFFT);
		  
		  j = (long) (vec[1]/dL);
		  dy = (vec[1] - j*dL)/dL;
#ifdef VERTEX_MIXED_PARTIAL
		  if(dy < 0.5) {
		    --j;
		    dy += 0.5;
		  } else {
		    dy -= 0.5;
		  }
#endif
		  WRAPIF(j,NFFT);
		  jp1 = j + 1;
		  WRAPIF(jp1,NFFT);
		  
		  k = (long) (vec[2]/dL);
		  dz = (vec[2] - k*dL)/dL;
#ifdef VERTEX_MIXED_PARTIAL
		  if(dz < 0.5) {
		    --k;
		    dz += 0.5;
		  } else {
		    dz -= 0.5;
		  }
#endif
		  WRAPIF(k,NFFT);
		  kp1 = k + 1;
		  WRAPIF(kp1,NFFT);
		  
		} else {
		  //cell centered for dind1 == dind2
		  i = (long) (vec[0]/dL);
		  dx = (vec[0] - i*dL)/dL;
		  WRAPIF(i,NFFT);
		  ip1 = i + 1;
		  WRAPIF(ip1,NFFT);
		  
		  j = (long) (vec[1]/dL);
		  dy = (vec[1] - j*dL)/dL;
		  WRAPIF(j,NFFT);
		  jp1 = j + 1;
		  WRAPIF(jp1,NFFT);
		  
		  k = (long) (vec[2]/dL);
		  dz = (vec[2] - k*dL)/dL;
		  WRAPIF(k,NFFT);
		  kp1 = k + 1;
		  WRAPIF(kp1,NFFT);
		}
		
		//interp deriv val
		val = 0.0;
		
		id = THREEDIND(i,j,k,NFFT);
		ind = getonlyid_gchash(gch,id);
		assert(ind != GCH_INVALID);
		assert(gbuff[ind].id != -1);
		assert(gbuff[ind].id == gch->GridCells[ind].id);
		val += gbuff[ind].val*(1.0 - dx)*(1.0 - dy)*(1.0 - dz);
		
		id = THREEDIND(i,j,kp1,NFFT);
		ind = getonlyid_gchash(gch,id);
		assert(ind != GCH_INVALID);
		assert(gbuff[ind].id != -1);
		assert(gbuff[ind].id == gch->GridCells[ind].id);
		val += gbuff[ind].val*(1.0 - dx)*(1.0 - dy)*dz;
		
		id = THREEDIND(i,jp1,k,NFFT);
		ind = getonlyid_gchash(gch,id);
		assert(ind != GCH_INVALID);
		assert(gbuff[ind].id != -1);
		assert(gbuff[ind].id == gch->GridCells[ind].id);
		val += gbuff[ind].val*(1.0 - dx)*dy*(1.0 - dz);
		
		id = THREEDIND(i,jp1,kp1,NFFT);
		ind = getonlyid_gchash(gch,id);
		assert(ind != GCH_INVALID);
		assert(gbuff[ind].id != -1);
		assert(gbuff[ind].id == gch->GridCells[ind].id);
		val += gbuff[ind].val*(1.0 - dx)*dy*dz;
		
		id = THREEDIND(ip1,j,k,NFFT);
		ind = getonlyid_gchash(gch,id);
		assert(ind != GCH_INVALID);
		assert(gbuff[ind].id != -1);
		assert(gbuff[ind].id == gch->GridCells[ind].id);
		val += gbuff[ind].val*dx*(1.0 - dy)*(1.0 - dz);
		
		id = THREEDIND(ip1,j,kp1,NFFT);
		ind = getonlyid_gchash(gch,id);
		assert(ind != GCH_INVALID);
		assert(gbuff[ind].id != -1);
		assert(gbuff[ind].id == gch->GridCells[ind].id);
		val += gbuff[ind].val*dx*(1.0 - dy)*dz;
		
		id = THREEDIND(ip1,jp1,k,NFFT);
		ind = getonlyid_gchash(gch,id);
		assert(ind != GCH_INVALID);
		assert(gbuff[ind].id != -1);
		assert(gbuff[ind].id == gch->GridCells[ind].id);
		val += gbuff[ind].val*dx*dy*(1.0 - dz);
		
		id = THREEDIND(ip1,jp1,kp1,NFFT);
		ind = getonlyid_gchash(gch,id);
		assert(ind != GCH_INVALID);
		assert(gbuff[ind].id != -1);
		assert(gbuff[ind].id == gch->GridCells[ind].id);
		val += gbuff[ind].val*dx*dy*dz;
		
		//do the projections and add to ray
		for(ii=0;ii<2;++ii)
		  for(jj=0;jj<2;++jj)
		    bundleCells[bind].rays[rind].U[ii*2+jj] += val*jac[dind1][ii]*jac[dind2][jj];
		
		if(dind1 != dind2) {
		  for(ii=0;ii<2;++ii)
		    for(jj=0;jj<2;++jj)
		      bundleCells[bind].rays[rind].U[ii*2+jj] += val*jac[dind2][ii]*jac[dind1][jj];
		}
		
		//check to make sure not inf or nan
		assert(gsl_finite(bundleCells[bind].rays[rind].U[0]));
		assert(gsl_finite(bundleCells[bind].rays[rind].U[1]));
		assert(gsl_finite(bundleCells[bind].rays[rind].U[2]));
		assert(gsl_finite(bundleCells[bind].rays[rind].U[3]));
		
	      }//for(n=0;n<Nint;++n)
	    }//for(rind=0;rind<bundleCells[bind].Nrays;++rind)
	
	  }// for(dind2=dind1;dind2<3;++dind2)
	
      }//if(ISSETBITFLAG(bundleCells[bind].active,PRIMARY_BUNDLECELL))
    }//if(abind < NumActiveBundleCells)
    
    //get units right
    if(abind < NumActiveBundleCells) {
      if(ISSETBITFLAG(bundleCells[bind].active,PRIMARY_BUNDLECELL)) {
	//fac for second derivs 2.0/CSOL/CSOL*dchi/chi*chi*chi
	fac2 = 2.0/CSOL/CSOL*dchi*rayTraceData.planeRad;
	
	//fac for first derivs 2.0/CSOL/CSOL*dchi/chi*chi
	fac1 = 2.0/CSOL/CSOL*dchi;
	
	for(rind=0;rind<bundleCells[bind].Nrays;++rind) {
	  for(ii=0;ii<2;++ii)
	    for(jj=0;jj<2;++jj)
	      bundleCells[bind].rays[rind].U[ii*2+jj] *= fac2;
	  
	  //make mixed partials symmetric
	  val = (bundleCells[bind].rays[rind].U[0*2+1] + bundleCells[bind].rays[rind].U[1*2+0])/2.0;
	  bundleCells[bind].rays[rind].U[0*2+1] = val;
	  bundleCells[bind].rays[rind].U[1*2+0] = val;
	  
	  for(ii=0;ii<2;++ii)
	    bundleCells[bind].rays[rind].alpha[ii] *= fac1;
	  
	  //sign shift to match convention of code
	  for(ii=0;ii<2;++ii)
	    bundleCells[bind].rays[rind].alpha[ii] *= -1.0;
	  
	  //do pot factor = 2/CSOL/CSOL*dchi/chi
	  bundleCells[bind].rays[rind].phi *= fac1/rayTraceData.planeRad;
	}//for(rind=0;rind<bundleCells[bind].Nrays;++rind)
      }//if(ISSETBITFLAG(bundleCells[bind].active,PRIMARY_BUNDLECELL))
    }//if(abind < NumActiveBundleCells)
    
    //clean it all up
    free_gchash(gch);
    if(Ngbuff > 0) {
      Ngbuff = 0;
      free(gbuff);
      gbuff = NULL;
    }
  
  }//for(abind=0;abind<MaxNumActiveBundleCells;++abind)
  
  free(activeBundleCellInds);
  
  t0 += MPI_Wtime();
  if(ThisTask == 0) {
    fprintf(stderr,"did interp and integral to rays in %lf seconds.\n",t0);
    fflush(stderr);
  }
}

static void get_units(char *fbase, double *L, double *a)
{
  char fname[MAX_FILENAME];

  sprintf(fname,"%s.0",fbase);

  if(ThisTask == 0)
    *L = get_period_length_LGADGET(fname);
  MPI_Bcast(L,1,MPI_DOUBLE,0,MPI_COMM_WORLD);
  *L = (*L)*rayTraceData.LengthConvFact;

  if(ThisTask == 0)
    *a = get_scale_factor_LGADGET(fname);
  MPI_Bcast(a,1,MPI_DOUBLE,0,MPI_COMM_WORLD);
}

static void read_snaps(NbodySnap **snaps, long *Nsnaps) {
  char line[MAX_FILENAME];
  FILE *fp;
  long n = 0;
  char fname[MAX_FILENAME];
  long nl;
  
  if(ThisTask == 0) {
    fp = fopen(rayTraceData.ThreeDPotSnapList,"r");
    assert(fp != NULL);
    while(fgets(line,1024,fp) != NULL) {
      if(line[0] == '#')
	continue;
      ++n;
    }
    fclose(fp);
    
    *snaps = (NbodySnap*)malloc(sizeof(NbodySnap)*n);
    assert((*snaps) != NULL);
    *Nsnaps = n;
    
    n = 0;
    fp = fopen(rayTraceData.ThreeDPotSnapList,"r");
    assert(fp != NULL);
    while(fgets(line,1024,fp) != NULL) {
      if(line[0] == '#')
	continue;
      assert(n < (*Nsnaps));
      nl = strlen(line);
      line[nl-1] = '\0';
      sprintf((*snaps)[n].fname,"%s",line);
      ++n;
    }
    fclose(fp);
    
    for(n=0;n<(*Nsnaps);++n) {
      sprintf(fname,"%s.0",(*snaps)[n].fname);
      (*snaps)[n].a = get_scale_factor_LGADGET(fname);
      (*snaps)[n].chi = comvdist((*snaps)[n].a);
    }
  }//if(ThisTask == 0)
  
  //send to other tasks
  MPI_Bcast(Nsnaps,1,MPI_LONG,0,MPI_COMM_WORLD);
  if(ThisTask != 0) {
    *snaps = (NbodySnap*)malloc(sizeof(NbodySnap)*(*Nsnaps));
    assert((*snaps) != NULL);
  }
  MPI_Bcast(*snaps,sizeof(NbodySnap)*(*Nsnaps),MPI_BYTE,0,MPI_COMM_WORLD);
}

#undef THREEDIND
#undef WARPIF
