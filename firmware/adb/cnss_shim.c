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
#define __NR_fsync      82
#define __NR_getpid     172
#define __NR_socket     198
#define __NR_sendto     206
#define __NR_recvfrom   207

#define AT_FDCWD_V    (-100)
#define O_WRONLY_V    01
#define O_CREAT_V     0100
#define O_APPEND_V    02000
#define AF_QIPCRTR_V  42

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
                                (long)"/mnt/sdcard/minios/qmi_trace.log",
                                O_WRONLY_V | O_CREAT_V | O_APPEND_V, 0644, 0, 0);
    if (fdlog >= 0) {
        shim_syscall6(__NR_write, fdlog, (long)line, p, 0, 0, 0);
        shim_syscall6(__NR_fsync, fdlog, 0, 0, 0, 0, 0);
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
                                (long)"/mnt/sdcard/minios/qmi_trace.log",
                                O_WRONLY_V | O_CREAT_V | O_APPEND_V, 0644, 0, 0);
    if (fdlog >= 0) {
        shim_syscall6(__NR_write, fdlog, (long)line, p, 0, 0, 0);
        shim_syscall6(__NR_fsync, fdlog, 0, 0, 0, 0, 0);
        shim_syscall6(__NR_close, fdlog, 0, 0, 0, 0, 0);
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
