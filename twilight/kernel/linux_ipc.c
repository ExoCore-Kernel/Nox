#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <twilight/linux_ipc.h>
#include <twilight/serial.h>
#include <twilight/tty.h>
#include <twilight/vmm.h>

/* Early Linux IPC layer for the surviving single-process Bash bring-up path.
 * It restores pipes and AF_UNIX/SOCK_STREAM semantics without depending on the
 * newer scheduler/process tree that is currently only on the unavailable
 * development checkout. */

#define USER_TOP 0x0000800000000000ull
#define PAGE_SIZE 4096ull
#define MAX_FDS 96
#define FIRST_FD 10
#define MAX_SOCKETS 48
#define MAX_PIPES 24
#define RX_CAP 4096u
#define PIPE_CAP 4096u
#define BACKLOG 16u
#define UNIX_ADDR_MAX 110u
#define IOV_MAX 64u

#define SYS_READ 0ull
#define SYS_WRITE 1ull
#define SYS_CLOSE 3ull
#define SYS_FSTAT 5ull
#define SYS_POLL 7ull
#define SYS_IOCTL 16ull
#define SYS_READV 19ull
#define SYS_WRITEV 20ull
#define SYS_DUP 32ull
#define SYS_DUP2 33ull
#define SYS_SOCKET 41ull
#define SYS_CONNECT 42ull
#define SYS_ACCEPT 43ull
#define SYS_SENDTO 44ull
#define SYS_RECVFROM 45ull
#define SYS_SENDMSG 46ull
#define SYS_RECVMSG 47ull
#define SYS_SHUTDOWN 48ull
#define SYS_BIND 49ull
#define SYS_LISTEN 50ull
#define SYS_GETSOCKNAME 51ull
#define SYS_GETPEERNAME 52ull
#define SYS_SOCKETPAIR 53ull
#define SYS_SETSOCKOPT 54ull
#define SYS_GETSOCKOPT 55ull
#define SYS_EXIT 60ull
#define SYS_FCNTL 72ull
#define SYS_EXIT_GROUP 231ull
#define SYS_PPOLL 271ull
#define SYS_ACCEPT4 288ull
#define SYS_DUP3 292ull
#define SYS_PIPE2 293ull

#define EINVAL 22
#define EBADF 9
#define EAGAIN 11
#define EFAULT 14
#define ENFILE 23
#define EMFILE 24
#define ENOTTY 25
#define EPIPE 32
#define ENOTSOCK 88
#define ENOPROTOOPT 92
#define EPROTONOSUPPORT 93
#define ESOCKTNOSUPPORT 94
#define EAFNOSUPPORT 97
#define EADDRINUSE 98
#define EISCONN 106
#define ENOTCONN 107
#define ECONNREFUSED 111

#define AF_UNIX 1
#define SOCK_STREAM 1
#define SOCK_TYPE_MASK 0x0fu
#define SOCK_NONBLOCK 0x800u
#define SOCK_CLOEXEC 0x80000u
#define O_ACCMODE 3u
#define O_RDONLY 0u
#define O_WRONLY 1u
#define O_RDWR 2u
#define O_NONBLOCK 0x800u
#define O_CLOEXEC 0x80000u
#define FD_CLOEXEC 1u
#define F_DUPFD 0u
#define F_GETFD 1u
#define F_SETFD 2u
#define F_GETFL 3u
#define F_SETFL 4u
#define F_DUPFD_CLOEXEC 1030u
#define POLLIN 0x0001
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020
#define SOL_SOCKET 1
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_ACCEPTCONN 30
#define FIONREAD 0x541bull
#define S_IFIFO 0010000u
#define S_IFSOCK 0140000u

struct __attribute__((packed)) pollfd64 { int32_t fd; int16_t events; int16_t revents; };
struct iovec64 { uint64_t base, len; };
struct msghdr64 {
    uint64_t name; uint32_t namelen, pad0; uint64_t iov, iovlen;
    uint64_t control, controllen; int32_t flags; uint32_t pad1;
};
struct timespec64 { int64_t sec, nsec; };

enum fd_kind { FD_NONE = 0, FD_SOCKET, FD_PIPE_R, FD_PIPE_W };
struct ipc_fd { enum fd_kind kind; int object; uint32_t flags; bool cloexec; };
struct unix_socket {
    bool used; unsigned refs; int peer; bool peer_eof; bool shut_r, shut_w;
    bool listening, queued; uint8_t addr[UNIX_ADDR_MAX]; uint32_t addr_len;
    int pending[BACKLOG]; unsigned pending_head, pending_count;
    uint8_t rx[RX_CAP]; size_t rx_head, rx_count;
};
struct pipe_object {
    bool used; unsigned readers, writers; uint8_t data[PIPE_CAP];
    size_t head, count;
};

