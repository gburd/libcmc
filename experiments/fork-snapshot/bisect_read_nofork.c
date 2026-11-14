/*
 * b7_nofork.c: read-intercepting vs not, but NO fork injection. Just
 * PTRACE_INTERRUPT, rewind rip to the syscall instruction, PTRACE_CONT.
 * Isolates whether the -ENOSYS on the re-issued accept is caused by the
 * FORK injection excursion or purely by having `read` in the seccomp
 * USER_NOTIF filter.  argv[1] = 1 (read intercepted) or 0 (not).
 * Linux x86-64 only.
 */
#define _GNU_SOURCE
#include <elf.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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
static int READINT=1;
static int inst(void){
  struct sock_filter f9[]={
    BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_socket,6,0),
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_bind,5,0),
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_listen,4,0),
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_accept,3,0),
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_accept4,2,0),
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_read,1,0),
    BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
    BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_USER_NOTIF)};
  struct sock_filter f8[]={
    BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_socket,5,0),
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_bind,4,0),
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_listen,3,0),
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_accept,2,0),
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_accept4,1,0),
    BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
    BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_USER_NOTIF)};
  struct sock_fprog p = READINT ? (struct sock_fprog){9,f9} : (struct sock_fprog){8,f8};
  if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0))return -1;
  return seccomp(SECCOMP_SET_MODE_FILTER,SECCOMP_FILTER_FLAG_NEW_LISTENER,&p);
}
static int sfd(int s,int fd){char cb[CMSG_SPACE(sizeof(int))]={0};char d=76;struct iovec io={&d,1};struct msghdr m={0};m.msg_iov=&io;m.msg_iovlen=1;m.msg_control=cb;m.msg_controllen=sizeof cb;struct cmsghdr*c=CMSG_FIRSTHDR(&m);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&fd,sizeof(int));return sendmsg(s,&m,0)<0?-1:0;}
static int rfd(int s){char cb[CMSG_SPACE(sizeof(int))]={0};char d;struct iovec io={&d,1};struct msghdr m={0};m.msg_iov=&io;m.msg_iovlen=1;m.msg_control=cb;m.msg_controllen=sizeof cb;if(recvmsg(s,&m,0)<0)return -1;struct cmsghdr*c=CMSG_FIRSTHDR(&m);int fd=-1;if(c)memcpy(&fd,CMSG_DATA(c),sizeof(int));return fd;}
static void node(int w){int l=inst();if(l<0||sfd(w,l))_exit(91);
  int s=socket(AF_INET,SOCK_STREAM,0); bind(s,0,0); listen(s,8);
  for(;;){ int c=accept(s,0,0); char b; (void)!read(c,&b,1); }
}
static long handle(long nr){ if(nr==SYS_socket)return 500; if(nr==SYS_read)return 0; return 0; }
int main(int argc,char**argv){setvbuf(stdout,NULL,_IONBF,0);alarm(30);
  if(argc>1)READINT=atoi(argv[1]);
  char eb[8]; snprintf(eb,sizeof eb,"%d",READINT); setenv("RI",eb,1);
  seccomp(SECCOMP_GET_NOTIF_SIZES,0,&sz);req=calloc(1,sz.seccomp_notif);resp=calloc(1,sz.seccomp_notif_resp);
  int sp[2];socketpair(AF_UNIX,SOCK_DGRAM,0,sp);pid_t pid=fork();
  if(pid==0){close(sp[0]);READINT=atoi(getenv("RI"));node(sp[1]);_exit(99);}
  close(sp[1]);lf=rfd(sp[0]);ptrace(PTRACE_SEIZE,pid,0,0);
  for(int k=0;k<3;k++){struct pollfd pf={lf,POLLIN,0};poll(&pf,1,4000);memset(req,0,sz.seccomp_notif);ioctl(lf,SECCOMP_IOCTL_NOTIF_RECV,req);long nr=req->data.nr;memset(resp,0,sz.seccomp_notif_resp);resp->id=req->id;resp->val=handle(nr);ioctl(lf,SECCOMP_IOCTL_NOTIF_SEND,resp);}
  struct pollfd pf={lf,POLLIN,0};poll(&pf,1,4000);memset(req,0,sz.seccomp_notif);ioctl(lf,SECCOMP_IOCTL_NOTIF_RECV,req);
  printf("READINT=%d quiescent: nr=%lld\n",READINT,(long long)req->data.nr);
  ptrace(PTRACE_INTERRUPT,pid,0,0);int st;waitpid(pid,&st,0);
  struct user_regs_struct s2;struct iovec r2={&s2,sizeof s2};ptrace(PTRACE_GETREGSET,pid,(void*)(long)NT_PRSTATUS,&r2);
  /* NO fork injection: just rewind rip to the syscall instruction and CONT */
  struct user_regs_struct b=s2;b.rip=s2.rip-2;b.rax=s2.orig_rax;struct iovec bv={&b,sizeof b};
  ptrace(PTRACE_SETREGSET,pid,(void*)(long)NT_PRSTATUS,&bv);
  ptrace(PTRACE_CONT,pid,0,0);
  struct pollfd p2={lf,POLLIN,0};int pr=poll(&p2,1,4000);
  if(pr>0){memset(req,0,sz.seccomp_notif);ioctl(lf,SECCOMP_IOCTL_NOTIF_RECV,req);printf("RESUMED (no-fork) nr=%lld a0=%llx (want 43)\n",(long long)req->data.nr,(unsigned long long)req->data.args[0]);}
  else printf("RESUMED (no-fork) NO notif poll=%d\n",pr);
  kill(pid,SIGKILL);return 0;
}
