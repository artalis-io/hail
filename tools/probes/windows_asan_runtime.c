/* Independent runtime sanity probe. --overflow must fail under ASan. */
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv)
{
    volatile unsigned char *p=malloc(8);
    if(!p) return 2;
    if(argc==2 && !strcmp(argv[1],"--overflow")) p[8]=1;
    else p[0]=1;
    free((void *)p);
    return 0;
}