static struct ipc_fd fds[MAX_FDS];
static struct unix_socket sockets[MAX_SOCKETS];
static struct pipe_object pipes[MAX_PIPES];

static void memzero(void *p, size_t n) { uint8_t *d = p; for (size_t i=0;i<n;++i) d[i]=0; }
static void memcopy(void *d0, const void *s0, size_t n) {
    uint8_t *d=d0; const uint8_t *s=s0; for (size_t i=0;i<n;++i) d[i]=s[i];
}
static void log_u64(uint64_t v) {
    char b[32]; size_t n=0; do { b[n++]=(char)('0'+v%10); v/=10; } while(v && n<sizeof(b));
    while(n) serial_write_char(b[--n]);
}
static bool user_range(uint64_t a, uint64_t n, bool write) {
    if (!n) return true;
    if (!a || a>=USER_TOP || a>UINT64_MAX-n || a+n>USER_TOP) return false;
    vmm_space_t sp=vmm_current_space(); if (sp==VMM_INVALID_SPACE) return false;
    uint64_t end=a+n, page=a&~(PAGE_SIZE-1ull);
    while(page<end) {
        uint64_t phys=0, flags=0;
        if(!vmm_translate(sp,page,&phys,&flags) || !(flags&VMM_FLAG_USER) ||
           (write && !(flags&VMM_FLAG_WRITE))) return false;
        page += PAGE_SIZE;
    }
    return true;
}
static bool uin(void *d, uint64_t s, size_t n) {
    if(!user_range(s,n,false)) return false; memcopy(d,(const void *)(uintptr_t)s,n); return true;
}
static bool uout(uint64_t d, const void *s, size_t n) {
    if(!user_range(d,n,true)) return false; memcopy((void *)(uintptr_t)d,s,n); return true;
}
static bool uzero(uint64_t d, size_t n) {
    if(!user_range(d,n,true)) return false; memzero((void *)(uintptr_t)d,n); return true;
}

static bool fd_ok(int fd) { return fd>=0 && fd<MAX_FDS && fds[fd].kind!=FD_NONE; }
static bool fd_socket(int fd) { return fd_ok(fd) && fds[fd].kind==FD_SOCKET; }
static int alloc_fd(int min) {
    if(min<0) min=0; for(int fd=min;fd<MAX_FDS;++fd) if(!fd_ok(fd)) return fd; return -1;
}
static int alloc_socket(void) {
    for(int i=0;i<MAX_SOCKETS;++i) if(!sockets[i].used) {
        memzero(&sockets[i],sizeof(sockets[i])); sockets[i].used=true; sockets[i].peer=-1; return i;
    }
    return -1;
}
static int alloc_pipe(void) {
    for(int i=0;i<MAX_PIPES;++i) if(!pipes[i].used) {
        memzero(&pipes[i],sizeof(pipes[i])); pipes[i].used=true; return i;
    }
    return -1;
}

