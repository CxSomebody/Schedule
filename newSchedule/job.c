#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include "job.h"

void prischeduler();
struct waitqueue* prijobselect();

/*
*问题一：未解决抢占式运行
*
*/
int jobid=0;
int siginfo=1;
int fifo;
int globalfd;

int timedelay = 1;

struct waitqueue *head[3]={NULL,NULL,NULL};
struct waitqueue *next=NULL,*current =NULL;

/* 调度程序 */
void scheduler()
{
	struct jobinfo *newjob=NULL;
	struct jobcmd cmd;
	int  count = 0;
	bzero(&cmd,DATALEN);
	if((count=read(fifo,&cmd,DATALEN))<0)
		error_sys("read fifo failed");
#ifdef DEBUG

	if(count){
		printf("cmd cmdtype\t%d\ncmd defpri\t%d\ncmd data\t%s\n",cmd.type,cmd.defpri,cmd.data);
	}
	else
		printf("no data read\n");
#endif

	/* 更新等待队列中的作业 */
	updateall();
	//printf("After updateall\n");//debug
	switch(cmd.type){
	case ENQ:
		do_enq(newjob,cmd);
		break;
	case DEQ:
		do_deq(cmd);
		break;
	case STAT:
		do_stat(cmd);
		break;
	default:
		break;
	}

	/* 选择高优先级作业 */
	next=jobselect();
	//printf("After jobselect\n");//debug
	/* 作业切换 */
	jobswitch();
	//printf("After jobswitch\n");//debug
}



int allocjid()
{
	return ++jobid;
}

void updateall()
{
	struct waitqueue *p,*prev,*select,*selectprev;
	struct waitqueue *q;
	int i = 0;
	/* 更新作业运行时间 */
	if(current)
		current->job->run_time += 1; /* 加1代表1000ms */

	/* 更新作业等待时间及优先级 */
	/* 更新最高优先级的队列等待时间 */
	for(p = head[0],prev = head[0]; p != NULL; prev = p , p = p->next)
		p->job->wait_time += 1000;
	/* 更新较低两个优先级的队列等待时间，涉及提升优先级 */
	for(i = 1; i <= 2; i++){
		for(p = head[i],prev = head[i]; p != NULL; prev = p , p = p->next){
			p->job->wait_time += 1000;
			if(p->job->wait_time >= 10000){
				select = p;
				selectprev = prev;
				select->job->curpri = i - 1;/* 提升优先级 */
				select->job->wait_time = 0;
				selectprev->next = select->next;
				/* select为队首元素 */
				if(select == selectprev)
					head[i] = head[i]->next;
				select->next = NULL;
				/* 提升队列 */
				if(head[select->job->curpri]){
					for(q = head[select->job->curpri]; q->next != NULL; q = q->next);
						q->next = select;
				}else{
					head[select->job->curpri] = select;
				}
			}
		}
	}
}

struct waitqueue* jobselect()
{
	struct waitqueue *select;
	int i = 0; 
	int pri = 2;/* 最低优先级 */

	select = NULL;
	if(current)
		pri = current->job->curpri;
	//quepri();//debug
	/* 只有大于等于当前任务优先级的队列才会被选中 */
	for(i = 0; i <= pri; i++){
		if(head[i]){
			/* 找到队列首作业即可,取出该作业 */
			select = head[i];
			head[i] = head[i]->next;
			select->next = NULL;//mark,为什么不需要？
			break;
		}
	}
	return select;
}

