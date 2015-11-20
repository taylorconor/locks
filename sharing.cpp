#include "stdafx.h"                             // pre-compiled headers
#include <iostream>                             // cout
#include <iomanip>                              // setprecision
#include "helper.h"                             //

using namespace std;                            // cout

#define K           1024                        //
#define GB          (K*K*K)                     //
#define NOPS        10000                       //
#define NSECONDS    2                           // run each test for NSECONDS

#define VINT    UINT64                          //  64 bit counter
#define ALIGNED_MALLOC(sz, align) _aligned_malloc(sz, align)
#define GINDX(n)    (g+n*lineSz/sizeof(VINT))   //

UINT64 tstart;                                  // start of test in ms
int sharing;                                    // % sharing
int lineSz;                                     // cache line size
int maxThread;                                  // max # of threads

THREADH *threadH;                               // thread handles
UINT64 *ops;                                    // for ops per thread

#if OPTYP == 3
UINT64 *aborts;                                 // for counting aborts
#endif

typedef struct {
    int sharing;                                // sharing
    int nt;                                     // # threads
    UINT64 rt;                                  // run time (ms)
    UINT64 ops;                                 // ops
    UINT64 incs;                                // should be equal ops
    UINT64 aborts;                              //
} Result;

Result *r;                                      // results
UINT indx;                                      // results index

volatile VINT *g;                               // NB: position of volatile

//
// test memory allocation [see lecture notes]
//
ALIGN(64) UINT64 cnt0;
ALIGN(64) UINT64 cnt1;
ALIGN(64) UINT64 cnt2;
UINT64 cnt3;                                    // NB: in Debug mode allocated in cache line occupied by cnt0

class Lock {
	private:
		string s;
	public:
		Lock(string s) {
			cout << "constructing lock: " << s << endl;
			this->s = s;
		}

		virtual	void increment(volatile VINT *gs) = 0;
		virtual void acquire() = 0;
		virtual void release() = 0;

		string str() {
			return s;
		}
};

class AtomicIncrement : public Lock {
	private:
		pthread_mutex_t mutex;
	
	public:
		AtomicIncrement() : Lock("Atomic Increment") {
			this->mutex = PTHREAD_MUTEX_INITIALIZER;
		}
		void increment(volatile VINT *gs) {
			(*gs)++;
		}

		void acquire() {
			pthread_mutex_lock(&mutex);
		}

		void release() {
			pthread_mutex_unlock(&mutex);
		}
};

#define LOCKTYPE	0
#if LOCKTYPE == 0

Lock *lock = new AtomicIncrement();

#endif

//
// worker
//
WORKER worker(void *vthread)
{
    int thread = (int)((size_t) vthread);

    UINT64 n = 0;

    volatile VINT *gt = GINDX(thread);
    volatile VINT *gs = GINDX(maxThread);

    runThreadOnCPU(thread % ncpu);

    while (1) {
        for (int i = 0; i < NOPS; i++) {
		lock->acquire();
		lock->increment(gs);
		lock->release();
	}
	n += NOPS;

	//
	// check if runtime exceeded
	//
	if ((getWallClockMS() - tstart) > NSECONDS*1000)
		break;

    }
    ops[thread] = n;
    return 0;
	
}