static void peer_eof(int obj) {
    if(obj<0 || obj>=MAX_SOCKETS || !sockets[obj].used) return;
    int p=sockets[obj].peer; sockets[obj].peer=-1;
    if(p<0 || p>=MAX_SOCKETS || !sockets[p].used) return;
    if(sockets[p].peer==obj) sockets[p].peer=-1;
    sockets[p].peer_eof=true;
    serial_write("[linux:socket] peer EOF object="); log_u64((uint64_t)p); serial_write("\n");
}
static void destroy_socket(int obj) {
    if(obj<0 || obj>=MAX_SOCKETS || !sockets[obj].used) return;
    struct unix_socket *s=&sockets[obj];
    while(s->pending_count) {
        int p=s->pending[s->pending_head]; s->pending_head=(s->pending_head+1)%BACKLOG; --s->pending_count;
        if(p>=0 && p<MAX_SOCKETS && sockets[p].used) {
            sockets[p].queued=false; peer_eof(p); if(!sockets[p].refs) memzero(&sockets[p],sizeof(sockets[p]));
        }
    }
    serial_write("[linux:socket] endpoint close object="); log_u64((uint64_t)obj); serial_write("\n");
    peer_eof(obj); memzero(s,sizeof(*s));
}
static void add_ref(const struct ipc_fd *f) {
    if(f->kind==FD_SOCKET && f->object>=0 && f->object<MAX_SOCKETS && sockets[f->object].used) ++sockets[f->object].refs;
    else if(f->kind==FD_PIPE_R && f->object>=0 && f->object<MAX_PIPES && pipes[f->object].used) ++pipes[f->object].readers;
    else if(f->kind==FD_PIPE_W && f->object>=0 && f->object<MAX_PIPES && pipes[f->object].used) ++pipes[f->object].writers;
}
static void drop_ref(const struct ipc_fd *f) {
    if(f->kind==FD_SOCKET && f->object>=0 && f->object<MAX_SOCKETS && sockets[f->object].used) {
        struct unix_socket *s=&sockets[f->object]; if(s->refs) --s->refs; if(!s->refs && !s->queued) destroy_socket(f->object);
    } else if((f->kind==FD_PIPE_R || f->kind==FD_PIPE_W) && f->object>=0 && f->object<MAX_PIPES && pipes[f->object].used) {
        struct pipe_object *p=&pipes[f->object]; if(f->kind==FD_PIPE_R) { if(p->readers)--p->readers; } else { if(p->writers)--p->writers; }
        if(!p->readers && !p->writers) memzero(p,sizeof(*p));
    }
}
static void close_fd(int fd) {
    if(!fd_ok(fd)) return; struct ipc_fd old=fds[fd]; memzero(&fds[fd],sizeof(fds[fd])); drop_ref(&old);
}
static int64_t install_fd(int fd, enum fd_kind k, int obj, uint32_t flags, bool cloexec) {
    if(fd<0 || fd>=MAX_FDS) return -EBADF; close_fd(fd); fds[fd]=(struct ipc_fd){k,obj,flags,cloexec}; add_ref(&fds[fd]); return fd;
}
static int64_t dup_min(int oldfd, int min, bool cloexec) {
    if(!fd_ok(oldfd)) return -EBADF; int fd=alloc_fd(min); if(fd<0) return -EMFILE;
    fds[fd]=fds[oldfd]; fds[fd].cloexec=cloexec; add_ref(&fds[fd]); return fd;
}
static int64_t dup_to(int oldfd, int newfd, bool cloexec, bool reject_same) {
    if(!fd_ok(oldfd)) return -EBADF; if(newfd<0 || newfd>=MAX_FDS) return -EBADF;
    if(oldfd==newfd) return reject_same ? -EINVAL : oldfd;
    struct ipc_fd f=fds[oldfd]; close_fd(newfd); fds[newfd]=f; fds[newfd].cloexec=cloexec; add_ref(&fds[newfd]); return newfd;
}

