/*
 * cnss_shim.c — libc/bionic symbol shims for CNSS vendor daemons.
 *
 * Android 12+ bionic added several symbols that our bundled recovery libc
 * (Android 10 era) does not export.  libbinder.so, libutils.so and other
 * VNDK libs reference them, so cnss-daemon/qrtr-ns/pd-mapper refuse to start
 * unless we provide these stubs via LD_PRELOAD.
 *
 * Build (aarch64, shared, no libc headers):
 *   aarch64-linux-gnu-gcc -shared -fPIC -O2 -nostdlib \
 *       -Wl,-soname,libcnss_shim.so \
 *       -o libcnss_shim.so cnss_shim.c
 */

/* Avoid pulling in any libc headers — we are the shim. */
typedef __SIZE_TYPE__  size_t;
typedef __INTPTR_TYPE__ intptr_t;

/* Bionic's real per-thread errno accessor. Raw syscalls (shim_syscall6)
 * return -errno directly on failure, NOT -1-with-errno-set the way libc
 * wrappers do -- fine for opendir() above (checked directly against the
 * raw value, and its own retry loop does the same), but openat()/open()
 * below are a generic override loaded into every LD_PRELOADed daemon
 * (rmt_storage, cnss-daemon, qrtr-ns, pd-mapper...), and real code
 * everywhere checks errno after a failed open() (e.g. "if ENOENT, try a
 * fallback path"). Without this, every such check reads a stale/garbage
 * errno instead of the real failure reason. */
extern int *__errno(void);

/* ------------------------------------------------------------------ */
/* memset_explicit / __memset_explicit                                  */
/* ------------------------------------------------------------------ */

/*
 * memset_explicit(dst, c, n):
 *   Same as memset but guaranteed not to be elided by the compiler.
 *   The barrier prevents the optimizer from removing the write.
 */
void *memset_explicit(void *dst, int c, size_t n)
{
    volatile unsigned char *p = (volatile unsigned char *)dst;
    while (n--)
        *p++ = (unsigned char)c;
    return dst;
}

void *__memset_explicit(void *dst, int c, size_t n)
    __attribute__((alias("memset_explicit")));

/* ------------------------------------------------------------------ */
/* android_get_application_target_sdk_version                          */
/* ------------------------------------------------------------------ */

/*
 * Used by libutils.so (Android 12+) to pick code paths.
 * Return Android 10 (API 29) — safe for our use case.
 */
int android_get_application_target_sdk_version(void)
{
    return 29;
}

/* ------------------------------------------------------------------ */
/* __system_property_get stub                                          */
/* ------------------------------------------------------------------ */

/*
 * Some vendor libs call __system_property_get() which lives in bionic libc.
 * Our recovery libc may export it, but provide a fallback returning empty
 * string so the property system "works" (returns 0 / empty).
 */
