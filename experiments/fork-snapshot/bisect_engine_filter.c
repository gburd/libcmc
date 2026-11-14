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
static int WITHREAD=1;
/* engine-scale filter: each OWN(n) is 2 insns. Build dynamically. */
static int inst(void){
  int withread=WITHREAD;
  int base[]={SYS_socket,SYS_setsockopt,SYS_bind,SYS_listen,SYS_connect,SYS_accept,SYS_accept4,SYS_write,SYS_recvfrom,SYS_sendto,SYS_clock_gettime,SYS_gettimeofday,SYS_getrandom,SYS_close,SYS_poll,SYS_ppoll,SYS_fcntl,SYS_eventfd2,SYS_epoll_create1,SYS_epoll_ctl,SYS_epoll_wait,SYS_clone,SYS_clone3,SYS_futex,SYS_exit};
  int n=sizeof base/sizeof*base;
  int list[40]; int m=0;
  for(int i=0;i<n;i++)list[m++]=base[i];
  if(withread)list[m++]=SYS_read;
  /* build: LD nr; then for each: JEQ n, +1, 0 ; RET USER_NOTIF ; (fall to next JEQ)... end RET ALLOW */
  struct sock_filter f[90]; int fi=0;
  f[fi++]=(struct sock_filter)BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr));
  for(int i=0;i<m;i++){ f[fi++]=(struct sock_filter)BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,list[i],0,1); f[fi++]=(struct sock_filter)BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_USER_NOTIF); }
  f[fi++]=(struct sock_filter)BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW);
  struct sock_fprog p={fi,f};
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
static void setr(pid_t p,struct user_regs_struct*r){struct iovec v={r,sizeof*r};ptrace(PTRACE_SETREGSET,p,(void*)(long)NT_PRSTATUS,&v);}
int main(int argc,char**argv){setvbuf(stdout,NULL,_IONBF,0);alarm(30);
  if(argc>1)WITHREAD=atoi(argv[1]); char eb[8];snprintf(eb,sizeof eb,"%d",WITHREAD);setenv("WR",eb,1);
  seccomp(SECCOMP_GET_NOTIF_SIZES,0,&sz);req=calloc(1,sz.seccomp_notif);resp=calloc(1,sz.seccomp_notif_resp);
  int sp[2];socketpair(AF_UNIX,SOCK_DGRAM,0,sp);pid_t pid=fork();
  if(pid==0){close(sp[0]);WITHREAD=atoi(getenv("WR"));node(sp[1]);_exit(99);}
  close(sp[1]);lf=rfd(sp[0]);ptrace(PTRACE_SEIZE,pid,0,0);
  for(int k=0;k<3;k++){struct pollfd pf={lf,POLLIN,0};poll(&pf,1,4000);memset(req,0,sz.seccomp_notif);ioctl(lf,SECCOMP_IOCTL_NOTIF_RECV,req);long nr=req->data.nr;memset(resp,0,sz.seccomp_notif_resp);resp->id=req->id;resp->val=handle(nr);ioctl(lf,SECCOMP_IOCTL_NOTIF_SEND,resp);}
  struct pollfd pf={lf,POLLIN,0};poll(&pf,1,4000);memset(req,0,sz.seccomp_notif);ioctl(lf,SECCOMP_IOCTL_NOTIF_RECV,req);
  printf("WITHREAD=%d quiescent nr=%lld\n",WITHREAD,(long long)req->data.nr);
  ptrace(PTRACE_INTERRUPT,pid,0,0);int st;waitpid(pid,&st,0);
  struct user_regs_struct s2;struct iovec r2={&s2,sizeof s2};ptrace(PTRACE_GETREGSET,pid,(void*)(long)NT_PRSTATUS,&r2);
  ptrace(PTRACE_SETOPTIONS,pid,0,(void*)(long)PTRACE_O_TRACEFORK);
  struct user_regs_struct r=s2;r.orig_rax=SYS_fork;r.rax=SYS_fork;r.rip=s2.rip-2;setr(pid,&r);
  ptrace(PTRACE_CONT,pid,0,0);waitpid(pid,&st,0);
  unsigned long m=0;ptrace(PTRACE_GETEVENTMSG,pid,0,&m);pid_t child=(pid_t)m;waitpid(child,&st,0);
  struct user_regs_struct b=s2;b.rip=s2.rip-2;b.rax=s2.orig_rax;setr(pid,&b);setr(child,&b);
  ptrace(PTRACE_CONT,pid,0,0);
  struct pollfd p2={lf,POLLIN,0};int pr=poll(&p2,1,4000);
  if(pr>0){memset(req,0,sz.seccomp_notif);ioctl(lf,SECCOMP_IOCTL_NOTIF_RECV,req);printf("RESUMED nr=%lld a0=%llx (want 43 a0=1f4)\n",(long long)req->data.nr,(unsigned long long)req->data.args[0]);}
  else printf("RESUMED NO notif poll=%d\n",pr);
  kill(pid,SIGKILL);kill(child,SIGKILL);return 0;
}
