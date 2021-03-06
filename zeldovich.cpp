/* v1.0 -- Initial version

v1.1 -- Fixed critical bug in Box-Muller implementation.
Fixed minor bug in spline_function initialization that was causing 
crashes under Linux.

v1.2 -- Added feature to output only one XY slab (at a chosen z)
so as to keep the files smaller for debugging or plotting.

v1.3-- Changed to use ParseHeader to handle input files

v1.4-- Changed output to match input specification for abacus

v1.5-- Changed standard random call to mersene twister from the GSL

v1.6-- Support for "oversampled" simulations (same modes at different PPD) via the k_cutoff option

v1.7-- Support for PLT eigenmodes and rescaling
*/

#define VERSION "zeldovich_v1.7"

#include <cmath>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cctype>
#include <cstring>
#include <complex>
#include <cassert>
#include <iostream>
#include <fstream>
#include <gsl/gsl_rng.h>
#include <time.h>
#include "spline_function.h"
#include "header.h"
#include "ParseHeader.hh"
#include <omp.h>

#ifdef DIRECTIO
// DIO libraries
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "file.h"
#include "file.cpp"
#include "iolib.cpp"
#endif

#define Complx std::complex<double>

static double __dcube;
#define CUBE(a) ((__dcube=(a))==0.0?0.0:__dcube*__dcube*__dcube)

gsl_rng ** rng; //The random number generator
double* eig_vecs;
int eig_vecs_ppd;
double max_disp[3];

#include "parameters.cpp"
#include "power_spectrum.cpp"
#include "block_array.cpp"
#include "output.cpp"

// ===============================================================

// TODO: Replace with our own FFT
#include "fftw3.h"
fftw_plan plan1d, plan2d;
void Setup_FFTW(int n) {
    fftw_complex *p;
    p = new fftw_complex[n*n];
    plan1d = fftw_plan_dft_1d(n, p, p, +1, FFTW_PATIENT);
    plan2d = fftw_plan_dft_2d(n, n, p, p, +1, FFTW_PATIENT);
    delete []p;
    return;
}

void Inverse1dFFT(Complx *p, int n) {
    // Given a pointer to a 1d complex vector, packed as p[n].
    // Do the 1d inverse FFT in place
    fftw_execute_dft(plan1d, (fftw_complex *)p, (fftw_complex *)p);
}
void Inverse2dFFT(Complx *p, int n) {
    // Given a pointer to a 2d complex array, contiguously packed as p[n][n].
    // Do the 2d inverse FFT in place
    fftw_execute_dft(plan2d, (fftw_complex *)p, (fftw_complex *)p);
}
void InverseFFT_Yonly(Complx *p, int n) {
    // Given a pointer to a 2d complex array, contiguously packed as p[n][n].
    // Do the 1d inverse FFT on the first index (the long stride one)
    // for each value of the second index.
    // Note that the Y in the title doesn't refer to the Y direction in 
    // our 3-d problem!
    Complx *tmp;
    int j,k;
    tmp = new Complx[n];
    for (j=0;j<n;j++) {
        // We will load one row at a time
        for (k=0;k<n;k++) tmp[k] = p[k*n+j];
        Inverse1dFFT(tmp, n);
        for (k=0;k<n;k++) p[k*n+j] = tmp[k];
    }
    delete []tmp;
}

//================================================================

// We use a set of X-Z arrays of Complx numbers (ordered by A and Y).
#define AYZX(_slab,_a,_y,_z,_x) _slab[(_x)+array.ppd*((_z)+array.ppd*((_a)+array.narray*(_y)))]

typedef struct {
    double vec[3];
    double val;
} eigenmode;