void jobswitch()
{
	struct waitqueue *p;
	int i;

	if(current && current->job->state == DONE){ /* 当前作业完成 */
		/* 作业完成，删除它 */
		for(i = 0;(current->job->cmdarg)[i] != NULL; i++){
			free((current->job->cmdarg)[i]);
			(current->job->cmdarg)[i] = NULL;
		}
		/* 释放空间 */
		free(current->job->cmdarg);
		free(current->job);
		free(current);

		current = NULL;
	}

	if(next == NULL && current == NULL) /* 没有作业要运行 */

		return;
	else if (next != NULL && current == NULL){ /* 开始新的作业 */

		printf("begin start new job\n");
		current = next;
		next = NULL;
		current->job->state = RUNNING;
		kill(current->job->pid,SIGCONT);
		return;
	}
	else if (next != NULL && current != NULL){ /* 切换作业 */

		printf("switch to Pid: %d\n",next->job->pid);
		kill(current->job->pid,SIGSTOP);
		current->job->curpri = current->job->defpri;
		current->job->wait_time = 0;
		current->job->state = READY;

		/* 放回等待队列 */
		if(head[current->job->curpri]){
			for(p = head[current->job->curpri]; p->next != NULL; p = p->next);
			p->next = current;
		}else{
			head[current->job->curpri] = current;
		}
		current = next;
		next = NULL;
		current->job->state = RUNNING;
		current->job->wait_time = 0;
		kill(current->job->pid,SIGCONT);
		return;
	}else{ /* next == NULL且current != NULL，不切换 */
		return;
	}
}

void sig_handler(int sig,siginfo_t *info,void *notused)
{
	int status;
	int ret;

	switch (sig) {
case SIGVTALRM: /* 到达计时器所设置的计时间隔 */
	timedelay--;
	if(timedelay == 0){
		scheduler();
		if(current != NULL && current->job->curpri == 1)
			timedelay = 2;
		else if(current != NULL && current->job->curpri == 2)
			timedelay = 5;
		else
			timedelay = 1;
	}
	else
		prischeduler();
	//printf("timedelay:%d\n",timedelay);//debug
	return;
case SIGCHLD: /* 子进程结束时传送给父进程的信号 */
	ret = waitpid(-1,&status,WNOHANG);
	if (ret == 0)
		return;
	if(WIFEXITED(status)){
		current->job->state = DONE;
		printf("normal termation, exit status = %d\n",WEXITSTATUS(status));
	}else if (WIFSIGNALED(status)){
		printf("abnormal termation, signal number = %d\n",WTERMSIG(status));
	}else if (WIFSTOPPED(status)){
		printf("child stopped, signal number = %d\n",WSTOPSIG(status));
	}
	return;
	default:
		return;
	}
}

void do_enq(struct jobinfo *newjob,struct jobcmd enqcmd)
{
	struct waitqueue *newnode,*p;
	int i=0,pid;
	char *offset,*argvec,*q;
	char **arglist;
	sigset_t zeromask;

	sigemptyset(&zeromask);

	/* 封装jobinfo数据结构 */
	newjob = (struct jobinfo *)malloc(sizeof(struct jobinfo));
	newjob->jid = allocjid();
	newjob->defpri = enqcmd.defpri;
	newjob->curpri = enqcmd.defpri;
	newjob->ownerid = enqcmd.owner;
	newjob->state = READY;
	newjob->create_time = time(NULL);
	newjob->wait_time = 0;
	newjob->run_time = 0;
	arglist = (char**)malloc(sizeof(char*)*(enqcmd.argnum+1));
	newjob->cmdarg = arglist;
	offset = enqcmd.data;
	argvec = enqcmd.data;
	while (i < enqcmd.argnum){
		if(*offset == ':'){
			*offset++ = '\0';
			q = (char*)malloc(offset - argvec);
			strcpy(q,argvec);
			arglist[i++] = q;
			argvec = offset;
		}else
			offset++;
	}

	arglist[i] = NULL;

#ifdef DEBUG

	printf("enqcmd argnum %d\n",enqcmd.argnum);
	for(i = 0;i < enqcmd.argnum; i++)
		printf("parse enqcmd:%s\n",arglist[i]);

#endif

	/*向等待队列中增加新的作业*/
	newnode = (struct waitqueue*)malloc(sizeof(struct waitqueue));
	newnode->next =NULL;
	newnode->job=newjob;

	if(head[newnode->job->defpri])
	{
		for(p=head[newnode->job->defpri];p->next != NULL; p=p->next);
		p->next =newnode;
	}else
		head[newnode->job->defpri]=newnode;

	/*为作业创建进程*/
	if((pid=fork())<0)
		error_sys("enq fork failed");

	if(pid==0){
		newjob->pid =getpid();
		/*阻塞子进程,等等执行*/
		raise(SIGSTOP);
#ifdef DEBUG

		printf("begin running\n");
		for(i=0;arglist[i]!=NULL;i++)
			printf("arglist %s\n",arglist[i]);
#endif

		/*复制文件描述符到标准输出*/
		dup2(globalfd,1);
		/* 执行命令 */
		if(execv(arglist[0],arglist)<0)
			printf("exec failed\n");
		exit(1);
	}else{
		newjob->pid=pid;
		waitpid(pid,NULL,0);///////Zhou Huaping,mark
	}
}

