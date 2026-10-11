#include <stdio.h>
int sos(int x){int s=0;for(int i=1;i<=x;i++){s+=i*i;}return s;}
int main(void){printf("%d\n",sos(10));return 0;}