double interp_eigmode(int ikx, int iky, int ikz, int i, int ppd){
#define EIGMODE(_kx,_ky,_kz,_i) (eig_vecs[(_kx)*eig_vecs_ppd*halfppd*4 + (_ky)*halfppd*4 + (_kz)*4 + (_i)])
    int halfppd = eig_vecs_ppd/2 + 1;
    if(eig_vecs_ppd % ppd == 0)
        return EIGMODE(ikx*eig_vecs_ppd/ppd, iky*eig_vecs_ppd/ppd, ikz*eig_vecs_ppd/ppd, i);
    
    double fx = ((double) eig_vecs_ppd) / ppd * ikx;
    double fy = ((double) eig_vecs_ppd) / ppd * iky;
    double fz = ((double) eig_vecs_ppd) / ppd * ikz;
    
    // For ppd 64, [0,32] are positive k, [33,63] are negative
    // So don't interpolate between 32-33!  Map upwards instead.
    if(fx > eig_vecs_ppd/2 && fx < eig_vecs_ppd/2 + 1)
        fx = floor(fx+1);
    if(fy > eig_vecs_ppd/2 && fy < eig_vecs_ppd/2 + 1)
        fy = floor(fy+1);
    if(fz > eig_vecs_ppd/2 && fz < eig_vecs_ppd/2 + 1)
        fz = floor(fz+1);
    
    // Build the indices of the nearest grid points
    int ikx_l = (int) fx;
    int ikx_h = ikx_l + 1;  // This is okay when ikx_l == eig_vecs_ppd/2 because fx is an integer so ikx_h is never used
    int iky_l = (int) fy;
    int iky_h = iky_l + 1;
    int ikz_l = (int) fz;
    int ikz_h = ikz_l + 1;
    
    // If ikx = 127, then kx = -1, so we should interpolate
    // between -1 and 0; i.e. between ikx = 63 and 0.
    if(ikx_h == eig_vecs_ppd) ikx_h = 0;
    if(iky_h == eig_vecs_ppd) iky_h = 0;
    if(ikz_h == eig_vecs_ppd) ikz_h = 0;
    
    // The fractional position between the grid points
    fx -= ikx_l;
    fy -= iky_l;
    fz -= ikz_l;
    
    // Trilinear interpolation coefficients
    double f[8];
    f[0] = (1 - fx) * (1 - fy) * (1 - fz);
    f[1] = (1 - fx) * (1 - fy) * (fz);
    f[2] = (1 - fx) * (fy) * (1 - fz);
    f[3] = (1 - fx) * (fy) * (fz);
    f[4] = (fx) * (1 - fy) * (1 - fz);
    f[5] = (fx) * (1 - fy) * (fz); 
    f[6] = (fx) * (fy) * (1 - fz);
    f[7] = (fx) * (fy) * (fz);
    
    return f[0]*EIGMODE(ikx_l, iky_l, ikz_l, i) + f[1]*EIGMODE(ikx_l, iky_l, ikz_h, i) +
           f[2]*EIGMODE(ikx_l, iky_h, ikz_l, i) + f[3]*EIGMODE(ikx_l, iky_h, ikz_h, i) +
           f[4]*EIGMODE(ikx_h, iky_l, ikz_l, i) + f[5]*EIGMODE(ikx_h, iky_l, ikz_h, i) +
           f[6]*EIGMODE(ikx_h, iky_h, ikz_l, i) + f[7]*EIGMODE(ikx_h, iky_h, ikz_h, i);
}

eigenmode get_eigenmode(int kx, int ky, int kz, int ppd, int qPLT){
    eigenmode e;
    
    if(qPLT){
        // undo nyquist wrapping
        // These are the necessary array indices
        int ikx = kx < 0 ? ppd + kx : kx;
        int iky = ky < 0 ? ppd + ky : ky;
        int ikz = kz < 0 ? ppd + kz : kz;
        // note: np.fft has the convention of freq[ppd/2] = -ppd/2, instead of +ppd/2
        // This is different from the convention in this code
        // but we normalize to this convention when we generate the eigenmodes.
        // kz is already okay because of rfft.
        ikz = ikz > ppd/2 ? ppd - ikz : ikz; // Use the index from the +k half-space
        double k2 = kx*kx + ky*ky + kz*kz;
        
        // Use interpolation to get the eigenmode
        eigenmode ehat;
        ehat.vec[0] = interp_eigmode(ikx, iky, ikz, 0, ppd);
        ehat.vec[1] = interp_eigmode(ikx, iky, ikz, 1, ppd);
        ehat.vec[2] = interp_eigmode(ikx, iky, ikz, 2, ppd);
        ehat.val = interp_eigmode(ikx, iky, ikz, 3, ppd);
        // Set the sign of the z component (because the real FFT only gives the +kz half-space)
        ehat.vec[2] *= copysign(1, kz);
        // Linear interpolation might not preserve |ehat| = 1, so enforce this
        double ehatmag = sqrt(ehat.vec[0]*ehat.vec[0] + ehat.vec[1]*ehat.vec[1] + ehat.vec[2]*ehat.vec[2]);
        ehat.vec[0] /= ehatmag; ehat.vec[1] /= ehatmag; ehat.vec[2] /= ehatmag;
        
        // This upweights each mode by 1/(khat*ehat)
        double norm = k2/( kx*ehat.vec[0] + ky*ehat.vec[1] + kz*ehat.vec[2] );
        if(k2 == 0.0 || !std::isfinite(norm)) norm = 0.0;
        e.vec[0] = norm*ehat.vec[0];
        e.vec[1] = norm*ehat.vec[1];
        e.vec[2] = norm*ehat.vec[2];
        e.val = ehat.val;
    } else {
        e.vec[0] = kx;
        e.vec[1] = ky;
        e.vec[2] = kz;
        e.val = 1;
    }

    return e;
}