static int64_t read_addr(uint64_t up, uint64_t un, uint8_t out[UNIX_ADDR_MAX], uint32_t *len) {
    if(!up || un<2) return -EINVAL; if(un>UNIX_ADDR_MAX) un=UNIX_ADDR_MAX;
    if(!uin(out,up,(size_t)un)) return -EFAULT;
    uint16_t fam=(uint16_t)out[0]|((uint16_t)out[1]<<8); if(fam!=AF_UNIX) return -EAFNOSUPPORT;
    uint32_t n=(uint32_t)un;
    if(n>2 && out[2]!=0) for(uint32_t i=2;i<n;++i) if(!out[i]) { n=i+1; break; }
    *len=n; return 0;
}
static bool addr_eq(const struct unix_socket *s,const uint8_t *a,uint32_t n) {
    if(!s->used || s->addr_len!=n) return false; for(uint32_t i=0;i<n;++i) if(s->addr[i]!=a[i]) return false; return true;
}
static int64_t socket_new(uint64_t domain,uint64_t type,uint64_t proto) {
    if(domain!=AF_UNIX) return -EAFNOSUPPORT;
    if((type&SOCK_TYPE_MASK)!=SOCK_STREAM) return -ESOCKTNOSUPPORT;
    if(proto) return -EPROTONOSUPPORT;
    int obj=alloc_socket(); if(obj<0) return -ENFILE; int fd=alloc_fd(FIRST_FD);
    if(fd<0) { memzero(&sockets[obj],sizeof(sockets[obj])); return -EMFILE; }
    uint32_t flags=O_RDWR|((type&SOCK_NONBLOCK)?O_NONBLOCK:0);
    install_fd(fd,FD_SOCKET,obj,flags,(type&SOCK_CLOEXEC)!=0);
    serial_write("[linux:socket] domain=");log_u64(domain);serial_write("\n[linux:socket] type=");log_u64(type);
    serial_write("\n[linux:socket] protocol=");log_u64(proto);serial_write("\n[linux:socket] fd=");log_u64((uint64_t)fd);serial_write("\n");
    return fd;
}
static int64_t socket_bind(int fd,uint64_t up,uint64_t un) {
    if(!fd_socket(fd)) return fd_ok(fd)?-ENOTSOCK:-EBADF; struct unix_socket *s=&sockets[fds[fd].object];
    if(s->addr_len) return -EINVAL; uint8_t a[UNIX_ADDR_MAX]; uint32_t n=0; int64_t rc=read_addr(up,un,a,&n); if(rc<0)return rc;
    for(int i=0;i<MAX_SOCKETS;++i) if(i!=fds[fd].object && addr_eq(&sockets[i],a,n)) return -EADDRINUSE;
    memcopy(s->addr,a,n); s->addr_len=n; return 0;
}
static int64_t socket_listen(int fd) {
    if(!fd_socket(fd)) return fd_ok(fd)?-ENOTSOCK:-EBADF; struct unix_socket *s=&sockets[fds[fd].object];
    if(!s->addr_len || s->peer>=0) return -EINVAL; s->listening=true; return 0;
}
static int64_t socket_connect(int fd,uint64_t up,uint64_t un) {
    if(!fd_socket(fd)) return fd_ok(fd)?-ENOTSOCK:-EBADF; int ci=fds[fd].object; struct unix_socket *c=&sockets[ci];
    if(c->peer>=0) return -EISCONN; uint8_t a[UNIX_ADDR_MAX]; uint32_t n=0; int64_t rc=read_addr(up,un,a,&n); if(rc<0)return rc;
    int li=-1; for(int i=0;i<MAX_SOCKETS;++i) if(sockets[i].used&&sockets[i].listening&&addr_eq(&sockets[i],a,n)){li=i;break;}
    if(li<0) return -ECONNREFUSED; struct unix_socket *l=&sockets[li]; if(l->pending_count>=BACKLOG) return -EAGAIN;
    int si=alloc_socket(); if(si<0) return -ENFILE; struct unix_socket *s=&sockets[si]; s->peer=ci;s->queued=true;c->peer=si;c->peer_eof=false;
    if(l->addr_len){memcopy(s->addr,l->addr,l->addr_len);s->addr_len=l->addr_len;}
    unsigned tail=(l->pending_head+l->pending_count)%BACKLOG;l->pending[tail]=si;++l->pending_count;
    serial_write("[linux:socket] connect queued listener=");log_u64((uint64_t)li);serial_write(" client=");log_u64((uint64_t)ci);serial_write(" server=");log_u64((uint64_t)si);serial_write("\n");
    return 0;
}
static int64_t socket_accept(int fd,uint64_t flags) {
    if(!fd_socket(fd)) return fd_ok(fd)?-ENOTSOCK:-EBADF; struct unix_socket *l=&sockets[fds[fd].object];
    if(!l->listening) return -EINVAL; if(!l->pending_count) return -EAGAIN;
    if(flags&~(uint64_t)(SOCK_NONBLOCK|SOCK_CLOEXEC)) return -EINVAL; int newfd=alloc_fd(FIRST_FD);if(newfd<0)return -EMFILE;
    int obj=l->pending[l->pending_head];l->pending_head=(l->pending_head+1)%BACKLOG;--l->pending_count;sockets[obj].queued=false;
    install_fd(newfd,FD_SOCKET,obj,O_RDWR|((flags&SOCK_NONBLOCK)?O_NONBLOCK:0),(flags&SOCK_CLOEXEC)!=0);return newfd;
}
static int64_t socket_write_obj(int obj,uint64_t up,uint64_t n) {
    if(obj<0||obj>=MAX_SOCKETS||!sockets[obj].used)return -EBADF;struct unix_socket *s=&sockets[obj];
    if(!n)return 0;if(!user_range(up,n,false))return -EFAULT;if(s->shut_w||s->peer<0||s->peer_eof)return -EPIPE;
    struct unix_socket *p=&sockets[s->peer];if(!p->used||p->shut_r)return -EPIPE;size_t room=RX_CAP-p->rx_count;if(!room)return -EAGAIN;
    uint64_t count=n;if(count>room)count=room;const uint8_t *src=(const uint8_t *)(uintptr_t)up;
    serial_write("[linux:socket] tx attempt fd-object=");log_u64((uint64_t)obj);serial_write(" len=");log_u64(n);serial_write("\n");
    for(uint64_t i=0;i<count;++i){size_t t=(p->rx_head+p->rx_count)%RX_CAP;p->rx[t]=src[i];++p->rx_count;}
    serial_write("[linux:socket] tx queued peer=");log_u64((uint64_t)s->peer);serial_write(" bytes=");log_u64(count);serial_write("\n");return (int64_t)count;
}
static int64_t socket_read_obj(int obj,uint64_t up,uint64_t n) {
    if(obj<0||obj>=MAX_SOCKETS||!sockets[obj].used)return -EBADF;struct unix_socket *s=&sockets[obj];
    if(!n)return 0;if(!user_range(up,n,true))return -EFAULT;if(s->shut_r)return 0;
    if(s->rx_count){uint64_t count=n;if(count>s->rx_count)count=s->rx_count;uint8_t *d=(uint8_t *)(uintptr_t)up;
        for(uint64_t i=0;i<count;++i){d[i]=s->rx[s->rx_head];s->rx_head=(s->rx_head+1)%RX_CAP;--s->rx_count;}return (int64_t)count;}
    if(s->peer_eof||s->peer<0){serial_write("[linux:socket] recv EOF object=");log_u64((uint64_t)obj);serial_write("\n");return 0;}
    return -EAGAIN;
}
static int64_t pipe_read_obj(int obj,uint64_t up,uint64_t n) {
    if(obj<0||obj>=MAX_PIPES||!pipes[obj].used)return -EBADF;struct pipe_object *p=&pipes[obj];if(!n)return 0;if(!user_range(up,n,true))return -EFAULT;
    if(!p->count)return p->writers? -EAGAIN:0;uint64_t count=n;if(count>p->count)count=p->count;uint8_t *d=(uint8_t *)(uintptr_t)up;
    for(uint64_t i=0;i<count;++i){d[i]=p->data[p->head];p->head=(p->head+1)%PIPE_CAP;--p->count;}return (int64_t)count;
}
static int64_t pipe_write_obj(int obj,uint64_t up,uint64_t n) {
    if(obj<0||obj>=MAX_PIPES||!pipes[obj].used)return -EBADF;struct pipe_object *p=&pipes[obj];if(!n)return 0;if(!user_range(up,n,false))return -EFAULT;
    if(!p->readers)return -EPIPE;size_t room=PIPE_CAP-p->count;if(!room)return -EAGAIN;uint64_t count=n;if(count>room)count=room;const uint8_t *s=(const uint8_t *)(uintptr_t)up;
    for(uint64_t i=0;i<count;++i){size_t t=(p->head+p->count)%PIPE_CAP;p->data[t]=s[i];++p->count;}return (int64_t)count;
}
static int64_t fd_read(int fd,uint64_t up,uint64_t n) {
    if(!fd_ok(fd))return -EBADF;if(fds[fd].kind==FD_SOCKET)return socket_read_obj(fds[fd].object,up,n);if(fds[fd].kind==FD_PIPE_R)return pipe_read_obj(fds[fd].object,up,n);return -EBADF;
}
static int64_t fd_write(int fd,uint64_t up,uint64_t n) {
    if(!fd_ok(fd))return -EBADF;if(fds[fd].kind==FD_SOCKET)return socket_write_obj(fds[fd].object,up,n);if(fds[fd].kind==FD_PIPE_W)return pipe_write_obj(fds[fd].object,up,n);return -EBADF;
}
static int64_t pipe2_new(uint64_t out,uint64_t flags) {
    if(flags&~(uint64_t)(O_NONBLOCK|O_CLOEXEC))return -EINVAL;if(!user_range(out,8,true))return -EFAULT;int obj=alloc_pipe();if(obj<0)return -ENFILE;
    int r=alloc_fd(FIRST_FD);if(r<0){memzero(&pipes[obj],sizeof(pipes[obj]));return -EMFILE;}install_fd(r,FD_PIPE_R,obj,O_RDONLY|(uint32_t)(flags&O_NONBLOCK),(flags&O_CLOEXEC)!=0);
    int w=alloc_fd(FIRST_FD);if(w<0){close_fd(r);return -EMFILE;}install_fd(w,FD_PIPE_W,obj,O_WRONLY|(uint32_t)(flags&O_NONBLOCK),(flags&O_CLOEXEC)!=0);
    int32_t pair[2]={r,w};if(!uout(out,pair,sizeof(pair))){close_fd(r);close_fd(w);return -EFAULT;}return 0;
}
static int64_t socketpair_new(uint64_t domain,uint64_t type,uint64_t proto,uint64_t out) {
    if(domain!=AF_UNIX)return -EAFNOSUPPORT;if((type&SOCK_TYPE_MASK)!=SOCK_STREAM)return -ESOCKTNOSUPPORT;if(proto)return -EPROTONOSUPPORT;if(!user_range(out,8,true))return -EFAULT;
    int a=alloc_socket(),b=alloc_socket();if(a<0||b<0){if(a>=0)memzero(&sockets[a],sizeof(sockets[a]));if(b>=0)memzero(&sockets[b],sizeof(sockets[b]));return -ENFILE;}sockets[a].peer=b;sockets[b].peer=a;
    int fa=alloc_fd(FIRST_FD);if(fa<0)return -EMFILE;uint32_t fl=O_RDWR|((type&SOCK_NONBLOCK)?O_NONBLOCK:0);install_fd(fa,FD_SOCKET,a,fl,(type&SOCK_CLOEXEC)!=0);
    int fb=alloc_fd(FIRST_FD);if(fb<0){close_fd(fa);return -EMFILE;}install_fd(fb,FD_SOCKET,b,fl,(type&SOCK_CLOEXEC)!=0);int32_t pair[2]={fa,fb};if(!uout(out,pair,8)){close_fd(fa);close_fd(fb);return -EFAULT;}return 0;
}
static int64_t fcntl_ipc(int fd,uint64_t cmd,uint64_t arg) {
    if(!fd_ok(fd))return -EBADF;switch(cmd){case F_DUPFD:return dup_min(fd,(int)arg,false);case F_GETFD:return fds[fd].cloexec?FD_CLOEXEC:0;case F_SETFD:fds[fd].cloexec=(arg&FD_CLOEXEC)!=0;return 0;
    case F_GETFL:return fds[fd].flags;case F_SETFL:fds[fd].flags=(fds[fd].flags&O_ACCMODE)|((uint32_t)arg&O_NONBLOCK);return 0;case F_DUPFD_CLOEXEC:return dup_min(fd,(int)arg,true);default:return -EINVAL;}
}
static int16_t poll_one(int fd,int16_t events) {
    if(!fd_ok(fd))return POLLNVAL;struct ipc_fd *f=&fds[fd];int16_t r=0;
    if(f->kind==FD_SOCKET){struct unix_socket *s=&sockets[f->object];if(s->listening){if((events&POLLIN)&&s->pending_count)r|=POLLIN;return r;}
        if((events&POLLIN)&&(s->rx_count||s->peer_eof||s->peer<0))r|=POLLIN;if(s->peer_eof||s->peer<0)r|=POLLHUP;
        if(events&POLLOUT){if(s->peer_eof||s->peer<0)r|=POLLERR|POLLHUP;else if(sockets[s->peer].used&&sockets[s->peer].rx_count<RX_CAP)r|=POLLOUT;}}
    else if(f->kind==FD_PIPE_R){struct pipe_object *p=&pipes[f->object];if((events&POLLIN)&&(p->count||!p->writers))r|=POLLIN;if(!p->writers)r|=POLLHUP;}
    else {struct pipe_object *p=&pipes[f->object];if(!p->readers)r|=POLLERR|POLLHUP;else if((events&POLLOUT)&&p->count<PIPE_CAP)r|=POLLOUT;}return r;
}
static int64_t poll_ipc(uint64_t up,uint64_t count,int64_t timeout,bool *handled) {
    if(count>96){*handled=true;return -EINVAL;}if(count&&!user_range(up,count*sizeof(struct pollfd64),true)){*handled=true;return -EFAULT;}
    bool has=false;for(uint64_t i=0;i<count;++i){struct pollfd64 p;if(!uin(&p,up+i*sizeof(p),sizeof(p))){*handled=true;return -EFAULT;}if(fd_ok(p.fd))has=true;}if(!has)return 0;*handled=true;
    for(;;){int ready=0;bool ttywait=false;for(uint64_t i=0;i<count;++i){struct pollfd64 p;if(!uin(&p,up+i*sizeof(p),sizeof(p)))return -EFAULT;p.revents=0;
        if(fd_ok(p.fd))p.revents=poll_one(p.fd,p.events);else if(p.fd>=0&&p.fd<=9){if((p.events&POLLIN)&&(p.fd==0||p.fd==3)){ttywait=true;if(tty_input_available())p.revents|=POLLIN;}if(p.events&POLLOUT)p.revents|=POLLOUT;}else if(p.fd>=0)p.revents=POLLNVAL;
        if(p.revents)++ready;if(!uout(up+i*sizeof(p),&p,sizeof(p)))return -EFAULT;}if(ready)return ready;if(timeout==0){serial_write("[linux:poll] immediate timeout=0 ready=0\n");return 0;}if(timeout>0)return 0;if(ttywait){tty_wait_for_input();continue;}return 0;}
}
static int64_t rwv(int fd,uint64_t up,uint64_t n,bool write) {
    if(n>IOV_MAX)return -EINVAL;if(n&&!user_range(up,n*sizeof(struct iovec64),false))return -EFAULT;int64_t total=0;
    for(uint64_t i=0;i<n;++i){struct iovec64 v;if(!uin(&v,up+i*sizeof(v),sizeof(v)))return total?total:-EFAULT;int64_t rc=write?fd_write(fd,v.base,v.len):fd_read(fd,v.base,v.len);if(rc<0)return total?total:rc;total+=rc;if((uint64_t)rc<v.len||!rc)break;}return total;
}
static int64_t msg_io(int fd,uint64_t up,bool write) {
    struct msghdr64 m;if(!uin(&m,up,sizeof(m)))return -EFAULT;int64_t rc=rwv(fd,m.iov,m.iovlen,write);if(!write&&rc>=0){m.flags=0;m.controllen=0;if(!uout(up,&m,sizeof(m)))return -EFAULT;}return rc;
}
static int64_t fstat_ipc(int fd,uint64_t up) {
    if(!fd_ok(fd))return -EBADF;if(!uzero(up,144))return -EFAULT;uint64_t one=1;uint32_t mode=(fds[fd].kind==FD_SOCKET?S_IFSOCK:S_IFIFO)|0666u;
    return uout(up+16,&one,8)&&uout(up+24,&mode,4)?0:-EFAULT;
}
static int64_t ioctl_ipc(int fd,uint64_t req,uint64_t arg) {
    if(!fd_ok(fd))return -EBADF;if(req!=FIONREAD)return -ENOTTY;int32_t n=0;if(fds[fd].kind==FD_SOCKET)n=(int32_t)sockets[fds[fd].object].rx_count;else n=(int32_t)pipes[fds[fd].object].count;return uout(arg,&n,4)?0:-EFAULT;
}
static int64_t sockopt_get(int fd,int level,int opt,uint64_t valp,uint64_t lenp) {
    if(!fd_socket(fd))return fd_ok(fd)?-ENOTSOCK:-EBADF;if(level!=SOL_SOCKET)return -ENOPROTOOPT;uint32_t n;if(!uin(&n,lenp,4))return -EFAULT;int32_t v;
    if(opt==SO_TYPE)v=SOCK_STREAM;else if(opt==SO_ERROR)v=0;else if(opt==SO_ACCEPTCONN)v=sockets[fds[fd].object].listening?1:0;else return -ENOPROTOOPT;uint32_t actual=4,copy=n<4?n:4;return (copy&&!uout(valp,&v,copy))||!uout(lenp,&actual,4)?-EFAULT:0;
}
static int64_t socket_shutdown(int fd,int how) {
    if(!fd_socket(fd))return fd_ok(fd)?-ENOTSOCK:-EBADF;if(how<0||how>2)return -EINVAL;struct unix_socket *s=&sockets[fds[fd].object];if(how==0||how==2){s->shut_r=true;s->rx_count=0;}if(how==1||how==2){s->shut_w=true;if(s->peer>=0&&sockets[s->peer].used)sockets[s->peer].peer_eof=true;}return 0;
}