//
// main
//
int main()
{
    ncpu = getNumberOfCPUs();   // number of logical CPUs
    maxThread = 2 * ncpu;       // max number of threads

    //
    // get date
    //
    char dateAndTime[256];
    getDateAndTime(dateAndTime, sizeof(dateAndTime));

    //
    // console output
    //
    cout << getHostName() << " " << getOSName() << " sharing " << (is64bitExe() ? "(64" : "(32") << "bit EXE)" ;
#ifdef _DEBUG
    cout << " DEBUG";
#else
    cout << " RELEASE";
#endif
    cout << " [" << lock->str() << "]" << " NCPUS=" << ncpu << " RAM=" << (getPhysicalMemSz() + GB - 1) / GB << "GB " << dateAndTime << endl;
#ifdef COUNTER64
    cout << "COUNTER64";
#else
    cout << "COUNTER32";
#endif
    cout << " NOPS=" << NOPS << " NSECONDS=" << NSECONDS << " LOCKTYPE=" << lock->str();
    cout << endl;
    cout << "Intel" << (cpu64bit() ? "64" : "32") << " family " << cpuFamily() << " model " << cpuModel() << " stepping " << cpuStepping() << " " << cpuBrandString() << endl;

    //
    // get cache info
    //
    lineSz = getCacheLineSz();
    //lineSz *= 2;

    if ((&cnt3 >= &cnt0) && (&cnt3 < (&cnt0 + lineSz/sizeof(UINT64))))
        cout << "Warning: cnt3 shares cache line used by cnt0" << endl;
    if ((&cnt3 >= &cnt1) && (&cnt3 < (&cnt1 + lineSz / sizeof(UINT64))))
        cout << "Warning: cnt3 shares cache line used by cnt1" << endl;
    if ((&cnt3 >= &cnt2) && (&cnt3 < (&cnt2 + lineSz / sizeof(UINT64))))
        cout << "Warning: cnt2 shares cache line used by cnt1" << endl;

    cout << endl;

    //
    // allocate global variable
    //
    // NB: each element in g is stored in a different cache line to stop false sharing
    //
    threadH = (THREADH*) ALIGNED_MALLOC(maxThread*sizeof(THREADH), lineSz);             // thread handles
    ops = (UINT64*) ALIGNED_MALLOC(maxThread*sizeof(UINT64), lineSz);                   // for ops per thread

    g = (VINT*) ALIGNED_MALLOC((maxThread + 1)*lineSz, lineSz);                         // local and shared global variables
    r = (Result*) ALIGNED_MALLOC(5*maxThread*sizeof(Result), lineSz);                   // for results
    memset(r, 0, 5*maxThread*sizeof(Result));                                           // zero

    indx = 0;

    //
    // use thousands comma separator
    //
    setCommaLocale();

    //
    // header
    //
    cout << "sharing";
    cout << setw(4) << "nt";
    cout << setw(6) << "rt";
    cout << setw(16) << "ops";
    cout << setw(6) << "rel";
    cout << endl;

    cout << "-------";              // sharing
    cout << setw(4) << "--";        // nt
    cout << setw(6) << "--";        // rt
    cout << setw(16) << "---";      // ops
    cout << setw(6) << "---";       // rel
    cout << endl;

    //
    // run tests
    //
    UINT64 ops1 = 1;

    for (sharing = 0; sharing <= 100; sharing += 25) {

        for (int nt = 1; nt <= maxThread; nt *= 2, indx++) {

            //
            //  zero shared memory
            //
            for (int thread = 0; thread < nt; thread++)
                *(GINDX(thread)) = 0;   // thread local
            *(GINDX(maxThread)) = 0;    // shared


            //
            // get start time
            //
            tstart = getWallClockMS();

            //
            // create worker threads
            //
            for (int thread = 0; thread < nt; thread++)
                createThread(&threadH[thread], worker, (void*)(size_t)thread);

            //
            // wait for ALL worker threads to finish
            //
            waitForThreadsToFinish(nt, threadH);
            UINT64 rt = getWallClockMS() - tstart;


            //
            // save results and output summary to console
            //
            for (int thread = 0; thread < nt; thread++) {
                r[indx].ops += ops[thread];
                r[indx].incs += *(GINDX(thread));
            }
            r[indx].incs += *(GINDX(maxThread));
            if ((sharing == 0) && (nt == 1))
                ops1 = r[indx].ops;
            r[indx].sharing = sharing;
            r[indx].nt = nt;
            r[indx].rt = rt;

            cout << setw(6) << sharing << "%";
            cout << setw(4) << nt;
            cout << setw(6) << fixed << setprecision(2) << (double) rt / 1000;
            cout << setw(16) << r[indx].ops;
            cout << setw(6) << fixed << setprecision(2) << (double) r[indx].ops / ops1;

            if (r[indx].ops != r[indx].incs)
                cout << " ERROR incs " << setw(3) << fixed << setprecision(0) << 100.0 * r[indx].incs / r[indx].ops << "% effective";

            cout << endl;

            //
            // delete thread handles
            //
            for (int thread = 0; thread < nt; thread++)
                closeThread(threadH[thread]);

        }

    }

    cout << endl;

    //
    // output results so they can easily be pasted into a spread sheet from console window
    //
    setLocale();
    cout << "sharing/nt/rt/ops/incs";
    cout << endl;
    for (UINT i = 0; i < indx; i++) {
        cout << r[i].sharing << "/"  << r[i].nt << "/" << r[i].rt << "/"  << r[i].ops << "/" << r[i].incs;
        cout << endl;
    }
    cout << endl;
    quit();

    return 0;

}

// eof
