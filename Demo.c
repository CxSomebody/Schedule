#include "time.h"
#include <stdio.h>

void main()
{
  time_t timer,timerc;
  int count=1;  
  struct tm *timeinfo;
  time(&timer);//系统开始的时间
  int i = 0;
  while(1)
  {
     time(&timerc);
     if((timerc-timer)>=1)//每过1秒打印
     {
       printf("程序经过%d秒\n",count++);
       timer=timerc;
       i++;
       if (i > 8)
          break;
     }
  }
  
}