void LoadPlane(BlockArray& array, Parameters& param, PowerSpectrum& Pk, 
                int yblock, int yres, Complx *slab, Complx *slabHer) {
    Complx D,F,G,H,f;
    Complx I(0.0,1.0);
    double k2;
    int a, x,y,z, kx,ky,kz, xHer,yresHer,zHer;
    double k2_cutoff = param.nyquist*param.nyquist/(param.k_cutoff*param.k_cutoff);

    y = yres+yblock*array.block;
    ky = y>array.ppd/2?y-array.ppd:y;        // Nyquist wrapping
    yresHer = array.block-1-yres;         // Reflection
    for (z=0;z<array.ppd;z++) {
        kz = z>array.ppd/2?z-array.ppd:z;        // Nyquist wrapping
        zHer = array.ppd-z; if (z==0) zHer=0;     // Reflection
        for (x=0;x<array.ppd;x++) {
            kx = x>array.ppd/2?x-array.ppd:x;        // Nyquist wrapping
            xHer = array.ppd-x; if (x==0) xHer=0;    // Reflection
            // We will pack two complex arrays
            k2 = (kx*kx+ky*ky+kz*kz)*param.fundamental*param.fundamental;
            
            // Force Nyquist elements to zero, being extra careful with rounding
            int kmax = array.ppd/2./param.k_cutoff+.5;
            if (abs(kx)==kmax || abs(kz)==kmax || abs(ky)==kmax) D = 0.0;
            // Force all elements with wavenumber above k_cutoff (nominally k_Nyquist) to zero
            else if (k2>=k2_cutoff) D = 0.0;
            // Pick out one mode
            else if (param.qonemode && !(kx==param.one_mode[0] && ky==param.one_mode[1] && kz==param.one_mode[2])) D=0.0;
            // We deliberately only call cgauss() if we are inside the k_cutoff region
            // to get the same phase for a given k and cutoff region, no matter the ppd
            else D = Pk.cgauss(sqrt(k2),yres);
            // D = 0.1;    // If we need a known level
            
            k2 /= param.fundamental; // Get units of F,G,H right
            if (k2==0.0) k2 = 1.0;  // Avoid divide by zero
            // if (!(ky==5)) D=0.0;    // Pick out one plane
            
            eigenmode e = get_eigenmode(kx, ky, kz, array.ppd, param.qPLT);
            double rescale = 1.;
            if(param.qPLTrescale){
                double a_NL = 1./(1+param.PLT_target_z);
                double a0 = 1./(1+param.z_initial);
                double alpha_m = (sqrt(1. + 24*e.val) - 1)/6.;
                rescale = pow(a_NL/a0, 1 - 1.5*alpha_m);
            }
            F = rescale*I*e.vec[0]/k2*D;
            G = rescale*I*e.vec[1]/k2*D;
            H = rescale*I*e.vec[2]/k2*D;
            
            if(param.qPLT)
                f = (sqrt(1. + 24*e.val) - 1)*.25; // 1/4 instead of 1/6 because v = alpha*u/t0 = 3/2*H*alpha*u
            
            // printf("%d %d %d   %d %d %d   %f   %f %f\n",
            // x,y,z, kx,ky,kz, k2, real(D), imag(D));
            // H = F = D = 0.0;   // Test that the Hermitian aspects work
            // Now A = D+iF and B = G+iH.  
            // A is in array 0; B is in array 1
            AYZX(slab,0,yres,z,x) = D+I*F;
            AYZX(slab,1,yres,z,x) = G+I*H;
            if(param.qPLT){
                AYZX(slab,2,yres,z,x) = Complx(0,0) + I*F*f;
                AYZX(slab,3,yres,z,x) = G*f + I*H*f;
            }
            // And we need to store the complex conjugate
            // in the reflected entry.  We are reflecting
            // each element.  Note that we are storing one element
            // displaced in y; we will need to fix this when loading
            // for the y transform.  For now, we want the two block
            // boundaries to match.  This means that the conjugates 
            // for y=0 are being saved, which will be used below.
            AYZX(slabHer,0,yresHer,zHer,xHer) = conj(D)+I*conj(F);
            AYZX(slabHer,1,yresHer,zHer,xHer) = conj(G)+I*conj(H);
            if(param.qPLT){
                AYZX(slabHer,2,yresHer,zHer,xHer) = 0. + I*conj(F*f);
                AYZX(slabHer,3,yresHer,zHer,xHer) = conj(G*f) + I*conj(H*f);
            }
        }
    } // End the x-z loops

    // Need to do something special for ky=0 to enforce the 
    // Hermitian structure.  Recall that this whole plane was
    // stored in reflection and conjugate; we just need to copy
    // half of it back.
    if (yblock==0&&yres==0) {
        // Copy the first half plane onto the second
        for (z=0;z<array.ppd/2;z++) {
            zHer = array.ppd-z; if (z==0) zHer=0;
            // Treat y=z=0 as a half line
            int xmax = (z==0?array.ppd/2:array.ppd);
            for (x=0;x<xmax;x++) {
                xHer = array.ppd-x;if (x==0) xHer=0;
                for (a=0;a<array.narray;a++) {
                    AYZX(slab,a,yres,zHer,xHer) =
                    (AYZX(slabHer,a,yresHer,zHer,xHer));
                }
            }
        }
        // And the origin must be zero
        for (a=0;a<array.narray;a++) AYZX(slab,a,0,0,0) = 0.0; 
    }

    // Now do the Z FFTs, since those data are contiguous
    for (a=0;a<array.narray;a++) {
        InverseFFT_Yonly(&(AYZX(slab,a,yres,0,0)),array.ppd);
        InverseFFT_Yonly(&(AYZX(slabHer,a,yresHer,0,0)),array.ppd);
    }
    return;
}