void do_deq(struct jobcmd deqcmd)
{
	int deqid,i;
	struct waitqueue *p,*prev,*select,*selectprev;
	deqid=atoi(deqcmd.data);

#ifdef DEBUG
	printf("deq jid %d\n",deqid);
#endif

	/*current jodid==deqid,终止当前作业*/
	if (current && current->job->jid ==deqid){
		printf("teminate current job\n");
		kill(current->job->pid,SIGKILL);
		for(i=0;(current->job->cmdarg)[i]!=NULL;i++){
			free((current->job->cmdarg)[i]);
			(current->job->cmdarg)[i]=NULL;
		}
		free(current->job->cmdarg);
		free(current->job);
		free(current);
		current=NULL;
	}
	else{ /* 或者在等待队列中查找deqid */
		select=NULL;
		selectprev=NULL;
		for(i = 0; i <= 2; i++){
			if(head[i]){
				for(prev=head[i],p=head[i];p!=NULL;prev=p,p=p->next)
					if(p->job->jid==deqid){
						select=p;
						selectprev=prev;
						break;
					}
				/* 当p==NULL时代表在此队列中没有寻找到deqid */
				if(p == NULL)
					continue;
				selectprev->next=select->next;
				if(select==selectprev)
					head[i]=NULL;
			}
		}
		if(select){
			for(i=0;(select->job->cmdarg)[i]!=NULL;i++){
				free((select->job->cmdarg)[i]);
				(select->job->cmdarg)[i]=NULL;
			}
			free(select->job->cmdarg);
			free(select->job);
			free(select);
			select=NULL;
		}
	}
}

void do_stat(struct jobcmd statcmd)
{
	struct waitqueue *p;
	char timebuf[BUFLEN];
	int i = 0;
	/*
	*打印所有作业的统计信息:
	*1.作业ID
	*2.进程ID
	*3.作业所有者
	*4.作业运行时间
	*5.作业等待时间
	*6.作业创建时间
	*7.作业状态
	*/

	/* 打印信息头部 */
	printf("JOBID\tPID\tOWNER\tRUNTIME\tWAITTIME\tCREATTIME\t\tSTATE\n");
	if(current){
		strcpy(timebuf,ctime(&(current->job->create_time)));
		timebuf[strlen(timebuf)-1]='\0';
		printf("%d\t%d\t%d\t%d\t%d\t%s\t%s\n",
			current->job->jid,
			current->job->pid,
			current->job->ownerid,
			current->job->run_time,
			current->job->wait_time,
			timebuf,"RUNNING");
	}
	for(i = 0; i <= 2; i++){
		for(p=head[i];p!=NULL;p=p->next){
			strcpy(timebuf,ctime(&(p->job->create_time)));
			timebuf[strlen(timebuf)-1]='\0';
			printf("%d\t%d\t%d\t%d\t%d\t%s\t%s\n",
				p->job->jid,
				p->job->pid,
				p->job->ownerid,
				p->job->run_time,
				p->job->wait_time,
				timebuf,
				"READY");
		}
	}
}