void twilight_linux_ipc_process_exit(void) {
    bool any=false;for(int fd=0;fd<MAX_FDS;++fd)if(fd_ok(fd)){any=true;close_fd(fd);}if(any)serial_write("[linux:process] IPC fdtable released\n");
}

int64_t twilight_linux_ipc_syscall(uint64_t nr,uint64_t a1,uint64_t a2,uint64_t a3,
                                  uint64_t a4,uint64_t a5,uint64_t a6,bool *handled) {
    if(!handled)return -EINVAL;*handled=false;if(nr==SYS_EXIT||nr==SYS_EXIT_GROUP){twilight_linux_ipc_process_exit();return 0;}
    switch(nr){
    case SYS_SOCKET:*handled=true;return socket_new(a1,a2,a3);
    case SYS_BIND:*handled=true;return socket_bind((int)a1,a2,a3);
    case SYS_LISTEN:*handled=true;return socket_listen((int)a1);
    case SYS_CONNECT:*handled=true;return socket_connect((int)a1,a2,a3);
    case SYS_ACCEPT:*handled=true;return socket_accept((int)a1,0);
    case SYS_ACCEPT4:*handled=true;return socket_accept((int)a1,a4);
    case SYS_SOCKETPAIR:*handled=true;return socketpair_new(a1,a2,a3,a4);
    case SYS_SENDTO:*handled=true;return fd_socket((int)a1)?fd_write((int)a1,a2,a3):(fd_ok((int)a1)?-ENOTSOCK:-EBADF);
    case SYS_RECVFROM:*handled=true;return fd_socket((int)a1)?fd_read((int)a1,a2,a3):(fd_ok((int)a1)?-ENOTSOCK:-EBADF);
    case SYS_SENDMSG:*handled=true;return fd_socket((int)a1)?msg_io((int)a1,a2,true):(fd_ok((int)a1)?-ENOTSOCK:-EBADF);
    case SYS_RECVMSG:*handled=true;return fd_socket((int)a1)?msg_io((int)a1,a2,false):(fd_ok((int)a1)?-ENOTSOCK:-EBADF);
    case SYS_SHUTDOWN:*handled=true;return socket_shutdown((int)a1,(int)a2);
    case SYS_GETSOCKNAME:case SYS_GETPEERNAME:*handled=true;return fd_socket((int)a1)?0:(fd_ok((int)a1)?-ENOTSOCK:-EBADF);
    case SYS_SETSOCKOPT:*handled=true;return fd_socket((int)a1)?0:(fd_ok((int)a1)?-ENOTSOCK:-EBADF);
    case SYS_GETSOCKOPT:*handled=true;return sockopt_get((int)a1,(int)a2,(int)a3,a4,a5);
    case SYS_PIPE2:*handled=true;return pipe2_new(a1,a2);
    case SYS_READ:if(!fd_ok((int)a1))return 0;*handled=true;return fd_read((int)a1,a2,a3);
    case SYS_WRITE:if(!fd_ok((int)a1))return 0;*handled=true;return fd_write((int)a1,a2,a3);
    case SYS_READV:if(!fd_ok((int)a1))return 0;*handled=true;return rwv((int)a1,a2,a3,false);
    case SYS_WRITEV:if(!fd_ok((int)a1))return 0;*handled=true;return rwv((int)a1,a2,a3,true);
    case SYS_CLOSE:if(!fd_ok((int)a1))return 0;*handled=true;close_fd((int)a1);return 0;
    case SYS_FSTAT:if(!fd_ok((int)a1))return 0;*handled=true;return fstat_ipc((int)a1,a2);
    case SYS_IOCTL:if(!fd_ok((int)a1))return 0;*handled=true;return ioctl_ipc((int)a1,a2,a3);
    case SYS_FCNTL:if(!fd_ok((int)a1))return 0;*handled=true;return fcntl_ipc((int)a1,a2,a3);
    case SYS_DUP:if(!fd_ok((int)a1))return 0;*handled=true;return dup_min((int)a1,0,false);
    case SYS_DUP2:if(!fd_ok((int)a1))return 0;*handled=true;return dup_to((int)a1,(int)a2,false,false);
    case SYS_DUP3:if(!fd_ok((int)a1))return 0;*handled=true;if(a3&~(uint64_t)O_CLOEXEC)return -EINVAL;return dup_to((int)a1,(int)a2,(a3&O_CLOEXEC)!=0,true);
    case SYS_POLL:return poll_ipc(a1,a2,(int64_t)a3,handled);
    case SYS_PPOLL:{int64_t t=-1;if(a3){struct timespec64 ts;if(!uin(&ts,a3,sizeof(ts))){*handled=true;return -EFAULT;}if(ts.sec<0||ts.nsec<0||ts.nsec>=1000000000ll){*handled=true;return -EINVAL;}t=(ts.sec||ts.nsec)?1:0;}return poll_ipc(a1,a2,t,handled);}
    default:(void)a6;return 0;}
}
