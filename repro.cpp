// Deterministic repro for the out-of-bounds read in
// PZBallisticsTarget::updateSkeletonBoneTransforms (PZBullet64.dll).
//
// Linux  libPZBullet64.so  0x172ef0
// Windows PZBullet64.dll    RVA 0x26590   (crash RVA 0x269fd)
//
// The real function walks the skeleton computing
//     world[i] = world[ bones[i].parent ] * local[i]
// with no check on the parent index. Human bone 34 (Translation_Data) is a
// top-level frame in MaleBody.x, so RagdollBuilder sends its parent as
// SkeletonBone.None.index() == -1, and the native skeleton stores -1 unchanged.
// Every aim therefore reads world[-1], i.e. 0x30..0x40 bytes before the world[]
// buffer. In the shipped game that read almost always lands in live heap, so it
// corrupts one unused transform and nobody notices. It faults only when the
// buffer happens to start a page whose predecessor is unmapped.
//
// This harness removes the luck: it places world[] at the start of a page with
// a PROT_NONE guard page in front, the way MSVC full PageHeap /backwards lays
// out heap blocks. world[-1] then lands on the guard page and the process dies
// at the same read the game does, every single run.
//
// Build + run:  see Makefile  (./repro should SIGSEGV / EXCEPTION_ACCESS_VIOLATION)

#include <cstdio>
#include <cstdint>
#include <cstring>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <unistd.h>
  #include <signal.h>
  #include <cstdlib>
#endif

#if !defined(_WIN32)
static void on_segv(int, siginfo_t* si, void*) {
    char buf[96];
    int n = snprintf(buf, sizeof buf,
                     "SIGSEGV reading %p  (inside world[-1], i.e. world base - 0x80)\n",
                     si->si_addr);
    ssize_t w = write(2, buf, n); (void)w;
    _exit(139);
}
static void install_handler() {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
}
#else
static void install_handler() {}
#endif

// A btTransform is a btMatrix3x3 (3 btVector3) + a btVector3 origin, double
// precision: 16 doubles = 0x80 bytes. This matches the 0x80 stride (shl 7) and
// the +0x30/+0x40/+0x50 field reads seen in the disassembly.
struct btTransform { double m[16]; };

// Skeleton::Bone is 0x20 bytes; only the parent index (offset 0) matters here.
struct Bone { int32_t parent; uint8_t pad[0x1c]; };

static const int NUM_BONES = 35;              // SkeletonBone.count()
static const int TRANSLATION_DATA = 34;       // last human bone, parentless

static btTransform* alloc_world_with_guard_before(int n) {
    size_t bytes = (size_t)n * sizeof(btTransform);   // 35 * 0x80 = 0x1180
#if defined(_WIN32)
    SYSTEM_INFO si; GetSystemInfo(&si);
    size_t pg = si.dwPageSize;
    size_t span = ((bytes + pg - 1) / pg) * pg;
    char* base = (char*)VirtualAlloc(NULL, pg + span, MEM_RESERVE, PAGE_NOACCESS);
    // guard page stays NOACCESS; commit only the buffer page(s) after it.
    VirtualAlloc(base + pg, span, MEM_COMMIT, PAGE_READWRITE);
    return (btTransform*)(base + pg);
#else
    long pg = sysconf(_SC_PAGESIZE);
    size_t span = ((bytes + pg - 1) / pg) * pg;
    char* base = (char*)mmap(NULL, pg + span, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mprotect(base, pg, PROT_NONE);            // guard page precedes the buffer
    return (btTransform*)(base + pg);
#endif
}

// A faithful copy of the vulnerable walk. Field math is omitted; the crash is
// the read of world[parent], not the multiply.
static void updateSkeletonBoneTransforms(const Bone* bones, int numberOfBones,
                                         btTransform* world) {
    memset(&world[0], 0, sizeof(btTransform));        // world[0] = this->transform
    for (int i = 1; i < numberOfBones; i++) {
        int parent = bones[i].parent;                 // movsxd rcx,[rax+r10]  (stride 0x20)
        btTransform p = world[parent];                // RVA 0x269fd  <-- OOB when parent == -1
        world[i] = p;                                 // world[i] = world[parent] * local[i]
    }
    printf("walk finished without faulting (this build got lucky heap layout)\n");
}

int main() {
    install_handler();
    btTransform* world = alloc_world_with_guard_before(NUM_BONES);

    // Native default skeleton: a parent per bone. Give every bone a valid parent
    // except Translation_Data, which RagdollBuilder reports parentless -> -1.
    Bone bones[NUM_BONES];
    for (int i = 0; i < NUM_BONES; i++) bones[i].parent = i - 1;   // simple chain
    bones[0].parent = -1;                                          // Dummy01 root (never read; loop starts at 1)
    bones[TRANSLATION_DATA].parent = -1;                          // the bug: a non-root bone with parent -1

    printf("world[] buffer at %p, guard page immediately before it\n", (void*)world);
    printf("walking %d bones; bone %d (Translation_Data) has parent -1\n",
           NUM_BONES, TRANSLATION_DATA);
    printf("expect a fault reading world[-1] at %p (world base - 0x80) ...\n",
           (void*)((char*)world - 0x80));
    fflush(stdout);

    updateSkeletonBoneTransforms(bones, NUM_BONES, world);
    return 0;
}
