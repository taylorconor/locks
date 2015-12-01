#include "stdafx.h"                             // pre-compiled headers
#include <iostream>                             // cout
#include <iomanip>                              // setprecision
#include "helper.h"                             //

using namespace std;                            // cout

#define K           1024                        //
#define GB          (K*K*K)                     //
#define NOPS        100                       //
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

class ALIGNEDMA {
	public:
		void *operator new(size_t);
		void operator delete(void *);
};

void *ALIGNEDMA::operator new(size_t sz) {
	sz = (sz + lineSz - 1) / lineSz * lineSz;
	return _aligned_malloc(sz, lineSz);
}

void ALIGNEDMA::operator delete(void *p) {
	_aligned_free(p);
}

struct QNode : public ALIGNEDMA {
	volatile int waiting;
	volatile QNode *next;
};

class Lock {
	private:
		string s;

	public:
		Lock(string s) {
			this->s = s;
		}

		virtual	void increment(volatile VINT *gs, int pid = 0) = 0;
		
		string str() {
			return s;
		}
};

class AtomicIncrement : public Lock {
	public:
		AtomicIncrement() : Lock("Atomic Increment") {}

		void increment(volatile VINT *gs, int pid = 0) {
			InterlockedIncrement(gs);
		}
};

class BakeryLock : public Lock {
	private:
		int *number;
		int *choosing;

		void acquire(int pid) {
			choosing[pid] = 1;
			_mm_mfence();
			int max = 0;
			for (int i = 0; i < maxThread; i++) {
				if (number[i] > max)
					max = number[i];
			}
			number[pid] = max+1;
			choosing[pid] = 0;
			_mm_mfence();
			for (int j = 0; j < maxThread; j++) {
				while(choosing[j]);
				while((number[j] != 0) && 
					((number[j] < number[pid]) || 
					 ((number[j] == number[pid]) && 
					  (j < pid))));
			}
		}
		void release(int pid) {
			number[pid] = 0;
		}
	public:
		BakeryLock() : Lock("Bakery Lock") {
			number = (int*)calloc(maxThread, sizeof(int));
			choosing = (int*)calloc(maxThread, sizeof(int));
		}

		void increment(volatile VINT *gs, int pid) {
			acquire(pid);
			(*gs)++;
			release(pid);
		}
};

class TestAndSetLock : public Lock {
	private:
		int lock;

		void acquire() {
			while(InterlockedExchange(&lock, 1));
		}
		void release() {
			lock = 0;
		}
	
	public:
		TestAndSetLock() : Lock("TestAndSet Lock") {
			lock = 0;
		}

		void increment(volatile VINT *gs, int pid = 0) {
			acquire();
			(*g)++;
			release();
		}
};

class TestAndTestAndSetLock : public Lock {
	private:
		int lock;

		void acquire() {
			do {
				while(lock == 1);
			} while(InterlockedExchange(&lock, 1));
		}
		void release() {
			lock = 0;
		}
	
	public:
		TestAndTestAndSetLock() : Lock("TestAndTestAndSet Lock") {
			lock = 0;
		}

		void increment(volatile VINT *gs, int pid = 0) {
			acquire();
			(*g)++;
			release();
		}
};

class MCSLock : public Lock {
	private:
		volatile QNode **lock;

		volatile QNode *acquire() {
			volatile QNode *qn = new volatile QNode();
			qn->next = NULL;
			volatile QNode *pred = (QNode*)InterlockedExchangePointer((PVOID*)lock, (PVOID)qn);
			if (pred == NULL)
				return qn;
			qn->waiting = 1;
			pred->next = qn;
			while(qn->waiting);
			return qn;
		}
		void release(volatile QNode *qn) {
			volatile QNode *succ;
			if (!(succ = qn->next)) {
				if (InterlockedCompareExchangePointer((PVOID*)lock, NULL, (PVOID)qn) == qn)
					return;
				do {
					succ = qn->next;
				} while(!succ);
			}
			succ->waiting = 0;
		}
	
	public:
		MCSLock() : Lock("MCS Lock") {
			lock = new volatile QNode*;
			*lock = NULL;
		}

		void increment(volatile VINT *gs, int pid = 0) {
			volatile QNode *qn = acquire();
			(*g)++;
			release(qn);
		}
};
		
Lock *lock;


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
		lock->increment(gs, thread);
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

#define LOCKTYPE 4

#if LOCKTYPE == 0
lock = new AtomicIncrement();
#elif LOCKTYPE == 1
lock = new BakeryLock();
#elif LOCKTYPE == 2
lock = new TestAndSetLock();
#elif LOCKTYPE == 3
lock = new TestAndTestAndSetLock();
#elif LOCKTYPE == 4
lock = new MCSLock();
#endif

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