void StoreBlock(BlockArray& array, int yblock, int zblock, Complx *slab) {
    // We must be sure to store the block sequentially.
    // data[zblock=0..NB-1][yblock=0..NB-1]
    //     [array=0..1][zresidual=0..P-1][yresidual=0..P-1][x=0..PPD-1]
    // Can't openMP an I/O loop.
    int a,yres,y,zres,z,yresHer;
    array.bopen(yblock,zblock,"w");
    for (a=0;a<array.narray;a++) 
    for (zres=0;zres<array.block;zres++) 
    for (yres=0;yres<array.block;yres++) {
        z = zres+array.block*zblock;
        y = yres+array.block*yblock;
        // Copy the whole X skewer
        array.bwrite(&(AYZX(slab,a,yres,z,0)),array.ppd);
    }
    array.bclose();
    return;
}

void ZeldovichZ(BlockArray& array, Parameters& param, PowerSpectrum& Pk) {
    // Generate the Fourier space density field, one Y block at a time
    // Use it to generate all arrays (density, qx, qy, qz) in Fourier space,
    // Do Z direction inverse FFTs.
    // Pack the result into 'array'.
    Complx *slab, *slabHer;
    int a,x,yres,yblock,y,zres,zblock,z, yresHer,zHer,xHer;
    unsigned long long int len = 1llu*array.block*array.ppd*array.ppd*array.narray;
    slab    = new Complx[len];
    slabHer = new Complx[len];
    //
    printf("Looping over Y: ");
    for (yblock=0;yblock<array.numblock/2;yblock++) {
        // We're going to do each pair of Y slabs separately.
        // Load the deltas and do the FFTs for each pair of planes
        printf(".."); fflush(stdout);
        #pragma omp parallel
        {  //begin parallel region
            #pragma omp for private(yres) schedule(static,1)
            for (yres=0;yres<array.block;yres++) {     
                LoadPlane(array,param,Pk,yblock,yres,slab,slabHer);
            }
        }//End Parallel region

        // Now store it into the primary BlockArray.  
        // Can't openMP an I/O loop.
        for (zblock=0;zblock<array.numblock;zblock++) {
            StoreBlock(array,yblock,zblock,slab);
            StoreBlock(array,array.numblock-1-yblock,zblock,slabHer);
        }
    }  // End yblock for loop
    delete []slabHer;
    delete []slab;
    printf("\n"); fflush(stdout);
    return;
}

