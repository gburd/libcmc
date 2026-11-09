/*
 * refork.c: the load-bearing DFS assumption for fork-from-snapshot --
 * from ONE frozen snapshot of a notif-blocked node, re-fork it TWICE and
 * resume each copy so it re-issues its blocked syscall and raises a
 * FRESH notification the checker services, with the snapshot staying
 * pristine between. Hard alarm() so it can never wedge the session.
 */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
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
static int seccomp(unsigned op,unsigned fl,void*a){return(int)syscall(SYS_seccomp,op,fl,a);}
#define CELL ((volatile int*)0x600000000000ULL)
static struct seccomp_notif_sizes sz;
static struct seccomp_notif *req; static struct seccomp_notif_resp *resp;
static int lf;

static int install_filter(void){
  struct sock_filter f[]={
    BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_write,0,1),
    BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_USER_NOTIF),
    BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
  struct sock_fprog p={sizeof f/sizeof f[0],f};
  if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0))return -1;
  return seccomp(SECCOMP_SET_MODE_FILTER,SECCOMP_FILTER_FLAG_NEW_LISTENER,&p);
}
static int send_fd(int s,int fd){char cb[CMSG_SPACE(sizeof(int))]={0};char d='L';
  struct iovec iov={&d,1};struct msghdr mh={0};mh.msg_iov=&iov;mh.msg_iovlen=1;mh.msg_control=cb;mh.msg_controllen=sizeof cb;
  struct cmsghdr*cm=CMSG_FIRSTHDR(&mh);cm->cmsg_level=SOL_SOCKET;cm->cmsg_type=SCM_RIGHTS;cm->cmsg_len=CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cm),&fd,sizeof(int));return sendmsg(s,&mh,0)<0?-1:0;}
static int recv_fd(int s){char cb[CMSG_SPACE(sizeof(int))]={0};char d;struct iovec iov={&d,1};struct msghdr mh={0};
  mh.msg_iov=&iov;mh.msg_iovlen=1;mh.msg_control=cb;mh.msg_controllen=sizeof cb;
  if(recvmsg(s,&mh,0)<0)return -1;struct cmsghdr*cm=CMSG_FIRSTHDR(&mh);int fd=-1;if(cm)memcpy(&fd,CMSG_DATA(cm),sizeof(int));return fd;}

/* the node: counter in a fixed cell; each step bumps it then blocks in write() */
static void node(int w){
  if(mmap((void*)CELL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)!=(void*)CELL)_exit(90);
  *CELL=10; int l=install_filter(); if(l<0)_exit(91); if(send_fd(w,l))_exit(92);
  for(;;){*CELL=*CELL+1; char c='x'; (void)!write(777,&c,1);}  /* never returns; checker kills */
}
static int read_cell(pid_t p){int v=-1;struct iovec a={&v,sizeof v},b={(void*)CELL,sizeof v};process_vm_readv(p,&a,1,&b,1,0);return v;}

/* bring a notif-blocked pid to a ptrace stop, read regs into *saved */
static int stop_and_getregs(pid_t pid, struct user_regs_struct *saved){
  if(ptrace(PTRACE_INTERRUPT,pid,0,0))return -1; int st;
  for(int t=0;t<200;t++){pid_t w=waitpid(pid,&st,WNOHANG);if(w==pid)goto got;if(w<0)return -1;usleep(5000);}
  return -1;
 got: {struct iovec rv={saved,sizeof*saved};return ptrace(PTRACE_GETREGSET,pid,(void*)(long)NT_PRSTATUS,&rv);}
}
static int getregs_stopped(pid_t pid, struct user_regs_struct *saved){
  struct iovec rv={saved,sizeof*saved}; return ptrace(PTRACE_GETREGSET,pid,(void*)(long)NT_PRSTATUS,&rv);
}
/* inject fork into a ptrace-stopped pid; return the new child pid */
static pid_t inject_fork(pid_t pid, struct user_regs_struct saved){
  ptrace(PTRACE_SETOPTIONS,pid,0,(void*)(long)PTRACE_O_TRACEFORK);
  struct user_regs_struct r=saved; r.orig_rax=SYS_fork; r.rax=SYS_fork; r.rip=saved.rip-2;
  struct iovec wv={&r,sizeof r}; ptrace(PTRACE_SETREGSET,pid,(void*)(long)NT_PRSTATUS,&wv);
  ptrace(PTRACE_CONT,pid,0,0); int st; waitpid(pid,&st,0);
  if((st>>8)!=(SIGTRAP|(PTRACE_EVENT_FORK<<8)))return -1;
  unsigned long m=0; ptrace(PTRACE_GETEVENTMSG,pid,0,&m); pid_t ch=(pid_t)m; waitpid(ch,&st,0);
  /* restore the PARENT (frozen snapshot) to the blocked-write boundary and leave it stopped */
  struct iovec rv={&saved,sizeof saved}; ptrace(PTRACE_SETREGSET,pid,(void*)(long)NT_PRSTATUS,&rv);
  /* restore the CHILD too, then let it run to re-issue the write -> fresh notif */
  ptrace(PTRACE_SETREGSET,ch,(void*)(long)NT_PRSTATUS,&rv);
  return ch;
}
/* let a freshly re-forked copy run and service its fresh write() notif once */
static int resume_and_service(pid_t ch, int bump_to_check){
  ptrace(PTRACE_DETACH,ch,0,0);
  struct pollfd pf={lf,POLLIN,0};
  if(poll(&pf,1,4000)<=0)return -2;
  if(ioctl(lf,SECCOMP_IOCTL_NOTIF_RECV,req))return -3;
  int cell=read_cell(ch);  /* its counter after re-running one step */
  memset(resp,0,sz.seccomp_notif_resp);resp->id=req->id;resp->val=1;ioctl(lf,SECCOMP_IOCTL_NOTIF_SEND,resp);
  return cell==bump_to_check?0:cell;
}

