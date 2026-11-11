/*
 * fdtest.c: isolates whether the engine's --fast livelock is a
 * fork-resume MECHANIC problem or an engine bookkeeping problem.
 *
 * It reproduces the engine's exact node pattern -- intercepted
 * socket -> bind -> listen -> accept, all answered with VIRTUAL fds
 * (socket = 500, the accept DEFERRED and never answered, as the engine
 * does) -- then freezes the accept-blocked node and fork-resumes it, the
 * same way the engine does.
 *
 * RESULT (Linux x86-64, ptrace_scope=1): the resumed worker correctly
 * re-raises nr=43 (accept).  So the mechanic works even with the full
 * virtual-fd syscall history; the engine's --fast livelock (worker ends
 * at read(-1), accept not re-trapped) is therefore an ENGINE sequencing
 * bug (CONT-all-then-drive_quiescent, or a stray waitpid during the next
 * node's fork_master consuming the worker's ptrace/notif event), NOT a
 * ptrace/seccomp limitation.  The fix is to service each worker's
 * re-issued notification immediately after its PTRACE_CONT (as this
 * probe and acctest.c do), before forking the next node.
 *
 * Linux x86-64 only.  cc -O2 -o fdtest fdtest.c && ./fdtest   (quiet host)
 */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef SECCOMP_FILTER_FLAG_NEW_LISTENER
#define SECCOMP_FILTER_FLAG_NEW_LISTENER (1UL<<3)
#endif
static int seccomp(unsigned op,unsigned fl,void*a){return (int)syscall(SYS_seccomp,op,fl,a);}
static struct seccomp_notif_sizes sz; static struct seccomp_notif *req; static struct seccomp_notif_resp *resp; static int lf;
static int inst(void){
  struct sock_filter f[]={
    BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_socket,5,0),
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_bind,4,0),
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_listen,3,0),
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_accept,2,0),
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_accept4,1,0),
    BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
    BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_USER_NOTIF)};
  struct sock_fprog p={8,f};
  if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0))return -1;
  return seccomp(SECCOMP_SET_MODE_FILTER,SECCOMP_FILTER_FLAG_NEW_LISTENER,&p);
}
static int sfd(int s,int fd){char cb[CMSG_SPACE(sizeof(int))]={0};char d=76;struct iovec io={&d,1};struct msghdr m={0};m.msg_iov=&io;m.msg_iovlen=1;m.msg_control=cb;m.msg_controllen=sizeof cb;struct cmsghdr*c=CMSG_FIRSTHDR(&m);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&fd,sizeof(int));return sendmsg(s,&m,0)<0?-1:0;}
static int rfd(int s){char cb[CMSG_SPACE(sizeof(int))]={0};char d;struct iovec io={&d,1};struct msghdr m={0};m.msg_iov=&io;m.msg_iovlen=1;m.msg_control=cb;m.msg_controllen=sizeof cb;if(recvmsg(s,&m,0)<0)return -1;struct cmsghdr*c=CMSG_FIRSTHDR(&m);int fd=-1;if(c)memcpy(&fd,CMSG_DATA(c),sizeof(int));return fd;}
static void node(int w){int l=inst();if(l<0||sfd(w,l))_exit(91);
  /* match the engine: socket->bind->listen->accept, all intercepted */
  int s=socket(AF_INET,SOCK_STREAM,0);  /* checker returns 500 */
  bind(s,0,0); listen(s,8);
  for(;;){ int c=accept(s,0,0); /* blocks in the notif */ char b; (void)!read(c,&b,1); }
}
/* checker: answer socket=500, bind/listen=0, DEFER accept (never answer) */
static long handle(long nr){ if(nr==SYS_socket)return 500; return 0; }
int main(void){setvbuf(stdout,NULL,_IONBF,0);alarm(30);
  seccomp(SECCOMP_GET_NOTIF_SIZES,0,&sz);req=calloc(1,sz.seccomp_notif);resp=calloc(1,sz.seccomp_notif_resp);
  int sp[2];socketpair(AF_UNIX,SOCK_DGRAM,0,sp);pid_t pid=fork();
  if(pid==0){close(sp[0]);node(sp[1]);_exit(99);}
  close(sp[1]);lf=rfd(sp[0]);ptrace(PTRACE_SEIZE,pid,0,0);
  /* service socket, bind, listen; then the accept notif arrives and we LEAVE it (deferred) */
  for(int k=0;k<3;k++){struct pollfd pf={lf,POLLIN,0};poll(&pf,1,4000);memset(req,0,sz.seccomp_notif);ioctl(lf,SECCOMP_IOCTL_NOTIF_RECV,req);long nr=req->data.nr;memset(resp,0,sz.seccomp_notif_resp);resp->id=req->id;resp->val=handle(nr);ioctl(lf,SECCOMP_IOCTL_NOTIF_SEND,resp);}
  /* now the node issues accept -> notif.  receive it to learn the boundary, but do NOT answer (deferred, like the engine) */
  struct pollfd pf={lf,POLLIN,0};poll(&pf,1,4000);memset(req,0,sz.seccomp_notif);ioctl(lf,SECCOMP_IOCTL_NOTIF_RECV,req);
  printf("quiescent: node blocked in nr=%lld (want 43=accept)\n",(long long)req->data.nr);
  /* freeze + fork-resume, like the engine */
  ptrace(PTRACE_INTERRUPT,pid,0,0);int st;waitpid(pid,&st,0);
  struct user_regs_struct s2;struct iovec r2={&s2,sizeof s2};ptrace(PTRACE_GETREGSET,pid,(void*)(long)NT_PRSTATUS,&r2);
  printf("frozen: rip=%llx orig_rax=%lld rax=%lld\n",(unsigned long long)s2.rip,(long long)s2.orig_rax,(long long)s2.rax);
  /* inject fork */
  ptrace(PTRACE_SETOPTIONS,pid,0,(void*)(long)PTRACE_O_TRACEFORK);
  struct user_regs_struct r=s2;r.orig_rax=SYS_fork;r.rax=SYS_fork;r.rip=s2.rip-2;struct iovec wv={&r,sizeof r};
  ptrace(PTRACE_SETREGSET,pid,(void*)(long)NT_PRSTATUS,&wv);ptrace(PTRACE_CONT,pid,0,0);waitpid(pid,&st,0);
  unsigned long m=0;ptrace(PTRACE_GETEVENTMSG,pid,0,&m);pid_t child=(pid_t)m;waitpid(child,&st,0);
  struct user_regs_struct b=s2;b.rip=s2.rip-2;b.rax=s2.orig_rax;struct iovec bv={&b,sizeof b};
  ptrace(PTRACE_SETREGSET,pid,(void*)(long)NT_PRSTATUS,&bv);ptrace(PTRACE_SETREGSET,child,(void*)(long)NT_PRSTATUS,&bv);
  /* resume the worker (parent); does its re-issued accept re-trap? */
  ptrace(PTRACE_CONT,pid,0,0);
  struct pollfd p2={lf,POLLIN,0};int pr=poll(&p2,1,4000);
  if(pr>0){memset(req,0,sz.seccomp_notif);ioctl(lf,SECCOMP_IOCTL_NOTIF_RECV,req);printf("RESUMED worker raised nr=%lld (want 43=accept re-trap)\n",(long long)req->data.nr);}
  else printf("RESUMED worker raised NO notif (poll=%d)\n",pr);
  kill(pid,SIGKILL);kill(child,SIGKILL);return 0;
}