// ===============================================================

// We use a set of X-Y arrays of Complx numbers (ordered by A and Z).
#define AZYX(_slab,_a,_z,_y,_x) _slab[(_x)+array.ppd*((_y)+array.ppd*((_a)+array.narray*(_z)))]

void LoadBlock(BlockArray& array, int yblock, int zblock, Complx *slab) {
    // We must be sure to access the block sequentially.
    // data[zblock=0..NB-1][yblock=0..NB-1]
    //     [array=0..1][zresidual=0..P-1][yresidual=0..P-1][x=0..PPD-1]
    // Can't openMP an I/O loop.
    int a,yres,y,zres,z,yshift;
    array.bopen(yblock,zblock,"r");
    for (a=0;a<array.narray;a++)
    for (zres=0;zres<array.block;zres++) 
    for (yres=0;yres<array.block;yres++) {
        z = zres+array.block*zblock;
        y = yres+array.block*yblock;
        // Copy the whole X skewer.  However, we want to
        // shift the y frequencies in the reflected half
        // by one.
        // FLAW: Assumes array.ppd is even.
        if (y>=array.ppd/2) yshift=y+1; else yshift=y;
        if (yshift==array.ppd) yshift=array.ppd/2;
        // Put it somewhere; this is about to be overwritten
        array.bread(&(AZYX(slab,a,zres,yshift,0)),array.ppd);
    }
    array.bclose();
    return;
}

void ZeldovichXY(BlockArray& array, Parameters& param, FILE *output, FILE *densoutput) {
    // Do the Y & X inverse FFT and output the results.
    // Do this one Z slab at a time; try to load the data in order.
    // Try to write the output file in z order
    void WriteParticlesSlab(FILE *output, FILE *densoutput, 
    int z, Complx *slab1, Complx *slab2, Complx *slab3, Complx *slab4,
    BlockArray& array, Parameters& param);
    Complx *slab;
    unsigned long long int len = 1llu*array.block*array.ppd*array.ppd*array.narray;
    slab = new Complx[len];
    int a,x,yres,yblock,y,zres,zblock,z,yshift;
    printf("Looping over Z: ");
    for (zblock=0;zblock<array.numblock;zblock++) {
        // We'll do one Z slab at a time
        // Load the slab back in.  
        // Can't openMP an I/O loop.
        printf("."); fflush(stdout);
        for (yblock=0;yblock<array.numblock;yblock++) {
            LoadBlock(array, yblock, zblock, slab);
        } 

        // The Nyquist frequency y=array.ppd/2 must now be set to 0
        // because we shifted the data by one location.
        // FLAW: this assumes PPD is even.
        y = array.ppd/2;
        for (zres=0;zres<array.block;zres++) {
            for (a=0;a<array.narray;a++) {
                for (x=0;x<array.ppd;x++) AZYX(slab,a,zres,y,x) = 0.0;
            }
        }

        // Now we want to do the Y & X inverse FFT.
        for (a=0;a<array.narray;a++) {
            #pragma omp parallel
            {
                #pragma omp for private(zres) schedule(static,1)
                for (zres=0;zres<array.block;zres++) {
                    Inverse2dFFT(&(AZYX(slab,a,zres,0,0)),array.ppd);
                }
            }//End parallel region
        }

        // Now write out these rows of [z][y][x] positions
        // Can't openMP an I/O loop.
        

        for (zres=0;zres<array.block;zres++) {
            z = zres+array.block*zblock;
            if (param.qoneslab<0||z==param.qoneslab) {
                // We have the option to output only one z slab.



                WriteParticlesSlab(output,densoutput,z,
                &(AZYX(slab,0,zres,0,0)), &(AZYX(slab,1,zres,0,0)),
                &(AZYX(slab,2,zres,0,0)), &(AZYX(slab,3,zres,0,0)),
                array, param);
            }
        }
    } // End zblock for loop
    delete []slab;
    printf("\n"); fflush(stdout);
    return;
}