/* 抢占式调度 */
void prischeduler()
{
	struct jobinfo *newjob=NULL;
	struct jobcmd cmd;
	int  count = 0;
	bzero(&cmd,DATALEN);
	if((count=read(fifo,&cmd,DATALEN))<0)
		error_sys("read fifo failed");
#ifdef DEBUG

	if(count){
		printf("cmd cmdtype\t%d\ncmd defpri\t%d\ncmd data\t%s\n",cmd.type,cmd.defpri,cmd.data);
	}
	else
		printf("no data read\n");
#endif

	/* 更新等待队列中的作业 */
	updateall();
	//printf("After updateall\n");//debug
	switch(cmd.type){
	case ENQ:
		do_enq(newjob,cmd);
		break;
	case DEQ:
		do_deq(cmd);
		break;
	case STAT:
		do_stat(cmd);
		break;
	default:
		break;
	}

	/* 选择高优先级作业 */	
	next=prijobselect();
	//printf("After jobselect\n");//debug
	/* 作业切换 */
	jobswitch();
	//printf("After jobswitch\n");//debug
}
/* 抢占式作业选择（仅用在抢占式调度里） */
struct waitqueue* prijobselect()
{
	struct waitqueue *select;
	int i = 0; 
	int pri = 2;/* 最低优先级 */

	select = NULL;
	if(current)
		pri = current->job->curpri;
	//quepri();//debug
	/* 只有大于当前任务优先级的队列才会被选中 */
	for(i = 0; i < pri; i++){
		if(head[i]){
			/* 找到队列首作业即可,取出该作业 */
			select = head[i];
			head[i] = head[i]->next;
			select->next = NULL;//mark,为什么不需要？
			break;
		}
	}
	return select;
}


/*void quepri(){
	int i = 0;
	struct waitqueue *p;
	char timebuf[BUFLEN];
	for(i = 0; i <= 2; i++){
		for(p=head[i];p!=NULL;p=p->next){
			strcpy(timebuf,ctime(&(p->job->create_time)));
			timebuf[strlen(timebuf)-1]='\0';
			printf("%d\t%d\t%d\t%d\t%d\t%s\t%s\n",
				p->job->jid,
				p->job->pid,
				p->job->ownerid,
				p->job->run_time,
				p->job->wait_time,
				timebuf,
				"READY");
		}
	}//////////////////////////////////////////////debug
}*/

int main()
{
	struct timeval interval;
	struct itimerval new,old;
	struct stat statbuf;
	struct sigaction newact,oldact1,oldact2;

	if(stat("/tmp/server",&statbuf)==0){
		/* 如果FIFO文件存在,删掉 */
		if(remove("/tmp/server")<0)
			error_sys("remove failed");
	}

	if(mkfifo("/tmp/server",0666)<0)
		error_sys("mkfifo failed");
	/* 在非阻塞模式下打开FIFO */
	if((fifo=open("/tmp/server",O_RDONLY|O_NONBLOCK))<0)
		error_sys("open fifo failed");

	/* 建立信号处理函数 */
	newact.sa_sigaction=sig_handler;
	sigemptyset(&newact.sa_mask);
	newact.sa_flags=SA_SIGINFO;
	sigaction(SIGCHLD,&newact,&oldact1);
	sigaction(SIGVTALRM,&newact,&oldact2);

	/* 设置时间间隔为1000毫秒 */
	interval.tv_sec=1;
	interval.tv_usec=0;

	new.it_interval=interval;
	new.it_value=interval;
	setitimer(ITIMER_VIRTUAL,&new,&old);

	while(siginfo==1);

	close(fifo);
	close(globalfd);
	return 0;
}
