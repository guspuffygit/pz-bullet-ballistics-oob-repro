// Repro for the out-of-bounds read in
// PZBallisticsTarget::updateSkeletonBoneTransforms (PZBullet64.dll).
//
// Linux   libPZBullet64.so  0x172ef0
// Windows PZBullet64.dll    RVA 0x26590   (crash RVA 0x269fd)
//
// The real function computes
//     world[i] = world[ bones[i].parent ] * local[i]     for i in 1..numberOfBones-1
// The native bone table has 35 entries. numberOfBones comes from the target
// character's body model and is never compared with 35. A body mesh with extra
// nodes under Dummy01 makes Java pass a larger count, so the walk reads parent
// indices from whatever follows the bone table and uses them to index world[].
//
// This harness walks 45 bones over a 35-entry table. The bytes after the table
// are ASCII text, which is what one of the field crashes had there.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <signal.h>
  #include <unistd.h>
#endif

// btTransform in this build is 16 doubles, 0x80 bytes.
struct btTransform { double m[16]; };

// Skeleton bone record, 0x20 bytes, parent index at offset 0.
struct Bone { int32_t parent; uint8_t pad[0x1c]; };

static const int TABLE_BONES = 35;   // SkeletonBone.count(), the native table size
static const int PASSED_BONES = 45;  // numberOfBones seen in the field crashes

static btTransform* g_world;

#if defined(_WIN32)
static LONG WINAPI on_av(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    fprintf(stderr, "ACCESS_VIOLATION reading %p (world base %p)\n",
            (void*)ep->ExceptionRecord->ExceptionInformation[1], (void*)g_world);
    ExitProcess(139);
}
static void install_handler() { SetUnhandledExceptionFilter(on_av); }
#else
static void on_segv(int, siginfo_t* si, void*) {
    char buf[128];
    int n = snprintf(buf, sizeof buf, "SIGSEGV reading %p (world base %p)\n",
                     si->si_addr, (void*)g_world);
    ssize_t w = write(2, buf, n); (void)w;
    _exit(139);
}
static void install_handler() {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
}
#endif

// Same indexing as the native walk. The transform multiply is left out because
// the fault is the load of world[parent].
static void updateSkeletonBoneTransforms(const Bone* bones, int numberOfBones,
                                         btTransform* world) {
    memset(&world[0], 0, sizeof(btTransform));
    for (int i = 1; i < numberOfBones; i++) {
        int parent = bones[i].parent;
        if (i >= TABLE_BONES) {
            printf("bone %d is past the table, parent index read as %d (0x%08x)\n",
                   i, parent, (unsigned)parent);
            fflush(stdout);
        }
        world[i] = world[parent];                     // RVA 0x269fd
    }
}

int main() {
    install_handler();

    // One block holds the 35-entry table followed by unrelated bytes, the way
    // the heap places another allocation after it.
    size_t tableBytes = TABLE_BONES * sizeof(Bone);
    size_t tailBytes = (PASSED_BONES - TABLE_BONES) * sizeof(Bone);
    uint8_t* block = (uint8_t*)malloc(tableBytes + tailBytes);
    Bone* bones = (Bone*)block;
    bones[0].parent = -1;
    for (int i = 1; i < TABLE_BONES; i++) bones[i].parent = i - 1;
    static const char text[] = "media/models_X/Skinned/data";
    for (size_t i = 0; i < tailBytes; i++) block[tableBytes + i] = text[i % (sizeof text - 1)];

    g_world = (btTransform*)malloc(PASSED_BONES * sizeof(btTransform));

    printf("bone table holds %d entries, walking %d bones\n", TABLE_BONES, PASSED_BONES);
    fflush(stdout);
    updateSkeletonBoneTransforms(bones, PASSED_BONES, g_world);
    printf("walk finished without faulting\n");
    return 0;
}