// ===============================================================

void load_eigmodes(Parameters &param){
    printf("Using PLT eigenmodes.\n");
    // The eigvecs file consists of the ppd (32-bit int)
    // followed by PPDxPPDx(PPD/2+1)*4 doubles
    std::ifstream eigf;
    eigf.open(param.PLT_filename, std::ios::in|std::ios::binary|std::ios::ate);  //opens to end of file
    if (!eigf){
        std::cerr << "[Error] Could not open eigenmode file \"" << param.PLT_filename << "\".\n";
        exit(1);
    }
    std::streampos size;
    size = eigf.tellg();
    eigf.seekg (0, std::ios::beg);
    eigf.read((char*) &eig_vecs_ppd, sizeof(eig_vecs_ppd));
    
    size_t nbytes = eig_vecs_ppd*eig_vecs_ppd*(eig_vecs_ppd/2 + 1)*4*sizeof(double);
    if(size != nbytes + sizeof(eig_vecs_ppd)){
        std::cerr << "[Error] Eigenmode file \"" << param.PLT_filename << "\" of size " << size
            << " did not match expected size " << nbytes << " from eig_vecs_ppd " << eig_vecs_ppd << ".\n";
        exit(1);
    }
    
    eig_vecs = (double*) malloc(nbytes);
    eigf.read((char*) eig_vecs, nbytes);
    
    eigf.close();
}

int main(int argc, char *argv[]) {
    if (argc != 2){
        printf("Usage: %s param_file\n", argv[0]);
        exit(1);
    }
    
    FILE *output, *densoutput;
    double memory;
    density_variance = 0.0;
    Parameters param(argv[1]);

    PowerSpectrum Pk(10000);
    if (Pk.LoadPower(param.Pk_filename,param)!=0) return 1;
    param.append_file_to_comments(param.Pk_filename);

    //param.print(stdout);   // Inform the command line user
    memory = CUBE(param.ppd/1024.0)*2*sizeof(Complx);
    printf("Total memory usage (GB): %5.3f\n", memory);
    printf("Two slab memory usage (GB): %5.3f\n", memory/param.numblock*2.0);
    printf("File sizes (GB): %5.3f\n", memory/param.numblock/param.numblock);

    /*
    if (param.qnoheader==0) 
        if (param.qascii) param.print(output,"zeldovich_ascii");
    else {
        if (param.qvelocity) param.print(output,"zeldovich_6float");
        else param.print(output,"zeldovich_3float");
    }
*/
    if (param.qdensity>0) {
        densoutput = fopen(param.density_filename,"w");
        assert(densoutput!=NULL);
        if (param.qnoheader==0) param.print(densoutput,"zeldovich_1float");
    } else densoutput = NULL;

    if(param.qPLT){
        load_eigmodes(param);
    }
    
    if(param.k_cutoff != 1){
        printf("Using k_cutoff = %f (effective ppd = %d)\n", param.k_cutoff, (int)(param.ppd/param.k_cutoff+.5));
    }

    Setup_FFTW(param.ppd);
    // Two arrays for dens,x,y,z, two more for vx,vy,vz
    int narray = param.qPLT ? 4 : 2;
    BlockArray array(param.ppd,param.numblock,narray,param.output_dir,param.ramdisk);    
    srandom(param.seed);
    ZeldovichZ(array, param, Pk);
    output = 0; // Current implementation doesn't use user-provided output
    ZeldovichXY(array, param, output, densoutput);

    printf("The rms density variation of the pixels is %f\n", sqrt(density_variance/CUBE(param.ppd)));
    printf("This could be compared to the P(k) prediction of %f\n",
    Pk.sigmaR(param.separation/4.0)*pow(param.boxsize,1.5));
    
    printf("The maximum component-wise displacements are (%g, %g, %g).\n", max_disp[0], max_disp[1], max_disp[2]);
    printf("For Abacus' 2LPT implementation to work (assuming FINISH_WAIT_RADIUS = 1),\nthis implies a maximum CPD of %d\n", (int) (param.boxsize/(2*max_disp[2])));  // The slab direction is z in this code
    // fclose(output);
    
    if(param.qPLT)
        free(eig_vecs);
    
    return 0;
}