int __attribute__((weak)) __system_property_get(const char *name, char *value)
{
    (void)name;
    if (value)
        value[0] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* pthread_attr_setinheritsched — missing in some toolchain libc       */
/* ------------------------------------------------------------------ */

/*
 * Some vendor shared libs linked against newer NDK call this symbol.
 * Return 0 (success) as a no-op.
 */
int __attribute__((weak)) pthread_attr_setinheritsched(void *attr, int inheritsched)
{
    (void)attr;
    (void)inheritsched;
    return 0;
}

/* ------------------------------------------------------------------ */
/* android_fdsan_* stubs — Android 10+ file-descriptor sanitizer      */
/* ------------------------------------------------------------------ */

/*
 * android_fdsan_exchange_owner_tag / close_with_tag appear in Android 11+
 * libc.  libbase.so and libutils.so call them for fd tracking.  No-op stubs
 * are safe: the worst case is a leaked fd goes undetected, not a crash.
 */
typedef unsigned long long uint64_t_compat;

void __attribute__((weak)) android_fdsan_exchange_owner_tag(int fd,
    uint64_t_compat expected, uint64_t_compat tag)
{
    (void)fd; (void)expected; (void)tag;
}

int __attribute__((weak)) android_fdsan_close_with_tag(int fd, uint64_t_compat tag)
{
    (void)tag;
    /* Delegate to the real close syscall via inline asm to avoid libc dep. */
    int ret;
    __asm__ volatile("mov x8, #57\n"   /* __NR_close aarch64 */
                     "mov x0, %1\n"
                     "svc #0\n"
                     "mov %0, x0\n"
                     : "=r"(ret) : "r"(fd)
                     : "x0", "x8", "memory");
    return ret;
}

/* ------------------------------------------------------------------ */
/* __libcpp_verbose_abort — newer libc++ hardening trap                */
/* ------------------------------------------------------------------ */

/*
 * Hardened libc++ builds (as used by newer hwservicemanager/vndservice-
 * manager builds) call this on internal invariant violations instead of
 * plain abort(). Our bundled libc++.so is from an older base and doesn't
 * export it, so the daemon's dynamic linker refused to load it at all
 * ("cannot locate symbol ... referenced by hwservicemanager") — it never
 * even got a chance to run. It's marked noreturn by libc++, so this stub
 * must not return either; it just exits with SIGABRT's conventional
 * 128+signal code via a raw syscall (no libc, no vararg handling needed
 * since we never inspect the format string).
 *
 * The real symbol is C++ name-mangled — "std::__1::__libcpp_verbose_abort
 * (char const*, ...)" — not the plain C name. A plain C function named
 * __libcpp_verbose_abort() exports as literally that string and does NOT
 * satisfy the mangled reference (confirmed: first attempt still hit the
 * identical "cannot locate symbol" error). GCC's asm-label extension binds
 * the definition to the exact mangled name instead.
 */
void __attribute__((weak, noreturn))
__libcpp_verbose_abort(const char *fmt, ...) __asm__("_ZNSt3__122__libcpp_verbose_abortEPKcz");

void __libcpp_verbose_abort(const char *fmt, ...)
{
    (void)fmt;
    __asm__ volatile("mov x8, #94\n"   /* __NR_exit_group aarch64 */
                     "mov x0, #134\n"  /* 128 + SIGABRT */
                     "svc #0\n"
                     : : : "x0", "x8", "memory");
    __builtin_unreachable();
}

/* ------------------------------------------------------------------ */
/* QMI wire-level trace — logs every QMI request/response/indication  */
/* cnss-daemon exchanges with the modem over its raw AF_QIPCRTR       */
/* socket, decoded per the real wire format from the LineageOS        */
/* sm6125 kernel source (include/linux/soc/qcom/qmi.h):               */
/*   struct qmi_header { u8 type; u16 txn_id; u16 msg_id; u16 msg_len; } */
/* __packed, i.e. a 7-byte little-endian header at the start of every */
/* QMI payload delivered via recvfrom()/sendto() on a qrtr socket.    */
/* Msg IDs match drivers/net/wireless/ath/ath10k/qmi_wlfw_v01.h       */
/* (QMI_WLFW_IND_REGISTER_REQ_V01=0x0020, FW_READY_IND=0x0021, etc). */
/* This is the direct, ground-truth answer to "what is cnss-daemon    */
/* actually saying to the modem, and does it ever get an answer" —    */
/* see MEMORY.md §4.5 for why every previous approach (SD-log races,  */
/* COM/fork-boundary confusion) failed to answer this directly.       */
/* -nostdlib: everything below uses raw syscalls, no libc calls.      */
/* ------------------------------------------------------------------ */

#define __NR_read       63
#define __NR_write      64
#define __NR_openat     56
#define __NR_close      57
#define __NR_nanosleep  101
#define __NR_newfstatat 79
#define __NR_fsync      82
#define __NR_fchmod     52
#define __NR_getpid     172
#define __NR_socket     198
#define __NR_sendto     206
#define __NR_recvfrom   207

#define AT_FDCWD_V    (-100)
#define O_WRONLY_V    01
#define O_CREAT_V     0100
#define O_APPEND_V    02000
#define AF_QIPCRTR_V  42
/* NOT the generic 00200000 -- arm64 overrides O_DIRECTORY to 040000
 * (kernel/arch/arm64/include/uapi/asm/fcntl.h), diverging from most other
 * architectures. Using the generic value here caused every opendir() call
 * in this shim to fail with EINVAL, on EVERY path including "/" itself --
 * a real, hour-costing self-inflicted diagnostic bug: since this shim is
 * LD_PRELOADed into pd-mapper, its (broken) opendir() override replaced
 * pd-mapper's own real bionic opendir() calls the moment it was added,
 * making every finding gathered with it (mount-timing race, exec-order
 * theory) an artifact of this bug rather than real pd-mapper behavior. */
#define O_DIRECTORY_V 040000

static long shim_syscall6(long nr, long a0, long a1, long a2,
                           long a3, long a4, long a5)
{
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    register long x5 __asm__("x5") = a5;

    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory", "cc");
    return x0;
}

static int qrtr_fds[16];
static int qrtr_fd_n;

static int shim_is_qrtr(int fd)
{
    int i;
    for (i = 0; i < qrtr_fd_n; i++)
        if (qrtr_fds[i] == fd)
            return 1;
    return 0;
}

static void shim_hex2(unsigned char v, char *out)
{
    static const char h[] = "0123456789abcdef";
    out[0] = h[(v >> 4) & 0xf];
    out[1] = h[v & 0xf];
}

static int shim_udec(unsigned v, char *out)
{
    char tmp[8];
    int n = 0, i;
    if (v == 0) { out[0] = '0'; return 1; }
    while (v && n < 8) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

static void shim_copy(char *dst, int *p, const char *s)
{
    while (*s)
        dst[(*p)++] = *s++;
}

/* Deliberately does NOT assume a fixed header layout (struct qmi_header
 * vs. struct qrtr_ctrl_pkt look different and which applies depends on
 * whether the destination is the qrtr control port 0xfffffffe or a real
 * service port — decode that by hand from addr_hex + hex afterward,
 * cross-referencing include/uapi/linux/qrtr.h and
 * include/linux/soc/qcom/qmi.h in kernel_src_reference). PID is included
 * because LD_PRELOAD puts this shim in every vendor daemon (cnss-daemon,
 * qrtr-ns, hwservicemanager, pd-mapper, ...) simultaneously, all sharing
 * one log file — fd numbers alone collide across processes. */
static void shim_log_qmi(const char *dir, int fd, const unsigned char *buf, long len,
                          const unsigned char *addr, long addrlen)
{
    char line[640];
    int p = 0, fdlog;
    long pid = shim_syscall6(__NR_getpid, 0, 0, 0, 0, 0, 0);

    shim_copy(line, &p, "QMI "); shim_copy(line, &p, dir);
    shim_copy(line, &p, " pid="); p += shim_udec((unsigned)pid, line + p);
    shim_copy(line, &p, " fd="); p += shim_udec((unsigned)fd, line + p);
    shim_copy(line, &p, " len="); p += shim_udec((unsigned)(len < 0 ? 0 : len), line + p);

    shim_copy(line, &p, " addr=");
    if (addr && addrlen > 0) {
        long i, n = addrlen < 16 ? addrlen : 16;
        for (i = 0; i < n && p < (int)sizeof(line) - 70; i++) {
            char h[2];
            shim_hex2(addr[i], h);
            line[p++] = h[0];
            line[p++] = h[1];
        }
    } else {
        line[p++] = '-';
    }

    shim_copy(line, &p, " hex=");
    if (buf) {
        /* Was capped at 40 bytes -- too short to ever see an embedded
         * service_path string (e.g. "tms/pdr_enabled") in a SERVREG_NOTIF
         * REGISTER_LISTENER_REQ/QUERY_STATE_REQ payload, which is exactly
         * the exchange this trace exists to observe. line[] is sized to
         * comfortably fit this wider cap. */
        long i, n = len < 220 ? len : 220;
        if (n < 0)
            n = 0;
        for (i = 0; i < n && p < (int)sizeof(line) - 4; i++) {
            char h[2];
            shim_hex2(buf[i], h);
            line[p++] = h[0];
            line[p++] = h[1];
        }
    }
    line[p++] = '\n';

    fdlog = (int)shim_syscall6(__NR_openat, AT_FDCWD_V,
                                (long)"/dev/kmsg",
                                O_WRONLY_V | O_APPEND_V, 0, 0, 0);
    if (fdlog >= 0) {
        shim_syscall6(__NR_write, fdlog, (long)line, p, 0, 0, 0);
        shim_syscall6(__NR_close, fdlog, 0, 0, 0, 0, 0);
    }
}

/* Unconditional load marker, independent of any qrtr socket activity --
 * added because qmi_trace.log stayed byte-identical across several fresh
 * boots with radio actively triggered each time, meaning either LD_PRELOAD
 * silently isn't taking effect for qrtr-ns/pd-mapper in the current build,
 * or they never reach socket(AF_QIPCRTR). This settles which. */
__attribute__((constructor))
static void shim_ctor_log(void)
{
    char line[128];
    int p = 0, fdlog, cfd;
    long pid = shim_syscall6(__NR_getpid, 0, 0, 0, 0, 0, 0);
    char comm[32];
    long n;

    shim_copy(line, &p, "SHIM loaded pid=");
    p += shim_udec((unsigned)pid, line + p);
    shim_copy(line, &p, " comm=");

    cfd = (int)shim_syscall6(__NR_openat, AT_FDCWD_V, (long)"/proc/self/comm",
                              0 /* O_RDONLY */, 0, 0, 0);
    n = 0;
    if (cfd >= 0) {
        n = shim_syscall6(__NR_read, cfd, (long)comm, sizeof(comm) - 1, 0, 0, 0);
        shim_syscall6(__NR_close, cfd, 0, 0, 0, 0, 0);
    }
    if (n > 0) {
        long i;
        if (comm[n - 1] == '\n')
            n--;
        for (i = 0; i < n; i++)
            line[p++] = comm[i];
    } else {
        line[p++] = '?';
    }
    line[p++] = '\n';

    fdlog = (int)shim_syscall6(__NR_openat, AT_FDCWD_V,
                                (long)"/dev/kmsg",
                                O_WRONLY_V | O_APPEND_V, 0, 0, 0);
    if (fdlog >= 0) {
        shim_syscall6(__NR_write, fdlog, (long)line, p, 0, 0, 0);
        shim_syscall6(__NR_close, fdlog, 0, 0, 0, 0, 0);
    }

    /* Step-by-step path-resolution probe: is this process's failure total
     * (can't even open "/") or specific to deeper paths? Bypasses the
     * opendir() override entirely (raw syscall, same as it uses) to rule
     * out any interposition quirk in that override itself. */
    {
        static const char *probes[] = {
            "/", "/vendor", "/vendor/firmware_mnt",
            "/vendor/firmware_mnt/image", 0
        };
        int i;
        for (i = 0; probes[i]; i++) {
            long pfd = shim_syscall6(__NR_openat, AT_FDCWD_V, (long)probes[i],
                                      O_DIRECTORY_V, 0, 0, 0);
            char pline[128];
            int pp = 0;

            shim_copy(pline, &pp, "SHIM probe ");
            shim_copy(pline, &pp, probes[i]);
            if (pfd < 0) {
                shim_copy(pline, &pp, " errno=");
                pp += shim_udec((unsigned)(-pfd), pline + pp);
            } else {
                shim_copy(pline, &pp, " OK fd=");
                pp += shim_udec((unsigned)pfd, pline + pp);
                shim_syscall6(__NR_close, pfd, 0, 0, 0, 0, 0);
            }
            pline[pp++] = '\n';

            fdlog = (int)shim_syscall6(__NR_openat, AT_FDCWD_V, (long)"/dev/kmsg",
                                        O_WRONLY_V | O_APPEND_V, 0, 0, 0);
            if (fdlog >= 0) {
                shim_syscall6(__NR_write, fdlog, (long)pline, pp, 0, 0, 0);
                shim_syscall6(__NR_close, fdlog, 0, 0, 0, 0, 0);
            }
        }
    }

    /* Control test: stat() (newfstatat) on the specific failing directory,
     * not open() at all -- isolates whether the VFS lookup itself succeeds
     * (stat works, only the open-a-directory-handle step fails) vs the
     * lookup failing outright regardless of which syscall performs it. */
    {
        char statbuf[256];
        long sret = shim_syscall6(__NR_newfstatat, AT_FDCWD_V,
                                   (long)"/vendor/firmware_mnt/image",
                                   (long)statbuf, 0, 0, 0);
        char sline[64];
        int sp = 0;
        int skfd;

        shim_copy(sline, &sp, "SHIM stat image ");
        if (sret < 0) {
            shim_copy(sline, &sp, "errno=");
            sp += shim_udec((unsigned)(-sret), sline + sp);
        } else {
            shim_copy(sline, &sp, "OK");
        }
        sline[sp++] = '\n';
        skfd = (int)shim_syscall6(__NR_openat, AT_FDCWD_V, (long)"/dev/kmsg",
                                   O_WRONLY_V | O_APPEND_V, 0, 0, 0);
        if (skfd >= 0) {
            shim_syscall6(__NR_write, skfd, (long)sline, sp, 0, 0, 0);
            shim_syscall6(__NR_close, skfd, 0, 0, 0, 0, 0);
        }
    }

    /* Control test: same path, flags=0 (plain O_RDONLY, no O_DIRECTORY) --
     * isolates whether the flag itself is the problem or the path/process
     * is denied outright regardless of flags. */
    {
        long pfd2 = shim_syscall6(__NR_openat, AT_FDCWD_V, (long)"/", 0, 0, 0, 0);
        char pline2[64];
        int pp2 = 0;

        shim_copy(pline2, &pp2, "SHIM plainopen /");
        if (pfd2 < 0) {
            shim_copy(pline2, &pp2, " errno=");
            pp2 += shim_udec((unsigned)(-pfd2), pline2 + pp2);
        } else {
            shim_copy(pline2, &pp2, " OK fd=");
            pp2 += shim_udec((unsigned)pfd2, pline2 + pp2);
            shim_syscall6(__NR_close, pfd2, 0, 0, 0, 0, 0);
        }
        pline2[pp2++] = '\n';

        fdlog = (int)shim_syscall6(__NR_openat, AT_FDCWD_V, (long)"/dev/kmsg",
                                    O_WRONLY_V | O_APPEND_V, 0, 0, 0);
        if (fdlog >= 0) {
            shim_syscall6(__NR_write, fdlog, (long)pline2, pp2, 0, 0, 0);
            shim_syscall6(__NR_close, fdlog, 0, 0, 0, 0, 0);
        }
    }
}

int socket(int domain, int type, int protocol)
{
    long ret = shim_syscall6(__NR_socket, domain, type, protocol, 0, 0, 0);
    if (ret >= 0 && domain == AF_QIPCRTR_V && qrtr_fd_n < (int)(sizeof(qrtr_fds) / sizeof(qrtr_fds[0])))
        qrtr_fds[qrtr_fd_n++] = (int)ret;
    return (int)ret;
}

long sendto(int fd, const void *buf, unsigned long len, int flags,
            const void *addr, unsigned int addrlen)
{
    if (shim_is_qrtr(fd))
        shim_log_qmi("TX", fd, (const unsigned char *)buf, (long)len,
                      (const unsigned char *)addr, (long)addrlen);
    return shim_syscall6(__NR_sendto, fd, (long)buf, (long)len, flags,
                          (long)addr, (long)addrlen);
}

long recvfrom(int fd, void *buf, unsigned long len, int flags,
              void *addr, void *addrlen_p)
{
    long ret = shim_syscall6(__NR_recvfrom, fd, (long)buf, (long)len, flags,
                              (long)addr, (long)addrlen_p);
    if (ret > 0 && shim_is_qrtr(fd)) {
        /* addrlen_p is an in/out socklen_t*; after the syscall it holds
         * the actual written length (0 if the caller passed NULL). */
        long alen = addrlen_p ? (long)(*(unsigned int *)addrlen_p) : 0;
        shim_log_qmi("RX", fd, (const unsigned char *)buf, ret,
                      (const unsigned char *)addr, alen);
    }
    return ret;
}

/* Catches the connect()-then-read()/write() pattern some QMI client libs
 * use once a qrtr socket is "connected" to a specific service port,
 * instead of specifying the address on every sendto()/recvfrom() call. */
long read(int fd, void *buf, unsigned long len)
{
    long ret = shim_syscall6(__NR_read, fd, (long)buf, (long)len, 0, 0, 0);
    if (ret > 0 && shim_is_qrtr(fd))
        shim_log_qmi("RX", fd, (const unsigned char *)buf, ret, 0, 0);
    return ret;
}

long write(int fd, const void *buf, unsigned long len)
{
    if (shim_is_qrtr(fd))
        shim_log_qmi("TX", fd, (const unsigned char *)buf, (long)len, 0, 0);
    return shim_syscall6(__NR_write, fd, (long)buf, (long)len, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* opendir() trace — direct ground truth for pd-mapper's json_dir_list */
/* config-directory scan, which pd_locator queries show returns zero  */
/* domains for every service despite the right .jsn files existing on */
/* disk with correct permissions. Static analysis (Ghidra) traced this */
/* to json_dir_list reading back NULL at runtime despite a valid       */
/* R_AARCH64_RELATIVE relocation for it existing in the ELF file       */
/* itself (confirmed via readelf -r) -- this hook settles definitively */
/* whether main()'s config loop ever calls opendir() at all, and with  */
/* what path, bypassing every open_snoop.ko capture-cap/hook-coverage  */
/* question that made that kprobe-based approach inconclusive here.    */
/* opendir() is a public bionic libc symbol so LD_PRELOAD interposition */
/* works on it the same way it already does for read/write/socket      */
/* above; fdopendir() is deliberately NOT overridden so the real bionic */
/* libc.so implementation still builds the actual DIR* we return.      */
/* ------------------------------------------------------------------ */

typedef struct __dirstream DIR;
extern DIR *fdopendir(int fd);

DIR *opendir(const char *path)
{
    long fd;
    char line[300];
    int p = 0, fdlog;
    int retries = 0;

    fd = shim_syscall6(__NR_openat, AT_FDCWD_V, (long)path,
                        O_DIRECTORY_V, 0, 0, 0);

    /* Retry on ENOENT specifically: confirmed live that pd-mapper's config
     * directory opendir() can fail with ENOENT for /vendor/firmware_mnt/
     * image even though the vfat mount itself is verified present and
     * accessible at the exact same moment (mountinfo dump + a probe of the
     * mount root both succeed) -- consistent with a stale VFS negative
     * dentry cache entry from an earlier, premature lookup rather than a
     * real absence. If that's right, a short wait plus retry should see
     * it clear; if it's something more persistent this just costs a few
     * hundred ms before falling through to the real (still failing)
     * result, which is what would have happened anyway. */
    while (fd == -2 && path && retries < 10) {
        struct { long tv_sec; long tv_nsec; } ts = { 0, 300000000L };
        shim_syscall6(__NR_nanosleep, (long)&ts, 0, 0, 0, 0, 0);
        fd = shim_syscall6(__NR_openat, AT_FDCWD_V, (long)path,
                            O_DIRECTORY_V, 0, 0, 0);
        retries++;
    }

    /* On failure for the specific mount we care about, dump this
     * process's OWN /proc/self/mountinfo right at the moment of failure --
     * ground truth for whether this process's mount namespace actually
     * shows /vendor/firmware_mnt mounted at all, rather than assuming
     * shared-namespace semantics apply. */
    if (fd < 0 && path) {
        const char *n1 = "firmware_mnt";
        int match = 0, ci, ni;
        for (ci = 0; path[ci]; ci++) {
            for (ni = 0; n1[ni] && path[ci + ni] == n1[ni]; ni++)
                ;
            if (!n1[ni]) { match = 1; break; }
        }
        if (match) {
            int mfd = (int)shim_syscall6(__NR_openat, AT_FDCWD_V,
                                          (long)"/proc/self/mountinfo", 0, 0, 0, 0);
            if (mfd >= 0) {
                char mbuf[2048];
                long mn = shim_syscall6(__NR_read, mfd, (long)mbuf, sizeof(mbuf) - 1, 0, 0, 0);
                shim_syscall6(__NR_close, mfd, 0, 0, 0, 0, 0);
                if (mn > 0) {
                    /* /dev/kmsg silently drops large multi-line single
                     * write()s -- write one line at a time instead,
                     * matching every other working log call in this shim,
                     * and only lines containing "firmware_mnt" (or the
                     * count of total lines scanned, as a control) to keep
                     * this bounded and readable. */
                    long lstart = 0, li;
                    int lines_seen = 0, lines_matched = 0;

                    mbuf[mn] = '\0';
                    for (li = 0; li <= mn; li++) {
                        if (li == mn || mbuf[li] == '\n') {
                            long llen = li - lstart;
                            lines_seen++;
                            if (llen > 0 && llen < 250) {
                                char tmp[256];
                                long ti;
                                int has_match = 0;
                                for (ti = 0; ti < llen - 12; ti++) {
                                    if (mbuf[lstart + ti] == 'f' &&
                                        mbuf[lstart + ti + 1] == 'i' &&
                                        mbuf[lstart + ti + 2] == 'r' &&
                                        mbuf[lstart + ti + 3] == 'm') {
                                        has_match = 1;
                                        break;
                                    }
                                }
                                if (has_match) {
                                    int kfd;
                                    int tp = 0;

                                    shim_copy(tmp, &tp, "SHIM mountinfo: ");
                                    for (ti = 0; ti < llen; ti++)
                                        tmp[tp++] = mbuf[lstart + ti];
                                    tmp[tp++] = '\n';
                                    lines_matched++;

                                    kfd = (int)shim_syscall6(__NR_openat, AT_FDCWD_V, (long)"/dev/kmsg",
                                                              O_WRONLY_V | O_APPEND_V, 0, 0, 0);
                                    if (kfd >= 0) {
                                        shim_syscall6(__NR_write, kfd, (long)tmp, tp, 0, 0, 0);
                                        shim_syscall6(__NR_close, kfd, 0, 0, 0, 0, 0);
                                    }
                                }
                            }
                            lstart = li + 1;
                        }
                    }
                    {
                        char sline[80];
                        int sp = 0, kfd;

                        shim_copy(sline, &sp, "SHIM mountinfo: lines_seen=");
                        sp += shim_udec((unsigned)lines_seen, sline + sp);
                        shim_copy(sline, &sp, " matched=");
                        sp += shim_udec((unsigned)lines_matched, sline + sp);
                        sline[sp++] = '\n';
                        kfd = (int)shim_syscall6(__NR_openat, AT_FDCWD_V, (long)"/dev/kmsg",
                                                  O_WRONLY_V | O_APPEND_V, 0, 0, 0);
                        if (kfd >= 0) {
                            shim_syscall6(__NR_write, kfd, (long)sline, sp, 0, 0, 0);
                            shim_syscall6(__NR_close, kfd, 0, 0, 0, 0, 0);
                        }
                    }
                }
            }
        }
    }

    {
        long pid = shim_syscall6(__NR_getpid, 0, 0, 0, 0, 0, 0);
        int cfd = (int)shim_syscall6(__NR_openat, AT_FDCWD_V,
                                      (long)"/proc/self/comm", 0, 0, 0, 0);
        char comm[32];
        long cn = 0;

        if (cfd >= 0) {
            cn = shim_syscall6(__NR_read, cfd, (long)comm, sizeof(comm) - 1, 0, 0, 0);
            shim_syscall6(__NR_close, cfd, 0, 0, 0, 0, 0);
        }
        shim_copy(line, &p, "SHIM opendir pid=");
        p += shim_udec((unsigned)pid, line + p);
        shim_copy(line, &p, " comm=");
        if (cn > 0) {
            long ci;
            if (comm[cn - 1] == '\n')
                cn--;
            for (ci = 0; ci < cn; ci++)
                line[p++] = comm[ci];
        } else {
            line[p++] = '?';
        }
    }
    shim_copy(line, &p, " path=");
    if (path)
        shim_copy(line, &p, path);
    else
        shim_copy(line, &p, "(null)");
    if (fd < 0) {
        shim_copy(line, &p, " errno=");
        p += shim_udec((unsigned)(-fd), line + p);
    } else {
        shim_copy(line, &p, " fd=");
        p += shim_udec((unsigned)fd, line + p);
    }
    line[p++] = '\n';

    fdlog = (int)shim_syscall6(__NR_openat, AT_FDCWD_V, (long)"/dev/kmsg",
                                O_WRONLY_V | O_APPEND_V, 0, 0, 0);
    if (fdlog >= 0) {
        shim_syscall6(__NR_write, fdlog, (long)line, p, 0, 0, 0);
        shim_syscall6(__NR_close, fdlog, 0, 0, 0, 0, 0);
    }

    if (fd < 0)
        return (DIR *)0;
    return fdopendir((int)fd);
}


static int shim_path_has(const char *path, const char *needle)
{
    int ci, ni;

    if (!path)
        return 0;
    for (ci = 0; path[ci]; ci++) {
        for (ni = 0; needle[ni] && path[ci + ni] == needle[ni]; ni++)
            ;
        if (!needle[ni])
            return 1;
    }
    return 0;
}

/* cnss-daemon connects to wlfw (confirmed via the QMI wire-trace above) but
 * then never sends WLFW_CAP_REQ the way real Lineage's cnss-daemon does
 * within milliseconds of connecting -- real Lineage's own logcat shows the
 * step in between is 5 wlfw_cal_NN.bin open() attempts under
 * /data/vendor/wifi/ (all ENOENT there too, non-fatal). Neither opendir()
 * above nor the QMI sendto/recvfrom hook cover plain open()/openat(), so we
 * have no visibility into whether cnss-daemon ever reaches that step at
 * all. Hook it here, logging only "wifi"-path opens to keep noise down. */
int openat(int dirfd, const char *path, int flags, ...)
{
    long fd;
    char line[300];
    int p = 0;
    int interesting = shim_path_has(path, "wifi");
    int mode = 0;

    if (flags & O_CREAT_V) {
        __builtin_va_list ap;
        __builtin_va_start(ap, flags);
        mode = __builtin_va_arg(ap, int);
        __builtin_va_end(ap);
    }

    fd = shim_syscall6(__NR_openat, dirfd, (long)path, flags, mode, 0, 0);

    if (interesting) {
        shim_copy(line, &p, "SHIM openat path=");
        shim_copy(line, &p, path ? path : "(null)");
        if (fd < 0) {
            shim_copy(line, &p, " errno=");
            p += shim_udec((unsigned)(-fd), line + p);
        } else {
            shim_copy(line, &p, " fd=");
            p += shim_udec((unsigned)fd, line + p);
        }
        line[p++] = '\n';
        {
            int fdlog = (int)shim_syscall6(__NR_openat, AT_FDCWD_V, (long)"/dev/kmsg",
                                            O_WRONLY_V | O_APPEND_V, 0, 0, 0);
            if (fdlog >= 0) {
                shim_syscall6(__NR_write, fdlog, (long)line, p, 0, 0, 0);
                shim_syscall6(__NR_close, fdlog, 0, 0, 0, 0, 0);
            }
        }
    }

    if (fd < 0) {
        *__errno() = (int)(-fd);
        return -1;
    }
    return (int)fd;
}

int open(const char *path, int flags, ...)
{
    int mode = 0;

    if (flags & O_CREAT_V) {
        __builtin_va_list ap;
        __builtin_va_start(ap, flags);
        mode = __builtin_va_arg(ap, int);
        __builtin_va_end(ap);
    }
    return openat(AT_FDCWD_V, path, flags, mode);
}

/* cnss-daemon's own binary (confirmed via readelf --dyn-syms on the real
 * vendor blob) imports ONLY dlopen and fopen -- no open/openat/open64/
 * openat64 at all. Bionic's fopen() calls its own internal open() through a
 * private, non-interposable path, so the openat()/open() override above
 * has been completely blind to every file cnss-daemon has ever touched
 * this whole investigation -- confirmed live: zero "SHIM openat" hits ever,
 * despite real Lineage's cnss-daemon reading /data/vendor/wifi/wlfw_cal_*
 * .bin files (via fopen(), per its own logcat: "wlfw_read_file") right
 * after connecting to wlfw. Hook fopen() itself via the standard
 * dlsym(RTLD_NEXT, ...) interposition pattern instead of a raw syscall,
 * since FILE* is an opaque bionic-internal structure this -nostdlib shim
 * has no business constructing itself. */
#define RTLD_NEXT_V ((void *)-1L)
extern void *dlsym(void *handle, const char *symbol);

typedef void *(*fopen_fn_t)(const char *path, const char *mode);
static fopen_fn_t real_fopen;

void *fopen(const char *path, const char *mode)
{
    void *result;
    int interesting;

    if (!real_fopen)
        real_fopen = (fopen_fn_t)dlsym(RTLD_NEXT_V, "fopen");

    interesting = shim_path_has(path, "wifi");
    result = real_fopen ? real_fopen(path, mode) : (void *)0;

    if (interesting) {
        char line[300];
        int p = 0;

        shim_copy(line, &p, "SHIM fopen path=");
        shim_copy(line, &p, path ? path : "(null)");
        shim_copy(line, &p, " mode=");
        shim_copy(line, &p, mode ? mode : "(null)");
        shim_copy(line, &p, result ? " -> OK" : " -> NULL");
        line[p++] = '\n';
        {
            int fdlog = (int)shim_syscall6(__NR_openat, AT_FDCWD_V, (long)"/dev/kmsg",
                                            O_WRONLY_V | O_APPEND_V, 0, 0, 0);
            if (fdlog >= 0) {
                shim_syscall6(__NR_write, fdlog, (long)line, p, 0, 0, 0);
                shim_syscall6(__NR_close, fdlog, 0, 0, 0, 0, 0);
            }
        }
    }

    return result;
}