int main(void){
  setvbuf(stdout,NULL,_IONBF,0); alarm(25); signal(SIGALRM,SIG_DFL);
  seccomp(SECCOMP_GET_NOTIF_SIZES,0,&sz);
  req=calloc(1,sz.seccomp_notif); resp=calloc(1,sz.seccomp_notif_resp);
  int sp[2]; socketpair(AF_UNIX,SOCK_DGRAM,0,sp);
  pid_t pid=fork(); if(pid==0){close(sp[0]);node(sp[1]);_exit(99);}
  close(sp[1]); lf=recv_fd(sp[0]); if(lf<0){printf("FAIL no lf\n");return 1;}
  if(ptrace(PTRACE_SEIZE,pid,0,0)){perror("seize");return 1;}
  struct pollfd pf={lf,POLLIN,0};
  if(poll(&pf,1,4000)<=0||ioctl(lf,SECCOMP_IOCTL_NOTIF_RECV,req)){printf("FAIL first notif\n");return 1;}
  printf("quiescent, counter=%d (want 11)\n",read_cell(pid));
  struct user_regs_struct saved;
  if(stop_and_getregs(pid,&saved)){printf("FAIL stop/getregs\n");kill(pid,SIGKILL);return 1;}
  /* SNAPSHOT: fork the frozen node once -> `snap` is the pristine freeze */
  pid_t snap=inject_fork(pid,saved);
  if(snap<0){printf("FAIL snapshot\n");kill(pid,SIGKILL);return 1;}
  printf("snapshot pid %d, counter=%d (want 11)\n",snap,read_cell(snap));
  /* the original (pid) was driven through fork; detach+kill it, we only keep the snapshot */
  ptrace(PTRACE_DETACH,pid,0,0); kill(pid,SIGKILL); { int st; waitpid(pid,&st,0); }
  /* RE-FORK the snapshot TWICE, resuming each copy to re-issue its write */
  int ok=1;
  for(int i=0;i<2;i++){
    struct user_regs_struct s2;
    if(getregs_stopped(snap,&s2)){printf("FAIL getregs snapshot (iter %d)\n",i);ok=0;break;}
    pid_t copy=inject_fork(snap,s2);
    if(copy<0){printf("FAIL re-fork (iter %d)\n",i);ok=0;break;}
    /* the copy re-issues write() -> bumps counter 11->12 and blocks */
    int r=resume_and_service(copy,12);
    printf("copy %d resumed: counter after one step = %d (want 12) -> %s\n",i,r==0?12:r,r==0?"OK":"MISMATCH");
    if(r!=0)ok=0;
    kill(copy,SIGKILL); { int st; waitpid(copy,&st,0); }
    /* the snapshot must be undisturbed: still 11 */
    int sc=read_cell(snap);
    printf("  snapshot still %d (want 11) -> %s\n",sc,sc==11?"OK":"DISTURBED");
    if(sc!=11)ok=0;
  }
  kill(snap,SIGKILL); { int st; waitpid(snap,&st,0); }
  printf("%s\n", ok?"OK: one snapshot re-forked and resumed twice, staying pristine":"FAIL");
  return ok?0:1;
}
